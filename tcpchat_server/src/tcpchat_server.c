/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>

#include "list.h"

#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define BUFFLEN 1024

static volatile sig_atomic_t run = 1;

/* Struct representing a client */
struct client {
	struct list_head list;
	struct sockaddr_in sa;
	int sd;
};

/* Initialize client list */
LIST_HEAD(cl);

static void
printHelp(const char *name)
{
	fprintf(stdout,"Usage: %s <local port>\n\n", name);
}

void sighandler(int signo)
{
	switch (signo)
	{
	case SIGINT:
	case SIGTERM:
		fprintf(stderr, "Received signal %d, shutting down...\n",
								signo);
		run = 0;
		break;
	default:
		fprintf(stderr, "Received signal %d, ignoring...\n",
								signo);
	}
}

/* Remove non-ASCII characters and ASCII control characters from the received
 * string. */
static char * sanitize(const char *str, size_t len)
{
	static char buffer[BUFFLEN+2];
	unsigned int i,j;

	if (len > BUFFLEN) {
		fprintf(stderr, "ERROR: message too long\n");
		return NULL;
	}

	memset(buffer, 0, sizeof(buffer));

	for (i=0,j=0; i<len; i++) {
		if (str[i] < 0x20 || str[i] > 0x7e)
			continue;
		buffer[j] = str[i];
		j++;
	}

	/* Add a line feed at the end of non-empty strings */
	if (j > 0)
		buffer[j] = 0x0a;

	return buffer;
}

int
main(int argc, char **argv)
{
	struct sockaddr_in sal, safrom;
	int lport, sd, maxfd, ret, csd, optval;
	fd_set rfds, rfd;
	char buffer[BUFFLEN];
	socklen_t slen;
	char *ptr;
	struct client *c, *tmp;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	/* Initialize variables */
	memset(&sal, 0, sizeof(sal));

	/* Parse commandline arguments */
	if (argc != 2) {
		printHelp(argv[0]);
		exit(1);
	}

	lport = atoi(argv[1]);

	if (lport < 1024 || lport > 65535) {
		fprintf(stderr, "invalid port number\n");
		exit(1);
	}

	sal.sin_family = AF_INET;
	sal.sin_port = htons((short)lport);
	sal.sin_addr.s_addr = htonl(INADDR_ANY);

	/* Create socket */
	if (0 > (sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))) {
		perror("socket() failed");
		exit(1);
	}

	optval = 1;
	if (setsockopt(sd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval))) {
		fprintf(stderr, "setsockopt() failed\n");
		exit(1);
	}

	/* Bind local address information to socket */
	if (0 > bind(sd, (struct sockaddr *)&sal, sizeof(sal))) {
		perror("bind() failed");
		exit(1);
	}

	/* Mark socket as passive for incoming connections */
	if (listen(sd, 32)) {
		perror("listen() failed");
		exit(1);
	}

	/* Preare file descriptor sets */
	FD_ZERO(&rfds);
	FD_SET(sd, &rfds);
	FD_SET(STDIN_FILENO, &rfds);
	maxfd = MAX(sd, STDIN_FILENO);

	while (run) {
		/* Reset variables */
		rfd = rfds;
		slen = sizeof(safrom);
		memset(&safrom, 0, sizeof(safrom));
		memset(buffer, 0, sizeof(buffer));
		ptr = 0;

		ret = select(maxfd+1, &rfd, NULL, NULL, NULL);

		if (0 > ret) {
			if (errno == EINTR)
				continue;
			perror("select() failed");
			exit(1);
		}

		if (0 == ret) {
			fprintf(stderr, "should not happen\n");
			continue;
		}

		if (FD_ISSET(sd, &rfd)) {
			csd = accept(sd, (struct sockaddr *)&safrom, &slen);

			if (0 > csd) {
				if (errno == EINTR)
					continue;
				perror("accept() failed");
				exit(1);
			}

			/* Add new client to list */
			c = malloc(sizeof(struct client));
			memset(c, 0, sizeof(struct client));
			c->sa = safrom;
			c->sd = csd;

			list_add(&c->list, &cl);
			FD_SET(c->sd, &rfds);
			maxfd = MAX(maxfd, c->sd);

			fprintf(stdout, "%s:%d connected\n",
				inet_ntoa(c->sa.sin_addr),
				ntohs(c->sa.sin_port));
		}
		else if (FD_ISSET(STDIN_FILENO, &rfd)) {
			/* We typed a message */
			if (NULL == fgets(buffer, sizeof(buffer), stdin)) {
				run = 0;
				continue;
			}

			ptr = sanitize(buffer, strlen(buffer));

			if (strlen(ptr) == 0)
				continue;
		}
		else {
			/* Iterate over clients and serve first one ready */
			list_for_each_entry(c, &cl, list) {
				if (!FD_ISSET(c->sd, &rfd))
					continue;

				ret = recv(c->sd, buffer, sizeof(buffer)-1,
					O_NONBLOCK);

				if (0 > ret) {
					if (errno == EINTR)
						continue;
					if (errno == ECONNRESET) {
						fprintf(stdout, "%s:%d disconnected\n",
							inet_ntoa(c->sa.sin_addr),
							ntohs(c->sa.sin_port));
						FD_CLR(c->sd, &rfds);
						list_del(&c->list);
						free(c);
						break;
					}
					perror("recv() failed");
					exit(1);
				}

				if (0 == ret) {
					fprintf(stdout, "%s:%d disconnected\n",
						inet_ntoa(c->sa.sin_addr),
						ntohs(c->sa.sin_port));
					FD_CLR(c->sd, &rfds);
					list_del(&c->list);
					free(c);
					break;
				}

				ptr = sanitize(buffer, strlen(buffer));

				if (strlen(ptr) == 0)
					continue;

				/* Remember from whom we received */
				safrom = c->sa;

				fprintf(stdout, "%s:%d >> %s",
					inet_ntoa(safrom.sin_addr),
					ntohs(safrom.sin_port),
					ptr);

				break;
			}
		}

		if (!ptr)
			continue;

		/* Iterate over clients and relay message */
		list_for_each_entry_safe(c, tmp, &cl, list) {
			if (0 == memcmp(&c->sa, &safrom, sizeof(safrom)))
				continue;

			ret = send(c->sd, ptr, strlen(ptr), O_NONBLOCK);

			if (0 > ret) {
				if (errno == EINTR)
					continue;
				if (errno == EPIPE || errno == ECONNRESET) {
					fprintf(stdout, "%s:%d disconnected\n",
						inet_ntoa(c->sa.sin_addr),
						ntohs(c->sa.sin_port));
					FD_CLR(c->sd, &rfds);
					list_del(&c->list);
					free(c);
					continue;
				}
				perror("send() failed");
				exit(1);
			}
		}
	}

	(void) close(sd);

	return 0;
}

