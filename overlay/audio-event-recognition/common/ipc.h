// SPDX-License-Identifier: MIT
/*
 * Module-to-module transport: AF_UNIX SOCK_SEQPACKET, one aer_hdr + payload
 * per packet.
 *
 * A producer owns an aer_server listening on <run_dir>/<name>/<name>.sock and
 * broadcasts every message to all connected consumers. Sends never block: a
 * consumer whose socket buffer is full misses that message (counted as a
 * drop) so a stalled consumer can never stall the audio capture.
 *
 * A consumer owns an aer_client that (re)connects to its upstream socket.
 * aer_client.discont is set after a reconnect or a gap in the sequence
 * numbers so stateful consumers (resampler, framer) can reset themselves.
 */
#ifndef AER_IPC_H
#define AER_IPC_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "aer.h"

#define AER_MAX_CLIENTS	8

struct aer_server {
	char path[108];
	int lfd;
	int cfd[AER_MAX_CLIENTS];
	int ncli;
	uint32_t seq;
	unsigned long drops;
};

struct aer_client {
	char path[108];
	int fd;
	uint32_t next_seq;
	bool synced;
	bool discont;
	bool warned;
	int64_t retry_ms;
};

int aer_server_open(struct aer_server *s, const char *run_dir, const char *name);
void aer_server_close(struct aer_server *s);
/* Call when the listening socket is readable. */
void aer_server_accept(struct aer_server *s);
/* Fills in magic and seq, then broadcasts. Returns the number of receivers. */
int aer_server_send(struct aer_server *s, struct aer_hdr *h, const void *payload, size_t len);

void aer_client_init(struct aer_client *c, const char *run_dir, const char *name);
/* Connects if not connected and the retry delay has passed. */
bool aer_client_connect(struct aer_client *c);
void aer_client_close(struct aer_client *c);
/*
 * Reads one packet after poll() reported the client readable.
 * Returns its length, 0 when the upstream went away (the client is closed and
 * will reconnect), or -1 when nothing usable was read.
 */
ssize_t aer_client_read(struct aer_client *c, void *buf, size_t cap);

/*
 * Main loop helper for the linear pipeline modules: waits up to timeout_ms
 * for one valid message of the given type from `in`, accepting new consumers
 * on `out` (may be NULL) meanwhile. Returns the message length, or 0 on
 * timeout / reconnect / signal.
 */
ssize_t aer_link_wait(struct aer_client *in, struct aer_server *out,
		      void *buf, size_t cap, uint16_t type, int timeout_ms);

/* True when buf holds a well formed message of the given type. */
bool aer_msg_valid(const void *buf, ssize_t len, uint16_t type);

#endif
