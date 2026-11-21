/*
 * Framer module
 *
 * Collects 16 kHz mono samples from the preprocess module and cuts them into
 * YAMNet input frames of 0.975 s (15,600 samples). A new frame starts every
 * 0.48 s (7,680 samples), so neighbouring frames overlap by about half and a
 * sound shorter than 0.48 s always lies entirely inside some frame.
 * Each frame carries its RMS input level and goes to the inference module
 * through AER_SOCK_FRAMER.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/config.h"
#include "common/protocol.h"
#include "common/socket.h"

#define LEVEL_FLOOR_DBFS	-120.0f

typedef struct {
	float *buffer;		/* samples not yet dropped by a hop */
	size_t count;		/* valid samples in buffer */
	size_t capacity;
	size_t frame_size;
	size_t hop_size;
} framer_t;

static volatile int running = 1;

static framer_t *create_framer(size_t frame_size, size_t hop_size)
{
	framer_t *framer = calloc(1, sizeof(*framer));

	if (!framer)
		return NULL;
	framer->frame_size = frame_size;
	framer->hop_size = hop_size;
	framer->capacity = frame_size + AER_PCM_MAX_SAMPLES;
	framer->buffer = calloc(framer->capacity, sizeof(float));
	if (!framer->buffer) {
		free(framer);
		return NULL;
	}
	return framer;
}

/* Append preprocessed samples to the framer's buffer. */
void add_samples_to_buffer(framer_t *framer, const float *preprocessed_samples,
			   size_t preprocessed_sample_count)
{
	size_t room = framer->capacity - framer->count;

	/* only when frames are not taken out; keep the newest samples */
	if (preprocessed_sample_count > room) {
		size_t drop = preprocessed_sample_count - room;

		if (drop > framer->count)
			drop = framer->count;
		memmove(framer->buffer, framer->buffer + drop,
			(framer->count - drop) * sizeof(float));
		framer->count -= drop;
		room = framer->capacity - framer->count;
		if (preprocessed_sample_count > room) {
			preprocessed_samples += preprocessed_sample_count - room;
			preprocessed_sample_count = room;
		}
	}
	memcpy(framer->buffer + framer->count, preprocessed_samples,
	       preprocessed_sample_count * sizeof(float));
	framer->count += preprocessed_sample_count;
}

/*
 * If a whole frame is buffered, copy it to frame, advance by one hop and
 * return 1. Otherwise return 0.
 */
int get_frame(framer_t *framer, float *frame)
{
	if (framer->count < framer->frame_size)
		return 0;
	memcpy(frame, framer->buffer, framer->frame_size * sizeof(float));
	memmove(framer->buffer, framer->buffer + framer->hop_size,
		(framer->count - framer->hop_size) * sizeof(float));
	framer->count -= framer->hop_size;
	return 1;
}

static float level_dbfs(const float *frame, size_t n)
{
	double sum = 0.0;
	float db;
	size_t i;

	for (i = 0; i < n; i++)
		sum += (double)frame[i] * frame[i];
	db = (float)(10.0 * log10(sum / n + 1e-30));
	return db < LEVEL_FLOOR_DBFS ? LEVEL_FLOOR_DBFS : db;
}

int main(int argc, char **argv)
{
	app_config_t config;
	framer_t *framer;
	msg_frame_t *frame;
	msg_pcm_t *in;
	server_t *server;
	uint32_t seq = 0;

	setvbuf(stderr, NULL, _IOLBF, 0);
	config_load_from_args(argc, argv, &config);
	install_signal_handlers(&running);

	server = listen_on_socket(AER_SOCK_FRAMER);
	framer = create_framer(YAMNET_FRAME_SAMPLES, YAMNET_HOP_SAMPLES);
	in = malloc(sizeof(*in));
	frame = malloc(sizeof(*frame));
	if (!server || !framer || !in || !frame)
		return 1;
	frame->type = MSG_FRAME;

	while (running) {
		int fd = connect_to_socket(AER_SOCK_PREPROCESS);

		/* a gap in the audio: do not join old and new samples */
		framer->count = 0;
		while (running) {
			ssize_t n = receive_on_socket(fd, in, sizeof(*in));

			if (n < 0 && (errno == EINTR || errno == EMSGSIZE))
				continue;
			if (n <= 0)
				break;
			if ((size_t)n < offsetof(msg_pcm_t, samples) ||
			    in->type != MSG_PCM ||
			    in->count > AER_PCM_MAX_SAMPLES ||
			    (size_t)n < offsetof(msg_pcm_t, samples) +
					in->count * sizeof(float))
				continue;

			add_samples_to_buffer(framer, in->samples, in->count);
			while (get_frame(framer, frame->samples)) {
				frame->seq = ++seq;
				frame->time_ms = now_ms();
				frame->level_dbfs = level_dbfs(frame->samples,
							YAMNET_FRAME_SAMPLES);
				send_to_socket(server, frame, sizeof(*frame));
			}
		}
		close(fd);
		if (running)
			fprintf(stderr, "preprocess disconnected\n");
	}

	close_server(server);
	free(framer->buffer);
	free(framer);
	free(in);
	free(frame);
	return 0;
}
