/**
 * @file menu.c  Interactive menu
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <time.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "menu"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/** Defines the status modes */
enum statmode {
	STATMODE_CALL = 0,
	STATMODE_OFF,
};


static uint64_t start_ticks;          /**< Ticks when app started         */
static time_t start_time;             /**< Start time of application      */
static struct tmr tmr_alert;          /**< Incoming call alert timer      */
static struct tmr tmr_stat;           /**< Call status timer              */
static enum statmode statmode;        /**< Status mode                    */


static void menu_set_incall(bool incall);
static void update_callstatus(void);


static int print_system_info(struct re_printf *pf, void *arg)
{
	uint32_t uptime;
	int err = 0;

	(void)arg;

	uptime = (uint32_t)((long long)(tmr_jiffies() - start_ticks)/1000);

	err |= re_hprintf(pf, "\n--- System info: ---\n");

	err |= re_hprintf(pf, " Machine:  %s/%s\n", sys_arch_get(),
			  sys_os_get());
	err |= re_hprintf(pf, " Version:  %s\n", sys_libre_version_get());
	err |= re_hprintf(pf, " Build:    %H\n", sys_build_get, NULL);
	err |= re_hprintf(pf, " Kernel:   %H\n", sys_kernel_get, NULL);
	err |= re_hprintf(pf, " Uptime:   %H\n", fmt_human_time, &uptime);
	err |= re_hprintf(pf, " Started:  %s", ctime(&start_time));

#ifdef __VERSION__
	err |= re_hprintf(pf, " Compiler: %s\n", __VERSION__);
#endif

	return err;
}


static int dial_handler(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	int err = 0;

	(void)pf;

	err = ua_connect(uag_cur(), carg->prm, NULL, NULL, VIDMODE_ON);
	if (err) {
		DEBUG_WARNING("connect failed: %m\n", err);
	}

	return err;
}


static int cmd_answer(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;

	ua_answer(uag_cur());

	return 0;
}


static int cmd_hangup(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;

	ua_hangup(uag_cur());

	/* note: must be called after ua_hangup() */
	menu_set_incall(uag_active_calls());

	return 0;
}


static int cmd_ua_next(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;

	uag_next();
	update_callstatus();

	return 0;
}


static int cmd_ua_debug(struct re_printf *pf, void *unused)
{
	(void)unused;
	return ua_debug(pf, uag_cur());
}


static int cmd_print_calls(struct re_printf *pf, void *unused)
{
	(void)unused;
	return ua_print_calls(pf, uag_cur());
}


static const struct cmd cmdv[] = {
	{'M',       0, "Main loop debug",          re_debug             },
	{'\n',      0, "Accept incoming call",     cmd_answer           },
	{'b',       0, "Hangup call",              cmd_hangup           },
	{'c',       0, "Call status",              ua_print_call_status },
	{'d', CMD_PRM, "Dial",                     dial_handler         },
	{'h',       0, "Help menu",                cmd_print            },
	{'i',       0, "SIP debug",                ua_print_sip_status  },
	{'l',       0, "List active calls",        cmd_print_calls      },
	{'m',       0, "Module debug",             mod_debug            },
	{'n',       0, "Network debug",            net_debug            },
	{'r',       0, "Registration info",        ua_print_reg_status  },
	{'s',       0, "System info",              print_system_info    },
	{'t',       0, "Timer debug",              tmr_status           },
	{'u',       0, "UA debug",                 cmd_ua_debug         },
	{'y',       0, "Memory status",            mem_status           },
	{0x1b,      0, "Hangup call",              cmd_hangup           },
	{' ',       0, "Toggle UAs",               cmd_ua_next          },

	{'#', CMD_PRM, NULL,   dial_handler },
	{'*', CMD_PRM, NULL,   dial_handler },
	{'0', CMD_PRM, NULL,   dial_handler },
	{'1', CMD_PRM, NULL,   dial_handler },
	{'2', CMD_PRM, NULL,   dial_handler },
	{'3', CMD_PRM, NULL,   dial_handler },
	{'4', CMD_PRM, NULL,   dial_handler },
	{'5', CMD_PRM, NULL,   dial_handler },
	{'6', CMD_PRM, NULL,   dial_handler },
	{'7', CMD_PRM, NULL,   dial_handler },
	{'8', CMD_PRM, NULL,   dial_handler },
	{'9', CMD_PRM, NULL,   dial_handler },
};


static int call_audio_debug(struct re_printf *pf, void *unused)
{
	(void)unused;
	return audio_debug(pf, call_audio(ua_call(uag_cur())));
}


static int call_audioenc_cycle(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;
	call_audioencoder_cycle(ua_call(uag_cur()));
	return 0;
}


static int call_reinvite(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;
	return call_modify(ua_call(uag_cur()));
}


static int call_mute(struct re_printf *pf, void *unused)
{
	static bool muted = false;
	(void)unused;

	muted = !muted;
	(void)re_hprintf(pf, "\ncall %smuted\n", muted ? "" : "un-");
	audio_mute(call_audio(ua_call(uag_cur())), muted);

	return 0;
}


static int call_xfer(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	(void)pf;

	statmode = STATMODE_OFF;

	return call_transfer(ua_call(uag_cur()), carg->prm);
}


static int call_holdresume(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	(void)pf;

	return call_hold(ua_call(uag_cur()), 'x' == carg->key);
}


#ifdef USE_VIDEO
static int call_videoenc_cycle(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;
	call_videoencoder_cycle(ua_call(uag_cur()));
	return 0;
}


static int call_video_debug(struct re_printf *pf, void *unused)
{
	(void)unused;
	return video_debug(pf, call_video(ua_call(uag_cur())));
}
#endif


static int digit_handler(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	struct call *call;
	int err = 0;

	(void)pf;

	call = ua_call(uag_cur());
	if (call)
		err = call_send_digit(call, carg->key);

	return err;
}


static int toggle_statmode(struct re_printf *pf, void *arg)
{
	(void)pf;
	(void)arg;

	if (statmode == STATMODE_OFF)
		statmode = STATMODE_CALL;
	else
		statmode = STATMODE_OFF;

	return 0;
}


static const struct cmd callcmdv[] = {
	{'I',       0, "Send re-INVITE",      call_reinvite         },
	{'X',       0, "Call resume",         call_holdresume       },
	{'a',       0, "Audio stream",        call_audio_debug      },
	{'e',       0, "Cycle audio encoder", call_audioenc_cycle   },
	{'m',       0, "Call mute/un-mute",   call_mute             },
	{'r', CMD_PRM, "Transfer call",       call_xfer             },
	{'x',       0, "Call hold",           call_holdresume       },

#ifdef USE_VIDEO
	{'E',       0, "Cycle video encoder", call_videoenc_cycle   },
	{'v',       0, "Video stream",        call_video_debug      },
#endif

	{'#',       0, NULL,                  digit_handler         },
	{'*',       0, NULL,                  digit_handler         },
	{'0',       0, NULL,                  digit_handler         },
	{'1',       0, NULL,                  digit_handler         },
	{'2',       0, NULL,                  digit_handler         },
	{'3',       0, NULL,                  digit_handler         },
	{'4',       0, NULL,                  digit_handler         },
	{'5',       0, NULL,                  digit_handler         },
	{'6',       0, NULL,                  digit_handler         },
	{'7',       0, NULL,                  digit_handler         },
	{'8',       0, NULL,                  digit_handler         },
	{'9',       0, NULL,                  digit_handler         },
	{0x00,      0, NULL,                  digit_handler         },

	{'S',       0, "Statusmode toggle",   toggle_statmode       },
};


static void menu_set_incall(bool incall)
{
	/* Dynamic menus */
	if (incall) {
		(void)cmd_register(callcmdv, ARRAY_SIZE(callcmdv));
	}
	else {
		cmd_unregister(callcmdv);
	}
}


static void tmrstat_handler(void *arg)
{
	struct call *call;
	(void)arg;

	/* the UI will only show the current active call */
	call = ua_call(uag_cur());
	if (!call)
		return;

	tmr_start(&tmr_stat, 100, tmrstat_handler, 0);

	if (STATMODE_OFF != statmode) {
		(void)re_fprintf(stderr, "%H\r", call_status, call);
	}
}


static void update_callstatus(void)
{
	/* if there are any active calls, enable the call status view */
	if (uag_active_calls())
		tmr_start(&tmr_stat, 100, tmrstat_handler, 0);
	else
		tmr_cancel(&tmr_stat);
}


static void alert_start(void *arg)
{
	(void)arg;

	ui_output("\033[10;1000]\033[11;1000]\a");

	tmr_start(&tmr_alert, 1000, alert_start, NULL);
}


static void alert_stop(void)
{
	if (tmr_isrunning(&tmr_alert))
		ui_output("\r");

	tmr_cancel(&tmr_alert);
}


static void ua_event_handler(struct ua *ua, enum ua_event ev, const char *prm)
{
	(void)ua;
	(void)prm;

	switch (ev) {

	case UA_EVENT_CALL_INCOMING:
		alert_start(0);
		break;

	case UA_EVENT_CALL_ESTABLISHED:
	case UA_EVENT_CALL_CLOSED:
		alert_stop();
		break;

	default:
		break;
	}

	menu_set_incall(uag_active_calls());
	update_callstatus();
}


static int module_init(void)
{
	int err;

	start_ticks = tmr_jiffies();
	(void)time(&start_time);
	tmr_init(&tmr_alert);
	statmode = STATMODE_CALL;

	err  = cmd_register(cmdv, ARRAY_SIZE(cmdv));
	err |= uag_event_register(ua_event_handler);

	return err;
}


static int module_close(void)
{
	uag_event_unregister(ua_event_handler);
	cmd_unregister(cmdv);

	menu_set_incall(false);
	tmr_cancel(&tmr_alert);
	tmr_cancel(&tmr_stat);

	return 0;
}


const struct mod_export DECL_EXPORTS(menu) = {
	"menu",
	"application",
	module_init,
	module_close
};
