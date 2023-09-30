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

#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define BUFFLEN 1024

static volatile sig_atomic_t run = 1;

static void
printHelp(const char *name)
{
	fprintf(stdout,"Usage: %s <local port> <remote_ip> <remote_port>\n\n",
									name);
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
	struct sockaddr_in sar, safrom;
	int rport, sd, maxfd, ret;
	fd_set rfds, rfd;
	char buffer[BUFFLEN];
	char *ptr;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	/* Initialize variables */
	memset(&sar, 0, sizeof(sar));

	/* Parse commandline arguments */
	if (argc != 3) {
		printHelp(argv[0]);
		exit(1);
	}

	rport = atoi(argv[2]);

	if (rport < 1024 || rport > 65535) {
		fprintf(stderr, "invalid port number\n");
		exit(1);
	}

	if (0 == inet_aton(argv[1], &sar.sin_addr)) {
		fprintf(stderr, "invalid IP address: %s\n", argv[2]);
		exit(1);
	}
	sar.sin_family = AF_INET;
	sar.sin_port = htons((short)rport);

	/* Create socket */
	if (0 > (sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))) {
		perror("socket() failed");
		exit(1);
	}

	/* Connect to server */
	if (0 > connect(sd, (struct sockaddr *)&sar, sizeof(sar))) {
		perror("connect() failed");
		exit(1);
	}

	/* Prepare file descriptor sets */
	FD_ZERO(&rfds);
	FD_SET(sd, &rfds);
	FD_SET(STDIN_FILENO, &rfds);
	maxfd = MAX(sd, STDIN_FILENO);

	while (run) {
		/* Reset variables */
		rfd = rfds;
		memset(&safrom, 0, sizeof(safrom));
		memset(buffer, 0, sizeof(buffer));

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
			/* Somebody sent us a message */
			ret = recv(sd, buffer, sizeof(buffer)-1, O_NONBLOCK);

			if (0 > ret) {
				if (errno == EINTR)
					continue;
				perror("recvfrom() failed");
				exit(1);
			}

			if (ret == 0) {
				fprintf(stdout, "server disconnected\n");
				run = 0;
				continue;
			}

			ptr = sanitize(buffer, strlen(buffer));

			if (strlen(ptr) == 0)
				continue;

			fprintf(stdout, ">> %s", ptr);
		}
		else if (FD_ISSET(STDIN_FILENO, &rfd)) {
			/* We typed a message */
			if (NULL == fgets(buffer, sizeof(buffer), stdin)) {
				run = 0;
				continue;
			}

			ptr = sanitize(buffer, strlen(buffer));

			ret = send(sd, ptr, strlen(ptr), O_NONBLOCK);

			if (0 > ret) {
				if (errno == EINTR)
					continue;
				perror("sendto() failed");
				exit(1);
			}
		}
	}

	(void) close(sd);

	return 0;
}

