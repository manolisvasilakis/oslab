/*
 * Virtio Crypto Device
 *
 * Implementation of virtio-crypto qemu backend device.
 *
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 * Stefanos Gerangelos <sgerag@cslab.ece.ntua.gr> 
 *
 */

#include <qemu/iov.h>
#include "hw/virtio/virtio-serial.h"
#include "hw/virtio/virtio-crypto.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <crypto/cryptodev.h>

#include <unistd.h>
#include <errno.h>

static uint32_t get_features(VirtIODevice *vdev, uint32_t features)
{
	DEBUG_IN();
	return features;
}

static void get_config(VirtIODevice *vdev, uint8_t *config_data)
{
	DEBUG_IN();
}

static void set_config(VirtIODevice *vdev, const uint8_t *config_data)
{
	DEBUG_IN();
}

static void set_status(VirtIODevice *vdev, uint8_t status)
{
	DEBUG_IN();
}

static void vser_reset(VirtIODevice *vdev)
{
	DEBUG_IN();
}

static void vq_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
	VirtQueueElement elem;
	unsigned int *syscall_type;
	
	int *host_fd, *ret;
	unsigned int *cmd;
	struct session_op *session_op;
	struct crypt_op crypt_op;
	__u32 *ses_id;

	DEBUG_IN();

	if (!virtqueue_pop(vq, &elem)) {
		DEBUG("No item to pop from VQ :(");
		return;
	} 

	DEBUG("I have got an item from VQ :)");

	syscall_type = elem.out_sg[0].iov_base;
	switch (*syscall_type) {
	case VIRTIO_CRYPTO_SYSCALL_TYPE_OPEN:
		DEBUG("VIRTIO_CRYPTO_SYSCALL_TYPE_OPEN");
		host_fd = elem.in_sg[0].iov_base; 
		*host_fd = open("/dev/crypto", O_RDWR);
		break;

	case VIRTIO_CRYPTO_SYSCALL_TYPE_CLOSE:
		DEBUG("VIRTIO_CRYPTO_SYSCALL_TYPE_CLOSE");
		host_fd = elem.in_sg[0].iov_base;
		*host_fd = close(*host_fd);
		if (*host_fd) *host_fd = errno;
		break;

	case VIRTIO_CRYPTO_SYSCALL_TYPE_IOCTL:
		DEBUG("VIRTIO_CRYPTO_SYSCALL_TYPE_IOCTL");
		host_fd = elem.out_sg[1].iov_base;
		cmd = elem.out_sg[2].iov_base;
		switch (*cmd) {
			case CIOCGSESSION:
				DEBUG("CIOCGSESSION IOCTL");
				ret = elem.in_sg[1].iov_base;
				session_op = elem.in_sg[0].iov_base;
				session_op->key = elem.out_sg[3].iov_base;
				*ret = ioctl(*host_fd, CIOCGSESSION, session_op);
				break;

			case CIOCFSESSION:
				DEBUG("CIOCFSESSION IOCTL");
				ret = elem.in_sg[0].iov_base;
				ses_id = elem.out_sg[3].iov_base;
				*ret = ioctl(*host_fd, CIOCFSESSION, ses_id);
				break;

			case CIOCCRYPT:
				DEBUG("CIOCCRYPT IOCTL");
				ret = elem.in_sg[1].iov_base;
				memcpy(&crypt_op, elem.out_sg[3].iov_base, sizeof(struct crypt_op));
				crypt_op.src = elem.out_sg[4].iov_base;
				crypt_op.iv = elem.out_sg[5].iov_base;
				crypt_op.dst = elem.in_sg[0].iov_base;
				*ret = ioctl(*host_fd, CIOCCRYPT, &crypt_op);
				break;

			default:
				DEBUG("Unknown ioctl command");
		}
		break;

	default:
		DEBUG("Unknown syscall_type");
	}

	virtqueue_push(vq, &elem, 0);
	virtio_notify(vdev, vq);
}

static void virtio_crypto_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);

	DEBUG_IN();

    virtio_init(vdev, "virtio-crypto", 13, 0);
	virtio_add_queue(vdev, 128, vq_handle_output);
}

static void virtio_crypto_unrealize(DeviceState *dev, Error **errp)
{
	DEBUG_IN();
}

static Property virtio_crypto_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_crypto_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *k = VIRTIO_DEVICE_CLASS(klass);

	DEBUG_IN();
    dc->props = virtio_crypto_properties;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);

    k->realize = virtio_crypto_realize;
    k->unrealize = virtio_crypto_unrealize;
    k->get_features = get_features;
    k->get_config = get_config;
    k->set_config = set_config;
    k->set_status = set_status;
    k->reset = vser_reset;
}

static const TypeInfo virtio_crypto_info = {
    .name          = TYPE_VIRTIO_CRYPTO,
    .parent        = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtCrypto),
    .class_init    = virtio_crypto_class_init,
};

static void virtio_crypto_register_types(void)
{
    type_register_static(&virtio_crypto_info);
}

type_init(virtio_crypto_register_types)
