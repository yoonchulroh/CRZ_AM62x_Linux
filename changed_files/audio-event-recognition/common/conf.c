// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aer.h"
#include "conf.h"

struct entry {
	char *section, *key, *value;
};

struct aer_conf {
	struct entry *e;
	size_t n, cap;
	char **sections;
	size_t nsec;
};

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

static void add_section(struct aer_conf *c, const char *name)
{
	for (size_t i = 0; i < c->nsec; i++)
		if (!strcmp(c->sections[i], name))
			return;
	c->sections = realloc(c->sections, (c->nsec + 1) * sizeof(*c->sections));
	c->sections[c->nsec++] = strdup(name);
}

static void add_entry(struct aer_conf *c, const char *sec, const char *key, const char *val)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 32;
		c->e = realloc(c->e, c->cap * sizeof(*c->e));
	}
	c->e[c->n].section = strdup(sec);
	c->e[c->n].key = strdup(key);
	c->e[c->n].value = strdup(val);
	c->n++;
}

struct aer_conf *aer_conf_load(const char *path)
{
	struct aer_conf *c = calloc(1, sizeof(*c));
	char section[128] = "general";
	char *line = NULL;
	size_t cap = 0;
	int lineno = 0;
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		aer_log("config %s: %s, using defaults", path, strerror(errno));
		return c;
	}
	while (getline(&line, &cap, f) > 0) {
		char *s = trim(line), *eq;

		lineno++;
		if (!*s || *s == '#' || *s == ';')
			continue;
		if (*s == '[') {
			char *end = strchr(s, ']');

			if (!end) {
				aer_log("%s:%d: missing ']'", path, lineno);
				continue;
			}
			*end = '\0';
			aer_strlcpy(section, trim(s + 1), sizeof(section));
			add_section(c, section);
			continue;
		}
		eq = strchr(s, '=');
		if (!eq) {
			aer_log("%s:%d: expected key = value", path, lineno);
			continue;
		}
		*eq = '\0';
		add_entry(c, section, trim(s), trim(eq + 1));
	}
	free(line);
	fclose(f);
	return c;
}

void aer_conf_free(struct aer_conf *c)
{
	if (!c)
		return;
	for (size_t i = 0; i < c->n; i++) {
		free(c->e[i].section);
		free(c->e[i].key);
		free(c->e[i].value);
	}
	for (size_t i = 0; i < c->nsec; i++)
		free(c->sections[i]);
	free(c->e);
	free(c->sections);
	free(c);
}

const char *aer_conf_get(const struct aer_conf *c, const char *section,
			 const char *key, const char *def)
{
	/* last assignment wins */
	for (size_t i = c ? c->n : 0; i-- > 0;)
		if (!strcmp(c->e[i].section, section) && !strcmp(c->e[i].key, key))
			return c->e[i].value;
	return def;
}

long aer_conf_int(const struct aer_conf *c, const char *section, const char *key, long def)
{
	const char *v = aer_conf_get(c, section, key, NULL);
	char *end;
	long r;

	if (!v || !*v)
		return def;
	r = strtol(v, &end, 0);
	if (*end) {
		aer_log("config [%s] %s: '%s' is not an integer", section, key, v);
		return def;
	}
	return r;
}

double aer_conf_double(const struct aer_conf *c, const char *section, const char *key, double def)
{
	const char *v = aer_conf_get(c, section, key, NULL);
	char *end;
	double r;

	if (!v || !*v)
		return def;
	r = strtod(v, &end);
	if (*end) {
		aer_log("config [%s] %s: '%s' is not a number", section, key, v);
		return def;
	}
	return r;
}

size_t aer_conf_nsections(const struct aer_conf *c)
{
	return c ? c->nsec : 0;
}

const char *aer_conf_section(const struct aer_conf *c, size_t i)
{
	return i < c->nsec ? c->sections[i] : NULL;
}
