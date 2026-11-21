/*
 * Audio input module
 *
 * Finds the USB (UAC) microphone named in the configuration, opens it with
 * ALSA and streams raw samples (48 kHz, stereo, 16-bit) to the preprocess
 * module through AER_SOCK_AUDIO. If the mic disappears it is searched for
 * and reopened.
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/config.h"
#include "common/protocol.h"
#include "common/socket.h"

#define MIC_RATE		48000
#define MIC_CHANNELS		2
#define MIC_BUFFER_FRAMES	(MIC_RATE / 4)	/* 250 ms of ALSA buffering */
#define CHUNK_FRAMES		1024		/* ~21 ms per message */

typedef struct {
	snd_pcm_t *pcm;
	sample_format_t format;		/* applied sample format */
	unsigned rate;			/* applied sampling rate */
	unsigned channels;		/* applied channel count */
	snd_pcm_uframes_t buffer_frames;/* frames the ALSA buffer holds */
	snd_pcm_uframes_t period_frames;
} mic_reader_t;

static volatile int running = 1;

static int has_capture(snd_ctl_t *ctl)
{
	snd_pcm_info_t *info;
	int dev = -1;

	snd_pcm_info_alloca(&info);
	while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
		snd_pcm_info_set_device(info, dev);
		snd_pcm_info_set_subdevice(info, 0);
		snd_pcm_info_set_stream(info, SND_PCM_STREAM_CAPTURE);
		if (snd_ctl_pcm_info(ctl, info) == 0)
			return dev;
	}
	return -1;
}

/*
 * Find the capture card whose id, name or long name contains mic_name
 * (case-insensitive) and return its ALSA name, e.g. "hw:CARD=CM420,DEV=0".
 */
int find_device_name(const char *mic_name, char *device_name, size_t max_len)
{
	int card = -1;

	while (snd_card_next(&card) == 0 && card >= 0) {
		snd_ctl_card_info_t *info;
		snd_ctl_t *ctl;
		char hw[16];
		int dev, match;

		snprintf(hw, sizeof(hw), "hw:%d", card);
		if (snd_ctl_open(&ctl, hw, 0) < 0)
			continue;
		snd_ctl_card_info_alloca(&info);
		if (snd_ctl_card_info(ctl, info) < 0) {
			snd_ctl_close(ctl);
			continue;
		}
		match = strcasestr(snd_ctl_card_info_get_name(info), mic_name) ||
			strcasestr(snd_ctl_card_info_get_longname(info), mic_name) ||
			!strcasecmp(snd_ctl_card_info_get_id(info), mic_name);
		dev = match ? has_capture(ctl) : -1;
		if (dev >= 0) {
			snprintf(device_name, max_len, "hw:CARD=%s,DEV=%d",
				 snd_ctl_card_info_get_id(info), dev);
			fprintf(stderr, "found mic \"%s\" as %s\n",
				snd_ctl_card_info_get_name(info), device_name);
			snd_ctl_close(ctl);
			return 0;
		}
		snd_ctl_close(ctl);
	}
	return -1;
}

/*
 * Open the mic and request rate/channels and an ALSA buffer of
 * samples_in_buffer frames. The device may apply a different format;
 * the applied one is stored in the returned reader.
 */
mic_reader_t *open_mic(const char *device_name, unsigned rate,
		       unsigned channels, unsigned samples_in_buffer)
{
	static const struct {
		snd_pcm_format_t alsa;
		sample_format_t ours;
	} formats[] = {
		{ SND_PCM_FORMAT_S16_LE, SAMPLE_S16_LE },
		{ SND_PCM_FORMAT_S24_3LE, SAMPLE_S24_3LE },
		{ SND_PCM_FORMAT_S32_LE, SAMPLE_S32_LE },
	};
	snd_pcm_hw_params_t *hw;
	mic_reader_t *reader;
	size_t i;
	int err;

	reader = calloc(1, sizeof(*reader));
	if (!reader)
		return NULL;

	err = snd_pcm_open(&reader->pcm, device_name, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		fprintf(stderr, "open %s: %s\n", device_name, snd_strerror(err));
		free(reader);
		return NULL;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(reader->pcm, hw);
	err = snd_pcm_hw_params_set_access(reader->pcm, hw,
					   SND_PCM_ACCESS_RW_INTERLEAVED);
	for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (snd_pcm_hw_params_set_format(reader->pcm, hw,
						 formats[i].alsa) == 0) {
			reader->format = formats[i].ours;
			break;
		}
	}
	if (err < 0 || i == sizeof(formats) / sizeof(formats[0])) {
		fprintf(stderr, "%s: no supported sample format\n", device_name);
		goto fail;
	}

	reader->channels = channels;
	reader->rate = rate;
	reader->buffer_frames = samples_in_buffer;
	reader->period_frames = samples_in_buffer / 4;
	if ((err = snd_pcm_hw_params_set_channels_near(reader->pcm, hw,
						       &reader->channels)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate_near(reader->pcm, hw,
						   &reader->rate, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(reader->pcm, hw,
						&reader->buffer_frames)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(reader->pcm, hw,
						&reader->period_frames, NULL)) < 0 ||
	    (err = snd_pcm_hw_params(reader->pcm, hw)) < 0 ||
	    (err = snd_pcm_prepare(reader->pcm)) < 0) {
		fprintf(stderr, "configure %s: %s\n", device_name,
			snd_strerror(err));
		goto fail;
	}

	fprintf(stderr, "mic opened: %u Hz, %u ch, %u-bit, buffer %lu frames\n",
		reader->rate, reader->channels,
		sample_format_bytes(reader->format) * 8,
		(unsigned long)reader->buffer_frames);
	return reader;

fail:
	snd_pcm_close(reader->pcm);
	free(reader);
	return NULL;
}

/*
 * Read sample_count frames (one sample per channel each) into samples.
 * Overruns are recovered. Returns the frames read, or a negative ALSA
 * error when the mic is unusable (e.g. unplugged).
 */
long read_mic(mic_reader_t *reader, void *samples, size_t sample_count)
{
	size_t frame_bytes = sample_format_bytes(reader->format) *
			     reader->channels;
	size_t done = 0;

	while (done < sample_count && running) {
		snd_pcm_sframes_t n;

		n = snd_pcm_readi(reader->pcm, (char *)samples + done * frame_bytes,
				  sample_count - done);
		if (n == -EAGAIN || n == -EINTR)
			continue;
		if (n < 0) {
			if (n == -EPIPE)
				fprintf(stderr, "mic overrun\n");
			if (snd_pcm_recover(reader->pcm, (int)n, 1) < 0) {
				fprintf(stderr, "mic read: %s\n",
					snd_strerror((int)n));
				return n;
			}
			continue;
		}
		done += (size_t)n;
	}
	return (long)done;
}

static void close_mic(mic_reader_t *reader)
{
	snd_pcm_close(reader->pcm);
	free(reader);
}

int main(int argc, char **argv)
{
	app_config_t config;
	char device[64];
	msg_audio_t *msg;
	server_t *server;
	int warned = 0;

	setvbuf(stderr, NULL, _IOLBF, 0);
	config_load_from_args(argc, argv, &config);
	install_signal_handlers(&running);

	server = listen_on_socket(AER_SOCK_AUDIO);
	msg = malloc(sizeof(*msg) + AER_AUDIO_MAX_BYTES);
	if (!server || !msg)
		return 1;

	while (running) {
		mic_reader_t *reader;

		if (find_device_name(config.mic_name, device, sizeof(device))) {
			if (!warned)
				fprintf(stderr, "waiting for mic \"%s\"\n",
					config.mic_name);
			warned = 1;
			sleep(1);
			continue;
		}
		warned = 0;

		reader = open_mic(device, MIC_RATE, MIC_CHANNELS,
				  MIC_BUFFER_FRAMES);
		if (!reader) {
			sleep(1);
			continue;
		}
		if ((size_t)CHUNK_FRAMES * reader->channels *
		    sample_format_bytes(reader->format) > AER_AUDIO_MAX_BYTES) {
			fprintf(stderr, "too many channels: %u\n",
				reader->channels);
			return 1;
		}

		msg->type = MSG_AUDIO;
		msg->format = reader->format;
		msg->rate = reader->rate;
		msg->channels = reader->channels;
		while (running) {
			long n = read_mic(reader, msg->data, CHUNK_FRAMES);

			if (n <= 0)
				break;
			msg->frames = (uint32_t)n;
			send_to_socket(server, msg, sizeof(*msg) +
				       (size_t)n * reader->channels *
				       sample_format_bytes(reader->format));
		}
		close_mic(reader);
		if (running) {
			fprintf(stderr, "mic lost, searching again\n");
			sleep(1);
		}
	}

	close_server(server);
	free(msg);
	return 0;
}
