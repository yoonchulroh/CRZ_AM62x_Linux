// SPDX-License-Identifier: MIT
/*
 * aer-notify - module 6-2: send detected events to user devices over MQTT.
 *
 *   AER_MSG_EVENT -> MQTT PUBLISH on <topic> (default aer/<device>/event):
 *
 *   {"time":"2026-10-17T13:20:01.123+09:00","device_name":"mango",
 *    "event_type":"siren","score":0.823}
 *
 * Publishes to the broker on the board (aer-broker); user devices subscribe
 * there. Events are queued while the broker is unreachable and, with QoS 1,
 * kept until the broker acknowledges them.
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
#include "ipc.h"
#include "mqtt.h"

#define QUEUE_LEN		64
#define CONNECT_TIMEOUT_MS	5000
#define RETRY_MS		3000

enum state { DISCONNECTED, CONNECTING, WAIT_CONNACK, CONNECTED };

struct pending {
	char *json;
	size_t len;
	uint16_t pid;
	bool sent;
};

static struct {
	const char *host, *port;
	char topic[256], client_id[128];
	int qos;
	uint16_t keepalive;

	enum state state;
	int fd;
	struct mbuf in, out;
	int64_t state_ms, retry_ms, last_tx_ms, last_rx_ms;
	bool ping_sent;
	uint16_t next_pid;
	bool warned;

	struct pending q[QUEUE_LEN];
	int qhead, qlen;
} m = { .fd = -1 };

/* ------------------------------------------------------------------- JSON */

static void json_str(struct mbuf *b, const char *s)
{
	mbuf_u8(b, '"');
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		char esc[8];

		if (ch == '"' || ch == '\\') {
			mbuf_u8(b, '\\');
			mbuf_u8(b, ch);
		} else if (ch < 0x20) {
			snprintf(esc, sizeof(esc), "\\u%04x", ch);
			mbuf_put(b, esc, 6);
		} else {
			mbuf_u8(b, ch);
		}
	}
	mbuf_u8(b, '"');
}

static void build_json(struct mbuf *b, int64_t t_ns, const char *device,
		       const struct aer_event *ev)
{
	char when[40], score[32];

	aer_format_time(t_ns, when, sizeof(when));
	snprintf(score, sizeof(score), "%.3f", ev->score);

	mbuf_put(b, "{\"time\":", 8);
	json_str(b, when);
	mbuf_put(b, ",\"device_name\":", 15);
	json_str(b, device);
	mbuf_put(b, ",\"event_type\":", 14);
	json_str(b, ev->type);
	mbuf_put(b, ",\"score\":", 9);
	mbuf_put(b, score, strlen(score));
	mbuf_u8(b, '}');
}

/* ------------------------------------------------------------------ queue */

static struct pending *q_at(int i)
{
	return &m.q[(m.qhead + i) % QUEUE_LEN];
}

static void q_pop_front(void)
{
	free(q_at(0)->json);
	m.qhead = (m.qhead + 1) % QUEUE_LEN;
	m.qlen--;
}

static void q_push(char *json, size_t len)
{
	struct pending *p;

	if (m.qlen == QUEUE_LEN) {
		aer_log("queue full, dropping the oldest event");
		q_pop_front();
	}
	p = q_at(m.qlen++);
	p->json = json;
	p->len = len;
	p->sent = false;
	if (++m.next_pid == 0)
		m.next_pid = 1;
	p->pid = m.next_pid;
}

/* Writes every unsent (or, after a reconnect, unacknowledged) message. */
static void q_flush(bool resend)
{
	if (m.state != CONNECTED)
		return;
	for (int i = 0; i < m.qlen; i++) {
		struct pending *p = q_at(i);

		if (p->sent && !resend)
			continue;
		mqtt_publish(&m.out, m.topic, strlen(m.topic), p->json, p->len, m.qos,
			     false, p->sent, p->pid);
		p->sent = true;
	}
	/* QoS 0 has no acknowledgement: done once written to the buffer */
	if (!m.qos)
		while (m.qlen)
			q_pop_front();
}

static void q_ack(uint16_t pid)
{
	for (int i = 0; i < m.qlen; i++) {
		if (q_at(i)->pid != pid)
			continue;
		/* acknowledgements come in order in practice; remove up to it */
		while (i-- >= 0)
			q_pop_front();
		return;
	}
}

/* ------------------------------------------------------------------- MQTT */

static void mqtt_drop(const char *why)
{
	if (m.fd >= 0) {
		if (why)
			aer_log("broker %s:%s: %s", m.host, m.port, why);
		close(m.fd);
	}
	m.fd = -1;
	m.state = DISCONNECTED;
	m.in.len = m.out.len = 0;
	m.retry_ms = aer_mono_ms() + RETRY_MS;
}

static void mqtt_start(void)
{
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res;
	int err, one = 1;

	if (aer_mono_ms() < m.retry_ms)
		return;
	m.retry_ms = aer_mono_ms() + RETRY_MS;
	err = getaddrinfo(m.host, m.port, &hints, &res);
	if (err) {
		if (!m.warned)
			aer_log("broker %s: %s", m.host, gai_strerror(err));
		m.warned = true;
		return;
	}
	m.fd = socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (m.fd >= 0) {
		setsockopt(m.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		if (connect(m.fd, res->ai_addr, res->ai_addrlen) == 0 || errno == EINPROGRESS) {
			m.state = CONNECTING;
			m.state_ms = aer_mono_ms();
		} else {
			close(m.fd);
			m.fd = -1;
		}
	}
	freeaddrinfo(res);
}

static void mqtt_connected_socket(void)
{
	int err = 0;
	socklen_t len = sizeof(err);

	getsockopt(m.fd, SOL_SOCKET, SO_ERROR, &err, &len);
	if (err) {
		if (!m.warned)
			aer_log("broker %s:%s: %s, retrying", m.host, m.port, strerror(err));
		m.warned = true;
		mqtt_drop(NULL);
		return;
	}
	mqtt_connect(&m.out, m.client_id, m.keepalive);
	m.state = WAIT_CONNACK;
	m.state_ms = aer_mono_ms();
}

static void mqtt_handle(const uint8_t *pkt, size_t hdr_len, size_t total)
{
	struct mrd r = { .p = pkt + hdr_len, .len = total - hdr_len };
	uint8_t type = pkt[0] >> 4;

	switch (type) {
	case MQTT_CONNACK:
		mrd_u8(&r);
		if (m.state != WAIT_CONNACK || mrd_u8(&r) != 0 || r.err) {
			mqtt_drop("connection refused");
			return;
		}
		aer_log("connected to broker %s:%s as '%s', publishing to %s",
			m.host, m.port, m.client_id, m.topic);
		m.warned = false;
		m.state = CONNECTED;
		m.ping_sent = false;
		q_flush(true);
		break;
	case MQTT_PUBACK:
		q_ack(mrd_u16(&r));
		break;
	case MQTT_PINGRESP:
		m.ping_sent = false;
		break;
	case MQTT_PUBLISH:	/* not subscribed to anything */
	default:
		break;
	}
}

static void mqtt_read(void)
{
	uint8_t buf[4096];
	ssize_t n = recv(m.fd, buf, sizeof(buf), MSG_DONTWAIT);

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		mqtt_drop("connection closed");
		return;
	}
	if (n < 0)
		return;
	m.last_rx_ms = aer_mono_ms();
	mbuf_put(&m.in, buf, (size_t)n);
	while (m.fd >= 0 && m.in.len) {
		size_t hdr_len;
		ssize_t total = mqtt_frame(m.in.d, m.in.len, MQTT_MAX_PACKET, &hdr_len);

		if (total == 0)
			break;
		if (total < 0) {
			mqtt_drop("malformed packet");
			return;
		}
		mqtt_handle(m.in.d, hdr_len, (size_t)total);
		if (m.fd >= 0)
			mbuf_consume(&m.in, (size_t)total);
	}
}

static void mqtt_write(void)
{
	ssize_t n;

	if (!m.out.len)
		return;
	n = send(m.fd, m.out.d, m.out.len, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n < 0 && errno != EAGAIN && errno != EINTR) {
		mqtt_drop("write failed");
		return;
	}
	if (n > 0) {
		mbuf_consume(&m.out, (size_t)n);
		m.last_tx_ms = aer_mono_ms();
	}
}

static void mqtt_timers(void)
{
	int64_t now = aer_mono_ms();

	if ((m.state == CONNECTING || m.state == WAIT_CONNACK) &&
	    now - m.state_ms > CONNECT_TIMEOUT_MS) {
		mqtt_drop("connect timeout");
		return;
	}
	if (m.state != CONNECTED || !m.keepalive)
		return;
	if (m.ping_sent && now - m.last_rx_ms > m.keepalive * 1000L) {
		mqtt_drop("no answer to keep alive");
		return;
	}
	if (!m.ping_sent && now - m.last_tx_ms > m.keepalive * 500L) {
		mqtt_simple(&m.out, MQTT_PINGREQ);
		m.ping_sent = true;
	}
}

/* ------------------------------------------------------------------- main */

static void expand(char *dst, size_t len, const char *tmpl, const char *device)
{
	const char *p = strstr(tmpl, "{device}");

	if (!p) {
		aer_strlcpy(dst, tmpl, len);
		return;
	}
	snprintf(dst, len, "%.*s%s%s", (int)(p - tmpl), tmpl, device, p + 8);
}

int main(int argc, char **argv)
{
	static uint8_t msg[AER_MAX_MSG];
	struct aer_client in;
	struct aer_conf *conf;
	char device[64], port[16];

	conf = aer_conf_load(aer_parse_common(argc, argv, NULL, NULL, NULL));
	aer_signals();

	aer_device_name(conf, device, sizeof(device));
	m.host = aer_conf_get(conf, "notify", "broker_host", "127.0.0.1");
	snprintf(port, sizeof(port), "%ld", aer_conf_int(conf, "notify", "broker_port", 1883));
	m.port = port;
	expand(m.topic, sizeof(m.topic),
	       aer_conf_get(conf, "notify", "topic", "aer/{device}/event"), device);
	expand(m.client_id, sizeof(m.client_id),
	       aer_conf_get(conf, "notify", "client_id", "aer-notify-{device}"), device);
	m.qos = aer_conf_int(conf, "notify", "qos", 1) ? 1 : 0;
	m.keepalive = (uint16_t)aer_conf_int(conf, "notify", "keepalive", 30);
	if (!mqtt_topic_valid(m.topic, strlen(m.topic))) {
		aer_log("invalid topic '%s'", m.topic);
		return 1;
	}
	aer_log("device '%s', broker %s:%s, topic %s, qos %d", device, m.host, m.port,
		m.topic, m.qos);

	aer_client_init(&in, aer_run_dir(conf), "detect");

	while (!aer_quit) {
		struct pollfd pfd[2];
		int n = 0, in_idx = -1, m_idx = -1;

		if (m.state == DISCONNECTED)
			mqtt_start();
		mqtt_timers();
		aer_client_connect(&in);

		if (in.fd >= 0) {
			pfd[n] = (struct pollfd){ .fd = in.fd, .events = POLLIN };
			in_idx = n++;
		}
		if (m.fd >= 0) {
			pfd[n] = (struct pollfd){ .fd = m.fd, .events = POLLIN };
			if (m.state == CONNECTING || m.out.len)
				pfd[n].events |= POLLOUT;
			m_idx = n++;
		}
		if (poll(pfd, (nfds_t)n, 500) <= 0)
			continue;

		if (in_idx >= 0 && pfd[in_idx].revents) {
			ssize_t len = aer_client_read(&in, msg, sizeof(msg));
			const struct aer_hdr *h = (const struct aer_hdr *)msg;

			if (len > 0 && aer_msg_valid(msg, len, AER_MSG_EVENT)) {
				struct mbuf js = { 0 };

				build_json(&js, h->t_ns, device, (const struct aer_event *)(h + 1));
				aer_log("%s %.*s", m.state == CONNECTED ? "publish" : "queue",
					(int)js.len, (const char *)js.d);
				q_push((char *)js.d, js.len);
				q_flush(false);
			}
		}
		if (m_idx >= 0 && m.fd >= 0) {
			short re = pfd[m_idx].revents;

			if (m.state == CONNECTING && (re & (POLLOUT | POLLERR | POLLHUP)))
				mqtt_connected_socket();
			else if (re & (POLLIN | POLLERR | POLLHUP))
				mqtt_read();
		}
		if (m.fd >= 0 && m.state != CONNECTING)
			mqtt_write();
	}

	if (m.state == CONNECTED) {
		mqtt_simple(&m.out, MQTT_DISCONNECT);
		mqtt_write();
	}
	mqtt_drop(NULL);
	aer_client_close(&in);
	while (m.qlen)
		q_pop_front();
	aer_conf_free(conf);
	return 0;
}
