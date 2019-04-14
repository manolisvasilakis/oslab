/*
 * crypto-common.h
 *
 * Simple TCP/IP communication using sockets and cryptodev
 *
 * Vasilakis Emmanouil, Giannou Aggeliki
 */

#ifndef _SOCKET_COMMON_H
#define _SOCKET_COMMON_H

/* Compile-time options */
#define TCP_PORT    35001
#define TCP_BACKLOG 5


#endif /* _SOCKET_COMMON_H */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <crypto/cryptodev.h>

#define DATA_SIZE		256
#define BLOCK_SIZE		16
#define KEY_SIZE		16  /* AES128 */

const char *MY_IV	= "liastesntomates";	/* length 15 + '\0' which is automatically added */
const char *MY_KEY	= "gyrosmetzatziki";


/* Insist until all of the data has been written */
ssize_t insist_write(int fd, const void *buf, size_t cnt)
{
	ssize_t ret;
	size_t orig_cnt = cnt;

	while (cnt > 0) {
		ret = write(fd, buf, cnt);
		if (ret < 0)
			return ret;
		buf += ret;
		cnt -= ret;
	}

	return orig_cnt;
}

/* Insist until all of the data has been read */
ssize_t insist_read(int fd, void *buf, size_t cnt)
{
	ssize_t ret;
	size_t orig_cnt = cnt;

	while (cnt > 0) {
		ret = read(fd, buf, cnt);
		if (ret <= 0) return ret;
		buf += ret;
		cnt -= ret;
	}

	return orig_cnt;
}

/*
 * Chat implementation, shared code between client and server
 * Different session for different clients
 */
void chat(int socket_fd)
{
	fd_set readfds;
	int useful_bytes;
	ssize_t n;
	int cryptofd;
	struct session_op sess;
	struct crypt_op cryp;
	struct {
		unsigned char 	plaintext[DATA_SIZE],
						ciphertext[DATA_SIZE],
						iv[BLOCK_SIZE],
						key[KEY_SIZE];
	} data;

	cryptofd = open("/dev/crypto", O_RDWR);
	if (cryptofd < 0) {
		perror("open(/dev/crypto)");
		exit(1);
	}

	memset(&sess, 0, sizeof(sess));
	memset(&cryp, 0, sizeof(cryp));

	memcpy(data.key, MY_KEY, KEY_SIZE);
	memcpy(data.iv, MY_IV, BLOCK_SIZE);

	/*
	 * Get crypto session for AES128
	 */
	sess.cipher = CRYPTO_AES_CBC;
	sess.keylen = KEY_SIZE;
	sess.key 	= data.key;

	if (ioctl(cryptofd, CIOCGSESSION, &sess)) {
		perror("ioctl(CIOCGSESSION)");
		exit(1);
	}

	for(;;) {
		FD_ZERO(&readfds);
		FD_SET(0, &readfds);
		FD_SET(socket_fd, &readfds);
		if (select(socket_fd+1, &readfds, NULL, NULL, NULL) == -1) {
			perror("select()\n");
			exit(1);
		}
		if (FD_ISSET(socket_fd, &readfds)) {
			/* Read from peer and write it to standard output */
			n = insist_read(socket_fd, data.ciphertext, sizeof(data.ciphertext));

			if (n < 0) {
				perror("read from socket failed\n");
				exit(1);
			}
			else if (n == 0) {		/* Peer is down */
				fprintf(stderr, "Peer went away\n");

				/* Finish crypto session */
				if (ioctl(cryptofd, CIOCFSESSION, &sess.ses)) {
					perror("ioctl(CIOCFSESSION)");
					exit(1);
				}
				if (close(cryptofd) < 0) {
					perror("close(cryptofd)");
					exit(1);
				}
				return;
			}

			/*
			 * Decrypt data.ciphertext to data.plaintext
			 */
			cryp.ses = sess.ses;
			cryp.src = data.ciphertext;
			cryp.dst = data.plaintext;
			cryp.len = DATA_SIZE;
			cryp.iv = data.iv;
			cryp.op = COP_DECRYPT;
			if (ioctl(cryptofd, CIOCCRYPT, &cryp)) {
				perror("ioctl(CIOCCRYPT)");
				exit(1);
			}

			/*
			 * Find number of useful bytes to print,
			 * '\0' + bytes read from stdin inside data.plaintext[0]
			 */
			useful_bytes = (int) data.plaintext[0] + 1;
			if (insist_write(1, data.plaintext + 1, useful_bytes) != useful_bytes) {
				perror("write to stdout failed\n");
				exit(1);
			}
		}
		if (FD_ISSET(0, &readfds)) {
			/* Read from stdin and write it to socket */
			n = read(0, data.plaintext + 1, sizeof(data.plaintext)-2);

			if (n < 0) {
				perror("read from stdin failed\n");
				exit(1);
			}
			else if (n == 0) {
				perror("ERROR:	EOF stdin\n");
				exit(1);
			}

			/* last character \0 */
			data.plaintext[n+1] = '\0';

			/* number of bytes read from stdin */
			data.plaintext[0] = (char) n;

			/*
			 * Encrypt data.plaintext to data.ciphertext
			 */
			cryp.ses = sess.ses;
			cryp.len = DATA_SIZE;
			cryp.src = data.plaintext;
			cryp.dst = data.ciphertext;
			cryp.iv = data.iv;
			cryp.op = COP_ENCRYPT;

			if (ioctl(cryptofd, CIOCCRYPT, &cryp)) {
				perror("ioctl(CIOCCRYPT)");
				exit(1);
			}

			if (insist_write(socket_fd, data.ciphertext, sizeof(data.ciphertext)) != sizeof(data.ciphertext)) {
				perror("write to remote peer failed\n");
				exit(1);
			}
		}
	}
}
