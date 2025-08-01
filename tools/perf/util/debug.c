// SPDX-License-Identifier: GPL-2.0
/* For general debugging purposes */

#include <inttypes.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <api/debug.h>
#include <linux/kernel.h>
#include <linux/time64.h>
#include <sys/time.h>
#ifdef HAVE_BACKTRACE_SUPPORT
#include <execinfo.h>
#endif
#include "addr_location.h"
#include "color.h"
#include "debug.h"
#include "env.h"
#include "event.h"
#include "machine.h"
#include "map.h"
#include "print_binary.h"
#include "srcline.h"
#include "symbol.h"
#include "synthetic-events.h"
#include "target.h"
#include "thread.h"
#include "trace-event.h"
#include "ui/helpline.h"
#include "ui/ui.h"
#include "util/parse-sublevel-options.h"

#include <linux/ctype.h>

#ifdef HAVE_LIBTRACEEVENT
#include <event-parse.h>
#else
#define LIBTRACEEVENT_VERSION 0
#endif

int verbose;
int debug_kmaps;
int debug_peo_args;
bool dump_trace = false, quiet = false;
int debug_ordered_events;
static int redirect_to_stderr;
int debug_data_convert;
static FILE *_debug_file;
bool debug_display_time;
int debug_type_profile;

FILE *debug_file(void)
{
	if (!_debug_file) {
		debug_set_file(stderr);
		pr_warning_once("debug_file not set");
	}
	return _debug_file;
}

void debug_set_file(FILE *file)
{
	_debug_file = file;
}

void debug_set_display_time(bool set)
{
	debug_display_time = set;
}

static int fprintf_time(FILE *file)
{
	struct timeval tod;
	struct tm ltime;
	char date[64];

	if (!debug_display_time)
		return 0;

	if (gettimeofday(&tod, NULL) != 0)
		return 0;

	if (localtime_r(&tod.tv_sec, &ltime) == NULL)
		return 0;

	strftime(date, sizeof(date),  "%F %H:%M:%S", &ltime);
	return fprintf(file, "[%s.%06lu] ", date, (long)tod.tv_usec);
}

int veprintf(int level, int var, const char *fmt, va_list args)
{
	int ret = 0;

	if (var >= level) {
		if (use_browser >= 1 && !redirect_to_stderr) {
			ui_helpline__vshow(fmt, args);
		} else {
			ret = fprintf_time(debug_file());
			ret += vfprintf(debug_file(), fmt, args);
		}
	}

	return ret;
}

int eprintf(int level, int var, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = veprintf(level, var, fmt, args);
	va_end(args);

	return ret;
}

static int veprintf_time(u64 t, const char *fmt, va_list args)
{
	int ret = 0;
	u64 secs, usecs, nsecs = t;

	secs   = nsecs / NSEC_PER_SEC;
	nsecs -= secs  * NSEC_PER_SEC;
	usecs  = nsecs / NSEC_PER_USEC;

	ret = fprintf(debug_file(), "[%13" PRIu64 ".%06" PRIu64 "] ", secs, usecs);
	ret += vfprintf(debug_file(), fmt, args);
	return ret;
}

int eprintf_time(int level, int var, u64 t, const char *fmt, ...)
{
	int ret = 0;
	va_list args;

	if (var >= level) {
		va_start(args, fmt);
		ret = veprintf_time(t, fmt, args);
		va_end(args);
	}

	return ret;
}

/*
 * Overloading libtraceevent standard info print
 * function, display with -v in perf.
 */
void pr_stat(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	veprintf(1, verbose, fmt, args);
	va_end(args);
	eprintf(1, verbose, "\n");
}

int dump_printf(const char *fmt, ...)
{
	va_list args;
	int ret = 0;

	if (dump_trace) {
		va_start(args, fmt);
		ret = vprintf(fmt, args);
		va_end(args);
	}

	return ret;
}

static int trace_event_printer(enum binary_printer_ops op,
			       unsigned int val, void *extra, FILE *fp)
{
	const char *color = PERF_COLOR_BLUE;
	union perf_event *event = (union perf_event *)extra;
	unsigned char ch = (unsigned char)val;
	int printed = 0;

	switch (op) {
	case BINARY_PRINT_DATA_BEGIN:
		printed += fprintf(fp, ".");
		printed += color_fprintf(fp, color, "\n. ... raw event: size %d bytes\n",
					 event->header.size);
		break;
	case BINARY_PRINT_LINE_BEGIN:
		printed += fprintf(fp, ".");
		break;
	case BINARY_PRINT_ADDR:
		printed += color_fprintf(fp, color, "  %04x: ", val);
		break;
	case BINARY_PRINT_NUM_DATA:
		printed += color_fprintf(fp, color, " %02x", val);
		break;
	case BINARY_PRINT_NUM_PAD:
		printed += color_fprintf(fp, color, "   ");
		break;
	case BINARY_PRINT_SEP:
		printed += color_fprintf(fp, color, "  ");
		break;
	case BINARY_PRINT_CHAR_DATA:
		printed += color_fprintf(fp, color, "%c",
			      isprint(ch) && isascii(ch) ? ch : '.');
		break;
	case BINARY_PRINT_CHAR_PAD:
		printed += color_fprintf(fp, color, " ");
		break;
	case BINARY_PRINT_LINE_END:
		printed += color_fprintf(fp, color, "\n");
		break;
	case BINARY_PRINT_DATA_END:
		printed += fprintf(fp, "\n");
		break;
	default:
		break;
	}

	return printed;
}

void trace_event(union perf_event *event)
{
	unsigned char *raw_event = (void *)event;

	if (!dump_trace)
		return;

	print_binary(raw_event, event->header.size, 16,
		     trace_event_printer, event);
}

static struct sublevel_option debug_opts[] = {
	{ .name = "verbose",		.value_ptr = &verbose },
	{ .name = "ordered-events",	.value_ptr = &debug_ordered_events},
	{ .name = "stderr",		.value_ptr = &redirect_to_stderr},
	{ .name = "data-convert",	.value_ptr = &debug_data_convert },
	{ .name = "perf-event-open",	.value_ptr = &debug_peo_args },
	{ .name = "kmaps",		.value_ptr = &debug_kmaps },
	{ .name = "type-profile",	.value_ptr = &debug_type_profile },
	{ .name = NULL, }
};

int perf_debug_option(const char *str)
{
	int ret;

	ret = perf_parse_sublevel_options(str, debug_opts);
	if (ret)
		return ret;

	/* Allow only verbose value in range (0, 10), otherwise set 0. */
	verbose = (verbose < 0) || (verbose > 10) ? 0 : verbose;

#if LIBTRACEEVENT_VERSION >= MAKE_LIBTRACEEVENT_VERSION(1, 3, 0)
	if (verbose == 1)
		tep_set_loglevel(TEP_LOG_INFO);
	else if (verbose == 2)
		tep_set_loglevel(TEP_LOG_DEBUG);
	else if (verbose >= 3)
		tep_set_loglevel(TEP_LOG_ALL);
#endif
	return 0;
}

int perf_quiet_option(void)
{
	struct sublevel_option *opt = &debug_opts[0];

	/* disable all debug messages */
	while (opt->name) {
		*opt->value_ptr = -1;
		opt++;
	}

	/* For debug variables that are used as bool types, set to 0. */
	redirect_to_stderr = 0;
	debug_peo_args = 0;
	debug_kmaps = 0;
	debug_type_profile = 0;

	return 0;
}

#define DEBUG_WRAPPER(__n, __l)				\
static int pr_ ## __n ## _wrapper(const char *fmt, ...)	\
{							\
	va_list args;					\
	int ret;					\
							\
	va_start(args, fmt);				\
	ret = veprintf(__l, verbose, fmt, args);	\
	va_end(args);					\
	return ret;					\
}

DEBUG_WRAPPER(warning, 0);
DEBUG_WRAPPER(debug, 1);

void perf_debug_setup(void)
{
	debug_set_file(stderr);
	libapi_set_print(pr_warning_wrapper, pr_warning_wrapper, pr_debug_wrapper);
}

void __dump_stack(FILE *file, void **stackdump, size_t stackdump_size)
{
	/* TODO: async safety. printf, malloc, etc. aren't safe inside a signal handler. */
	pid_t pid = getpid();
	struct machine *machine;
	struct thread *thread = NULL;
	struct perf_env host_env;

	perf_env__init(&host_env);
	machine = machine__new_live(&host_env, /*kernel_maps=*/false, pid);

	if (machine)
		thread = machine__find_thread(machine, pid, pid);

#ifdef HAVE_BACKTRACE_SUPPORT
	if (!machine || !thread) {
		/*
		 * Backtrace functions are async signal safe. Fall back on them
		 * if machine/thread creation fails.
		 */
		backtrace_symbols_fd(stackdump, stackdump_size, fileno(file));
		machine__delete(machine);
		perf_env__exit(&host_env);
		return;
	}
#endif

	for (size_t i = 0; i < stackdump_size; i++) {
		struct addr_location al;
		u64 addr = (u64)(uintptr_t)stackdump[i];
		bool printed = false;

		addr_location__init(&al);
		if (thread && thread__find_map(thread, PERF_RECORD_MISC_USER, addr, &al)) {
			al.sym = map__find_symbol(al.map, al.addr);
			if (al.sym) {
				fprintf(file, "    #%zd %p in %s ", i, stackdump[i], al.sym->name);
				printed = true;
			}
		}
		if (!printed)
			fprintf(file, "    #%zd %p ", i, stackdump[i]);

		map__fprintf_srcline(al.map, al.addr, "", file);
		fprintf(file, "\n");
		addr_location__exit(&al);
	}
	thread__put(thread);
	machine__delete(machine);
	perf_env__exit(&host_env);
}

/* Obtain a backtrace and print it to stdout. */
#ifdef HAVE_BACKTRACE_SUPPORT
void dump_stack(void)
{
	void *stackdump[32];
	size_t size = backtrace(stackdump, ARRAY_SIZE(stackdump));

	__dump_stack(stdout, stackdump, size);
}
#else
void dump_stack(void) {}
#endif

void sighandler_dump_stack(int sig)
{
	psignal(sig, "perf");
	dump_stack();
	signal(sig, SIG_DFL);
	raise(sig);
}
