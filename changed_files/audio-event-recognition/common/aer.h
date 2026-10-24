// SPDX-License-Identifier: MIT
/*
 * Audio event recognition (AER) - definitions shared by every module.
 *
 * Pipeline (one process per box, linked by AF_UNIX SOCK_SEQPACKET sockets):
 *
 *   aer-capture -> aer-preprocess -> aer-framer -> aer-infer -> aer-detect
 *                                                                 |-> aer-display
 *                                                                 '-> aer-notify -> aer-broker (MQTT)
 *
 * Every module that produces data listens on <run_dir>/<name>/<name>.sock and
 * broadcasts one message per packet to whoever is connected. Consumers connect
 * to their upstream socket and reconnect whenever it goes away, so the modules
 * can be started, stopped and restarted in any order.
 */
#ifndef AER_H
#define AER_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AER_DEFAULT_CONF	"/etc/aer/aer.conf"
#define AER_DEFAULT_RUN_DIR	"/run/aer"

/* Model input format (YAMNet) */
#define AER_MODEL_RATE		16000

#define AER_MAGIC		0x31524541u	/* "AER1" little endian */
#define AER_MAX_MSG		(128 * 1024)
#define AER_NAME_LEN		64
#define AER_EVENT_NAME_LEN	32
#define AER_MAX_EVENTS		16

enum aer_msg_type {
	AER_MSG_PCM_S16 = 1,	/* capture    -> preprocess: interleaved int16 */
	AER_MSG_PCM_F32 = 2,	/* preprocess -> framer: mono float32 in [-1, 1] */
	AER_MSG_FRAME   = 3,	/* framer     -> infer: one model input window */
	AER_MSG_SCORES  = 4,	/* infer      -> detect: struct aer_scores + float[count] */
	AER_MSG_STATUS  = 5,	/* detect     -> outputs: struct aer_status, every frame */
	AER_MSG_EVENT   = 6,	/* detect     -> outputs: struct aer_event, on detection */
};

struct aer_hdr {
	uint32_t magic;
	uint16_t type;		/* enum aer_msg_type */
	uint16_t channels;	/* PCM: interleaved channels, otherwise 1 */
	uint32_t seq;		/* per-producer message counter, +1 per message */
	uint32_t rate;		/* PCM/FRAME: sample rate in Hz */
	uint32_t count;		/* elements in the payload (frames, samples, scores) */
	uint32_t reserved;
	int64_t t_ns;		/* CLOCK_REALTIME of the first sample covered */
};

struct aer_scores {
	float level_dbfs;	/* RMS level of the analysed frame */
	float infer_ms;		/* time spent in the interpreter */
	float score[];		/* hdr.count class scores */
};

struct aer_status {
	float level_dbfs;
	float infer_ms;
	int32_t top_class;
	float top_score;
	char top_name[AER_NAME_LEN];
	uint32_t n_events;
	struct {
		char name[AER_EVENT_NAME_LEN];
		float score;
		float threshold;
	} ev[AER_MAX_EVENTS];
};

struct aer_event {
	char type[AER_EVENT_NAME_LEN];	/* event type, e.g. "siren" */
	char cls[AER_NAME_LEN];		/* strongest YAMNet class of the group */
	float score;
	float threshold;
};

/* ------------------------------------------------------------------ util.c */

extern const char *aer_prog;
extern int aer_verbose;
extern volatile sig_atomic_t aer_quit;

void aer_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void aer_dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Installs SIGINT/SIGTERM handlers that set aer_quit, ignores SIGPIPE. */
void aer_signals(void);

int64_t aer_now_ns(void);		/* CLOCK_REALTIME */
int64_t aer_mono_ms(void);		/* CLOCK_MONOTONIC */

/* "2026-10-17T13:20:01.123+09:00" */
void aer_format_time(int64_t t_ns, char *buf, size_t len);

/* Copies with truncation and always terminates. */
void aer_strlcpy(char *dst, const char *src, size_t len);

/* Common command line: -c <conf> -v -h. Returns the config path. */
const char *aer_parse_common(int argc, char **argv, const char *usage_extra,
			     const char *extra_opts, int (*extra)(int opt, const char *arg));

/* Run directory: $AER_RUN_DIR, else [general] run_dir, else /run/aer */
struct aer_conf;
const char *aer_run_dir(const struct aer_conf *conf);

/* Device name used in MQTT messages and on the display: [general]
 * device_name, or the hostname when that is empty. */
void aer_device_name(const struct aer_conf *conf, char *buf, size_t len);

#endif
