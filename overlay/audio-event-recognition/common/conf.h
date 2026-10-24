// SPDX-License-Identifier: MIT
/*
 * Minimal INI reader for /etc/aer/aer.conf.
 *
 *   # comment (only at the start of a line)
 *   [section]
 *   key = value
 *
 * Values run to the end of the line, so they may contain ',', ';' or '#'.
 */
#ifndef AER_CONF_H
#define AER_CONF_H

#include <stddef.h>

struct aer_conf;

/* Never returns NULL: a missing file gives an empty config (all defaults). */
struct aer_conf *aer_conf_load(const char *path);
void aer_conf_free(struct aer_conf *conf);

const char *aer_conf_get(const struct aer_conf *conf, const char *section,
			 const char *key, const char *def);
long aer_conf_int(const struct aer_conf *conf, const char *section,
		  const char *key, long def);
double aer_conf_double(const struct aer_conf *conf, const char *section,
		       const char *key, double def);

/* Sections in file order, without duplicates. */
size_t aer_conf_nsections(const struct aer_conf *conf);
const char *aer_conf_section(const struct aer_conf *conf, size_t i);

#endif
