/*
 * Unix domain socket transport between audio event recognition modules.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "socket.h"

/* Room for several frames (62 KB each) before a slow reader loses data */
#define AER_SNDBUF	(1 << 20)

static int fill_addr(struct sockaddr_un *addr, const char *path)
{
	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr->sun_path)) {
		fprintf(stderr, "socket path too long: %s\n", path);
		return -1;
	}
	strcpy(addr->sun_path, path);
	return 0;
}

server_t *listen_on_socket(const char *path)
{
	struct sockaddr_un addr;
	server_t *server;
	int fd;

	if (fill_addr(&addr, path))
		return NULL;

	/* the directory normally comes from systemd's RuntimeDirectory= */
	mkdir(AER_SOCKET_DIR, 0770);

	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket");
		return NULL;
	}
	unlink(path);	/* stale socket from a previous run */
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, AER_MAX_CLIENTS) < 0) {
		fprintf(stderr, "listen %s: %s\n", path, strerror(errno));
		close(fd);
		return NULL;
	}
	chmod(path, 0660);

	server = calloc(1, sizeof(*server));
	if (!server) {
		close(fd);
		return NULL;
	}
	server->listen_fd = fd;
	snprintf(server->path, sizeof(server->path), "%s", path);
	return server;
}

static void drop_client(server_t *server, int i)
{
	close(server->clients[i]);
	server->clients[i] = server->clients[--server->num_clients];
}

int accept_clients(server_t *server)
{
	int fd, sndbuf = AER_SNDBUF;

	while ((fd = accept4(server->listen_fd, NULL, NULL,
			     SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
		if (server->num_clients == AER_MAX_CLIENTS) {
			fprintf(stderr, "%s: too many clients\n", server->path);
			close(fd);
			continue;
		}
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
		server->clients[server->num_clients++] = fd;
		fprintf(stderr, "%s: client connected (%d)\n", server->path,
			server->num_clients);
	}
	return server->num_clients;
}

int send_to_socket(server_t *server_socket, const void *message, size_t len)
{
	int i, sent = 0;

	if (!server_socket)
		return -1;
	accept_clients(server_socket);

	for (i = 0; i < server_socket->num_clients; ) {
		ssize_t n = send(server_socket->clients[i], message, len,
				 MSG_DONTWAIT | MSG_NOSIGNAL);

		if (n == (ssize_t)len) {
			sent++;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			/* reader is behind: this message is dropped for it */
		} else {
			fprintf(stderr, "%s: client disconnected\n",
				server_socket->path);
			drop_client(server_socket, i);
			continue;
		}
		i++;
	}
	return sent;
}

void close_server(server_t *server)
{
	if (!server)
		return;
	while (server->num_clients)
		drop_client(server, 0);
	close(server->listen_fd);
	unlink(server->path);
	free(server);
}

int try_connect_to_socket(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (fill_addr(&addr, path))
		return -1;
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int connect_to_socket(const char *path)
{
	struct timespec delay = { 0, 500 * 1000 * 1000 };
	int fd, warned = 0;

	while ((fd = try_connect_to_socket(path)) < 0) {
		if (!warned) {
			fprintf(stderr, "waiting for %s\n", path);
			warned = 1;
		}
		nanosleep(&delay, NULL);
	}
	fprintf(stderr, "connected to %s\n", path);
	return fd;
}

ssize_t receive_on_socket(int fd, void *message, size_t max_len)
{
	ssize_t n;

	/* EINTR is returned to the caller so a stop signal ends its loop */
	n = recv(fd, message, max_len, MSG_TRUNC);
	if (n > (ssize_t)max_len) {
		fprintf(stderr, "message of %zd bytes truncated\n", n);
		errno = EMSGSIZE;
		return -1;
	}
	return n;
}

static volatile int *running_ptr;

static void on_signal(int sig)
{
	(void)sig;
	if (running_ptr)
		*running_ptr = 0;
}

void install_signal_handlers(volatile int *running_flag)
{
	struct sigaction sa;

	running_ptr = running_flag;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}
