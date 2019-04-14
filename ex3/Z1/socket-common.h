/*
 * socket-common.h
 *
 * Simple TCP/IP communication using sockets
 *
 * Vasilakis Emmanouil, Giannou Aggeliki
 */

#ifndef _SOCKET_COMMON_H
#define _SOCKET_COMMON_H

/* Compile-time options */
#define TCP_PORT    35001
#define TCP_BACKLOG 5


#endif /* _SOCKET_COMMON_H */

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

/* Chat implementation, shared code between client and server */
void chat(int socket_fd)
{
	char buf[100];
	fd_set readfds;
	ssize_t n;

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
			n = read(socket_fd, buf, sizeof(buf));

			if (n < 0) {
				perror("read from socket failed\n");
				exit(1);
			}
			else if (n == 0) {		/* Peer is down */
				fprintf(stderr, "Peer went away\n");
				return;
			}

			if (insist_write(1, buf, n) != n) {
				perror("write to stdout failed\n");
				exit(1);
			}
		}
		if (FD_ISSET(0, &readfds)) {
			/* Read from stdin and write it to socket */
			n = read(0, buf, sizeof(buf)-1);

			if (n < 0) {
				perror("read from stdin failed\n");
				exit(1);
			}
			else if (n == 0) {
				perror("ERROR:	EOF stdin\n");
				exit(1);
			}

			buf[n] = '\0';			/* last character \0 */
			if (insist_write(socket_fd, buf, n+1) != n+1) {
				perror("write to remote peer failed\n");
				exit(1);
			}
		}
	}
}
