// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>

#include "mqtt.h"

void mbuf_put(struct mbuf *b, const void *p, size_t n)
{
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap : 256;

		while (cap < b->len + n)
			cap *= 2;
		b->d = realloc(b->d, cap);
		b->cap = cap;
	}
	if (n)
		memcpy(b->d + b->len, p, n);
	b->len += n;
}

void mbuf_u8(struct mbuf *b, uint8_t v)
{
	mbuf_put(b, &v, 1);
}

void mbuf_u16(struct mbuf *b, uint16_t v)
{
	uint8_t be[2] = { v >> 8, v & 0xff };

	mbuf_put(b, be, 2);
}

void mbuf_str(struct mbuf *b, const char *s, size_t n)
{
	mbuf_u16(b, (uint16_t)n);
	mbuf_put(b, s, n);
}

void mbuf_consume(struct mbuf *b, size_t n)
{
	if (n >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->d, b->d + n, b->len - n);
	b->len -= n;
}

void mbuf_free(struct mbuf *b)
{
	free(b->d);
	memset(b, 0, sizeof(*b));
}

uint8_t mrd_u8(struct mrd *r)
{
	if (r->pos + 1 > r->len) {
		r->err = true;
		return 0;
	}
	return r->p[r->pos++];
}

uint16_t mrd_u16(struct mrd *r)
{
	uint16_t v;

	if (r->pos + 2 > r->len) {
		r->err = true;
		return 0;
	}
	v = (uint16_t)(r->p[r->pos] << 8 | r->p[r->pos + 1]);
	r->pos += 2;
	return v;
}

const char *mrd_str(struct mrd *r, size_t *n)
{
	size_t len = mrd_u16(r);
	const char *s;

	if (r->err || r->pos + len > r->len) {
		r->err = true;
		*n = 0;
		return "";
	}
	s = (const char *)r->p + r->pos;
	r->pos += len;
	*n = len;
	return s;
}

size_t mrd_left(const struct mrd *r)
{
	return r->len - r->pos;
}

ssize_t mqtt_frame(const uint8_t *buf, size_t len, size_t max_len, size_t *hdr_len)
{
	size_t rem = 0, mult = 1;

	for (size_t i = 1; i <= 4; i++) {
		if (i >= len)
			return 0;
		rem += (buf[i] & 0x7f) * mult;
		if (!(buf[i] & 0x80)) {
			*hdr_len = i + 1;
			if (*hdr_len + rem > max_len)
				return -1;
			return *hdr_len + rem <= len ? (ssize_t)(*hdr_len + rem) : 0;
		}
		mult *= 128;
	}
	return -1;	/* remaining length longer than 4 bytes */
}

void mqtt_fixed_header(struct mbuf *out, uint8_t type_flags, size_t rem_len)
{
	mbuf_u8(out, type_flags);
	do {
		uint8_t byte = rem_len % 128;

		rem_len /= 128;
		if (rem_len)
			byte |= 0x80;
		mbuf_u8(out, byte);
	} while (rem_len);
}

void mqtt_connect(struct mbuf *out, const char *client_id, uint16_t keepalive)
{
	size_t id_len = strlen(client_id);

	/* "MQTT", level 4, clean session */
	mqtt_fixed_header(out, MQTT_CONNECT << 4, 10 + 2 + id_len);
	mbuf_str(out, "MQTT", 4);
	mbuf_u8(out, 4);
	mbuf_u8(out, 0x02);
	mbuf_u16(out, keepalive);
	mbuf_str(out, client_id, id_len);
}

void mqtt_publish(struct mbuf *out, const char *topic, size_t topic_len,
		  const void *payload, size_t len, int qos, bool retain, bool dup,
		  uint16_t packet_id)
{
	uint8_t flags = (uint8_t)((qos & 3) << 1 | (retain ? 1 : 0) | (dup && qos ? 8 : 0));

	mqtt_fixed_header(out, MQTT_PUBLISH << 4 | flags, 2 + topic_len + (qos ? 2 : 0) + len);
	mbuf_str(out, topic, topic_len);
	if (qos)
		mbuf_u16(out, packet_id);
	mbuf_put(out, payload, len);
}

void mqtt_simple(struct mbuf *out, enum mqtt_type type)
{
	mqtt_fixed_header(out, (uint8_t)(type << 4), 0);
}

void mqtt_ack(struct mbuf *out, enum mqtt_type type, uint16_t packet_id)
{
	/* PUBREL must carry flags 0010 */
	mqtt_fixed_header(out, (uint8_t)(type << 4 | (type == MQTT_PUBREL ? 2 : 0)), 2);
	mbuf_u16(out, packet_id);
}

void mqtt_connack(struct mbuf *out, bool session_present, uint8_t rc)
{
	mqtt_fixed_header(out, MQTT_CONNACK << 4, 2);
	mbuf_u8(out, session_present ? 1 : 0);
	mbuf_u8(out, rc);
}

bool mqtt_topic_valid(const char *t, size_t n)
{
	if (!n)
		return false;
	for (size_t i = 0; i < n; i++)
		if (t[i] == '+' || t[i] == '#' || t[i] == '\0')
			return false;
	return true;
}

bool mqtt_filter_valid(const char *f, size_t n)
{
	if (!n)
		return false;
	for (size_t i = 0; i < n; i++) {
		bool level_start = i == 0 || f[i - 1] == '/';
		bool level_end = i + 1 == n || f[i + 1] == '/';

		if (f[i] == '\0')
			return false;
		if (f[i] == '+' && !(level_start && level_end))
			return false;
		if (f[i] == '#' && !(level_start && i + 1 == n))
			return false;
	}
	return true;
}

bool mqtt_topic_match(const char *filter, const char *topic)
{
	/* wildcards at the first level do not match topics starting with '$' */
	if (topic[0] == '$' && (filter[0] == '+' || filter[0] == '#'))
		return false;

	while (*filter) {
		if (*filter == '#')
			return true;		/* matches the parent level too */
		if (*filter == '+') {
			while (*topic && *topic != '/')
				topic++;
			filter++;
		} else {
			while (*filter && *filter != '/' && *filter == *topic) {
				filter++;
				topic++;
			}
			if (*filter && *filter != '/')
				return false;
			if (*topic && *topic != '/')
				return false;
		}
		/* both at the end of a level */
		if (!*filter)
			return !*topic;
		/* filter at '/' */
		if (!*topic) {
			/* "a/#" matches "a" */
			return filter[0] == '/' && filter[1] == '#' && !filter[2];
		}
		filter++;
		topic++;
	}
	return !*topic;
}
