/*
 * INI configuration parser shared by all audio event recognition modules.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return s;
}

static void copy_str(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src);
}

static int parse_int(const char *v, long min, long max, long *out)
{
	char *end;
	long n;

	errno = 0;
	n = strtol(v, &end, 10);
	if (errno || end == v || *trim(end) != '\0' || n < min || n > max)
		return -1;
	*out = n;
	return 0;
}

static int parse_float(const char *v, float min, float max, float *out)
{
	char *end;
	float f;

	errno = 0;
	f = strtof(v, &end);
	if (errno || end == v || *trim(end) != '\0' || f < min || f > max)
		return -1;
	*out = f;
	return 0;
}

static void set_defaults(app_config_t *c)
{
	memset(c, 0, sizeof(*c));
	copy_str(c->board_name, sizeof(c->board_name), "CRZ AM62x");
	copy_str(c->mic_name, sizeof(c->mic_name), "USB");
	copy_str(c->model_path, sizeof(c->model_path),
		 "/opt/audio-event/yamnet.tflite");
	c->num_threads = 1;
	c->cooldown_sec = 5;
	c->alert_dismiss_sec = 60;
	copy_str(c->class_map_path, sizeof(c->class_map_path),
		 AER_DEFAULT_CLASS_MAP);
	c->broker_port = 1883;
	copy_str(c->topic, sizeof(c->topic), "audio-event");
}

/* "<threshold>, <name>"; the name itself may contain commas */
static int parse_watched(app_config_t *c, const char *key, char *value)
{
	watched_class_t *w;
	char *comma;
	long index;
	int i;

	if (parse_int(key, 0, AER_NUM_CLASSES - 1, &index))
		return -1;
	comma = strchr(value, ',');
	if (!comma)
		return -1;
	*comma = '\0';

	for (i = 0; i < c->num_watched; i++)
		if (c->watched[i].index == index)
			break;
	if (i == c->num_watched) {
		if (c->num_watched == AER_MAX_WATCHED)
			return -1;
		c->num_watched++;
	}
	w = &c->watched[i];
	w->index = (int)index;
	if (parse_float(trim(value), 0.0f, 1.0f, &w->threshold))
		return -1;
	copy_str(w->name, sizeof(w->name), trim(comma + 1));
	if (w->name[0] == '\0')
		return -1;
	return 0;
}

static int apply(app_config_t *c, const char *section, const char *key,
		 char *value)
{
	long n;

	if (!strcmp(section, "watched_classes"))
		return parse_watched(c, key, value);

	if (!strcmp(section, "board") && !strcmp(key, "name")) {
		copy_str(c->board_name, sizeof(c->board_name), value);
	} else if (!strcmp(section, "audio_input") && !strcmp(key, "mic_name")) {
		copy_str(c->mic_name, sizeof(c->mic_name), value);
	} else if (!strcmp(section, "inference") && !strcmp(key, "model_path")) {
		copy_str(c->model_path, sizeof(c->model_path), value);
	} else if (!strcmp(section, "inference") && !strcmp(key, "num_threads")) {
		if (parse_int(value, 1, 64, &n))
			return -1;
		c->num_threads = (int)n;
	} else if (!strcmp(section, "detector") && !strcmp(key, "cooldown_sec")) {
		if (parse_int(value, 0, 86400, &n))
			return -1;
		c->cooldown_sec = (int)n;
	} else if (!strcmp(section, "display") &&
		   !strcmp(key, "alert_dismiss_sec")) {
		if (parse_int(value, 0, 86400, &n))
			return -1;
		c->alert_dismiss_sec = (int)n;
	} else if (!strcmp(section, "display") &&
		   !strcmp(key, "class_map_path")) {
		copy_str(c->class_map_path, sizeof(c->class_map_path), value);
	} else if (!strcmp(section, "mqtt") && !strcmp(key, "broker_port")) {
		if (parse_int(value, 1, 65535, &n))
			return -1;
		c->broker_port = (uint16_t)n;
	} else if (!strcmp(section, "mqtt") && !strcmp(key, "topic")) {
		if (value[0] == '\0')
			return -1;
		copy_str(c->topic, sizeof(c->topic), value);
	} else {
		fprintf(stderr, "config: ignoring unknown key [%s] %s\n",
			section, key);
	}
	return 0;
}

int config_parse(const char *path, app_config_t *configuration, char *err,
		 size_t errlen_max)
{
	char line[512], section[AER_NAME_MAX] = "";
	int lineno = 0;
	FILE *f;

	set_defaults(configuration);

	f = fopen(path, "r");
	if (!f) {
		snprintf(err, errlen_max, "%s: %s", path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line), *eq, *key, *value;

		lineno++;
		if (*s == '\0' || *s == ';' || *s == '#')
			continue;
		if (*s == '[') {
			char *close = strchr(s, ']');

			if (!close)
				goto syntax;
			*close = '\0';
			copy_str(section, sizeof(section), trim(s + 1));
			continue;
		}
		eq = strchr(s, '=');
		if (!eq)
			goto syntax;
		*eq = '\0';
		key = trim(s);
		value = trim(eq + 1);
		if (apply(configuration, section, key, value)) {
			snprintf(err, errlen_max, "%s:%d: invalid value for [%s] %s",
				 path, lineno, section, key);
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return 0;

syntax:
	snprintf(err, errlen_max, "%s:%d: syntax error", path, lineno);
	fclose(f);
	return -1;
}

const watched_class_t *config_find_watched(const app_config_t *configuration,
					   int class_index)
{
	int i;

	for (i = 0; i < configuration->num_watched; i++)
		if (configuration->watched[i].index == class_index)
			return &configuration->watched[i];
	return NULL;
}

void config_load_from_args(int argc, char **argv, app_config_t *configuration)
{
	const char *path = AER_DEFAULT_CONFIG;
	char err[512];
	int opt;

	while ((opt = getopt(argc, argv, "c:h")) != -1) {
		switch (opt) {
		case 'c':
			path = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-c config.ini]\n", argv[0]);
			exit(opt == 'h' ? 0 : 2);
		}
	}
	if (config_parse(path, configuration, err, sizeof(err))) {
		fprintf(stderr, "config: %s\n", err);
		exit(1);
	}
}

int load_class_names(const char *path, char names[][AER_NAME_MAX])
{
	char line[512];
	int count = 0;
	FILE *f;

	for (int i = 0; i < AER_NUM_CLASSES; i++)
		snprintf(names[i], AER_NAME_MAX, "Class %d", i);

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "class map %s: %s\n", path, strerror(errno));
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		char *p, *name, *end;
		long index;

		/* index,mid,display_name   (display_name may be quoted) */
		index = strtol(line, &p, 10);
		if (p == line || *p != ',' || index < 0 || index >= AER_NUM_CLASSES)
			continue;
		p = strchr(p + 1, ',');
		if (!p)
			continue;
		name = trim(p + 1);
		if (*name == '"') {
			name++;
			end = strrchr(name, '"');
			if (end)
				*end = '\0';
		}
		snprintf(names[index], AER_NAME_MAX, "%s", name);
		count++;
	}
	fclose(f);
	return count;
}
