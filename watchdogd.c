/* A small userspace watchdog daemon
 *
 * Copyright (C) 2008 Michele d'Amico <michele.damico@fitre.it>
 * Copyright (C) 2008 Mike Frysinger <vapier@gentoo.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <paths.h>
#include <syslog.h>

#define WDT_DEVNODE          "/dev/watchdog"
#define WDT_TIMEOUT_DEFAULT  20
#define WDT_KICK_DEFAULT     (WDT_TIMEOUT_DEFAULT / 2)

#define UNUSED(arg) arg __attribute__((unused))
#define print(prio, fmt, args...)  (sys_log ? syslog(prio, fmt, ##args) : fprintf(stderr, fmt, ##args))
#define ERROR(fmt, args...)                   print(LOG_DAEMON | LOG_ERR, "%s: " fmt, __progname, ##args)
#define PERROR(fmt, args...)                  print(LOG_DAEMON | LOG_ERR, "%s: " fmt ": %s\n", __progname, ##args, strerror(errno))
#define DEBUG(fmt, args...) do { if (verbose) print(LOG_DAEMON | LOG_DEBUG, "%s: " fmt, __progname, ##args); } while(0)

int fd      = -1;
int verbose = 0;
int sys_log = 0;
extern char *__progname;

int daemonize(char *output);


/*
 * This function simply sends an IOCTL to the driver, which in turn ticks
 * the PC Watchdog card to reset its internal timer so it doesn't trigger
 * a computer reset.
 */
static void wdt_kick(void)
{
	int dummy;

	DEBUG("Kicking watchdog.\n");
	ioctl(fd, WDIOC_KEEPALIVE, &dummy);
}

static void wdt_set_timeout(int count)
{
	int arg = count;

	DEBUG("Setting watchdog timeout to %d sec.\n", count);
	if (ioctl(fd, WDIOC_SETTIMEOUT, &arg))
		PERROR("Failed setting HW watchdog timeout");
	else
		DEBUG("Previous timeout was %d sec\n", arg);
}

static int wdt_get_timeout(void)
{
	int count;
	int err;

	if ((err = ioctl(fd, WDIOC_GETTIMEOUT, &count)))
		count = err;

	DEBUG("Watchdog timeout is set to %d sec.\n", count);

	return count;
}

static void wdt_magic_close(int UNUSED(signo))
{
	if (fd != -1) {
		DEBUG("Safe exit, disabling HW watchdog.\n");
		write(fd, "V", 1);
		close(fd);
	}
	exit(0);
}

static void setup_magic_close(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = wdt_magic_close;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static int usage(int status)
{
	printf("Usage: %s [-f] [-w <sec>] [-k <sec>] [-s] [-h|--help]\n"
               "A simple watchdog deamon that kicks /dev/watchdog every %d sec, by default.\n"
               "Options:\n"
               "  --foreground, -f       Start in foreground (background is default)\n"
	       "  --logfile, -l <file>   Log to <file> when backgrounding, otherwise silent\n"
               "  --timeout, -w <sec>    Set the HW watchdog timeout to <sec> seconds\n"
               "  --interval, -k <sec>   Set watchdog kick interval to <sec> seconds\n"
               "  --safe-exit, -s        Disable watchdog on exit from SIGINT/SIGTERM\n"
	       "  --verbose, -V          Verbose operation, noisy output suitable for debugging\n"
	       "  --version, -v          Display daemon version and exit\n"
               "  --help, -h             Display this help message and exit\n",
               __progname, WDT_TIMEOUT_DEFAULT);

	return status;
}

int main(int argc, char *argv[])
{
	int timeout = WDT_TIMEOUT_DEFAULT;
	int real_timeout = 0;
	int period = -1;
	int background = 1;
	int c;
	char *logfile = NULL;
	struct option long_options[] = {
		{"foreground", 0, 0, 'f'},
		{"logfile",    1, 0, 'l'},
		{"timeout",    1, 0, 'w'},
		{"interval",   1, 0, 'k'},
		{"safe-exit",  0, 0, 's'},
		{"verbose",    0, 0, 'V'},
		{"version",    0, 0, 'v'},
		{"help",       0, 0, 'h'},
		{NULL, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "fl:w:k:sVvh?", long_options, NULL)) != EOF) {
		switch (c) {
		case 'f':	/* Run in foreground */
			background = 0;
			break;

		case 'l':	/* Log to file */
			if (!optarg) {
				ERROR("Missing logfile argument.\n");
				return usage(1);
			}
			logfile = strdup(optarg);
			break;

		case 'w':	/* Watchdog timeout */
			timeout = atoi(optarg);
			break;

		case 'k':	/* Watchdog kick interval */
			period = atoi(optarg);
			break;

		case 's':	/* Safe exit, i.e., don't reboot if we exit and close device */
			setup_magic_close();
			break;

		case 'v':
			printf("v%s\n", VERSION);
			return 0;

		case 'V':
			verbose = 1;
			break;

		case 'h':
			return usage(0);

		default:
			printf("Unrecognized option \"-%c\".\n", c);
			return usage(1);
		}
	}

	if (background) {
		pid_t pid;

		/* If backgrounding and no logfile is given, use syslog */
		if (!logfile)
			sys_log = 1;

		/* Exit on parent or error. */
		pid = daemonize(logfile);
		if (pid)
			exit(pid < 0 ? 1 : 0);

		DEBUG("Starting in deamon mode.\n");
	}

	fd = open(WDT_DEVNODE, O_WRONLY);
	if (fd == -1) {
		PERROR("Failed opening watchdog device, %s", WDT_DEVNODE);
		exit(1);
	}

	wdt_set_timeout(timeout);

	real_timeout = wdt_get_timeout();
	if (real_timeout < 0) {
		PERROR("Failed reading current watchdog timeout");
	} else {
		if (real_timeout <= period) {
			ERROR("Warning, watchdog timeout <= kick interval: %d <= %d\n",
			      real_timeout, period);
		}
	}

	/* If user did not provide '-k' argument, set to half actual timeout */
	if (-1 == period) {
		if (real_timeout < 0)
			period = WDT_KICK_DEFAULT;
		else
			period = real_timeout / 2;
	}
	DEBUG("Watchdog kick interval set to %d sec.\n", period);

	while (1) {
		wdt_kick();
		sleep(period);
	}
}

/**
 * Local Variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  version-control: t
 * End:
 */
