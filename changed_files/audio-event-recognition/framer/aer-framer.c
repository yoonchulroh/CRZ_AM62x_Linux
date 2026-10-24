// SPDX-License-Identifier: MIT
/*
 * aer-framer - module 3: cut the 16 kHz stream into overlapping model windows.
 *
 *   AER_MSG_PCM_F32 (16 kHz mono, arbitrary chunk sizes)
 *   AER_MSG_FRAME   (frame_samples long, one every hop_samples)
 *
 * Defaults: 15600 samples (0.975 s, the fixed input of the YAMNet TFLite
 * model: 96 x 10 ms hops plus one 25 ms STFT window) every 7680 samples
 * (0.48 s), i.e. about 50 % overlap.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aer.h"
#include "conf.h"
#include "ipc.h"

int main(int argc, char **argv)
{
	static uint8_t msg[AER_MAX_MSG];
	struct aer_client in;
	struct aer_server out;
	struct aer_conf *conf;
	const char *conf_path;
	long frame_len, hop;
	float *buf;
	size_t len = 0, cap;
	int64_t buf_t = 0;	/* CLOCK_REALTIME of buf[0] */

	conf_path = aer_parse_common(argc, argv, NULL, NULL, NULL);
	conf = aer_conf_load(conf_path);
	aer_signals();

	frame_len = aer_conf_int(conf, "framer", "frame_samples", 15600);
	hop = aer_conf_int(conf, "framer", "hop_samples", 7680);
	if (frame_len < 1 || hop < 1 ||
	    (size_t)frame_len * sizeof(float) + sizeof(struct aer_hdr) > AER_MAX_MSG) {
		aer_log("invalid [framer] frame_samples/hop_samples");
		return 1;
	}
	aer_log("frames of %ld samples (%.3f s) every %ld samples (%.3f s)",
		frame_len, (double)frame_len / AER_MODEL_RATE, hop, (double)hop / AER_MODEL_RATE);

	cap = (size_t)frame_len + AER_MODEL_RATE;
	buf = malloc(cap * sizeof(float));

	aer_client_init(&in, aer_run_dir(conf), "preprocess");
	if (aer_server_open(&out, aer_run_dir(conf), "framer") < 0)
		return 1;

	while (!aer_quit) {
		const struct aer_hdr *h = (const struct aer_hdr *)msg;
		const float *pcm = (const float *)(h + 1);

		if (aer_link_wait(&in, &out, msg, sizeof(msg), AER_MSG_PCM_F32, 1000) <= 0)
			continue;
		if (h->rate != AER_MODEL_RATE || h->channels != 1) {
			aer_log("expected 16 kHz mono input, got %u Hz %u ch", h->rate, h->channels);
			continue;
		}
		if (in.discont) {
			if (len)
				aer_dbg("input discontinuity, dropping %zu buffered samples", len);
			len = 0;
			in.discont = false;
		}

		if (len + h->count > cap) {
			cap = len + h->count + AER_MODEL_RATE;
			buf = realloc(buf, cap * sizeof(float));
		}
		memcpy(buf + len, pcm, h->count * sizeof(float));
		/* re-anchor the buffer start on the newest upstream timestamp */
		buf_t = h->t_ns - (int64_t)len * 1000000000 / AER_MODEL_RATE;
		len += h->count;

		while (len >= (size_t)frame_len) {
			struct aer_hdr oh = {
				.type = AER_MSG_FRAME, .channels = 1, .rate = AER_MODEL_RATE,
				.count = (uint32_t)frame_len, .t_ns = buf_t,
			};
			size_t adv = (size_t)hop < len ? (size_t)hop : len;

			aer_server_send(&out, &oh, buf, (size_t)frame_len * sizeof(float));
			aer_dbg("frame %u", oh.seq);
			memmove(buf, buf + adv, (len - adv) * sizeof(float));
			len -= adv;
			buf_t += (int64_t)adv * 1000000000 / AER_MODEL_RATE;
		}
	}

	aer_server_close(&out);
	aer_client_close(&in);
	aer_conf_free(conf);
	free(buf);
	return 0;
}
