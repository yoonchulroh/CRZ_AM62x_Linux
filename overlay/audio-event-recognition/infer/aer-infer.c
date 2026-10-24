// SPDX-License-Identifier: MIT
/*
 * aer-infer - module 4: run YAMNet with TensorFlow Lite on every frame.
 *
 *   AER_MSG_FRAME  (15600 float32 samples at 16 kHz)
 *   AER_MSG_SCORES (struct aer_scores + 521 class scores)
 *
 * Uses the TensorFlow Lite C API (libtensorflow-lite from Ubuntu). The model
 * is the YAMNet classification model: input float32[15600], output
 * float32[1, 521].
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tensorflow/lite/c/c_api.h>

#include "aer.h"
#include "conf.h"
#include "ipc.h"

static void tflite_error(void *user, const char *fmt, va_list args)
{
	char buf[512];

	(void)user;
	vsnprintf(buf, sizeof(buf), fmt, args);
	aer_log("tflite: %s", buf);
}

static size_t tensor_elems(const TfLiteTensor *t)
{
	size_t n = 1;

	for (int d = 0; d < TfLiteTensorNumDims(t); d++)
		n *= (size_t)TfLiteTensorDim(t, d);
	return n;
}

static double elapsed_ms(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1e3 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

static float level_dbfs(const float *x, size_t n)
{
	double acc = 0;

	for (size_t i = 0; i < n; i++)
		acc += (double)x[i] * x[i];
	acc = n ? sqrt(acc / n) : 0;
	return acc > 1e-6 ? (float)(20.0 * log10(acc)) : -120.0f;
}

int main(int argc, char **argv)
{
	static uint8_t msg[AER_MAX_MSG];
	struct aer_client in;
	struct aer_server out;
	struct aer_conf *conf;
	const char *conf_path, *model_path;
	TfLiteModel *model;
	TfLiteInterpreterOptions *opts;
	TfLiteInterpreter *interp;
	TfLiteTensor *input;
	const TfLiteTensor *output;
	size_t in_elems, out_elems;
	struct aer_scores *scores;
	long threads;

	conf_path = aer_parse_common(argc, argv, NULL, NULL, NULL);
	conf = aer_conf_load(conf_path);
	aer_signals();

	model_path = aer_conf_get(conf, "infer", "model", "/usr/local/share/aer/yamnet.tflite");
	threads = aer_conf_int(conf, "infer", "threads", 2);

	aer_log("TensorFlow Lite %s, model %s, %ld thread(s)", TfLiteVersion(), model_path, threads);
	model = TfLiteModelCreateFromFile(model_path);
	if (!model) {
		aer_log("cannot load model %s", model_path);
		return 1;
	}
	opts = TfLiteInterpreterOptionsCreate();
	TfLiteInterpreterOptionsSetNumThreads(opts, (int32_t)threads);
	TfLiteInterpreterOptionsSetErrorReporter(opts, tflite_error, NULL);
	interp = TfLiteInterpreterCreate(model, opts);
	if (!interp || TfLiteInterpreterAllocateTensors(interp) != kTfLiteOk) {
		aer_log("cannot create the interpreter");
		return 1;
	}

	input = TfLiteInterpreterGetInputTensor(interp, 0);
	output = TfLiteInterpreterGetOutputTensor(interp, 0);
	if (TfLiteTensorType(input) != kTfLiteFloat32 || TfLiteTensorType(output) != kTfLiteFloat32) {
		aer_log("model must have float32 input and output");
		return 1;
	}
	in_elems = tensor_elems(input);
	out_elems = tensor_elems(output);
	aer_log("model input %zu samples (%.3f s), output %zu classes",
		in_elems, (double)in_elems / AER_MODEL_RATE, out_elems);
	if (sizeof(struct aer_hdr) + sizeof(*scores) + out_elems * sizeof(float) > AER_MAX_MSG) {
		aer_log("model output too large");
		return 1;
	}
	scores = calloc(1, sizeof(*scores) + out_elems * sizeof(float));

	aer_client_init(&in, aer_run_dir(conf), "framer");
	if (aer_server_open(&out, aer_run_dir(conf), "infer") < 0)
		return 1;

	while (!aer_quit) {
		const struct aer_hdr *h = (const struct aer_hdr *)msg;
		const float *frame = (const float *)(h + 1);
		struct aer_hdr oh = { .type = AER_MSG_SCORES, .channels = 1 };
		struct timespec t0, t1;

		if (aer_link_wait(&in, &out, msg, sizeof(msg), AER_MSG_FRAME, 1000) <= 0)
			continue;
		in.discont = false;
		if (h->count != in_elems || h->rate != AER_MODEL_RATE) {
			aer_log("frame of %u samples at %u Hz does not match the model input "
				"(%zu samples at %d Hz)", h->count, h->rate, in_elems, AER_MODEL_RATE);
			continue;
		}

		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (TfLiteTensorCopyFromBuffer(input, frame, in_elems * sizeof(float)) != kTfLiteOk ||
		    TfLiteInterpreterInvoke(interp) != kTfLiteOk ||
		    TfLiteTensorCopyToBuffer(output, scores->score, out_elems * sizeof(float)) != kTfLiteOk) {
			aer_log("inference failed");
			continue;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);

		scores->infer_ms = (float)elapsed_ms(&t0, &t1);
		scores->level_dbfs = level_dbfs(frame, in_elems);
		oh.t_ns = h->t_ns;
		oh.rate = h->rate;
		oh.count = (uint32_t)out_elems;
		aer_dbg("frame %u: %.1f ms, level %.1f dBFS", h->seq, scores->infer_ms,
			scores->level_dbfs);
		aer_server_send(&out, &oh, scores, sizeof(*scores) + out_elems * sizeof(float));
	}

	aer_server_close(&out);
	aer_client_close(&in);
	TfLiteInterpreterDelete(interp);
	TfLiteInterpreterOptionsDelete(opts);
	TfLiteModelDelete(model);
	aer_conf_free(conf);
	free(scores);
	return 0;
}
