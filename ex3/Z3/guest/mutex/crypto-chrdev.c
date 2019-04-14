/*
 * crypto-chrdev.c
 *
 * Implementation of character devices
 * for virtio-crypto device
 *
 * Vangelis Koukis <vkoukis@cslab.ece.ntua.gr>
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 * Stefanos Gerangelos <sgerag@cslab.ece.ntua.gr>
 *
 */
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>

#include "crypto.h"
#include "crypto-chrdev.h"
#include "debug.h"

#include "cryptodev.h"

/*
 * Global data
 */
struct cdev crypto_chrdev_cdev;

/**
 * Given the minor number of the inode return the crypto device
 * that owns that number.
 **/
static struct crypto_device *get_crypto_dev_by_minor(unsigned int minor)
{
	struct crypto_device *crdev;
	unsigned long flags;

	debug("Entering");

	spin_lock_irqsave(&crdrvdata.lock, flags);
	list_for_each_entry(crdev, &crdrvdata.devs, list) {
		if (crdev->minor == minor)
			goto out;
	}
	crdev = NULL;

out:
	spin_unlock_irqrestore(&crdrvdata.lock, flags);

	debug("Leaving");
	return crdev;
}

/*************************************
 * Implementation of file operations
 * for the Crypto character device
 *************************************/

static int crypto_chrdev_open(struct inode *inode, struct file *filp)
{
	int ret = 0;
	unsigned int len, num_out, num_in;
	struct crypto_open_file *crof;
	struct crypto_device *crdev;
	unsigned int *syscall_type;
	int *host_fd;
	struct scatterlist syscall_type_sg, host_fd_sg, *sgs[2];

	debug("Entering open");

	num_in	= 0;
	num_out	= 0;
	syscall_type = kzalloc(sizeof(unsigned int), GFP_KERNEL);
	*syscall_type = VIRTIO_CRYPTO_SYSCALL_OPEN;
	host_fd = kzalloc(sizeof(int), GFP_KERNEL);
	*host_fd = -1;

	ret = -ENODEV;
	if ((ret = nonseekable_open(inode, filp)) < 0)
		goto fail;

	/* Associate this open file with the relevant crypto device. */
	crdev = get_crypto_dev_by_minor(iminor(inode));
	if (!crdev) {
		debug("Could not find crypto device with %u minor", iminor(inode));
		ret = -ENODEV;
		goto fail;
	}

	crof = kzalloc(sizeof(*crof), GFP_KERNEL);
	if (!crof) {
		ret = -ENOMEM;
		goto fail;
	}
	crof->crdev = crdev;
	crof->host_fd = -1;
	filp->private_data = crof;

	/**
	 * We need two sg lists, one for syscall_type and one to get the
	 * file descriptor from the host.
	 **/
	sg_init_one(&syscall_type_sg, syscall_type, sizeof(unsigned int));
	sgs[num_out++] = &syscall_type_sg;
	sg_init_one(&host_fd_sg, host_fd, sizeof(int));
	sgs[num_out + num_in++] = &host_fd_sg;

	/**
	 * Send data to the host.
	 **/

	/* Lock ?? */
	if (down_interruptible(&crdev->lock)) return -ERESTARTSYS;
	ret = virtqueue_add_sgs(crdev->vq, sgs, num_out, num_in, &syscall_type_sg, GFP_ATOMIC);
	if (ret) {
		debug("virtqueue_add_sgs failed in open");
		up(&crdev->lock);
		goto fail;
	}
	virtqueue_kick(crdev->vq);

	/**
	 * Wait for the host to process our data.
	 **/
	while (virtqueue_get_buf(crdev->vq, &len) == NULL);	/* do nothing */

	/* Unlock */
	up(&crdev->lock);

	/* If host failed to open() return -ENODEV. */
	if (*host_fd == -1) {
		debug("open(/dev/crypto)");
		ret = -ENODEV;
		goto fail;
	}
	crof->host_fd = *host_fd;

fail:
	kfree(syscall_type);
	kfree(host_fd);
	debug("Leaving open");
	return ret;
}

static int crypto_chrdev_release(struct inode *inode, struct file *filp)
{
	int ret = 0;
	struct crypto_open_file *crof = filp->private_data;
	struct crypto_device *crdev = crof->crdev;
	unsigned int *syscall_type;
	unsigned int num_out, num_in, len;
	int *host_fd;
	struct scatterlist syscall_type_sg, host_fd_sg, *sgs[2];
	struct virtqueue *vq = crdev->vq;


	debug("Entering release");

	num_out = 0;
	num_in = 0;

	syscall_type = kzalloc(sizeof(unsigned int), GFP_KERNEL);
	*syscall_type = VIRTIO_CRYPTO_SYSCALL_CLOSE;
	host_fd = kzalloc(sizeof(int), GFP_KERNEL);
	*host_fd = crof->host_fd;

	sg_init_one(&syscall_type_sg, syscall_type, sizeof(unsigned int));
	sgs[num_out++] = &syscall_type_sg;
	sg_init_one(&host_fd_sg, host_fd, sizeof(int));
	sgs[num_out + num_in++] = &host_fd_sg;
	/* host_fd with W flag, so we can return errno */

	/**
	 * Send data to the host.
	 **/

	/* Lock ?? */
	if (down_interruptible(&crdev->lock)) return -ERESTARTSYS;

	ret = virtqueue_add_sgs(vq, sgs, num_out, num_in, &syscall_type_sg, GFP_ATOMIC);
	if (ret) {
		debug("virtqueue_add_sgs failed in close");
		up(&crdev->lock);
		goto out1;
	}
	virtqueue_kick(vq);

	/**
	 * Wait for the host to process our data.
	 **/
	while (virtqueue_get_buf(vq, &len) == NULL);	/* do nothing */

	/* Unlock */
	up(&crdev->lock);

	if (*host_fd) {	/* *host_fd = 0->SUCCESS | errno->failure */
		debug("release failed");
		ret = *host_fd;
	}

out1:
	kfree(syscall_type);
	kfree(host_fd);
	kfree(crof);

	debug("Leaving release");
	return ret;

}

static long crypto_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long ret = 0;
	int err;
	struct crypto_open_file *crof = filp->private_data;
	struct crypto_device *crdev = crof->crdev;
	struct virtqueue *vq = crdev->vq;
	struct scatterlist syscall_type_sg, host_fd_sg, ioctl_cmd_sg, host_return_val_sg,
						session_key_sg, session_op_sg, ses_id_sg,
						crypt_op_sg, src_sg, iv_sg, dst_sg, *sgs[8];
	unsigned int num_out, num_in, len;
	unsigned int *syscall_type, *ioctl_cmd;
	int *host_fd, *host_return_val;
	struct session_op *session_op;
	struct crypt_op *crypt_op;
	unsigned char *src, *iv, *dst, *session_key, *temp;
	__u32 *ses_id;

	debug("Entering ioctl");

	/**
	 * Allocate all data that will be sent to the host.
	 **/
	syscall_type = kzalloc(sizeof(unsigned int), GFP_KERNEL);
	*syscall_type = VIRTIO_CRYPTO_SYSCALL_IOCTL;
	host_fd = kzalloc(sizeof(int), GFP_KERNEL);
	*host_fd = crof->host_fd;
	ioctl_cmd = kzalloc(sizeof(unsigned int), GFP_KERNEL);
	*ioctl_cmd = cmd;

	host_return_val = kzalloc(sizeof(int), GFP_KERNEL);
	*host_return_val = -1;

	num_out = 0;
	num_in = 0;

	/**
	 *  These are common to all ioctl commands.
	 **/
	sg_init_one(&syscall_type_sg, syscall_type, sizeof(unsigned int));
	sgs[num_out++] = &syscall_type_sg;
	sg_init_one(&host_fd_sg, host_fd, sizeof(int));
	sgs[num_out++] = &host_fd_sg;
	sg_init_one(&ioctl_cmd_sg, ioctl_cmd, sizeof(unsigned int));
	sgs[num_out++] = &ioctl_cmd_sg;

	/**
	 *  Add all the cmd specific sg lists.
	 **/
	switch (cmd) {
		case CIOCGSESSION:
			debug("CIOCGSESSION");
			session_op = kzalloc(sizeof(struct session_op), GFP_KERNEL);
			if (copy_from_user(session_op, (struct session_op *) arg, sizeof(struct session_op)))
			{
				kfree(session_op);
				return -EFAULT;
			}

			session_key = kzalloc(sizeof(char) * session_op->keylen, GFP_KERNEL);
			if (copy_from_user(session_key, session_op->key, sizeof(char) * session_op->keylen)) {
				kfree(session_op);
				kfree(session_key);
				return -EFAULT;
			}

			temp = session_op->key;
			session_op->key = session_key;

			sg_init_one(&session_key_sg, session_key, sizeof(char) * session_op->keylen);
			sgs[num_out++] = &session_key_sg;
			sg_init_one(&session_op_sg, session_op, sizeof(struct session_op));
			sgs[num_out + num_in++] = &session_op_sg;

			sg_init_one(&host_return_val_sg, host_return_val, sizeof(int));
			sgs[num_out + num_in++] = &host_return_val_sg;

			/* Lock ?? */
			if (down_interruptible(&crdev->lock)) return -ERESTARTSYS;

			err = virtqueue_add_sgs(vq, sgs, num_out, num_in, &syscall_type_sg, GFP_ATOMIC);
			if (err) {
				debug("virtqueue_add_sgs failed in CIOCGSESSION");
				up(&crdev->lock);
				ret = err;
				goto out2;
			}
			virtqueue_kick(vq);

			/**
			 * Wait for the host to process our data.
			 **/
			while (virtqueue_get_buf(vq, &len) == NULL);	/* do nothing */

			/* Unlock */
			up(&crdev->lock);

			session_op->key = temp;
			if (copy_to_user((struct session_op *) arg, session_op, sizeof(struct session_op))){
				kfree(session_key);
				kfree(session_op);
				return -EFAULT;
			}

			ret = *host_return_val;

			kfree(session_key);
			kfree(session_op);
			break;

		case CIOCFSESSION:
			debug("CIOCFSESSION");
			ses_id = kzalloc(sizeof(__u32), GFP_KERNEL);
			if (copy_from_user(ses_id, (__u32 *) arg, sizeof(__u32)))
			{
				kfree(ses_id);
				return -EFAULT;
			}
			sg_init_one(&ses_id_sg, ses_id, sizeof(__u32));
			sgs[num_out++] = &ses_id_sg;

			sg_init_one(&host_return_val_sg, host_return_val, sizeof(int));
			sgs[num_out + num_in++] = &host_return_val_sg;

			/* Lock ?? */
			if (down_interruptible(&crdev->lock)) return -ERESTARTSYS;

			err = virtqueue_add_sgs(vq, sgs, num_out, num_in, &syscall_type_sg, GFP_ATOMIC);
			if (err) {
				debug("virtqueue_add_sgs failed in CIOCFSESSION");
				up(&crdev->lock);
				ret = err;
				goto out2;
			}
			virtqueue_kick(vq);

			/**
			 * Wait for the host to process our data.
			 **/
			while (virtqueue_get_buf(vq, &len) == NULL);	/* do nothing */

			/* Unlock */
			up(&crdev->lock);

			ret = *host_return_val;

			kfree(ses_id);
			break;

		case CIOCCRYPT:
			debug("CIOCCRYPT");
			crypt_op = kzalloc(sizeof(struct crypt_op), GFP_KERNEL);
			if (copy_from_user(crypt_op, (struct crypt_op *) arg, sizeof(struct crypt_op)))
			{
				kfree(crypt_op);
				return -EFAULT;
			}

			src = kzalloc(sizeof(char) * crypt_op->len, GFP_KERNEL);
			if (copy_from_user(src, crypt_op->src, sizeof(char) * crypt_op->len)) {
				kfree(crypt_op);
				kfree(src);
				return -EFAULT;
			}

			iv = kzalloc(sizeof(char) * VIRTIO_CRYPTO_BLOCK_SIZE, GFP_KERNEL);
			if (copy_from_user(iv, crypt_op->iv, sizeof(char) * VIRTIO_CRYPTO_BLOCK_SIZE)) {
				kfree(crypt_op);
				kfree(src);
				kfree(iv);
				return -EFAULT;
			}

			dst = kzalloc(sizeof(char) * crypt_op->len, GFP_KERNEL);
			
			temp = crypt_op->dst;
			crypt_op->src = src;
			crypt_op->iv = iv;
			crypt_op->dst = dst;

			sg_init_one(&crypt_op_sg, crypt_op, sizeof(struct crypt_op));
			sgs[num_out++] = &crypt_op_sg;
			sg_init_one(&src_sg, crypt_op->src, sizeof(char) * crypt_op->len);
			sgs[num_out++] = &src_sg;
			sg_init_one(&iv_sg, crypt_op->iv, sizeof(char) * VIRTIO_CRYPTO_BLOCK_SIZE);
			sgs[num_out++] = &iv_sg;
			sg_init_one(&dst_sg, crypt_op->dst, sizeof(char) * crypt_op->len);
			sgs[num_out + num_in++] = &dst_sg;

			sg_init_one(&host_return_val_sg, host_return_val, sizeof(int));
			sgs[num_out + num_in++] = &host_return_val_sg;

			/* Lock ?? */
			if (down_interruptible(&crdev->lock)) return -ERESTARTSYS;

			err = virtqueue_add_sgs(vq, sgs, num_out, num_in, &syscall_type_sg, GFP_ATOMIC);
			if (err) {
				debug("virtqueue_add_sgs failed in CIOCCRYPT");
				up(&crdev->lock);
				ret = err;
				goto out2;
			}
			virtqueue_kick(vq);

			/**
			 * Wait for the host to process our data.
			 **/
			while (virtqueue_get_buf(vq, &len) == NULL);	/* do nothing */

			/* Unlock */
			up(&crdev->lock);

			if (copy_to_user(temp, dst, sizeof(char) * crypt_op->len))
			{
				kfree(crypt_op);
				kfree(src);
				kfree(iv);
				kfree(dst);
				return -EFAULT;
			}

			ret = *host_return_val;
			kfree(crypt_op);
			kfree(src);
			kfree(iv);
			kfree(dst);
			break;

		default:
			debug("Unsupported ioctl command");
			break;
	}

out2:
	kfree(host_return_val);
	kfree(host_fd);
	kfree(ioctl_cmd);
	kfree(syscall_type);

	debug("Leaving ioctl");

	return ret;
}

static ssize_t crypto_chrdev_read(struct file *filp, char __user *usrbuf,
                                  size_t cnt, loff_t *f_pos)
{
	debug("Entering");
	debug("Leaving");
	return -EINVAL;
}

static struct file_operations crypto_chrdev_fops =
{
	.owner          = THIS_MODULE,
	.open           = crypto_chrdev_open,
	.release        = crypto_chrdev_release,
	.read           = crypto_chrdev_read,
	.unlocked_ioctl = crypto_chrdev_ioctl,
};

int crypto_chrdev_init(void)
{
	int ret;
	dev_t dev_no;
	unsigned int crypto_minor_cnt = CRYPTO_NR_DEVICES;

	debug("Initializing character device...");
	cdev_init(&crypto_chrdev_cdev, &crypto_chrdev_fops);
	crypto_chrdev_cdev.owner = THIS_MODULE;

	dev_no = MKDEV(CRYPTO_CHRDEV_MAJOR, 0);
	ret = register_chrdev_region(dev_no, crypto_minor_cnt, "crypto_devs");
	if (ret < 0) {
		debug("failed to register region, ret = %d", ret);
		goto out;
	}
	ret = cdev_add(&crypto_chrdev_cdev, dev_no, crypto_minor_cnt);
	if (ret < 0) {
		debug("failed to add character device");
		goto out_with_chrdev_region;
	}

	debug("Completed successfully");
	return 0;

out_with_chrdev_region:
	unregister_chrdev_region(dev_no, crypto_minor_cnt);
out:
	return ret;
}

void crypto_chrdev_destroy(void)
{
	dev_t dev_no;
	unsigned int crypto_minor_cnt = CRYPTO_NR_DEVICES;

	debug("entering");
	dev_no = MKDEV(CRYPTO_CHRDEV_MAJOR, 0);
	cdev_del(&crypto_chrdev_cdev);
	unregister_chrdev_region(dev_no, crypto_minor_cnt);
	debug("leaving");
}
