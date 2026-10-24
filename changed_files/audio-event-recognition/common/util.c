// SPDX-License-Identifier: MIT
/* Logging, signals, time and command line helpers shared by all modules. */
#define _GNU_SOURCE
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "aer.h"
#include "conf.h"

const char *aer_prog = "aer";
int aer_verbose;
volatile sig_atomic_t aer_quit;

static void vlog(const char *fmt, va_list ap)
{
	/* stderr goes to the journal when started by systemd */
	fprintf(stderr, "%s: ", aer_prog);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

void aer_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

void aer_dbg(const char *fmt, ...)
{
	va_list ap;

	if (!aer_verbose)
		return;
	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

static void on_signal(int sig)
{
	(void)sig;
	aer_quit = 1;
}

void aer_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;	/* no SA_RESTART: poll() must return EINTR */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

int64_t aer_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int64_t aer_mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void aer_format_time(int64_t t_ns, char *buf, size_t len)
{
	time_t sec = t_ns / 1000000000;
	int ms = (int)((t_ns / 1000000) % 1000);
	struct tm tm;
	char date[32], zone[8];

	localtime_r(&sec, &tm);
	strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
	strftime(zone, sizeof(zone), "%z", &tm);	/* +0900 */
	snprintf(buf, len, "%s.%03d%.3s:%.2s", date, ms, zone, zone + 3);
}

void aer_strlcpy(char *dst, const char *src, size_t len)
{
	if (!len)
		return;
	snprintf(dst, len, "%s", src ? src : "");
}

const char *aer_parse_common(int argc, char **argv, const char *usage_extra,
			     const char *extra_opts, int (*extra)(int opt, const char *arg))
{
	const char *conf = AER_DEFAULT_CONF;
	char opts[64];
	const char *slash = strrchr(argv[0], '/');
	int c;

	aer_prog = slash ? slash + 1 : argv[0];
	snprintf(opts, sizeof(opts), "c:vh%s", extra_opts ? extra_opts : "");

	while ((c = getopt(argc, argv, opts)) != -1) {
		switch (c) {
		case 'c':
			conf = optarg;
			break;
		case 'v':
			aer_verbose = 1;
			break;
		case 'h':
		case '?':
			fprintf(stderr,
				"usage: %s [-c config] [-v]%s\n"
				"  -c FILE  configuration file (default %s)\n"
				"  -v       verbose logging\n",
				aer_prog, usage_extra ? usage_extra : "", AER_DEFAULT_CONF);
			exit(c == 'h' ? 0 : 2);
		default:
			if (!extra || extra(c, optarg) < 0)
				exit(2);
		}
	}
	return conf;
}

const char *aer_run_dir(const struct aer_conf *conf)
{
	const char *env = getenv("AER_RUN_DIR");

	if (env && *env)
		return env;
	return aer_conf_get(conf, "general", "run_dir", AER_DEFAULT_RUN_DIR);
}

void aer_device_name(const struct aer_conf *conf, char *buf, size_t len)
{
	const char *name = aer_conf_get(conf, "general", "device_name", "");

	if (*name) {
		aer_strlcpy(buf, name, len);
		return;
	}
	if (gethostname(buf, len) < 0 || !*buf)
		aer_strlcpy(buf, "am62x", len);
	buf[len - 1] = '\0';
}
