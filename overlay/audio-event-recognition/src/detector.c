/*
 * Detector module
 *
 * Compares the scores of the watched classes from the configuration with
 * their thresholds. When one or more watched classes pass their threshold,
 * the highest-scoring one that is not cooling down becomes an event; the
 * other classes over threshold in that frame also start their cooldown so a
 * single sound (e.g. a siren matching Siren, Ambulance and Police car) is
 * reported once.
 *
 * AER_SOCK_DETECTOR carries, to both the display and the notify module:
 *   MSG_EVENT   only when an event fires
 *   MSG_RESULT  every frame: top-N classes, input level, all 521 scores
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/config.h"
#include "common/protocol.h"
#include "common/socket.h"

typedef struct {
	const app_config_t *config;	/* watched classes and thresholds */
	int64_t cooldown_ms;
	int64_t last_fire_ms[AER_MAX_WATCHED];	/* per watched class, 0: never */
	uint32_t next_event_id;
} detector_t;

static volatile int running = 1;

static void init_detector(detector_t *detector, const app_config_t *config)
{
	memset(detector, 0, sizeof(*detector));
	detector->config = config;
	detector->cooldown_ms = (int64_t)config->cooldown_sec * 1000;
	detector->next_event_id = 1;
}

/*
 * Check one frame's scores. If an event fires, fill event and return 1;
 * otherwise return 0.
 */
int check_event(detector_t *detector, const float *scores, event_t *event)
{
	const app_config_t *c = detector->config;
	int64_t now = now_ms();
	int i, best = -1;

	for (i = 0; i < c->num_watched; i++) {
		const watched_class_t *w = &c->watched[i];
		int64_t last = detector->last_fire_ms[i];

		if (scores[w->index] < w->threshold)
			continue;
		if (last && now - last < detector->cooldown_ms)
			continue;
		if (best < 0 || scores[w->index] > scores[c->watched[best].index])
			best = i;
	}
	if (best < 0)
		return 0;

	/* the reported class and every class it was heard with cool down */
	for (i = 0; i < c->num_watched; i++)
		if (scores[c->watched[i].index] >= c->watched[i].threshold)
			detector->last_fire_ms[i] = now;

	memset(event, 0, sizeof(*event));
	event->event_id = detector->next_event_id++;
	event->class_index = c->watched[best].index;
	snprintf(event->class_name, sizeof(event->class_name), "%s",
		 c->watched[best].name);
	event->score = scores[event->class_index];
	event->threshold = c->watched[best].threshold;
	event->time_ms = now;
	return 1;
}

/* Pick the n highest-scoring classes of one frame, best first. */
void find_top_n(const float *scores, int n, frame_result_t *top_n_result)
{
	int i, j;

	if (n > AER_TOP_N)
		n = AER_TOP_N;
	top_n_result->n = 0;
	for (i = 0; i < AER_NUM_CLASSES; i++) {
		int count = top_n_result->n;

		if (count == n && scores[i] <= top_n_result->score[n - 1])
			continue;
		if (count < n)
			top_n_result->n = ++count;
		/* insertion into the sorted list */
		for (j = count - 1; j > 0 && top_n_result->score[j - 1] < scores[i];
		     j--) {
			top_n_result->score[j] = top_n_result->score[j - 1];
			top_n_result->class_index[j] =
				top_n_result->class_index[j - 1];
		}
		top_n_result->score[j] = scores[i];
		top_n_result->class_index[j] = i;
	}
}

int main(int argc, char **argv)
{
	app_config_t config;
	detector_t detector;
	msg_scores_t in;
	msg_result_t result;
	msg_event_t event;
	server_t *server;

	setvbuf(stderr, NULL, _IOLBF, 0);
	config_load_from_args(argc, argv, &config);
	install_signal_handlers(&running);

	init_detector(&detector, &config);
	server = listen_on_socket(AER_SOCK_DETECTOR);
	if (!server)
		return 1;
	result.type = MSG_RESULT;
	event.type = MSG_EVENT;
	fprintf(stderr, "watching %d classes, cooldown %d s\n",
		config.num_watched, config.cooldown_sec);

	while (running) {
		int fd = connect_to_socket(AER_SOCK_INFERENCE);

		while (running) {
			ssize_t n = receive_on_socket(fd, &in, sizeof(in));

			if (n < 0 && (errno == EINTR || errno == EMSGSIZE))
				continue;
			if (n <= 0)
				break;
			if ((size_t)n != sizeof(in) || in.type != MSG_SCORES)
				continue;

			if (check_event(&detector, in.scores, &event.event)) {
				fprintf(stderr, "event %u: %s %.3f (threshold %.2f)\n",
					event.event.event_id,
					event.event.class_name,
					event.event.score,
					event.event.threshold);
				send_to_socket(server, &event, sizeof(event));
			}

			result.seq = in.seq;
			find_top_n(in.scores, AER_TOP_N, &result.top_n);
			result.top_n.level_dbfs = in.level_dbfs;
			result.top_n.time_ms = in.time_ms;
			memcpy(result.scores, in.scores, sizeof(result.scores));
			send_to_socket(server, &result, sizeof(result));
		}
		close(fd);
		if (running)
			fprintf(stderr, "inference disconnected\n");
	}

	close_server(server);
	return 0;
}
