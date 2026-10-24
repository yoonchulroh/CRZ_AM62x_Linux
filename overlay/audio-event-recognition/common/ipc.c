// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc.h"

#define RETRY_MS	500
#define SNDBUF_BYTES	(1024 * 1024)

static int socket_path(char *dst, size_t len, const char *run_dir, const char *name)
{
	int n = snprintf(dst, len, "%s/%s/%s.sock", run_dir, name, name);

	if (n < 0 || (size_t)n >= len) {
		aer_log("socket path for %s too long", name);
		return -1;
	}
	return 0;
}

int aer_server_open(struct aer_server *s, const char *run_dir, const char *name)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	char dir[108];
	int bufsz = SNDBUF_BYTES;

	memset(s, 0, sizeof(*s));
	s->lfd = -1;
	if (socket_path(s->path, sizeof(s->path), run_dir, name) < 0)
		return -1;

	/* systemd creates the directory (RuntimeDirectory=); do it for manual runs */
	snprintf(dir, sizeof(dir), "%s", run_dir);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/%s", run_dir, name);
	mkdir(dir, 0755);

	s->lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s->lfd < 0) {
		aer_log("socket: %s", strerror(errno));
		return -1;
	}
	setsockopt(s->lfd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
	aer_strlcpy(addr.sun_path, s->path, sizeof(addr.sun_path));
	unlink(s->path);
	if (bind(s->lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(s->lfd, AER_MAX_CLIENTS) < 0) {
		aer_log("listen on %s: %s", s->path, strerror(errno));
		close(s->lfd);
		s->lfd = -1;
		return -1;
	}
	/* consumers run as other users (aer-display runs as the Weston user) */
	chmod(s->path, 0666);
	aer_log("serving %s", s->path);
	return 0;
}

void aer_server_close(struct aer_server *s)
{
	for (int i = 0; i < s->ncli; i++)
		close(s->cfd[i]);
	s->ncli = 0;
	if (s->lfd >= 0) {
		close(s->lfd);
		unlink(s->path);
		s->lfd = -1;
	}
}

void aer_server_accept(struct aer_server *s)
{
	int fd, bufsz = SNDBUF_BYTES;

	while ((fd = accept4(s->lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
		if (s->ncli == AER_MAX_CLIENTS) {
			aer_log("too many consumers, refusing one");
			close(fd);
			continue;
		}
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
		s->cfd[s->ncli++] = fd;
		aer_log("consumer connected (%d total)", s->ncli);
	}
}

static void drop_client(struct aer_server *s, int i)
{
	close(s->cfd[i]);
	s->cfd[i] = s->cfd[--s->ncli];
	aer_log("consumer disconnected (%d left)", s->ncli);
}

int aer_server_send(struct aer_server *s, struct aer_hdr *h, const void *payload, size_t len)
{
	struct iovec iov[2] = {
		{ .iov_base = h, .iov_len = sizeof(*h) },
		{ .iov_base = (void *)payload, .iov_len = len },
	};
	struct msghdr msg = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
	int sent = 0;

	h->magic = AER_MAGIC;
	h->seq = s->seq++;

	for (int i = 0; i < s->ncli;) {
		if (sendmsg(s->cfd[i], &msg, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) {
			sent++;
			i++;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
			/* consumer too slow: drop this message for it only */
			if ((s->drops++ % 100) == 0)
				aer_log("consumer not keeping up, %lu message(s) dropped",
					s->drops);
			i++;
		} else {
			drop_client(s, i);
		}
	}
	return sent;
}

void aer_client_init(struct aer_client *c, const char *run_dir, const char *name)
{
	memset(c, 0, sizeof(*c));
	c->fd = -1;
	if (socket_path(c->path, sizeof(c->path), run_dir, name) < 0)
		c->path[0] = '\0';
}

bool aer_client_connect(struct aer_client *c)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int64_t now = aer_mono_ms();

	if (c->fd >= 0)
		return true;
	if (!c->path[0] || now < c->retry_ms)
		return false;
	c->retry_ms = now + RETRY_MS;

	c->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (c->fd < 0)
		return false;
	aer_strlcpy(addr.sun_path, c->path, sizeof(addr.sun_path));
	if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (!c->warned)
			aer_log("waiting for %s (%s)", c->path, strerror(errno));
		c->warned = true;
		close(c->fd);
		c->fd = -1;
		return false;
	}
	aer_log("connected to %s", c->path);
	c->warned = false;
	c->synced = false;
	c->discont = true;
	return true;
}

void aer_client_close(struct aer_client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->retry_ms = aer_mono_ms() + RETRY_MS;
}

ssize_t aer_client_read(struct aer_client *c, void *buf, size_t cap)
{
	struct iovec iov = { .iov_base = buf, .iov_len = cap };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	const struct aer_hdr *h = buf;
	ssize_t n;

	if (c->fd < 0)
		return -1;
	n = recvmsg(c->fd, &msg, MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		aer_log("upstream %s went away", c->path);
		aer_client_close(c);
		return 0;
	}
	if (n < 0)
		return -1;
	if ((msg.msg_flags & MSG_TRUNC) || (size_t)n < sizeof(*h) || h->magic != AER_MAGIC) {
		aer_log("discarding malformed message (%zd bytes)", n);
		return -1;
	}
	if (c->synced && h->seq != c->next_seq) {
		aer_dbg("sequence gap: expected %u got %u", c->next_seq, h->seq);
		c->discont = true;
	}
	c->synced = true;
	c->next_seq = h->seq + 1;
	return n;
}

ssize_t aer_link_wait(struct aer_client *in, struct aer_server *out,
		      void *buf, size_t cap, uint16_t type, int timeout_ms)
{
	struct pollfd pfd[2];
	int n = 0, in_idx = -1, out_idx = -1;
	ssize_t len;

	aer_client_connect(in);
	if (in->fd >= 0) {
		pfd[n] = (struct pollfd){ .fd = in->fd, .events = POLLIN };
		in_idx = n++;
	} else if (timeout_ms > RETRY_MS) {
		timeout_ms = RETRY_MS;	/* come back soon to retry the connect */
	}
	if (out && out->lfd >= 0) {
		pfd[n] = (struct pollfd){ .fd = out->lfd, .events = POLLIN };
		out_idx = n++;
	}
	if (poll(pfd, n, timeout_ms) <= 0)
		return 0;
	if (out_idx >= 0 && (pfd[out_idx].revents & POLLIN))
		aer_server_accept(out);
	if (in_idx < 0 || !pfd[in_idx].revents)
		return 0;

	len = aer_client_read(in, buf, cap);
	if (len <= 0)
		return 0;
	if (!aer_msg_valid(buf, len, type)) {
		aer_log("unexpected message type %u (%zd bytes)",
			((struct aer_hdr *)buf)->type, len);
		return 0;
	}
	return len;
}

bool aer_msg_valid(const void *buf, ssize_t len, uint16_t type)
{
	const struct aer_hdr *h = buf;
	size_t payload, want;

	if (len < (ssize_t)sizeof(*h) || h->magic != AER_MAGIC || h->type != type)
		return false;
	payload = (size_t)len - sizeof(*h);

	switch (type) {
	case AER_MSG_PCM_S16:
		if (h->channels < 1 || h->channels > 8 || !h->rate)
			return false;
		want = (size_t)h->count * h->channels * sizeof(int16_t);
		break;
	case AER_MSG_PCM_F32:
	case AER_MSG_FRAME:
		if (!h->rate)
			return false;
		want = (size_t)h->count * sizeof(float);
		break;
	case AER_MSG_SCORES:
		want = sizeof(struct aer_scores) + (size_t)h->count * sizeof(float);
		break;
	case AER_MSG_STATUS:
		want = sizeof(struct aer_status);
		break;
	case AER_MSG_EVENT:
		want = sizeof(struct aer_event);
		break;
	default:
		return false;
	}
	return payload == want;
}
