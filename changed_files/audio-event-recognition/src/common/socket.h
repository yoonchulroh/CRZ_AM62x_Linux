/*
 * Unix domain socket transport between audio event recognition modules.
 *
 * Each module that produces data owns a SOCK_SEQPACKET socket at a fixed path
 * and acts as the server; the next module connects as a client. SEQPACKET keeps
 * message boundaries, so one send is one received message.
 */
#ifndef AER_SOCKET_H
#define AER_SOCKET_H

#include <stddef.h>
#include <sys/types.h>

#define AER_SOCKET_DIR		"/run/audio-event"
#define AER_SOCK_AUDIO		AER_SOCKET_DIR "/audio.sock"
#define AER_SOCK_PREPROCESS	AER_SOCKET_DIR "/preprocess.sock"
#define AER_SOCK_FRAMER		AER_SOCKET_DIR "/framer.sock"
#define AER_SOCK_INFERENCE	AER_SOCKET_DIR "/inference.sock"
#define AER_SOCK_DETECTOR	AER_SOCKET_DIR "/detector.sock"

#define AER_MAX_CLIENTS		8

typedef struct {
	int listen_fd;
	int clients[AER_MAX_CLIENTS];
	int num_clients;
	char path[108];
} server_t;

/* Create the socket at path and wait for clients (accepted on send). */
server_t *listen_on_socket(const char *path);

/* Accept every pending client. Returns the number of connected clients. */
int accept_clients(server_t *server);

/*
 * Send one message to every connected client without blocking. A client whose
 * socket buffer is full misses this message; a closed client is dropped.
 * Returns the number of clients that received the message, -1 on error.
 */
int send_to_socket(server_t *server_socket, const void *message, size_t len);

void close_server(server_t *server);

/*
 * Connect to the socket at path, retrying until the server is up. Returns a
 * blocking socket fd.
 */
int connect_to_socket(const char *path);

/* Single non-blocking attempt for event loops. Returns fd or -1. */
int try_connect_to_socket(const char *path);

/*
 * Receive one message (blocking unless the fd is non-blocking). Returns the
 * message length, 0 if the server went away (reconnect), -1 on error
 * (errno EINTR when interrupted by a signal, EAGAIN when non-blocking and
 * empty). A message longer than max_len is reported as an error.
 */
ssize_t receive_on_socket(int fd, void *message, size_t max_len);

/* Ignore SIGPIPE and make SIGINT/SIGTERM set *flag to 0. */
void install_signal_handlers(volatile int *running_flag);

#endif
