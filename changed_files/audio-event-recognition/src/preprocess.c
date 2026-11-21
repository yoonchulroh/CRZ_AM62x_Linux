/*
 * Preprocess module
 *
 * Converts raw mic audio from the audio input module into the format YAMNet
 * expects: integer samples are normalized to [-1.0, 1.0] float32, channels
 * are averaged to mono and the result is resampled to 16 kHz. Output goes
 * to the framer module through AER_SOCK_PREPROCESS.
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

/*
 * Rational polyphase resampler: out_rate / in_rate = up / down. A windowed
 * sinc low-pass is designed at in_rate * up and split into 'up' phases.
 */
#define ZERO_CROSSINGS	16	/* sinc lobes on each side of the filter */
#define KAISER_BETA	8.0	/* ~80 dB stopband */
#define CUTOFF_RATIO	0.92	/* passband edge relative to output Nyquist */

typedef struct {
	unsigned in_rate, out_rate;
	unsigned up, down;
	unsigned taps;		/* taps per phase */
	float *coeffs;		/* [up][taps], phase-major, reversed */
	float *history;		/* last taps-1 inputs followed by new input */
	size_t history_len;	/* valid samples in history */
	size_t history_cap;
	unsigned long long pos;	/* next output position in 1/up input units */
} resampler_t;

static volatile int running = 1;

static unsigned gcd(unsigned a, unsigned b)
{
	while (b) {
		unsigned t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static double bessel_i0(double x)
{
	double sum = 1.0, term = 1.0;
	int k;

	for (k = 1; k < 50; k++) {
		term *= (x / (2.0 * k)) * (x / (2.0 * k));
		sum += term;
		if (term < sum * 1e-12)
			break;
	}
	return sum;
}

static void free_resampler(resampler_t *r)
{
	if (!r)
		return;
	free(r->coeffs);
	free(r->history);
	free(r);
}

static resampler_t *create_resampler(unsigned in_rate, unsigned out_rate)
{
	unsigned g = gcd(in_rate, out_rate), p, k, len;
	double fc, center, i0beta;
	resampler_t *r;

	r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->in_rate = in_rate;
	r->out_rate = out_rate;
	r->up = out_rate / g;
	r->down = in_rate / g;

	/* cutoff in cycles per sample of the upsampled signal */
	fc = 0.5 * CUTOFF_RATIO / (r->up > r->down ? r->up : r->down);
	r->taps = (unsigned)ceil(2.0 * ZERO_CROSSINGS / (2.0 * fc) / r->up) + 1;
	len = r->taps * r->up;
	center = (len - 1) / 2.0;
	i0beta = bessel_i0(KAISER_BETA);

	r->coeffs = calloc(len, sizeof(float));
	r->history_cap = r->taps + 2 * AER_AUDIO_MAX_BYTES;
	r->history = calloc(r->history_cap, sizeof(float));
	if (!r->coeffs || !r->history) {
		free_resampler(r);
		return NULL;
	}

	/*
	 * h[j] for j = p + k*up is the weight of input x[i - k] in the phase p
	 * output. Store it as coeffs[p][taps-1-k] so each output is a plain dot
	 * product over consecutive history samples.
	 */
	for (p = 0; p < r->up; p++) {
		for (k = 0; k < r->taps; k++) {
			double n = p + (double)k * r->up, t = n - center, h, w;
			double x = 2.0 * (n - center) / (len - 1);

			h = t == 0.0 ? 2.0 * fc :
			    sin(2.0 * M_PI * fc * t) / (M_PI * t);
			w = fabs(x) <= 1.0 ?
			    bessel_i0(KAISER_BETA * sqrt(1.0 - x * x)) / i0beta :
			    0.0;
			r->coeffs[p * r->taps + (r->taps - 1 - k)] =
				(float)(h * w * r->up);
		}
	}

	/* start with taps-1 zeros of history; skip the filter delay */
	r->history_len = r->taps - 1;
	r->pos = (unsigned long long)(r->taps - 1) * r->up +
		 (unsigned long long)center;
	fprintf(stderr, "resampler %u -> %u Hz: %u/%u, %u taps per phase\n",
		in_rate, out_rate, r->up, r->down, r->taps);
	return r;
}

/*
 * Convert integer samples (sample_count frames of 'channels' interleaved
 * samples) to mono float32 in [-1.0, 1.0] by averaging channels.
 */
void convert_to_mono_f32(const void *samples, size_t sample_count,
			 sample_format_t format, unsigned channels,
			 float *mono_samples)
{
	const uint8_t *p = samples;
	size_t i;
	unsigned c;

	for (i = 0; i < sample_count; i++) {
		float sum = 0.0f;

		for (c = 0; c < channels; c++) {
			int32_t v;

			switch (format) {
			case SAMPLE_S16_LE:
				v = (int16_t)(p[0] | p[1] << 8);
				sum += v / 32768.0f;
				p += 2;
				break;
			case SAMPLE_S24_3LE:
				v = (int32_t)((uint32_t)p[0] << 8 |
					      (uint32_t)p[1] << 16 |
					      (uint32_t)p[2] << 24) >> 8;
				sum += v / 8388608.0f;
				p += 3;
				break;
			default:
				v = (int32_t)((uint32_t)p[0] |
					      (uint32_t)p[1] << 8 |
					      (uint32_t)p[2] << 16 |
					      (uint32_t)p[3] << 24);
				sum += (float)(v / 2147483648.0);
				p += 4;
				break;
			}
		}
		mono_samples[i] = sum / channels;
	}
}

/*
 * Resample sample_count mono samples from the mic rate to the resampler's
 * output rate (16 kHz). Filter state carries over between calls. Returns the
 * number of samples written to preprocessed_samples.
 */
size_t resample(resampler_t *resampler, const float *mono_samples,
		size_t sample_count, float *preprocessed_samples,
		size_t max_preprocessed_samples)
{
	resampler_t *r = resampler;
	size_t out = 0, consumed, keep;

	if (r->history_len + sample_count > r->history_cap)
		sample_count = r->history_cap - r->history_len;
	memcpy(r->history + r->history_len, mono_samples,
	       sample_count * sizeof(float));
	r->history_len += sample_count;

	/* output at position pos needs inputs up to index pos / up */
	while (out < max_preprocessed_samples) {
		size_t i = (size_t)(r->pos / r->up);
		unsigned p = (unsigned)(r->pos % r->up), k;
		const float *x, *h;
		float acc = 0.0f;

		if (i >= r->history_len)
			break;
		x = r->history + i - (r->taps - 1);
		h = r->coeffs + (size_t)p * r->taps;
		for (k = 0; k < r->taps; k++)
			acc += h[k] * x[k];
		preprocessed_samples[out++] = acc;
		r->pos += r->down;
	}

	/* keep only the taps-1 inputs before the next output's newest input */
	consumed = (size_t)(r->pos / r->up);
	if (consumed > r->history_len)
		consumed = r->history_len;
	keep = r->taps - 1;
	if (consumed >= keep) {
		size_t drop = consumed - keep;

		memmove(r->history, r->history + drop,
			(r->history_len - drop) * sizeof(float));
		r->history_len -= drop;
		r->pos -= (unsigned long long)drop * r->up;
	}
	return out;
}

int main(int argc, char **argv)
{
	resampler_t *resampler = NULL;
	app_config_t config;
	msg_audio_t *in;
	msg_pcm_t out;
	float *mono;
	server_t *server;

	setvbuf(stderr, NULL, _IOLBF, 0);
	config_load_from_args(argc, argv, &config);
	install_signal_handlers(&running);

	server = listen_on_socket(AER_SOCK_PREPROCESS);
	in = malloc(sizeof(*in) + AER_AUDIO_MAX_BYTES);
	mono = malloc(AER_AUDIO_MAX_BYTES * sizeof(float));
	if (!server || !in || !mono)
		return 1;
	out.type = MSG_PCM;
	out.rate = YAMNET_SAMPLE_RATE;

	while (running) {
		int fd = connect_to_socket(AER_SOCK_AUDIO);

		while (running) {
			ssize_t n = receive_on_socket(fd, in, sizeof(*in) +
						      AER_AUDIO_MAX_BYTES);
			size_t bytes, produced;

			if (n < 0 && (errno == EINTR || errno == EMSGSIZE))
				continue;
			if (n <= 0)
				break;
			if ((size_t)n < sizeof(*in) || in->type != MSG_AUDIO ||
			    in->channels == 0 || in->rate == 0 ||
			    in->format > SAMPLE_S32_LE)
				continue;
			bytes = (size_t)in->frames * in->channels *
				sample_format_bytes(in->format);
			if (bytes > (size_t)n - sizeof(*in))
				continue;

			/* mic format changed (e.g. a different mic): rebuild */
			if (!resampler || resampler->in_rate != in->rate) {
				free_resampler(resampler);
				resampler = create_resampler(in->rate,
							     YAMNET_SAMPLE_RATE);
				if (!resampler)
					return 1;
			}

			convert_to_mono_f32(in->data, in->frames,
					    (sample_format_t)in->format,
					    in->channels, mono);
			produced = resample(resampler, mono, in->frames,
					    out.samples, AER_PCM_MAX_SAMPLES);
			if (!produced)
				continue;
			out.count = (uint32_t)produced;
			send_to_socket(server, &out,
				       offsetof(msg_pcm_t, samples) +
				       produced * sizeof(float));
		}
		close(fd);
		if (running)
			fprintf(stderr, "audio input disconnected\n");
	}

	free_resampler(resampler);
	close_server(server);
	free(in);
	free(mono);
	return 0;
}
