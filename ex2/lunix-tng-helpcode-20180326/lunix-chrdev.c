/*
 * lunix-chrdev.c
 *
 * Implementation of character devices
 * for Lunix:TNG
 *
 * < Angeliki Giannou, Emmanouil Vasilakis >
 *
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mmzone.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>

#include "lunix.h"
#include "lunix-chrdev.h"
#include "lunix-lookup.h"

/*
 * Global data
 */
struct cdev lunix_chrdev_cdev;

/*
 * Just a quick [unlocked] check to see if the cached
 * chrdev state needs to be updated from sensor measurements.
 */
static int lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;
	WARN_ON ( !(sensor = state->sensor));

	if (state->buf_timestamp < sensor->msr_data[state->type]->last_update) return 1;
	else return 0;
}

/*
 * Updates the cached state of a character device
 * based on sensor data. Must be called with the
 * character device state lock held.
 */
static int lunix_chrdev_state_update(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;
	uint16_t temp;								//grab measurement without formatting in spinlock
	long num, akeraio, dekadiko;
	char sign;

	WARN_ON ( !(sensor = state->sensor));

	debug("updating\n");

	if (lunix_chrdev_state_needs_refresh(state) == 0) {
		debug("leaving without updating\n");
		return -EAGAIN;
	}

	spin_lock(&sensor->lock);					/* Why use spinlocks? See LDD3, p. 119 */
	state->buf_timestamp = sensor->msr_data[state->type]->last_update;
	temp = sensor->msr_data[state->type]->values[0];
	spin_unlock(&sensor->lock);

	if (state->type == BATT) num = lookup_voltage[temp];
	else if (state->type == TEMP) num = lookup_temperature[temp];
	else num = lookup_light[temp];

	if (num == 0) {
		state->buf_lim = sprintf(state->buf_data, "0\n");
		debug("leaving\n");
		return 0;
	}
	else if (num > 0) sign = '+';
	else {
		sign = '-';
		num *= -1;
	}

	dekadiko = num % 1000;				// edo ta 3 psifia poy deixnoyn to float kommati
	akeraio = num / 1000;				// edo ta 2 psifia poy deixnoyn to int kommati

	state->buf_lim = sprintf(state->buf_data, "%c%ld.%ld\n", sign, akeraio, dekadiko);

	debug("leaving\n");
	return 0;
}

/*************************************
 * Implementation of file operations
 * for the Lunix character device
 *************************************/

static int lunix_chrdev_open(struct inode *inode, struct file *filp)
{
	/* Declarations */
	unsigned int minor, type_no, sensor_no;
	struct lunix_chrdev_state_struct *state;
	int ret;

	debug("entering\n");
	ret = -ENODEV;
	if ((ret = nonseekable_open(inode, filp)) < 0)
		goto out;

	/*
	 * Associate this open file with the relevant sensor based on
	 * the minor number of the device node [/dev/sensor<NO>-<TYPE>]
	 */
	minor = iminor(inode);
	type_no = minor % 8;
	sensor_no = minor / 8;

	state = (struct lunix_chrdev_state_struct *) kmalloc(sizeof(struct lunix_chrdev_state_struct), GFP_KERNEL);
	if (state == NULL) goto out;

	state->buf_timestamp = 0;
	if (type_no == 0) state->type = BATT;
	else if (type_no == 1) state->type = TEMP;
	else if (type_no == 2) state->type = LIGHT;
	else goto out;
	sema_init(&state->lock, 1);
	state->sensor = &lunix_sensors[sensor_no];

	filp->private_data = state;

	/* Allocate a new Lunix character device private state structure */
	/* ? */
out:
	debug("leaving, with ret = %d\n", ret);
	return ret;
}

static int lunix_chrdev_release(struct inode *inode, struct file *filp)
{
	struct lunix_chrdev_state_struct *state;
	WARN_ON ( !(state = filp->private_data));

	state->sensor = NULL;
	kfree(state);
	return 0;
	/*kfree(filp->private_data);
	return 0;*/
}

static long lunix_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/* Why? */
	return -EINVAL;
}

static ssize_t lunix_chrdev_read(struct file *filp, char __user *usrbuf, size_t cnt, loff_t *f_pos)
{
	ssize_t ret;

	struct lunix_sensor_struct *sensor;
	struct lunix_chrdev_state_struct *state;

	state = filp->private_data;
	WARN_ON(!state);

	sensor = state->sensor;
	WARN_ON(!sensor);

	/* Corner Case */
	if (cnt == 0)
		return 0;

	/* Lock? */
	if (down_interruptible(&state->lock))
		return -ERESTARTSYS;

	/*
	 * If the cached character device state needs to be
	 * updated by actual sensor data (i.e. we need to report
	 * on a "fresh" measurement, do so
	 */
	if (*f_pos == 0) {
		while (lunix_chrdev_state_update(state) == -EAGAIN) {
			/* The process needs to sleep */
			/* See LDD3, page 153 for a hint */
			up(&state->lock); /* release the lock */
			if (wait_event_interruptible(sensor->wq, lunix_chrdev_state_needs_refresh(state) == 1))
				return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
			/* otherwise loop, but first reacquire the lock */
			if (down_interruptible(&state->lock))
				return -ERESTARTSYS;
		}
	}

	/* Determine the number of cached bytes to copy to userspace */
	if (cnt < ((sizeof(unsigned char) * state->buf_lim) - *f_pos)) ret = cnt;
	else ret = (sizeof(unsigned char) * state->buf_lim) - *f_pos;

	if (copy_to_user(usrbuf, state->buf_data + *f_pos, (unsigned long) ret)) {
		up (&state->lock);
		return -EFAULT;
	}

	/* Auto-rewind on EOF mode? */
	/* ? */
	*f_pos += ret;
	if (*f_pos == sizeof(unsigned char) * state->buf_lim) *f_pos = 0; /* wrapped */

//out:
	/* Unlock? */
	up (&state->lock);
	return ret;
}

static int lunix_chrdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return -EINVAL;
}

static struct file_operations lunix_chrdev_fops =
{
    .owner          = THIS_MODULE,
	.open           = lunix_chrdev_open,
	.release        = lunix_chrdev_release,
	.read           = lunix_chrdev_read,
	.unlocked_ioctl = lunix_chrdev_ioctl,
	.mmap           = lunix_chrdev_mmap
};

int lunix_chrdev_init(void)
{
	/*
	 * Register the character device with the kernel, asking for
	 * a range of minor numbers (number of sensors * 8 measurements / sensor)
	 * beginning with LINUX_CHRDEV_MAJOR:0
	 */
	int ret;
	dev_t dev_no;
	unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;

	debug("initializing character device\n");
	cdev_init(&lunix_chrdev_cdev, &lunix_chrdev_fops);
	lunix_chrdev_cdev.owner = THIS_MODULE;

	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
	/* ? */
	ret = register_chrdev_region(dev_no, lunix_minor_cnt, "Lunix:TNG");
	/* register_chrdev_region? */
	if (ret < 0) {
		debug("failed to register region, ret = %d\n", ret);
		goto out;
	}
	/* ? */
	ret = cdev_add(&lunix_chrdev_cdev, dev_no, lunix_minor_cnt);
	/* cdev_add? */
	if (ret < 0) {
		debug("failed to add character device\n");
		goto out_with_chrdev_region;
	}
	debug("completed successfully\n");
	return 0;

out_with_chrdev_region:
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
out:
	return ret;
}

void lunix_chrdev_destroy(void)
{
	dev_t dev_no;
	unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;

	debug("entering\n");
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
	cdev_del(&lunix_chrdev_cdev);
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
	debug("leaving\n");
}
