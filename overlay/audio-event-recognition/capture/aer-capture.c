// SPDX-License-Identifier: MIT
/*
 * aer-capture - module 1: capture audio from the USB microphone with ALSA.
 *
 * Reads interleaved S16_LE frames in the microphone's native format (the
 * COMS CM420 only offers stereo at 44.1/48 kHz) and publishes them unchanged
 * as AER_MSG_PCM_S16 on the "capture" socket. Format conversion is left to
 * aer-preprocess.
 *
 * The device is reopened whenever it disappears (USB unplug, boot ordering),
 * so the service can start before the microphone is enumerated.
 *
 * -w FILE replays a 16-bit PCM WAV file in real time instead of the
 * microphone, so the whole pipeline can be checked with a known input.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "aer.h"
#include "conf.h"
#include "ipc.h"

#define REOPEN_MS	2000

static const char *wav_path;
static bool wav_loop;
/* set while waiting for an absent device: its retries would repeat the same
 * ALSA error every REOPEN_MS */
static bool alsa_quiet;

struct capture_cfg {
	const char *device;
	unsigned int rate;
	unsigned int channels;
	unsigned int period_ms;
	unsigned int periods;
};

static int extra_opt(int opt, const char *arg)
{
	switch (opt) {
	case 'w':
		wav_path = arg;
		return 0;
	case 'l':
		wav_loop = true;
		return 0;
	}
	return -1;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------- ALSA */

static void alsa_log(int prio, int interface, const char *file, int line, const char *function,
		     int errcode, const char *fmt, va_list arg)
{
	char buf[256];

	(void)interface; (void)file; (void)line; (void)function; (void)errcode;
	if (prio > SND_LOG_WARN || (alsa_quiet && !aer_verbose))
		return;
	vsnprintf(buf, sizeof(buf), fmt, arg);
	aer_log("alsa: %s", buf);
}

static snd_pcm_t *pcm_open(const struct capture_cfg *cfg, unsigned int *rate,
			   snd_pcm_uframes_t *period)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t buffer;
	snd_pcm_t *pcm;
	unsigned int r = cfg->rate;
	int err;

	err = snd_pcm_open(&pcm, cfg->device, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0)
		return NULL;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, cfg->channels)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, NULL)) < 0) {
		aer_log("%s: unsupported format S16_LE/%u ch/%u Hz: %s",
			cfg->device, cfg->channels, cfg->rate, snd_strerror(err));
		goto fail;
	}
	*period = (snd_pcm_uframes_t)r * cfg->period_ms / 1000;
	buffer = *period * cfg->periods;
	snd_pcm_hw_params_set_period_size_near(pcm, hw, period, NULL);
	snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
	if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
		aer_log("%s: hw_params: %s", cfg->device, snd_strerror(err));
		goto fail;
	}
	snd_pcm_hw_params_get_period_size(hw, period, NULL);
	snd_pcm_hw_params_get_buffer_size(hw, &buffer);
	if ((err = snd_pcm_prepare(pcm)) < 0 || (err = snd_pcm_start(pcm)) < 0) {
		aer_log("%s: start: %s", cfg->device, snd_strerror(err));
		goto fail;
	}
	*rate = r;
	aer_log("capturing from %s: S16_LE, %u ch, %u Hz, period %lu, buffer %lu frames",
		cfg->device, cfg->channels, r, (unsigned long)*period, (unsigned long)buffer);
	return pcm;
fail:
	snd_pcm_close(pcm);
	return NULL;
}

static int run_alsa(const struct capture_cfg *cfg, struct aer_server *srv)
{
	snd_pcm_uframes_t period = 0;
	snd_pcm_t *pcm = NULL;
	unsigned int rate = 0;
	int16_t *buf = NULL;
	bool warned = false;

	while (!aer_quit) {
		struct aer_hdr h = { .type = AER_MSG_PCM_S16 };
		snd_pcm_sframes_t n, avail;
		int64_t now;

		aer_server_accept(srv);

		if (!pcm) {
			pcm = pcm_open(cfg, &rate, &period);
			if (!pcm) {
				if (!warned)
					aer_log("waiting for capture device %s", cfg->device);
				warned = true;
				alsa_quiet = true;
				sleep_ms(REOPEN_MS);
				continue;
			}
			warned = false;
			alsa_quiet = false;
			free(buf);
			buf = malloc(period * cfg->channels * sizeof(int16_t));
		}

		n = snd_pcm_readi(pcm, buf, period);
		if (n < 0) {
			if (n == -EPIPE) {
				aer_log("overrun, restarting capture");
			} else if (n == -ESTRPIPE) {
				while (snd_pcm_resume(pcm) == -EAGAIN)
					sleep_ms(100);
			} else if (n == -EINTR || n == -EAGAIN) {
				continue;
			} else {
				aer_log("capture error: %s, reopening", snd_strerror((int)n));
				snd_pcm_close(pcm);
				pcm = NULL;
				continue;
			}
			if (snd_pcm_prepare(pcm) < 0 || snd_pcm_start(pcm) < 0) {
				snd_pcm_close(pcm);
				pcm = NULL;
			}
			continue;
		}
		if (n == 0)
			continue;

		/* time of the first frame: the frames just read plus what is still queued */
		now = aer_now_ns();
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0)
			avail = 0;
		h.t_ns = now - (int64_t)(n + avail) * 1000000000 / rate;
		h.rate = rate;
		h.channels = cfg->channels;
		h.count = (uint32_t)n;
		aer_server_send(srv, &h, buf, (size_t)n * cfg->channels * sizeof(int16_t));
	}
	if (pcm)
		snd_pcm_close(pcm);
	free(buf);
	return 0;
}

/* -------------------------------------------------------------------- WAV */

struct wav {
	FILE *f;
	unsigned int rate, channels;
	long data_off;
	uint32_t data_len;
};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

static int wav_open(struct wav *w, const char *path)
{
	uint8_t hdr[12], ch[8], fmt[40];
	bool have_fmt = false;

	memset(w, 0, sizeof(*w));
	w->f = fopen(path, "rb");
	if (!w->f) {
		aer_log("%s: %s", path, strerror(errno));
		return -1;
	}
	if (fread(hdr, 1, 12, w->f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
		goto bad;
	while (fread(ch, 1, 8, w->f) == 8) {
		uint32_t len = le32(ch + 4);

		if (!memcmp(ch, "fmt ", 4)) {
			uint16_t format, bits;

			if (len < 16 || len > sizeof(fmt) || fread(fmt, 1, len, w->f) != len)
				goto bad;
			format = le16(fmt);
			w->channels = le16(fmt + 2);
			w->rate = le32(fmt + 4);
			bits = le16(fmt + 14);
			/* WAVE_FORMAT_EXTENSIBLE: subformat GUID starts with the format tag */
			if (format == 0xfffe && len >= 26)
				format = le16(fmt + 24);
			if (format != 1 || bits != 16 || !w->channels || w->channels > 8 || !w->rate) {
				aer_log("%s: only 16-bit PCM WAV is supported", path);
				goto fail;
			}
			have_fmt = true;
			if (len & 1)
				fseek(w->f, 1, SEEK_CUR);
		} else if (!memcmp(ch, "data", 4)) {
			if (!have_fmt)
				goto bad;
			w->data_off = ftell(w->f);
			w->data_len = len;
			return 0;
		} else {
			fseek(w->f, len + (len & 1), SEEK_CUR);
		}
	}
bad:
	aer_log("%s: not a valid WAV file", path);
fail:
	fclose(w->f);
	w->f = NULL;
	return -1;
}

static void add_ns(struct timespec *ts, int64_t ns)
{
	ns += ts->tv_nsec;
	ts->tv_sec += ns / 1000000000;
	ts->tv_nsec = ns % 1000000000;
}

static int run_wav(const struct capture_cfg *cfg, struct aer_server *srv)
{
	struct wav w;
	size_t period, frame_bytes;
	uint64_t sent = 0;
	int64_t t0;
	struct timespec next;
	int16_t *buf;
	/* one second of trailing silence so the last frame gets analysed */
	size_t tail_left;

	if (wav_open(&w, wav_path) < 0)
		return 1;
	period = (size_t)w.rate * cfg->period_ms / 1000;
	frame_bytes = w.channels * sizeof(int16_t);
	buf = calloc(period, frame_bytes);
	tail_left = w.rate;
	aer_log("replaying %s: %u ch, %u Hz, %.1f s%s", wav_path, w.channels, w.rate,
		(double)w.data_len / frame_bytes / w.rate, wav_loop ? ", looping" : "");

	/* give the downstream modules a moment to connect */
	for (int i = 0; i < 20 && !aer_quit; i++) {
		aer_server_accept(srv);
		sleep_ms(100);
	}

	fseek(w.f, w.data_off, SEEK_SET);
	t0 = aer_now_ns();
	clock_gettime(CLOCK_MONOTONIC, &next);
	while (!aer_quit) {
		struct aer_hdr h = { .type = AER_MSG_PCM_S16 };
		uint64_t pos = (uint64_t)(ftell(w.f) - w.data_off);
		size_t want = period, got = 0;

		aer_server_accept(srv);
		if (pos < w.data_len) {
			if (want > (w.data_len - pos) / frame_bytes)
				want = (w.data_len - pos) / frame_bytes;
			got = fread(buf, frame_bytes, want, w.f);
		}
		if (got == 0) {
			if (wav_loop) {
				fseek(w.f, w.data_off, SEEK_SET);
				continue;
			}
			if (!tail_left)
				break;
			got = tail_left < period ? tail_left : period;
			memset(buf, 0, got * frame_bytes);
			tail_left -= got;
		}

		h.rate = w.rate;
		h.channels = w.channels;
		h.count = (uint32_t)got;
		h.t_ns = t0 + (int64_t)(sent * 1000000000 / w.rate);
		sent += got;

		/* real-time pacing */
		add_ns(&next, (int64_t)got * 1000000000 / w.rate);
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		aer_server_send(srv, &h, buf, got * frame_bytes);
	}
	aer_log("end of %s", wav_path);
	/* let the downstream modules drain before the socket disappears */
	for (int i = 0; i < 30 && !aer_quit; i++)
		sleep_ms(100);
	fclose(w.f);
	free(buf);
	return 0;
}

int main(int argc, char **argv)
{
	struct capture_cfg cfg;
	struct aer_server srv;
	struct aer_conf *conf;
	const char *conf_path;
	int ret;

	conf_path = aer_parse_common(argc, argv, " [-w file.wav [-l]]\n"
				     "  -w FILE  replay a 16-bit PCM WAV file instead of the microphone\n"
				     "  -l       loop the WAV file", "w:l", extra_opt);
	conf = aer_conf_load(conf_path);
	aer_signals();

	cfg.device = aer_conf_get(conf, "capture", "device", "hw:CM420,0");
	cfg.rate = (unsigned int)aer_conf_int(conf, "capture", "rate", 48000);
	cfg.channels = (unsigned int)aer_conf_int(conf, "capture", "channels", 2);
	cfg.period_ms = (unsigned int)aer_conf_int(conf, "capture", "period_ms", 50);
	cfg.periods = (unsigned int)aer_conf_int(conf, "capture", "buffer_periods", 8);
	if (cfg.channels < 1 || cfg.channels > 8 || cfg.period_ms < 5 || cfg.period_ms > 500 ||
	    cfg.periods < 2) {
		aer_log("invalid [capture] settings");
		return 1;
	}

	if (aer_server_open(&srv, aer_run_dir(conf), "capture") < 0)
		return 1;
	snd_lib_log_set_handler(alsa_log);
	ret = wav_path ? run_wav(&cfg, &srv) : run_alsa(&cfg, &srv);
	aer_server_close(&srv);
	aer_conf_free(conf);
	return ret;
}
