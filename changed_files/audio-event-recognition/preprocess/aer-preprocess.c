// SPDX-License-Identifier: MIT
/*
 * aer-preprocess - module 2: convert captured audio to the YAMNet input format.
 *
 *   AER_MSG_PCM_S16 (any rate, any channel count, interleaved int16)
 *     -> down-mix to mono
 *     -> int16 to float32, normalised to [-1.0, 1.0]
 *     -> resample to 16 kHz with a polyphase Kaiser-windowed sinc filter
 *        (rational L/M, e.g. 48 kHz: 1/3, 44.1 kHz: 160/441)
 *   AER_MSG_PCM_F32 (16 kHz mono)
 *
 * The filter is rebuilt when the input rate changes and its history is
 * cleared after any discontinuity in the input stream.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aer.h"
#include "conf.h"
#include "ipc.h"

/* Zero crossings of the low-pass sinc kept on each side of the centre and
 * Kaiser beta (~80 dB stop band). */
#define SINC_ZC		24
#define KAISER_BETA	8.0
/* -6 dB point as a fraction of the output Nyquist frequency (7.36 kHz):
 * above YAMNet's highest mel band (7.5 kHz) is irrelevant, below must stay. */
#define CUTOFF		0.92

struct resampler {
	unsigned int in_rate, out_rate;
	int L, M, T;		/* up factor, down factor, taps per phase */
	float *taps;		/* L phases of T taps: taps[p * T + i] = h[p + i * L] */
	float *hist;		/* input samples, hist[0] has absolute index base */
	size_t hlen, hcap;
	int64_t base;		/* absolute input index of hist[0] */
	int64_t in_count;	/* absolute index of the next input sample */
	int64_t n;		/* absolute index of the next output sample */
};

static unsigned int gcd(unsigned int a, unsigned int b)
{
	while (b) {
		unsigned int t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static double bessel_i0(double x)
{
	double sum = 1.0, term = 1.0, q = x * x / 4.0;

	for (int k = 1; k < 64; k++) {
		term *= q / ((double)k * k);
		sum += term;
		if (term < sum * 1e-12)
			break;
	}
	return sum;
}

static void rs_reset(struct resampler *r)
{
	/* T-1 zeros of history so the first output sample can be computed */
	r->hlen = (size_t)r->T - 1;
	memset(r->hist, 0, r->hlen * sizeof(float));
	r->base = -(int64_t)(r->T - 1);
	r->in_count = 0;
	r->n = 0;
}

static int rs_init(struct resampler *r, unsigned int in_rate, unsigned int out_rate)
{
	unsigned int g = gcd(in_rate, out_rate);
	double fc, sum = 0, *h;
	int N;

	free(r->taps);
	free(r->hist);
	memset(r, 0, sizeof(*r));
	r->in_rate = in_rate;
	r->out_rate = out_rate;
	r->L = (int)(out_rate / g);
	r->M = (int)(in_rate / g);
	if (r->L > 4096) {
		aer_log("cannot resample %u Hz to %u Hz (L=%d)", in_rate, out_rate, r->L);
		return -1;
	}

	/* prototype low-pass at the up-sampled rate L * in_rate */
	fc = CUTOFF * 0.5 * (in_rate < out_rate ? in_rate : out_rate) / ((double)r->L * in_rate);
	r->T = (int)ceil(SINC_ZC / fc / r->L);
	if (r->T < 4)
		r->T = 4;
	N = r->T * r->L;

	h = malloc((size_t)N * sizeof(double));
	for (int k = 0; k < N; k++) {
		double t = k - (N - 1) / 2.0;
		double x = 2.0 * fc * t;
		double sinc = fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x);
		double w = 2.0 * k / (N - 1) - 1.0;

		h[k] = 2.0 * fc * sinc * bessel_i0(KAISER_BETA * sqrt(fmax(0.0, 1.0 - w * w))) /
		       bessel_i0(KAISER_BETA);
		sum += h[k];
	}
	/* unity DC gain for every phase on average (zero stuffing loses 1/L) */
	r->taps = malloc((size_t)N * sizeof(float));
	for (int p = 0; p < r->L; p++)
		for (int i = 0; i < r->T; i++)
			r->taps[p * r->T + i] = (float)(h[p + i * r->L] * r->L / sum);
	free(h);

	r->hcap = (size_t)r->T * 2 + 65536;
	r->hist = malloc(r->hcap * sizeof(float));
	rs_reset(r);
	aer_log("resampling %u Hz -> %u Hz (L=%d, M=%d, %d taps/phase)",
		in_rate, out_rate, r->L, r->M, r->T);
	return 0;
}

/*
 * Feeds `count` mono samples, writes up to `cap` output samples to `out` and
 * returns how many were written. out[0] is output sample number r->n as it
 * was before the call.
 */
static size_t rs_process(struct resampler *r, const float *in, size_t count,
			 float *out, size_t cap)
{
	size_t produced = 0;
	int64_t keep_from;
	size_t drop;

	if (r->hlen + count > r->hcap) {
		r->hcap = r->hlen + count + 4096;
		r->hist = realloc(r->hist, r->hcap * sizeof(float));
	}
	memcpy(r->hist + r->hlen, in, count * sizeof(float));
	r->hlen += count;
	r->in_count += (int64_t)count;

	for (;;) {
		int64_t u = r->n * r->M;
		int64_t j0 = u / r->L;		/* newest input sample used */
		int p = (int)(u % r->L);
		const float *tp = r->taps + (size_t)p * r->T;
		const float *x;
		float acc = 0.0f;

		if (j0 >= r->in_count || produced == cap)
			break;
		x = r->hist + (j0 - r->base);	/* x[-i] is input sample j0 - i */
		for (int i = 0; i < r->T; i++)
			acc += tp[i] * x[-i];
		out[produced++] = acc;
		r->n++;
	}

	/* drop history the next output sample no longer needs */
	keep_from = (r->n * r->M) / r->L - (r->T - 1);
	if (keep_from > r->base) {
		drop = (size_t)(keep_from - r->base);
		if (drop > r->hlen)
			drop = r->hlen;
		memmove(r->hist, r->hist + drop, (r->hlen - drop) * sizeof(float));
		r->hlen -= drop;
		r->base += (int64_t)drop;
	}
	return produced;
}

int main(int argc, char **argv)
{
	static uint8_t msg[AER_MAX_MSG];
	struct resampler rs = { 0 };
	struct aer_client in;
	struct aer_server out;
	struct aer_conf *conf;
	const char *conf_path;
	float gain, *mono = NULL, *res = NULL;
	size_t mono_cap = 0, res_cap = 0;
	long channel;

	conf_path = aer_parse_common(argc, argv, NULL, NULL, NULL);
	conf = aer_conf_load(conf_path);
	aer_signals();

	gain = (float)aer_conf_double(conf, "preprocess", "gain", 1.0);
	/* -1: average all channels, otherwise use only that channel */
	channel = aer_conf_int(conf, "preprocess", "channel", -1);

	aer_client_init(&in, aer_run_dir(conf), "capture");
	if (aer_server_open(&out, aer_run_dir(conf), "preprocess") < 0)
		return 1;

	while (!aer_quit) {
		const struct aer_hdr *h = (const struct aer_hdr *)msg;
		const int16_t *pcm = (const int16_t *)(h + 1);
		struct aer_hdr oh = { .type = AER_MSG_PCM_F32, .channels = 1,
				      .rate = AER_MODEL_RATE };
		const float scale = gain / 32768.0f;
		int64_t in_index;
		size_t n;

		if (aer_link_wait(&in, &out, msg, sizeof(msg), AER_MSG_PCM_S16, 1000) <= 0)
			continue;

		if (h->rate != rs.in_rate) {
			if (rs_init(&rs, h->rate, AER_MODEL_RATE) < 0) {
				rs.in_rate = 0;
				continue;
			}
		} else if (in.discont) {
			aer_dbg("input discontinuity, resetting filter");
			rs_reset(&rs);
		}
		in.discont = false;

		if (h->count > mono_cap) {
			mono_cap = h->count;
			mono = realloc(mono, mono_cap * sizeof(float));
		}
		for (uint32_t f = 0; f < h->count; f++) {
			const int16_t *fr = pcm + (size_t)f * h->channels;
			int32_t acc = 0;

			if (channel >= 0 && channel < h->channels) {
				acc = fr[channel];
				mono[f] = (float)acc * scale;
			} else {
				for (unsigned int c = 0; c < h->channels; c++)
					acc += fr[c];
				mono[f] = (float)acc * scale / (float)h->channels;
			}
		}

		/* worst case output count for this chunk, plus slack */
		n = (size_t)h->count * (size_t)rs.L / (size_t)rs.M + 2;
		if (n > res_cap) {
			res_cap = n;
			res = realloc(res, res_cap * sizeof(float));
		}
		in_index = rs.in_count;
		oh.t_ns = h->t_ns + (int64_t)(((double)rs.n * rs.M / rs.L - (double)in_index) *
					      1e9 / rs.in_rate);
		n = rs_process(&rs, mono, h->count, res, res_cap);
		if (!n)
			continue;
		for (size_t i = 0; i < n; i++) {
			if (res[i] > 1.0f)
				res[i] = 1.0f;
			else if (res[i] < -1.0f)
				res[i] = -1.0f;
		}
		oh.count = (uint32_t)n;
		aer_server_send(&out, &oh, res, n * sizeof(float));
	}

	aer_server_close(&out);
	aer_client_close(&in);
	aer_conf_free(conf);
	free(mono);
	free(res);
	free(rs.taps);
	free(rs.hist);
	return 0;
}
