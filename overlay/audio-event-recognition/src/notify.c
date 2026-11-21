/*
 * Notify module
 *
 * Receives detected events from the detector module and publishes each one
 * as a JSON message (spec/mqtt_spec.md) to the board's MQTT broker at
 * 127.0.0.1, on the topic from the configuration. User devices subscribed to
 * that topic get the notification. The MQTT connection is kept alive with
 * PINGREQ and re-established if the broker restarts.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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

#define BROKER_IP		"127.0.0.1"
#define CLIENT_ID		"aer-notify"
#define KEEPALIVE_SEC		60
#define CONNACK_TIMEOUT_MS	5000
#define RETRY_MS		2000

typedef struct {
	int fd;
	uint16_t keepalive;		/* seconds */
	int64_t last_tx_ms;		/* for PINGREQ when idle */
	int64_t ping_sent_ms;		/* 0: no PINGRESP outstanding */
} mqtt_connection_t;

static volatile int running = 1;

static int write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
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

static void disconnect_from_broker(mqtt_connection_t *connection)
{
	static const uint8_t pkt[2] = { 0xe0, 0 };	/* DISCONNECT */

	if (!connection)
		return;
	write_all(connection->fd, pkt, sizeof(pkt));
	close(connection->fd);
	free(connection);
}

/*
 * Connect to the broker over TCP, send CONNECT (MQTT 3.1.1, clean session,
 * keepalive 60 s) and wait for a successful CONNACK.
 */
mqtt_connection_t *connect_to_broker(const char *broker_ip,
				     uint16_t broker_port,
				     const char *client_id)
{
	struct sockaddr_in addr;
	uint8_t pkt[128], ack[4];
	size_t id_len = strlen(client_id), body, h, got = 0;
	struct pollfd pfd;
	mqtt_connection_t *c;
	int fd, one = 1;

	if (id_len > 23)
		return NULL;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(broker_port);
	if (inet_pton(AF_INET, broker_ip, &addr.sin_addr) != 1)
		return NULL;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return NULL;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	/* variable header: "MQTT", level 4, clean session, keepalive */
	body = 10 + 2 + id_len;
	pkt[0] = 0x10;
	h = 1 + encode_length(pkt + 1, (uint32_t)body);
	memcpy(pkt + h, "\0\4MQTT\4\2", 8);
	pkt[h + 8] = KEEPALIVE_SEC >> 8;
	pkt[h + 9] = KEEPALIVE_SEC & 0xff;
	pkt[h + 10] = id_len >> 8;
	pkt[h + 11] = id_len & 0xff;
	memcpy(pkt + h + 12, client_id, id_len);
	if (write_all(fd, pkt, h + body))
		goto fail;

	pfd.fd = fd;
	pfd.events = POLLIN;
	while (got < sizeof(ack)) {
		ssize_t n;

		if (poll(&pfd, 1, CONNACK_TIMEOUT_MS) <= 0)
			goto fail;
		n = recv(fd, ack + got, sizeof(ack) - got, 0);
		if (n <= 0)
			goto fail;
		got += (size_t)n;
	}
	if (ack[0] != 0x20 || ack[1] != 2 || ack[3] != 0) {
		fprintf(stderr, "broker refused connection (code %u)\n", ack[3]);
		goto fail;
	}

	c = calloc(1, sizeof(*c));
	if (!c)
		goto fail;
	c->fd = fd;
	c->keepalive = KEEPALIVE_SEC;
	c->last_tx_ms = now_ms();
	fprintf(stderr, "connected to broker %s:%u\n", broker_ip, broker_port);
	return c;

fail:
	close(fd);
	return NULL;
}

/* Append s to out as JSON string content. Returns -1 if it does not fit. */
static int json_escape(char *out, size_t max_len, size_t *pos, const char *s)
{
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		int n;

		if (ch == '"' || ch == '\\')
			n = snprintf(out + *pos, max_len - *pos, "\\%c", ch);
		else if (ch < 0x20)
			n = snprintf(out + *pos, max_len - *pos, "\\u%04x", ch);
		else
			n = snprintf(out + *pos, max_len - *pos, "%c", ch);
		if (n < 0 || (size_t)n >= max_len - *pos)
			return -1;
		*pos += (size_t)n;
	}
	return 0;
}

/*
 * Build the notification JSON for an event:
 * {"device":..,"event_id":..,"class":..,"score":..,"threshold":..,"time":..}
 * Returns the length, or -1 if message is too small.
 */
int build_json(const event_t *event, const char *device_id, char *message,
	       size_t max_len)
{
	size_t pos = 0;
	int n;

#define APPEND(...)							\
	do {								\
		n = snprintf(message + pos, max_len - pos, __VA_ARGS__);\
		if (n < 0 || (size_t)n >= max_len - pos)		\
			return -1;					\
		pos += (size_t)n;					\
	} while (0)

	if (max_len == 0)
		return -1;
	APPEND("{\"device\":\"");
	if (json_escape(message, max_len, &pos, device_id))
		return -1;
	APPEND("\",\"event_id\":%u,\"class\":\"", event->event_id);
	if (json_escape(message, max_len, &pos, event->class_name))
		return -1;
	APPEND("\",\"score\":%.3f,\"threshold\":%.3g,\"time\":%lld}",
	       event->score, event->threshold,
	       (long long)(event->time_ms / 1000));
#undef APPEND
	return (int)pos;
}

/* Publish message on topic at QoS 0, retain 0. Returns 0 or -1. */
int send_to_broker(mqtt_connection_t *connection, const char *topic,
		   const void *message, size_t len)
{
	size_t topic_len = strlen(topic), h, total;
	uint8_t *pkt;
	int ret;

	if (!connection || topic_len > 0xffff)
		return -1;
	pkt = malloc(5 + 2 + topic_len + len);
	if (!pkt)
		return -1;
	pkt[0] = 0x30;	/* PUBLISH, QoS 0, retain 0 */
	h = 1 + encode_length(pkt + 1, (uint32_t)(2 + topic_len + len));
	pkt[h] = topic_len >> 8;
	pkt[h + 1] = topic_len & 0xff;
	memcpy(pkt + h + 2, topic, topic_len);
	memcpy(pkt + h + 2 + topic_len, message, len);
	total = h + 2 + topic_len + len;

	ret = write_all(connection->fd, pkt, total);
	free(pkt);
	if (ret == 0)
		connection->last_tx_ms = now_ms();
	return ret;
}

static int send_ping(mqtt_connection_t *connection)
{
	static const uint8_t pkt[2] = { 0xc0, 0 };	/* PINGREQ */

	if (write_all(connection->fd, pkt, sizeof(pkt)))
		return -1;
	connection->last_tx_ms = now_ms();
	if (!connection->ping_sent_ms)
		connection->ping_sent_ms = connection->last_tx_ms;
	return 0;
}

/* Read what the broker sent. Returns -1 if the connection is gone. */
static int read_broker(mqtt_connection_t *connection)
{
	uint8_t buf[256];
	ssize_t n = recv(connection->fd, buf, sizeof(buf), MSG_DONTWAIT);

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
		return -1;
	/* only PINGRESP (0xd0 0x00) is expected on this connection */
	if (n > 0 && memchr(buf, 0xd0, (size_t)n))
		connection->ping_sent_ms = 0;
	return 0;
}

static void publish_event(mqtt_connection_t **conn, const app_config_t *config,
			  const event_t *event)
{
	char json[512];
	int len = build_json(event, config->board_name, json, sizeof(json));

	if (len < 0) {
		fprintf(stderr, "event %u: JSON too long\n", event->event_id);
		return;
	}
	if (!*conn) {
		fprintf(stderr, "event %u dropped: broker not connected\n",
			event->event_id);
		return;
	}
	if (send_to_broker(*conn, config->topic, json, (size_t)len)) {
		fprintf(stderr, "event %u dropped: broker connection lost\n",
			event->event_id);
		close((*conn)->fd);
		free(*conn);
		*conn = NULL;
		return;
	}
	fprintf(stderr, "published %s\n", json);
}

int main(int argc, char **argv)
{
	mqtt_connection_t *conn = NULL;
	int64_t last_broker_try = 0, last_detector_try = 0;
	app_config_t config;
	msg_result_t *msg;
	int det_fd = -1, det_warned = 0;

	setvbuf(stderr, NULL, _IOLBF, 0);
	config_load_from_args(argc, argv, &config);
	install_signal_handlers(&running);

	/* the largest message on the detector socket */
	msg = malloc(sizeof(*msg));
	if (!msg)
		return 1;

	while (running) {
		struct pollfd pfd[2];
		int64_t now = now_ms();
		nfds_t nfds = 0;
		int det_idx = -1, brk_idx = -1;

		if (det_fd < 0 && now - last_detector_try >= 1000) {
			last_detector_try = now;
			det_fd = try_connect_to_socket(AER_SOCK_DETECTOR);
			if (det_fd >= 0) {
				fprintf(stderr, "connected to %s\n",
					AER_SOCK_DETECTOR);
				det_warned = 0;
			} else if (!det_warned) {
				fprintf(stderr, "waiting for %s\n",
					AER_SOCK_DETECTOR);
				det_warned = 1;
			}
		}
		if (!conn && now - last_broker_try >= RETRY_MS) {
			last_broker_try = now;
			conn = connect_to_broker(BROKER_IP, config.broker_port,
						 CLIENT_ID);
		}
		if (conn) {
			/* keepalive: PINGREQ after 60 s without sending */
			if (now - conn->last_tx_ms >= conn->keepalive * 1000LL &&
			    send_ping(conn))
				goto broker_lost;
			if (conn->ping_sent_ms &&
			    now - conn->ping_sent_ms > conn->keepalive * 1000LL) {
				fprintf(stderr, "no PINGRESP from broker\n");
				goto broker_lost;
			}
		}

		if (det_fd >= 0) {
			pfd[nfds].fd = det_fd;
			pfd[nfds].events = POLLIN;
			det_idx = (int)nfds++;
		}
		if (conn) {
			pfd[nfds].fd = conn->fd;
			pfd[nfds].events = POLLIN;
			brk_idx = (int)nfds++;
		}
		if (poll(pfd, nfds, 1000) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (det_idx >= 0 && pfd[det_idx].revents) {
			ssize_t n = receive_on_socket(det_fd, msg, sizeof(*msg));

			if (n == 0 || (n < 0 && errno != EINTR &&
				       errno != EMSGSIZE)) {
				fprintf(stderr, "detector disconnected\n");
				close(det_fd);
				det_fd = -1;
			} else if ((size_t)n == sizeof(msg_event_t) &&
				   msg->type == MSG_EVENT) {
				msg_event_t *ev = (msg_event_t *)msg;

				publish_event(&conn, &config, &ev->event);
			}
			/* MSG_RESULT (per-frame scores) is for the display */
		}
		if (brk_idx >= 0 && conn && pfd[brk_idx].revents &&
		    read_broker(conn))
			goto broker_lost;
		continue;

broker_lost:
		fprintf(stderr, "broker connection lost\n");
		close(conn->fd);
		free(conn);
		conn = NULL;
	}

	disconnect_from_broker(conn);
	if (det_fd >= 0)
		close(det_fd);
	free(msg);
	return 0;
}
