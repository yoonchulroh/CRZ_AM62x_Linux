// SPDX-License-Identifier: MIT
/*
 * aer-broker - minimal MQTT 3.1.1 broker for the board.
 *
 * aer-notify publishes detected events to it, and user devices on the network
 * subscribe to it (e.g. mosquitto_sub -h 192.168.0.227 -t 'aer/#' -v).
 *
 * Supported: MQTT 3.1 / 3.1.1 clients, PUBLISH QoS 0/1/2 inbound, delivery
 * at QoS 0/1 (subscriptions asking for QoS 2 are granted QoS 1), '+' and '#'
 * wildcards, retained messages, last will, keep alive.
 * Not supported: persistent sessions (every session behaves as clean),
 * redelivery after reconnect, MQTT 5, TLS, authentication.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aer.h"
#include "conf.h"
#include "mqtt.h"

#define CONNECT_TIMEOUT_MS	10000
#define CLOSE_FLUSH_MS		5000	/* time a closing client gets to read its last packets */
#define MAX_OUT_BYTES		(1024 * 1024)
#define MAX_SUBS		64
#define MAX_RETAINED		256

struct sub {
	char *filter;
	uint8_t qos;
};

struct client {
	int fd;
	char addr[64];
	struct mbuf in, out;
	bool connected;
	bool closing;		/* flush the output buffer, then close */
	bool clean_close;	/* DISCONNECT received: do not send the will */
	char *id;
	uint16_t keepalive;
	int64_t last_rx_ms, accepted_ms;
	struct sub subs[MAX_SUBS];
	int nsubs;
	uint16_t next_pid;

	bool has_will, will_retain;
	uint8_t will_qos;
	char *will_topic;
	uint8_t *will_msg;
	size_t will_len;
};

struct retained {
	char *topic;
	uint8_t *payload;
	size_t len;
	uint8_t qos;
};

static struct client **clients;
static int nclients, max_clients;
static struct retained retained[MAX_RETAINED];
static int nretained;
static unsigned long auto_id;

/* --------------------------------------------------------------- delivery */

static void client_close(struct client *c, const char *why);

static void send_publish(struct client *c, const char *topic, const void *payload,
			 size_t len, int qos, bool retain)
{
	uint16_t pid = 0;

	if (c->closing)
		return;
	if (qos) {
		if (++c->next_pid == 0)
			c->next_pid = 1;
		pid = c->next_pid;
	}
	mqtt_publish(&c->out, topic, strlen(topic), payload, len, qos, retain, false, pid);
	if (c->out.len > MAX_OUT_BYTES) {
		aer_log("%s: not reading, dropping it", c->id ? c->id : c->addr);
		c->out.len = 0;
		c->closing = true;
	}
}

static void route(const char *topic, const void *payload, size_t len, int qos)
{
	for (int i = 0; i < nclients; i++) {
		struct client *c = clients[i];
		int best = -1;

		if (!c->connected || c->closing)
			continue;
		for (int s = 0; s < c->nsubs; s++)
			if (c->subs[s].qos > best && mqtt_topic_match(c->subs[s].filter, topic))
				best = c->subs[s].qos;
		if (best >= 0)
			send_publish(c, topic, payload, len, qos < best ? qos : best, false);
	}
}

static void retain_store(const char *topic, const void *payload, size_t len, int qos)
{
	int i;

	for (i = 0; i < nretained; i++)
		if (!strcmp(retained[i].topic, topic))
			break;
	if (i < nretained) {
		free(retained[i].payload);
		if (!len) {		/* empty retained message clears it */
			free(retained[i].topic);
			retained[i] = retained[--nretained];
			return;
		}
	} else {
		if (!len)
			return;
		if (nretained == MAX_RETAINED) {
			aer_log("retained message table full, not retaining %s", topic);
			return;
		}
		retained[i].topic = strdup(topic);
		nretained++;
	}
	retained[i].payload = malloc(len);
	memcpy(retained[i].payload, payload, len);
	retained[i].len = len;
	retained[i].qos = (uint8_t)(qos > 1 ? 1 : qos);
}

static void publish(const char *topic, const void *payload, size_t len, int qos, bool retain)
{
	if (qos > 1)
		qos = 1;
	if (retain)
		retain_store(topic, payload, len, qos);
	route(topic, payload, len, qos);
}

/* ---------------------------------------------------------------- packets */

static int handle_connect(struct client *c, struct mrd *r)
{
	size_t n, id_len;
	const char *proto = mrd_str(r, &n);
	uint8_t level = mrd_u8(r), flags = mrd_u8(r);
	const char *id;

	c->keepalive = mrd_u16(r);
	if (r->err)
		return -1;
	if (!((n == 4 && !memcmp(proto, "MQTT", 4) && level == 4) ||
	      (n == 6 && !memcmp(proto, "MQIsdp", 6) && level == 3))) {
		aer_log("%s: unsupported protocol level %u (only MQTT 3.1/3.1.1)", c->addr, level);
		mqtt_connack(&c->out, false, 1);
		c->closing = true;
		return 0;
	}
	if (flags & 0x01)
		return -1;

	id = mrd_str(r, &id_len);
	if (r->err)
		return -1;
	if (!id_len && !(flags & 0x02)) {
		mqtt_connack(&c->out, false, 2);	/* identifier rejected */
		c->closing = true;
		return 0;
	}
	if (id_len) {
		c->id = strndup(id, id_len);
	} else if (asprintf(&c->id, "auto-%lu", ++auto_id) < 0) {
		return -1;
	}

	if (flags & 0x04) {
		const char *wt, *wm;
		size_t wt_len, wm_len;

		wt = mrd_str(r, &wt_len);
		wm = mrd_str(r, &wm_len);
		if (r->err || !mqtt_topic_valid(wt, wt_len) || ((flags >> 3) & 3) == 3)
			return -1;
		c->has_will = true;
		c->will_topic = strndup(wt, wt_len);
		c->will_msg = malloc(wm_len ? wm_len : 1);
		memcpy(c->will_msg, wm, wm_len);
		c->will_len = wm_len;
		c->will_qos = (flags >> 3) & 3;
		c->will_retain = flags & 0x20;
	}
	/* user name and password are accepted but not checked */
	if (flags & 0x80)
		mrd_str(r, &n);
	if (flags & 0x40)
		mrd_str(r, &n);
	if (r->err)
		return -1;

	/* a second connection with the same id takes over */
	for (int i = 0; i < nclients; i++) {
		struct client *o = clients[i];

		if (o != c && o->connected && !o->closing && !strcmp(o->id, c->id)) {
			aer_log("%s: client id '%s' reconnected, closing the old connection",
				o->addr, o->id);
			client_close(o, NULL);
			break;
		}
	}

	c->connected = true;
	mqtt_connack(&c->out, false, 0);
	aer_log("%s: '%s' connected (keep alive %u s)", c->addr, c->id, c->keepalive);
	return 0;
}

static int handle_publish(struct client *c, uint8_t flags, struct mrd *r)
{
	int qos = (flags >> 1) & 3;
	bool retain = flags & 1;
	size_t tlen;
	const char *t = mrd_str(r, &tlen);
	uint16_t pid = 0;
	char *topic;

	if (qos == 3 || r->err || !mqtt_topic_valid(t, tlen))
		return -1;
	if (qos)
		pid = mrd_u16(r);
	if (r->err)
		return -1;

	topic = strndup(t, tlen);
	aer_dbg("%s: publish %s (%zu bytes, qos %d%s)", c->id, topic, mrd_left(r), qos,
		retain ? ", retain" : "");
	publish(topic, r->p + r->pos, mrd_left(r), qos, retain);
	free(topic);

	if (qos == 1)
		mqtt_ack(&c->out, MQTT_PUBACK, pid);
	else if (qos == 2)
		mqtt_ack(&c->out, MQTT_PUBREC, pid);
	return 0;
}

static int handle_subscribe(struct client *c, struct mrd *r)
{
	uint16_t pid = mrd_u16(r);
	struct mbuf codes = { 0 };
	int first_new[MAX_SUBS], nnew = 0;

	if (r->err || !mrd_left(r))
		return -1;
	while (mrd_left(r)) {
		size_t flen;
		const char *f = mrd_str(r, &flen);
		uint8_t qos = mrd_u8(r);
		char *filter;
		int s;

		if (r->err || (qos & 0xfc)) {
			mbuf_free(&codes);
			return -1;
		}
		if (!mqtt_filter_valid(f, flen)) {
			mbuf_u8(&codes, 0x80);
			continue;
		}
		filter = strndup(f, flen);
		if (qos > 1)
			qos = 1;
		for (s = 0; s < c->nsubs; s++)
			if (!strcmp(c->subs[s].filter, filter))
				break;
		if (s == c->nsubs) {
			if (c->nsubs == MAX_SUBS) {
				free(filter);
				mbuf_u8(&codes, 0x80);
				continue;
			}
			c->subs[c->nsubs++].filter = filter;
		} else {
			free(filter);
		}
		c->subs[s].qos = qos;
		mbuf_u8(&codes, qos);
		if (nnew < MAX_SUBS)
			first_new[nnew++] = s;
		aer_log("%s: subscribed to %s (qos %u)", c->id, c->subs[s].filter, qos);
	}

	mqtt_fixed_header(&c->out, MQTT_SUBACK << 4, 2 + codes.len);
	mbuf_u16(&c->out, pid);
	mbuf_put(&c->out, codes.d, codes.len);
	mbuf_free(&codes);

	/* then hand over the retained messages matching the new subscriptions */
	for (int k = 0; k < nnew; k++) {
		const struct sub *sub = &c->subs[first_new[k]];

		for (int i = 0; i < nretained; i++)
			if (mqtt_topic_match(sub->filter, retained[i].topic))
				send_publish(c, retained[i].topic, retained[i].payload, retained[i].len,
					     retained[i].qos < sub->qos ? retained[i].qos : sub->qos, true);
	}
	return 0;
}

static int handle_unsubscribe(struct client *c, struct mrd *r)
{
	uint16_t pid = mrd_u16(r);

	if (r->err || !mrd_left(r))
		return -1;
	while (mrd_left(r)) {
		size_t flen;
		const char *f = mrd_str(r, &flen);

		if (r->err)
			return -1;
		for (int s = 0; s < c->nsubs; s++) {
			if (strlen(c->subs[s].filter) == flen && !memcmp(c->subs[s].filter, f, flen)) {
				free(c->subs[s].filter);
				c->subs[s] = c->subs[--c->nsubs];
				break;
			}
		}
	}
	mqtt_ack(&c->out, MQTT_UNSUBACK, pid);
	return 0;
}

static int handle_packet(struct client *c, const uint8_t *pkt, size_t hdr_len, size_t total)
{
	uint8_t type = pkt[0] >> 4, flags = pkt[0] & 0x0f;
	struct mrd r = { .p = pkt + hdr_len, .len = total - hdr_len };

	if (!c->connected) {
		if (type != MQTT_CONNECT || c->id)
			return -1;
		return handle_connect(c, &r);
	}

	switch (type) {
	case MQTT_PUBLISH:
		return handle_publish(c, flags, &r);
	case MQTT_PUBACK:
	case MQTT_PUBCOMP:
		return 0;		/* no redelivery state is kept */
	case MQTT_PUBREC:
		mqtt_ack(&c->out, MQTT_PUBREL, mrd_u16(&r));
		return r.err ? -1 : 0;
	case MQTT_PUBREL:
		mqtt_ack(&c->out, MQTT_PUBCOMP, mrd_u16(&r));
		return r.err ? -1 : 0;
	case MQTT_SUBSCRIBE:
		if (flags != 2)
			return -1;
		return handle_subscribe(c, &r);
	case MQTT_UNSUBSCRIBE:
		if (flags != 2)
			return -1;
		return handle_unsubscribe(c, &r);
	case MQTT_PINGREQ:
		mqtt_simple(&c->out, MQTT_PINGRESP);
		return 0;
	case MQTT_DISCONNECT:
		c->clean_close = true;
		c->closing = true;
		return 0;
	default:
		return -1;
	}
}

/* ------------------------------------------------------------ connections */

static void client_free(struct client *c)
{
	free(c->id);
	for (int s = 0; s < c->nsubs; s++)
		free(c->subs[s].filter);
	free(c->will_topic);
	free(c->will_msg);
	mbuf_free(&c->in);
	mbuf_free(&c->out);
	free(c);
}

/* Closes right away (why != NULL is logged). Publishes the will if any. */
static void client_close(struct client *c, const char *why)
{
	int i;

	if (c->fd < 0)
		return;
	if (why)
		aer_log("%s: '%s' %s", c->addr, c->id ? c->id : "-", why);
	close(c->fd);
	c->fd = -1;
	c->connected = false;
	if (c->has_will && !c->clean_close) {
		aer_dbg("publishing will of '%s' on %s", c->id, c->will_topic);
		c->has_will = false;
		publish(c->will_topic, c->will_msg, c->will_len, c->will_qos, c->will_retain);
	}
	for (i = 0; i < nclients; i++)
		if (clients[i] == c)
			break;
	if (i < nclients)
		clients[i] = clients[--nclients];
	client_free(c);
}

static void client_read(struct client *c)
{
	uint8_t buf[16384];
	ssize_t n = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT);

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		client_close(c, n == 0 ? "disconnected" : "connection error");
		return;
	}
	if (n < 0)
		return;
	mbuf_put(&c->in, buf, (size_t)n);
	c->last_rx_ms = aer_mono_ms();

	while (c->in.len && !c->closing) {
		size_t hdr_len;
		ssize_t total = mqtt_frame(c->in.d, c->in.len, MQTT_MAX_PACKET, &hdr_len);

		if (total == 0)
			break;
		if (total < 0 || handle_packet(c, c->in.d, hdr_len, (size_t)total) < 0) {
			client_close(c, "protocol error");
			return;
		}
		mbuf_consume(&c->in, (size_t)total);
	}
}

static void client_write(struct client *c)
{
	ssize_t n;

	if (!c->out.len)
		return;
	n = send(c->fd, c->out.d, c->out.len, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n < 0 && errno != EAGAIN && errno != EINTR) {
		client_close(c, "connection error");
		return;
	}
	if (n > 0)
		mbuf_consume(&c->out, (size_t)n);
}

static void accept_clients(int lfd)
{
	struct sockaddr_storage ss;
	socklen_t sl = sizeof(ss);
	int fd, one = 1;

	while ((fd = accept4(lfd, (struct sockaddr *)&ss, &sl, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
		struct client *c;
		char host[NI_MAXHOST], serv[NI_MAXSERV];

		if (nclients == max_clients) {
			aer_log("too many clients, refusing connection");
			close(fd);
			sl = sizeof(ss);
			continue;
		}
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		c = calloc(1, sizeof(*c));
		c->fd = fd;
		c->accepted_ms = c->last_rx_ms = aer_mono_ms();
		if (getnameinfo((struct sockaddr *)&ss, sl, host, sizeof(host), serv, sizeof(serv),
				NI_NUMERICHOST | NI_NUMERICSERV) == 0)
			snprintf(c->addr, sizeof(c->addr), "%.48s:%.8s", host, serv);
		else
			snprintf(c->addr, sizeof(c->addr), "?");
		clients[nclients++] = c;
		aer_dbg("%s: accepted", c->addr);
		sl = sizeof(ss);
	}
}

static int listen_on(const char *bind_addr, const char *port)
{
	struct addrinfo hints = { .ai_flags = AI_PASSIVE, .ai_socktype = SOCK_STREAM }, *res, *ai;
	int fd = -1, one = 1, err;

	err = getaddrinfo(*bind_addr ? bind_addr : NULL, port, &hints, &res);
	if (err) {
		aer_log("bind address %s:%s: %s", bind_addr, port, gai_strerror(err));
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 16) == 0)
			break;
		err = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		aer_log("cannot listen on %s:%s: %s", bind_addr, port, strerror(err));
	else
		aer_log("listening on %s:%s", *bind_addr ? bind_addr : "*", port);
	return fd;
}

int main(int argc, char **argv)
{
	struct aer_conf *conf;
	struct pollfd *pfd;
	char port[16];
	int lfd;

	conf = aer_conf_load(aer_parse_common(argc, argv, NULL, NULL, NULL));
	aer_signals();

	max_clients = (int)aer_conf_int(conf, "broker", "max_clients", 64);
	if (max_clients < 1)
		max_clients = 1;
	snprintf(port, sizeof(port), "%ld", aer_conf_int(conf, "broker", "port", 1883));
	lfd = listen_on(aer_conf_get(conf, "broker", "bind", "0.0.0.0"), port);
	if (lfd < 0)
		return 1;

	clients = calloc((size_t)max_clients, sizeof(*clients));
	pfd = calloc((size_t)max_clients + 1, sizeof(*pfd));

	while (!aer_quit) {
		int64_t now = aer_mono_ms();
		int n = 0;

		/* timeouts and finished closes; iterate backwards, close() reorders */
		for (int i = nclients - 1; i >= 0; i--) {
			struct client *c = clients[i];

			if (c->closing && !c->out.len)
				client_close(c, c->clean_close ? "disconnected" : "closed");
			else if (c->closing && now - c->last_rx_ms > CLOSE_FLUSH_MS)
				client_close(c, "not reading, closed");
			else if (!c->connected && now - c->accepted_ms > CONNECT_TIMEOUT_MS)
				client_close(c, "no CONNECT received");
			else if (c->connected && c->keepalive &&
				 now - c->last_rx_ms > c->keepalive * 1500L)
				client_close(c, "keep alive timeout");
		}

		pfd[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		for (int i = 0; i < nclients; i++)
			pfd[n++] = (struct pollfd){
				.fd = clients[i]->fd,
				.events = (short)((clients[i]->closing ? 0 : POLLIN) |
						  (clients[i]->out.len ? POLLOUT : 0)),
			};
		if (poll(pfd, (nfds_t)n, 1000) <= 0)
			continue;

		/* pfd[1 + i] belongs to the client at index i when poll() was called */
		{
			struct client *snap[max_clients];
			int nsnap = nclients;

			memcpy(snap, clients, (size_t)nclients * sizeof(*snap));
			for (int i = 0; i < nsnap; i++) {
				struct client *c = snap[i];
				short re = pfd[1 + i].revents;
				bool alive = false;

				for (int k = 0; k < nclients; k++)
					alive |= clients[k] == c;
				if (!alive || !re)
					continue;
				if (re & (POLLERR | POLLHUP | POLLNVAL) && !(re & POLLIN)) {
					client_close(c, "connection lost");
					continue;
				}
				if (re & POLLIN)
					client_read(c);
			}
		}
		if (pfd[0].revents & POLLIN)
			accept_clients(lfd);

		/* write after reading so replies and routed messages go out at once */
		for (int i = nclients - 1; i >= 0; i--)
			client_write(clients[i]);
	}

	while (nclients)
		client_close(clients[0], NULL);
	close(lfd);
	aer_conf_free(conf);
	return 0;
}
