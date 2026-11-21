/*
 * MQTT broker module
 *
 * A small MQTT 3.1.1 broker (see spec/mqtt_spec.md). It listens on the port
 * from the configuration on all interfaces. The notify module publishes
 * events to it over 127.0.0.1, and user devices on the network subscribe.
 * Each PUBLISH is delivered at QoS 0, without retain, to every client with a
 * matching subscription (+ and # wildcards are supported). Sessions are not
 * stored (clean session). A client that sends nothing for 1.5 x its
 * keepalive is disconnected.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/config.h"
#include "common/protocol.h"
#include "common/socket.h"

#define MAX_SESSIONS		32
#define MAX_FILTERS		16
#define MAX_TOPIC		256
#define MAX_PACKET		(64 * 1024)
#define CONNECT_TIMEOUT_MS	10000

enum {
	CONNECT = 1, CONNACK, PUBLISH, PUBACK, PUBREC, PUBREL, PUBCOMP,
	SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK, PINGREQ, PINGRESP, DISCONNECT,
};

/* One received MQTT control packet */
typedef struct {
	uint8_t type;			/* control packet type, 1 - 14 */
	uint8_t flags;			/* low nibble of the fixed header */
	uint32_t length;		/* remaining length */
	const uint8_t *body;		/* variable header + payload */
} mqtt_packet_t;

/* One client connection */
typedef struct {
	int fd;
	int connected;			/* CONNECT accepted */
	int closing;			/* close after this poll round */
	char addr[INET_ADDRSTRLEN + 8];
	char client_id[64];
	uint16_t keepalive;		/* seconds, from CONNECT */
	int64_t last_rx_ms;
	char filters[MAX_FILTERS][MAX_TOPIC];
	int num_filters;
	uint8_t *rx;			/* partial packet bytes */
	size_t rx_len;
} session_t;

typedef struct {
	int listen_fd;
	session_t sessions[MAX_SESSIONS];
	int num_sessions;
	unsigned long published;
} broker_t;

static volatile int running = 1;

/* Read a UTF-8 string (2-byte length + bytes) at *off. */
static int read_string(const mqtt_packet_t *p, uint32_t *off, char *out,
		       size_t out_size)
{
	uint32_t len;

	if (*off + 2 > p->length)
		return -1;
	len = (uint32_t)p->body[*off] << 8 | p->body[*off + 1];
	*off += 2;
	if (*off + len > p->length)
		return -1;
	if (out) {
		if (len >= out_size)
			return -1;
		memcpy(out, p->body + *off, len);
		out[len] = '\0';
		if (memchr(out, '\0', len))
			return -1;
	}
	*off += len;
	return 0;
}

static int send_all(session_t *s, const void *buf, size_t len)
{
	ssize_t n = send(s->fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);

	if (n != (ssize_t)len) {
		/* a client too slow to take a small packet is dropped */
		s->closing = 1;
		return -1;
	}
	return 0;
}

static int send_ack(session_t *s, uint8_t header, uint16_t packet_id)
{
	uint8_t pkt[4] = { header, 2, packet_id >> 8, packet_id & 0xff };

	return send_all(s, pkt, sizeof(pkt));
}

static size_t encode_length(uint8_t *out, uint32_t len)
{
	size_t n = 0;

	do {
		uint8_t b = len % 128;

		len /= 128;
		out[n++] = b | (len ? 0x80 : 0);
	} while (len);
	return n;
}

/* MQTT topic filter match with '+' (one level) and '#' (rest) */
static int topic_matches(const char *filter, const char *topic)
{
	/* topics starting with '$' are not matched by wildcards at level 1 */
	if (topic[0] == '$' && (filter[0] == '+' || filter[0] == '#'))
		return 0;
	for (;;) {
		if (*filter == '#')
			return 1;
		if (*filter == '+') {
			while (*topic && *topic != '/')
				topic++;
			filter++;
			continue;
		}
		if (*filter == '\0' || *topic == '\0')
			/* "a/#" also matches its parent "a" */
			return *filter == *topic ||
			       (*topic == '\0' && !strcmp(filter, "/#"));
		if (*filter != *topic)
			return 0;
		filter++;
		topic++;
	}
}

static int valid_filter(const char *f)
{
	size_t i, n = strlen(f);

	if (n == 0)
		return 0;
	for (i = 0; i < n; i++) {
		if (f[i] == '#' && (i != n - 1 || (i && f[i - 1] != '/')))
			return 0;
		if (f[i] == '+' && ((i && f[i - 1] != '/') ||
				    (i + 1 < n && f[i + 1] != '/')))
			return 0;
	}
	return 1;
}

/*
 * Deliver a published message to every client subscribed to a matching
 * filter, at QoS 0 without retain. Returns the number of receivers.
 */
int broadcast_to_subscribers(broker_t *broker, const char *topic,
			     const void *message, size_t len)
{
	size_t topic_len = strlen(topic), hdr, total;
	uint8_t *pkt;
	int i, f, count = 0;

	pkt = malloc(5 + 2 + topic_len + len);
	if (!pkt)
		return -1;
	pkt[0] = PUBLISH << 4;
	hdr = 1 + encode_length(pkt + 1, (uint32_t)(2 + topic_len + len));
	pkt[hdr] = topic_len >> 8;
	pkt[hdr + 1] = topic_len & 0xff;
	memcpy(pkt + hdr + 2, topic, topic_len);
	memcpy(pkt + hdr + 2 + topic_len, message, len);
	total = hdr + 2 + topic_len + len;

	for (i = 0; i < broker->num_sessions; i++) {
		session_t *s = &broker->sessions[i];

		if (!s->connected || s->closing)
			continue;
		for (f = 0; f < s->num_filters; f++) {
			if (topic_matches(s->filters[f], topic)) {
				if (send_all(s, pkt, total) == 0)
					count++;
				break;
			}
		}
	}
	free(pkt);
	return count;
}

static int handle_connect(session_t *session, const mqtt_packet_t *p)
{
	uint8_t connack[4] = { CONNACK << 4, 2, 0, 0 };
	char proto[8];
	uint32_t off = 0;
	uint8_t level, flags;

	if (session->connected)
		return -1;	/* a second CONNECT is a protocol violation */
	if (read_string(p, &off, proto, sizeof(proto)) || off + 4 > p->length)
		return -1;
	level = p->body[off];
	flags = p->body[off + 1];
	session->keepalive = (uint16_t)(p->body[off + 2] << 8 | p->body[off + 3]);
	off += 4;

	if (strcmp(proto, "MQTT") || level != 4) {
		connack[3] = 1;	/* unacceptable protocol version */
		send_all(session, connack, sizeof(connack));
		return -1;
	}
	if (flags & 1)
		return -1;	/* reserved bit */
	if (read_string(p, &off, session->client_id,
			sizeof(session->client_id))) {
		connack[3] = 2;	/* identifier rejected */
		send_all(session, connack, sizeof(connack));
		return -1;
	}
	/* will topic/message, user name and password are accepted, unused */
	if ((flags & 0x04) &&
	    (read_string(p, &off, NULL, 0) || read_string(p, &off, NULL, 0)))
		return -1;
	if ((flags & 0x80) && read_string(p, &off, NULL, 0))
		return -1;
	if ((flags & 0x40) && read_string(p, &off, NULL, 0))
		return -1;

	session->connected = 1;
	fprintf(stderr, "%s: CONNECT client \"%s\", keepalive %u s\n",
		session->addr, session->client_id, session->keepalive);
	return send_all(session, connack, sizeof(connack));
}

static int handle_subscribe(session_t *session, const mqtt_packet_t *p)
{
	/* each filter takes at least 3 bytes: 2-byte length and QoS */
	static uint8_t pkt[5 + 2 + MAX_PACKET / 3];
	uint8_t codes[MAX_PACKET / 3];
	uint32_t off = 2;
	size_t n = 0, h;
	uint16_t id;

	if (p->flags != 2 || p->length < 2)
		return -1;
	id = (uint16_t)(p->body[0] << 8 | p->body[1]);
	while (off < p->length) {
		char filter[MAX_TOPIC];
		int i, ok;

		if (read_string(p, &off, filter, sizeof(filter)) ||
		    off >= p->length)
			return -1;
		off++;	/* requested QoS: everything is delivered at QoS 0 */
		ok = valid_filter(filter);
		for (i = 0; ok && i < session->num_filters; i++)
			if (!strcmp(session->filters[i], filter))
				break;
		if (ok && i == session->num_filters) {
			if (session->num_filters < MAX_FILTERS)
				strcpy(session->filters[session->num_filters++],
				       filter);
			else
				ok = 0;
		}
		codes[n++] = ok ? 0x00 : 0x80;	/* granted QoS 0 / failure */
		fprintf(stderr, "%s: SUBSCRIBE \"%s\"%s\n", session->addr,
			filter, ok ? "" : " rejected");
	}
	if (n == 0)
		return -1;	/* at least one filter is required */

	pkt[0] = SUBACK << 4;
	h = 1 + encode_length(pkt + 1, (uint32_t)(2 + n));
	pkt[h] = id >> 8;
	pkt[h + 1] = id & 0xff;
	memcpy(pkt + h + 2, codes, n);
	return send_all(session, pkt, h + 2 + n);
}

static int handle_unsubscribe(session_t *session, const mqtt_packet_t *p)
{
	uint32_t off = 2;
	uint16_t id;

	if (p->flags != 2 || p->length < 2)
		return -1;
	id = (uint16_t)(p->body[0] << 8 | p->body[1]);
	while (off < p->length) {
		char filter[MAX_TOPIC];
		int i;

		if (read_string(p, &off, filter, sizeof(filter)))
			return -1;
		for (i = 0; i < session->num_filters; i++) {
			if (!strcmp(session->filters[i], filter)) {
				strcpy(session->filters[i],
				       session->filters[--session->num_filters]);
				break;
			}
		}
	}
	return send_ack(session, UNSUBACK << 4, id);
}

static int handle_publish(broker_t *broker, session_t *session,
			  const mqtt_packet_t *p)
{
	unsigned qos = (p->flags >> 1) & 3;
	char topic[MAX_TOPIC];
	uint32_t off = 0;
	uint16_t id = 0;
	int receivers;

	if (qos == 3 || read_string(p, &off, topic, sizeof(topic)) ||
	    !topic[0] || strpbrk(topic, "+#"))
		return -1;
	if (qos) {
		if (off + 2 > p->length)
			return -1;
		id = (uint16_t)(p->body[off] << 8 | p->body[off + 1]);
		off += 2;
	}
	receivers = broadcast_to_subscribers(broker, topic, p->body + off,
					     p->length - off);
	broker->published++;
	fprintf(stderr, "%s: PUBLISH \"%s\" %u bytes -> %d subscriber(s)\n",
		session->addr, topic, p->length - off, receivers);

	if (qos == 1)
		return send_ack(session, PUBACK << 4, id);
	if (qos == 2)
		return send_ack(session, PUBREC << 4, id);
	return 0;
}

/*
 * Handle one packet from a client: CONNECT, SUBSCRIBE, UNSUBSCRIBE, PUBLISH,
 * PINGREQ and DISCONNECT. Returns 0 to keep the connection, -1 to close it.
 */
int handle_packet(broker_t *broker, session_t *session,
		  const mqtt_packet_t *packet)
{
	if (!session->connected && packet->type != CONNECT)
		return -1;

	switch (packet->type) {
	case CONNECT:
		return handle_connect(session, packet);
	case SUBSCRIBE:
		return handle_subscribe(session, packet);
	case UNSUBSCRIBE:
		return handle_unsubscribe(session, packet);
	case PUBLISH:
		return handle_publish(broker, session, packet);
	case PUBREL:	/* end of a QoS 2 publish from a client */
		if (packet->length < 2)
			return -1;
		return send_ack(session, PUBCOMP << 4,
				(uint16_t)(packet->body[0] << 8 |
					   packet->body[1]));
	case PUBACK:
	case PUBREC:
	case PUBCOMP:
		return 0;	/* broker only sends QoS 0, nothing to track */
	case PINGREQ:
		{
			uint8_t resp[2] = { PINGRESP << 4, 0 };

			return send_all(session, resp, sizeof(resp));
		}
	case DISCONNECT:
		fprintf(stderr, "%s: DISCONNECT\n", session->addr);
		return -1;
	default:
		return -1;
	}
}

/* Parse complete packets out of the session's receive buffer. */
static int process_rx(broker_t *broker, session_t *s)
{
	size_t start = 0;

	while (s->rx_len - start >= 2) {
		const uint8_t *b = s->rx + start;
		size_t avail = s->rx_len - start, hdr = 1;
		uint32_t len = 0, mult = 1;
		mqtt_packet_t pkt;

		for (;;) {
			if (hdr >= avail)
				goto incomplete;
			len += (b[hdr] & 0x7f) * mult;
			if (!(b[hdr++] & 0x80))
				break;
			mult *= 128;
			if (hdr > 4)
				return -1;	/* malformed length */
		}
		if (len + hdr > MAX_PACKET)
			return -1;
		if (avail < hdr + len)
			break;

		pkt.type = b[0] >> 4;
		pkt.flags = b[0] & 0x0f;
		pkt.length = len;
		pkt.body = b + hdr;
		if (handle_packet(broker, s, &pkt))
			return -1;
		start += hdr + len;
	}
incomplete:
	memmove(s->rx, s->rx + start, s->rx_len - start);
	s->rx_len -= start;
	return 0;
}

static void close_session(broker_t *broker, int i)
{
	session_t *s = &broker->sessions[i];

	fprintf(stderr, "%s: closed\n", s->addr);
	close(s->fd);
	free(s->rx);
	broker->sessions[i] = broker->sessions[--broker->num_sessions];
}

static void accept_session(broker_t *broker)
{
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);
	session_t *s;
	int fd, one = 1;

	fd = accept4(broker->listen_fd, (struct sockaddr *)&addr, &alen,
		     SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0)
		return;
	if (broker->num_sessions == MAX_SESSIONS) {
		fprintf(stderr, "too many clients\n");
		close(fd);
		return;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	s = &broker->sessions[broker->num_sessions];
	memset(s, 0, sizeof(*s));
	s->rx = malloc(MAX_PACKET);
	if (!s->rx) {
		close(fd);
		return;
	}
	s->fd = fd;
	s->last_rx_ms = now_ms();
	inet_ntop(AF_INET, &addr.sin_addr, s->addr, INET_ADDRSTRLEN);
	snprintf(s->addr + strlen(s->addr), 8, ":%u", ntohs(addr.sin_port));
	broker->num_sessions++;
	fprintf(stderr, "%s: connected\n", s->addr);
}

static void read_session(broker_t *broker, session_t *s)
{
	ssize_t n = recv(s->fd, s->rx + s->rx_len, MAX_PACKET - s->rx_len, 0);

	if (n <= 0) {
		if (n == 0 || (errno != EAGAIN && errno != EINTR))
			s->closing = 1;
		return;
	}
	s->rx_len += (size_t)n;
	s->last_rx_ms = now_ms();
	if (process_rx(broker, s))
		s->closing = 1;
}

static int open_listener(uint16_t port)
{
	struct sockaddr_in addr;
	int fd, one = 1;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 16) < 0) {
		fprintf(stderr, "listen on port %u: %s\n", port,
			strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	struct pollfd pfd[MAX_SESSIONS + 1];
	app_config_t config;
	broker_t broker;
	int i;

	setvbuf(stderr, NULL, _IOLBF, 0);
	config_load_from_args(argc, argv, &config);
	install_signal_handlers(&running);

	memset(&broker, 0, sizeof(broker));
	broker.listen_fd = open_listener(config.broker_port);
	if (broker.listen_fd < 0)
		return 1;
	fprintf(stderr, "MQTT broker listening on 0.0.0.0:%u\n",
		config.broker_port);

	while (running) {
		int64_t now;
		int n = broker.num_sessions;

		pfd[0].fd = broker.listen_fd;
		pfd[0].events = POLLIN;
		for (i = 0; i < n; i++) {
			pfd[i + 1].fd = broker.sessions[i].fd;
			pfd[i + 1].events = POLLIN;
		}
		if (poll(pfd, (nfds_t)n + 1, 1000) < 0 && errno != EINTR)
			break;

		/* sessions keep their index until the cleanup below */
		for (i = 0; i < n; i++)
			if (pfd[i + 1].revents)
				read_session(&broker, &broker.sessions[i]);
		if (pfd[0].revents & POLLIN)
			accept_session(&broker);

		now = now_ms();
		for (i = broker.num_sessions - 1; i >= 0; i--) {
			session_t *s = &broker.sessions[i];
			int64_t idle = now - s->last_rx_ms;

			if (!s->connected && idle > CONNECT_TIMEOUT_MS)
				s->closing = 1;
			if (s->keepalive && idle > (int64_t)s->keepalive * 1500) {
				fprintf(stderr, "%s: keepalive timeout\n",
					s->addr);
				s->closing = 1;
			}
			if (s->closing)
				close_session(&broker, i);
		}
	}

	while (broker.num_sessions)
		close_session(&broker, 0);
	close(broker.listen_fd);
	return 0;
}
