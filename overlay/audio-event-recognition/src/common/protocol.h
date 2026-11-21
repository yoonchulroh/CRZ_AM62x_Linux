/*
 * Messages exchanged between audio event recognition modules.
 *
 * audio_input -> preprocess  : msg_audio_t      raw mic samples
 * preprocess  -> framer      : msg_pcm_t        16 kHz mono float32
 * framer      -> inference   : msg_frame_t      15,600 samples (0.975 s)
 * inference   -> detector    : msg_scores_t     521 class scores
 * detector    -> display     : msg_result_t     every frame
 *             -> notify      : msg_event_t      only when an event fires
 *
 * Every message starts with a uint32_t type so a reader can check it.
 * All modules run on the same board, so structs are sent as-is.
 */
#ifndef AER_PROTOCOL_H
#define AER_PROTOCOL_H

#include <stdint.h>
#include <time.h>

#include "config.h"

/* Wall-clock time in ms since 1970-01-01 UTC */
static inline int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define YAMNET_SAMPLE_RATE	16000
#define YAMNET_FRAME_SAMPLES	15600	/* 0.975 s */
#define YAMNET_HOP_SAMPLES	7680	/* 0.48 s */
#define AER_TOP_N		5

enum {
	MSG_AUDIO = 0x41455201,
	MSG_PCM,
	MSG_FRAME,
	MSG_SCORES,
	MSG_RESULT,
	MSG_EVENT,
};

/* Integer sample layout delivered by the mic */
typedef enum {
	SAMPLE_S16_LE,
	SAMPLE_S24_3LE,		/* 24-bit packed in 3 bytes */
	SAMPLE_S32_LE,
} sample_format_t;

static inline unsigned sample_format_bytes(sample_format_t f)
{
	return f == SAMPLE_S16_LE ? 2 : f == SAMPLE_S24_3LE ? 3 : 4;
}

/* A detected event */
typedef struct {
	uint32_t event_id;		/* +1 per event, starting at 1 */
	int32_t class_index;
	char class_name[AER_NAME_MAX];	/* name from [watched_classes] */
	float score;
	float threshold;
	int64_t time_ms;		/* ms since 1970-01-01 UTC */
} event_t;

/* Top-N classes of one frame, plus the frame's input level */
typedef struct {
	int32_t n;
	int32_t class_index[AER_TOP_N];
	float score[AER_TOP_N];
	float level_dbfs;
	int64_t time_ms;
} frame_result_t;

/* Header followed by frames * channels samples of 'format' */
#define AER_AUDIO_MAX_BYTES	(4096 * 8 * 4)
typedef struct {
	uint32_t type;			/* MSG_AUDIO */
	uint32_t format;		/* sample_format_t */
	uint32_t rate;
	uint32_t channels;
	uint32_t frames;
	uint8_t data[];
} msg_audio_t;

#define AER_PCM_MAX_SAMPLES	8192
typedef struct {
	uint32_t type;			/* MSG_PCM */
	uint32_t rate;			/* YAMNET_SAMPLE_RATE */
	uint32_t count;
	float samples[AER_PCM_MAX_SAMPLES];
} msg_pcm_t;

typedef struct {
	uint32_t type;			/* MSG_FRAME */
	uint32_t seq;
	int64_t time_ms;		/* when the frame was completed */
	float level_dbfs;		/* RMS level of the frame */
	float samples[YAMNET_FRAME_SAMPLES];
} msg_frame_t;

typedef struct {
	uint32_t type;			/* MSG_SCORES */
	uint32_t seq;
	int64_t time_ms;
	float level_dbfs;
	float scores[AER_NUM_CLASSES];
} msg_scores_t;

typedef struct {
	uint32_t type;			/* MSG_RESULT */
	uint32_t seq;
	frame_result_t top_n;
	float scores[AER_NUM_CLASSES];
} msg_result_t;

typedef struct {
	uint32_t type;			/* MSG_EVENT */
	event_t event;
} msg_event_t;

#endif
