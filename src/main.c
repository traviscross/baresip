/**
 * @file main.c  Main application code
 *
 * Copyright (C) 2010 - 2011 Creytiv.com
 */
#ifdef SOLARIS
#define __EXTENSIONS__ 1
#endif
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if defined(DARWIN) && defined(USE_SDL)
#include <SDL/SDL_main.h>
#endif
#include <string.h>
#ifdef HAVE_GETOPT
#include <getopt.h>
#endif
#ifdef HAVE_SYSLOG
#define __USE_BSD 1
#include <syslog.h>
#endif
#include <re.h>
#include <baresip.h>
#include "core.h"
#include "version.h"


#define DEBUG_MODULE "main"
#define DEBUG_LEVEL 6
#include <re_dbg.h>


static struct {
	const char *peeruri;  /**< Optional Peer URI               */
	uint32_t n_uas;       /**< Number of User Agents           */
	bool prefer_ipv6;     /**< Prefer IPv6 transport flag      */
	bool run_daemon;      /**< Run process as daemon flag      */
	bool terminating;     /**< Application is terminating flag */
} app;


static const char software[] = "baresip v" VERSION " (" ARCH "/" OS ")";


static void quit(int ret);


#ifdef HAVE_SYSLOG
static void syslog_handler(int level, const char *p, size_t len, void *arg)
{
	(void)arg;

	syslog(level | LOG_LOCAL0, "%.*s", (int)len, p);
}
#endif


static void ua_exit_handler(int err)
{
	(void)err;
	re_cancel();
}


static int ua_add(const struct pl *addr)
{
	struct sip_addr sip_addr;
	struct ua *ua;
	int err;

	if (0 == sip_addr_decode(&sip_addr, addr)) {
		err = natbd_init(net_laddr(), &sip_addr.uri.host,
				 sip_addr.uri.port, net_dnsc());
		if (err)
			return err;
	}

	err = ua_alloc(&ua, addr);
	if (err)
		return err;

	/* skip status-line in daemon mode */
	if (app.run_daemon)
		ua_set_statmode(ua, STATMODE_OFF);

	return err;
}


static int app_init(void)
{
	bool ansi = !app.run_daemon;
	char uuid[37] = "";
	uint32_t n;
	int err;

#ifdef __SYMBIAN32__
	dbg_logfile_set("c:\\data\\baresip.log");
#endif

	/* Initialise System library */
	err = libre_init();
	if (err)
		return err;

	/* Initialise debugging */
#ifdef __SYMBIAN32__
	ansi = false;
#elif defined(WIN32) && !defined(CYGWIN)
	ansi = false;
#endif
	dbg_init(DBG_INFO, ansi ? DBG_ANSI : DBG_NONE);

#ifdef HAVE_SYSLOG
	if (app.run_daemon)
		dbg_handler_set(syslog_handler, NULL);
#endif

	/* Initialise dynamic modules */
	mod_init();

	/* Initialise User Interface */
	err = ui_init(quit, app.run_daemon);
	if (err)
		return err;

	/* Parse configuration */
	err = configure();
	if (err) {
		DEBUG_WARNING("configure: %s\n", strerror(err));
		return err;
	}

	n = list_count(aucodec_list());
	(void)re_fprintf(stderr, "Populated %u audio codec%s\n",
			 n, 1==n?"":"s");
	if (0 == n) {
		DEBUG_WARNING("No audio-codec modules loaded!\n");
	}
	n = list_count(vidcodec_list());
	(void)re_fprintf(stderr, "Populated %u video codec%s\n",
			 n, 1==n?"":"s");
	n = list_count(aufilt_list());
	(void)re_fprintf(stderr, "Populated %u audio filter%s\n",
			 n, 1==n?"":"s");

	/* Initialise Network */
	err = net_init(app.prefer_ipv6);
	if (err) {
		DEBUG_WARNING("network init failed (%s)\n", strerror(err));
		return err;
	}

	/* Initialise User Agents */
	err = ua_init(software, true, true, true, ua_exit_handler);
	if (err)
		return err;

	/* UUID */
	if (0 == uuid_load(uuid, sizeof(uuid)))
		ua_set_uuid(uuid);

	return 0;
}


static void net_change_handler(void *arg)
{
	(void)arg;

	re_printf("IP-address changed: %j\n", net_laddr());

	ua_reset_transp(true, true);
}


static int app_start(void)
{
	uint32_t n;
	int err;

	/* Populate SIP accounts */
	err = conf_accounts_get(ua_add, &app.n_uas);
	if (err)
		return err;
	if (!app.n_uas) {
		DEBUG_WARNING("No SIP accounts found - check your config\n");
		return ENOENT;
	}
	(void)re_fprintf(stderr, "Populated %u account%s\n",
			 app.n_uas, 1==app.n_uas?"":"s");

	/* Start all User-Agents */
	err = ua_start_all();
	if (err)
		return err;

	/* Populate contacts */
	contact_init();
	n = 0;
	err = conf_contacts_get(contact_add, &n);
	if (err) {
		DEBUG_WARNING("Could not get contacts (%s)\n", strerror(err));
		return err;
	}
	else {
		(void)re_fprintf(stderr, "Populated %u contact%s\n",
				 n, 1==n?"":"s");
	}

	err = ui_start();
	if (err)
		return err;

	net_change(60, net_change_handler, NULL);

	return 0;
}


static void app_close(void)
{
	natbd_close();
	ua_close();
	ui_close();
	contact_close();
	net_close();
	play_close();
	audio_loop_test(true);
#ifdef USE_VIDEO
	video_loop_test(true);
#endif
	mod_close();
	libre_close();

	/* Check for memory leaks */
	tmr_debug();
	mem_debug();
}


static void quit(int sig)
{
	if (!app.terminating) {
		ua_stop_all(false);
		app.terminating = true;
		return;
	}

	re_fprintf(stderr, "forced exit - signal %d\n", sig);

	ua_stop_all(true);
}


static void signal_handler(int sig)
{
	re_fprintf(stderr, "terminated by signal %d\n", sig);
	quit(sig);
}


#ifdef HAVE_GETOPT
static void usage(void)
{
	(void)re_fprintf(stderr, "Usage: baresip [options]\n");
	(void)re_fprintf(stderr, "options:\n");
	(void)re_fprintf(stderr, "\t-h -?            Help\n");
	(void)re_fprintf(stderr, "\t-6               Prefer IPv6\n");
	(void)re_fprintf(stderr, "\t-d               Daemon\n");
	(void)re_fprintf(stderr, "\t-f <path>        Config path\n");
	(void)re_fprintf(stderr, "\t-m <threshold>   Set heap threshold\n");
	(void)re_fprintf(stderr, "\t-p <Peer URI>    Call Peer on start\n");
	(void)re_fprintf(stderr, "\t-e <commands>    Execute commands\n");
}
#endif


static int stderr_handler(const char *p, size_t size, void *arg)
{
	(void)arg;

	if (1 != fwrite(p, size, 1, stderr))
		return ENOMEM;

	return 0;
}


/**
 * Main entry point
 */
int main(int argc, char *argv[])
{
	const char *exec = NULL;
	int err;

	(void)re_fprintf(stderr, "baresip v%s"
			 " Copyright (C) 2010 - 2011"
			 " Alfred E. Heggestad <aeh@db.org>\n",
			 VERSION);

	err = sys_coredump_set(true);
	if (err) {
		DEBUG_NOTICE("could not enable coredump: %s\n", strerror(err));
	}

#ifdef HAVE_GETOPT
	for (;;) {
		const int c = getopt(argc, argv, "6dhf:m:p:e:");
		if (0 > c)
			break;

		switch (c) {

		case '?':
		case 'h':
			usage();
			return -2;

		case '6':
			app.prefer_ipv6 = true;
			break;

		case 'd':
			app.run_daemon = true;
			break;

		case 'f':
			conf_path_set(optarg);
			break;

		case 'm':
			mem_threshold_set(atoi(optarg));
			break;

		case 'p':
			app.peeruri = optarg;
			break;

		case 'e':
			exec = optarg;
			break;

		default:
			break;
		}
	}
#else
	(void)argc;
	(void)argv;
#endif

	err = app_init();
	if (err) {
		DEBUG_WARNING("app_init failed (%s)\n", strerror(err));
		goto out;
	}

	err = app_start();
	if (err) {
		DEBUG_WARNING("app_start failed (%s)\n", strerror(err));
		goto out;
	}

	if (app.run_daemon) {
		DEBUG_NOTICE("Starting in daemon mode\n");
		err = sys_daemon();
		if (err) {
			DEBUG_WARNING("daemon: %s\n", strerror(err));
			goto out;
 		}
	}

	/* Automatically call peer uri if set */
	if (app.peeruri) {
		err = ua_connect(ua_cur(), app.peeruri, NULL, VIDMODE_ON);
		if (err) {
			DEBUG_WARNING("connect failed: %s\n", strerror(err));
		}
	}

	if (exec) {
		struct re_printf pf;

		pf.vph = stderr_handler;

		while (*exec)
			ui_input(*exec++, &pf);
	}

	/* Main loop */
	while (!app.terminating) {

		err = re_main(signal_handler);

		if (!app.terminating)
			ua_stack_resume(software, true, true, true);
	}

 out:
	app_close();

	if (err) {
		DEBUG_WARNING("main exit with error (%s)\n", strerror(err));
	}

	return err;
}
