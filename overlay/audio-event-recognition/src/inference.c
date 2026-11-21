/*
 * Inference module
 *
 * Loads the YAMNet .tflite model from the board's file system with the
 * TensorFlow Lite C API and runs it on every frame from the framer module.
 * The 521 class scores go to the detector module through AER_SOCK_INFERENCE.
 *
 * The latency of every inference (wall-clock time of run_model(): input copy,
 * model invocation and output read, nothing before it in the pipeline) is
 * appended in microseconds, one value per line, to INFERENCE_LATENCY_LOG.
 *
 * Optional overrides from the environment (set by "aer optimize" through a
 * systemd drop-in, unset normally):
 *   AER_INFERENCE_MODEL=<path>  load this model instead of model_path
 *   AER_INFERENCE_MLOCKALL=1    lock all current and future memory in RAM
 *                               (needs LimitMEMLOCK=infinity)
 *   AER_INFERENCE_XNNPACK=1     run the model through the XNNPACK delegate
 *                               (otherwise TFLite's built-in CPU kernels;
 *                               the C API never adds XNNPACK on its own)
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#include <tensorflow/lite/c/c_api.h>
#include <tensorflow/lite/delegates/xnnpack/xnnpack_delegate.h>

#include "common/config.h"
#include "common/protocol.h"
#include "common/socket.h"

/* the directory comes from LogsDirectory= in aer-inference.service */
#define INFERENCE_LATENCY_LOG	"/var/log/audio-event/inference_latency.log"

typedef struct {
	TfLiteModel *model;
	TfLiteInterpreterOptions *options;
	TfLiteInterpreter *interpreter;
	TfLiteDelegate *xnnpack;	/* NULL: built-in kernels */
	TfLiteTensor *input;
	const TfLiteTensor *output;	/* scores, [patches x 521] */
	int output_rows;
} model_t;

static volatile int running = 1;
static int use_xnnpack;		/* from AER_INFERENCE_XNNPACK */

static void free_model(model_t *model)
{
	if (!model)
		return;
	if (model->interpreter)
		TfLiteInterpreterDelete(model->interpreter);
	/* the delegate must outlive the interpreter that uses it */
	if (model->xnnpack)
		TfLiteXNNPackDelegateDelete(model->xnnpack);
	if (model->options)
		TfLiteInterpreterOptionsDelete(model->options);
	if (model->model)
		TfLiteModelDelete(model->model);
	free(model);
}

static size_t tensor_elements(const TfLiteTensor *t)
{
	size_t n = 1;
	int i;

	for (i = 0; i < TfLiteTensorNumDims(t); i++)
		n *= (size_t)TfLiteTensorDim(t, i);
	return n;
}

/* TFLite errors and warnings (e.g. why a delegate was not applied) */
static void tflite_error(void *user_data, const char *format, va_list args)
{
	(void)user_data;
	fprintf(stderr, "tflite: ");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}

static void print_tensor(const char *what, int i, const TfLiteTensor *t)
{
	int d;

	fprintf(stderr, "%s %d \"%s\" type %d shape [", what, i,
		TfLiteTensorName(t), (int)TfLiteTensorType(t));
	for (d = 0; d < TfLiteTensorNumDims(t); d++)
		fprintf(stderr, "%s%d", d ? "," : "", TfLiteTensorDim(t, d));
	fprintf(stderr, "]\n");
}

/*
 * Create model->interpreter with the XNNPACK delegate. Returns 1 on success.
 * Returns 0 if XNNPACK cannot run this model (TFLite refuses it for graphs
 * with dynamic-size tensors, like the float YAMNet); the caller then uses
 * the built-in kernels.
 */
static int create_xnnpack_interpreter(model_t *model, int num_threads)
{
	TfLiteXNNPackDelegateOptions xopts = TfLiteXNNPackDelegateOptionsDefault();
	TfLiteInterpreterOptions *options;

	xopts.num_threads = num_threads;
	/*
	 * Quantized operators (signed and unsigned 8-bit) are off in the
	 * default options of CMake builds; without them XNNPACK takes almost
	 * nothing of a quantized model.
	 */
	xopts.flags |= TFLITE_XNNPACK_DELEGATE_FLAG_QS8 |
		       TFLITE_XNNPACK_DELEGATE_FLAG_QU8;
	model->xnnpack = TfLiteXNNPackDelegateCreate(&xopts);
	if (!model->xnnpack) {
		fprintf(stderr, "cannot create XNNPACK delegate\n");
		return 0;
	}

	/* separate options, so a failure leaves model->options delegate-free */
	options = TfLiteInterpreterOptionsCreate();
	TfLiteInterpreterOptionsSetNumThreads(options, num_threads);
	TfLiteInterpreterOptionsSetErrorReporter(options, tflite_error, NULL);
	TfLiteInterpreterOptionsAddDelegate(options, model->xnnpack);
	model->interpreter = TfLiteInterpreterCreate(model->model, options);
	if (model->interpreter) {
		TfLiteInterpreterOptionsDelete(model->options);
		model->options = options;
		return 1;
	}

	fprintf(stderr, "XNNPACK cannot run this model (e.g. it has "
		"dynamic-size tensors); using built-in CPU kernels\n");
	TfLiteInterpreterOptionsDelete(options);
	TfLiteXNNPackDelegateDelete(model->xnnpack);
	model->xnnpack = NULL;
	return 0;
}

/*
 * Load the YAMNet model and let TFLite use up to num_threads threads.
 * The input must take 15,600 float samples; the output used is the one
 * whose last dimension is 521 (class scores).
 */
model_t *load_model(const char *model_path, int num_threads)
{
	int i, n;
	model_t *model = calloc(1, sizeof(*model));

	if (!model)
		return NULL;
	model->model = TfLiteModelCreateFromFile(model_path);
	if (!model->model) {
		fprintf(stderr, "cannot load model %s\n", model_path);
		goto fail;
	}
	model->options = TfLiteInterpreterOptionsCreate();
	TfLiteInterpreterOptionsSetNumThreads(model->options, num_threads);
	TfLiteInterpreterOptionsSetErrorReporter(model->options, tflite_error,
						 NULL);
	if (use_xnnpack && create_xnnpack_interpreter(model, num_threads))
		goto loaded;
	model->interpreter = TfLiteInterpreterCreate(model->model,
						     model->options);
	if (!model->interpreter) {
		fprintf(stderr, "cannot create interpreter\n");
		goto fail;
	}

loaded:
	model->input = TfLiteInterpreterGetInputTensor(model->interpreter, 0);
	if (TfLiteInterpreterGetInputTensorCount(model->interpreter) != 1 ||
	    TfLiteTensorType(model->input) != kTfLiteFloat32) {
		fprintf(stderr, "unexpected model input\n");
		goto fail;
	}
	if (tensor_elements(model->input) != YAMNET_FRAME_SAMPLES) {
		/* waveform input of flexible length: fix it to one frame */
		int dims[] = { YAMNET_FRAME_SAMPLES };

		if (TfLiteInterpreterResizeInputTensor(model->interpreter, 0,
						       dims, 1) != kTfLiteOk) {
			fprintf(stderr, "cannot resize model input\n");
			goto fail;
		}
	}
	if (TfLiteInterpreterAllocateTensors(model->interpreter) != kTfLiteOk) {
		fprintf(stderr, "cannot allocate tensors\n");
		goto fail;
	}
	model->input = TfLiteInterpreterGetInputTensor(model->interpreter, 0);
	print_tensor("input", 0, model->input);

	n = TfLiteInterpreterGetOutputTensorCount(model->interpreter);
	for (i = 0; i < n; i++) {
		const TfLiteTensor *t =
			TfLiteInterpreterGetOutputTensor(model->interpreter, i);
		int nd = TfLiteTensorNumDims(t);

		print_tensor("output", i, t);
		if (!model->output && nd >= 1 &&
		    TfLiteTensorDim(t, nd - 1) == AER_NUM_CLASSES &&
		    TfLiteTensorType(t) == kTfLiteFloat32) {
			model->output = t;
			model->output_rows = (int)(tensor_elements(t) /
						   AER_NUM_CLASSES);
		}
	}
	if (!model->output || model->output_rows < 1) {
		fprintf(stderr, "model has no %d-class score output\n",
			AER_NUM_CLASSES);
		goto fail;
	}
	fprintf(stderr, "model %s loaded, %d threads, %s\n", model_path,
		num_threads, model->xnnpack ? "XNNPACK delegate" :
		"built-in CPU kernels");
	return model;

fail:
	free_model(model);
	return NULL;
}

/*
 * Run YAMNet on one frame of 15,600 samples and write the 521 class scores.
 * If the model yields several patches, their scores are averaged.
 */
int run_model(model_t *model, const float *frame, float *scores)
{
	const float *out;
	int r, c;

	if (TfLiteTensorCopyFromBuffer(model->input, frame,
				       YAMNET_FRAME_SAMPLES * sizeof(float)) !=
	    kTfLiteOk)
		return -1;
	if (TfLiteInterpreterInvoke(model->interpreter) != kTfLiteOk)
		return -1;

	out = TfLiteTensorData(model->output);
	for (c = 0; c < AER_NUM_CLASSES; c++) {
		float sum = 0.0f;

		for (r = 0; r < model->output_rows; r++)
			sum += out[r * AER_NUM_CLASSES + c];
		scores[c] = sum / model->output_rows;
	}
	return 0;
}

/* Elapsed time from a to b in microseconds */
static long long elapsed_us(const struct timespec *a, const struct timespec *b)
{
	return (long long)(b->tv_sec - a->tv_sec) * 1000000 +
	       (b->tv_nsec - a->tv_nsec) / 1000;
}

/* Open the latency log for appending, line-buffered so each value lands
 * in the file as soon as it is measured. NULL if it cannot be opened. */
static FILE *open_latency_log(void)
{
	FILE *f = fopen(INFERENCE_LATENCY_LOG, "a");

	if (!f) {
		fprintf(stderr, "latency log %s: %s (not logging)\n",
			INFERENCE_LATENCY_LOG, strerror(errno));
		return NULL;
	}
	setvbuf(f, NULL, _IOLBF, 0);
	fprintf(stderr, "logging inference latency (us) to %s\n",
		INFERENCE_LATENCY_LOG);
	return f;
}

int main(int argc, char **argv)
{
	app_config_t config;
	msg_frame_t *frame;
	msg_scores_t out;
	server_t *server;
	model_t *model;
	const char *model_path, *mlock_env, *xnnpack_env;
	FILE *latency_log;
	long long total_us = 0;
	int runs = 0;

	setvbuf(stderr, NULL, _IOLBF, 0);
	config_load_from_args(argc, argv, &config);
	install_signal_handlers(&running);

	xnnpack_env = getenv("AER_INFERENCE_XNNPACK");
	use_xnnpack = xnnpack_env && !strcmp(xnnpack_env, "1");
	model_path = getenv("AER_INFERENCE_MODEL");
	if (model_path && *model_path)
		fprintf(stderr, "model overridden by AER_INFERENCE_MODEL\n");
	else
		model_path = config.model_path;
	model = load_model(model_path, config.num_threads);
	if (!model)
		return 1;

	/*
	 * After loading, so the model is locked too; MCL_FUTURE also covers
	 * the TFLite worker threads and buffers created at the first invoke.
	 */
	mlock_env = getenv("AER_INFERENCE_MLOCKALL");
	if (mlock_env && !strcmp(mlock_env, "1")) {
		if (mlockall(MCL_CURRENT | MCL_FUTURE))
			fprintf(stderr, "mlockall: %s (continuing unlocked)\n",
				strerror(errno));
		else
			fprintf(stderr, "memory locked (mlockall)\n");
	}
	server = listen_on_socket(AER_SOCK_INFERENCE);
	frame = malloc(sizeof(*frame));
	if (!server || !frame)
		return 1;
	out.type = MSG_SCORES;
	latency_log = open_latency_log();

	while (running) {
		int fd = connect_to_socket(AER_SOCK_FRAMER);

		while (running) {
			ssize_t n = receive_on_socket(fd, frame, sizeof(*frame));
			struct timespec t0, t1;
			long long latency_us;

			if (n < 0 && (errno == EINTR || errno == EMSGSIZE))
				continue;
			if (n <= 0)
				break;
			if ((size_t)n != sizeof(*frame) ||
			    frame->type != MSG_FRAME)
				continue;

			/* wall-clock (not CPU) time of this inference only */
			clock_gettime(CLOCK_MONOTONIC, &t0);
			if (run_model(model, frame->samples, out.scores)) {
				fprintf(stderr, "inference failed\n");
				continue;
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);
			latency_us = elapsed_us(&t0, &t1);
			if (latency_log)
				fprintf(latency_log, "%lld\n", latency_us);
			total_us += latency_us;
			if (++runs == 100) {
				fprintf(stderr, "average inference %.1f ms\n",
					total_us / 1000.0 / runs);
				runs = 0;
				total_us = 0;
			}

			out.seq = frame->seq;
			out.time_ms = frame->time_ms;
			out.level_dbfs = frame->level_dbfs;
			send_to_socket(server, &out, sizeof(out));
		}
		close(fd);
		if (running)
			fprintf(stderr, "framer disconnected\n");
	}

	if (latency_log)
		fclose(latency_log);
	close_server(server);
	free_model(model);
	free(frame);
	return 0;
}
