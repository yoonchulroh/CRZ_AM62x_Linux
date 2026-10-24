// SPDX-License-Identifier: MIT
/*
 * MQTT 3.1.1 packet encoding/decoding shared by aer-broker and aer-notify.
 * Only what the two programs need: no MQTT 5 properties, no persistence.
 */
#ifndef AER_MQTT_H
#define AER_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum mqtt_type {
	MQTT_CONNECT = 1,
	MQTT_CONNACK,
	MQTT_PUBLISH,
	MQTT_PUBACK,
	MQTT_PUBREC,
	MQTT_PUBREL,
	MQTT_PUBCOMP,
	MQTT_SUBSCRIBE,
	MQTT_SUBACK,
	MQTT_UNSUBSCRIBE,
	MQTT_UNSUBACK,
	MQTT_PINGREQ,
	MQTT_PINGRESP,
	MQTT_DISCONNECT,
};

#define MQTT_MAX_PACKET		(256 * 1024)

/* growable byte buffer */
struct mbuf {
	uint8_t *d;
	size_t len, cap;
};

void mbuf_put(struct mbuf *b, const void *p, size_t n);
void mbuf_u8(struct mbuf *b, uint8_t v);
void mbuf_u16(struct mbuf *b, uint16_t v);
void mbuf_str(struct mbuf *b, const char *s, size_t n);	/* u16 length + bytes */
void mbuf_consume(struct mbuf *b, size_t n);
void mbuf_free(struct mbuf *b);

/* bounds-checked reader over a packet body */
struct mrd {
	const uint8_t *p;
	size_t len, pos;
	bool err;
};

uint8_t mrd_u8(struct mrd *r);
uint16_t mrd_u16(struct mrd *r);
/* u16 length prefixed string; returns a pointer into the packet (not terminated) */
const char *mrd_str(struct mrd *r, size_t *n);
size_t mrd_left(const struct mrd *r);

/*
 * Looks for a complete packet at the start of buf. Returns its total length
 * and sets *hdr_len to the fixed header size, 0 if more bytes are needed, or
 * -1 if the fixed header is malformed or the packet exceeds max_len.
 */
ssize_t mqtt_frame(const uint8_t *buf, size_t len, size_t max_len, size_t *hdr_len);

/* Appends a fixed header for a packet of rem_len body bytes. */
void mqtt_fixed_header(struct mbuf *out, uint8_t type_flags, size_t rem_len);

void mqtt_connect(struct mbuf *out, const char *client_id, uint16_t keepalive);
void mqtt_publish(struct mbuf *out, const char *topic, size_t topic_len,
		  const void *payload, size_t len, int qos, bool retain, bool dup,
		  uint16_t packet_id);
/* two byte packets: PINGREQ, PINGRESP, DISCONNECT */
void mqtt_simple(struct mbuf *out, enum mqtt_type type);
/* packet id only: PUBACK, PUBREC, PUBREL, PUBCOMP, UNSUBACK */
void mqtt_ack(struct mbuf *out, enum mqtt_type type, uint16_t packet_id);
void mqtt_connack(struct mbuf *out, bool session_present, uint8_t rc);

/* topic name: non-empty, no wildcards */
bool mqtt_topic_valid(const char *t, size_t n);
/* topic filter: '+' fills a whole level, '#' only as the whole last level */
bool mqtt_filter_valid(const char *f, size_t n);
bool mqtt_topic_match(const char *filter, const char *topic);

#endif
