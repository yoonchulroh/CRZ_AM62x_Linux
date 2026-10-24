// SPDX-License-Identifier: MIT
/*
 * aer-detect - module 5: decide whether a frame contains an event of interest.
 *
 *   AER_MSG_SCORES -> AER_MSG_STATUS (every frame)
 *                  -> AER_MSG_EVENT  (when an event fires)
 *
 * Event types are configured as [event <name>] sections:
 *
 *   [event siren]
 *   threshold = 0.3
 *   classes = Siren | Civil defense siren | Police car (siren)
 *
 * The score of an event type is the highest score among its classes. The
 * event fires when that score reaches the threshold, and the same type does
 * not fire again until cooldown_sec has passed (frame time).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aer.h"
#include "conf.h"
#include "ipc.h"

#define MAX_CLASSES_PER_EVENT	32

struct event_def {
	char name[AER_EVENT_NAME_LEN];
	float threshold;
	int cls[MAX_CLASSES_PER_EVENT];
	int ncls;
	int64_t last_fire_ns;
};

static char **labels;
static int nlabels;

static int load_labels(const char *path)
{
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	FILE *f = fopen(path, "r");

	if (!f) {
		aer_log("labels %s: %s", path, strerror(errno));
		return -1;
	}
	while ((n = getline(&line, &cap, f)) > 0) {
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		labels = realloc(labels, (nlabels + 1) * sizeof(*labels));
		labels[nlabels++] = strdup(line);
	}
	free(line);
	fclose(f);
	aer_log("%d class labels from %s", nlabels, path);
	return nlabels ? 0 : -1;
}

static const char *label(int i)
{
	return i >= 0 && i < nlabels ? labels[i] : "?";
}

static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

static int find_class(const char *name)
{
	char *end;
	long idx = strtol(name, &end, 10);

	if (*name && !*end)
		return idx >= 0 && idx < nlabels ? (int)idx : -1;
	for (int i = 0; i < nlabels; i++)
		if (!strcasecmp(labels[i], name))
			return i;
	return -1;
}

static int load_events(const struct aer_conf *conf, struct event_def *ev)
{
	double def_thr = aer_conf_double(conf, "detect", "default_threshold", 0.3);
	int nev = 0;

	for (size_t s = 0; s < aer_conf_nsections(conf); s++) {
		const char *sec = aer_conf_section(conf, s);
		struct event_def *e = &ev[nev];
		char *list, *tok, *save, tmp[128];

		if (strncmp(sec, "event ", 6))
			continue;
		if (nev == AER_MAX_EVENTS) {
			aer_log("more than %d event types, ignoring [%s]", AER_MAX_EVENTS, sec);
			continue;
		}
		memset(e, 0, sizeof(*e));
		aer_strlcpy(tmp, sec + 6, sizeof(tmp));
		aer_strlcpy(e->name, trim(tmp), sizeof(e->name));
		e->threshold = (float)aer_conf_double(conf, sec, "threshold", def_thr);

		list = strdup(aer_conf_get(conf, sec, "classes", ""));
		for (tok = strtok_r(list, "|", &save); tok; tok = strtok_r(NULL, "|", &save)) {
			char *name = trim(tok);
			int idx;

			if (!*name)
				continue;
			idx = find_class(name);
			if (idx < 0) {
				aer_log("[%s]: unknown class '%s'", sec, name);
				continue;
			}
			if (e->ncls < MAX_CLASSES_PER_EVENT)
				e->cls[e->ncls++] = idx;
		}
		free(list);
		if (!e->name[0] || !e->ncls) {
			aer_log("[%s]: no name or no valid classes, ignored", sec);
			continue;
		}
		aer_log("event '%s': threshold %.2f, %d class(es)", e->name, e->threshold, e->ncls);
		nev++;
	}
	return nev;
}

int main(int argc, char **argv)
{
	static uint8_t msg[AER_MAX_MSG];
	static struct event_def ev[AER_MAX_EVENTS];
	struct aer_client in;
	struct aer_server out;
	struct aer_conf *conf;
	const char *conf_path;
	int64_t cooldown_ns;
	int nev;

	conf_path = aer_parse_common(argc, argv, NULL, NULL, NULL);
	conf = aer_conf_load(conf_path);
	aer_signals();

	if (load_labels(aer_conf_get(conf, "detect", "labels",
				     "/usr/local/share/aer/yamnet_labels.txt")) < 0)
		return 1;
	nev = load_events(conf, ev);
	if (!nev)
		aer_log("no [event ...] sections configured, nothing will be detected");
	cooldown_ns = (int64_t)(aer_conf_double(conf, "detect", "cooldown_sec", 5.0) * 1e9);

	aer_client_init(&in, aer_run_dir(conf), "infer");
	if (aer_server_open(&out, aer_run_dir(conf), "detect") < 0)
		return 1;

	while (!aer_quit) {
		const struct aer_hdr *h = (const struct aer_hdr *)msg;
		const struct aer_scores *sc = (const struct aer_scores *)(h + 1);
		struct aer_hdr sh = { .type = AER_MSG_STATUS, .channels = 1 };
		struct aer_status st = { 0 };
		int top = 0;

		if (aer_link_wait(&in, &out, msg, sizeof(msg), AER_MSG_SCORES, 1000) <= 0)
			continue;
		in.discont = false;
		if ((int)h->count != nlabels) {
			aer_log("got %u scores but have %d labels", h->count, nlabels);
			continue;
		}

		for (int i = 1; i < nlabels; i++)
			if (sc->score[i] > sc->score[top])
				top = i;
		st.level_dbfs = sc->level_dbfs;
		st.infer_ms = sc->infer_ms;
		st.top_class = top;
		st.top_score = sc->score[top];
		aer_strlcpy(st.top_name, label(top), sizeof(st.top_name));
		st.n_events = (uint32_t)nev;

		for (int e = 0; e < nev; e++) {
			int best = ev[e].cls[0];

			for (int k = 1; k < ev[e].ncls; k++)
				if (sc->score[ev[e].cls[k]] > sc->score[best])
					best = ev[e].cls[k];
			aer_strlcpy(st.ev[e].name, ev[e].name, sizeof(st.ev[e].name));
			st.ev[e].score = sc->score[best];
			st.ev[e].threshold = ev[e].threshold;

			if (sc->score[best] >= ev[e].threshold &&
			    (!ev[e].last_fire_ns || h->t_ns < ev[e].last_fire_ns ||
			     h->t_ns - ev[e].last_fire_ns >= cooldown_ns)) {
				struct aer_hdr eh = { .type = AER_MSG_EVENT, .channels = 1,
						      .t_ns = h->t_ns };
				struct aer_event evt = { 0 };
				char when[40];

				aer_strlcpy(evt.type, ev[e].name, sizeof(evt.type));
				aer_strlcpy(evt.cls, label(best), sizeof(evt.cls));
				evt.score = sc->score[best];
				evt.threshold = ev[e].threshold;
				ev[e].last_fire_ns = h->t_ns;

				aer_format_time(h->t_ns, when, sizeof(when));
				aer_log("EVENT %s score %.3f (%s) at %s", evt.type, evt.score, evt.cls, when);
				aer_server_send(&out, &eh, &evt, sizeof(evt));
			}
		}

		sh.t_ns = h->t_ns;
		aer_dbg("top: %s %.3f, level %.1f dBFS, %.0f ms", st.top_name, st.top_score,
			st.level_dbfs, st.infer_ms);
		aer_server_send(&out, &sh, &st, sizeof(st));
	}

	aer_server_close(&out);
	aer_client_close(&in);
	aer_conf_free(conf);
	return 0;
}
