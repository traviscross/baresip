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


static uint64_t start_ticks;          /**< Ticks when app started         */
static time_t start_time;             /**< Start time of application      */


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


static int codec_status(struct re_printf *pf, void *unused)
{
	int err;

	(void)unused;

	err  = aucodec_debug(pf, aucodec_list());
	err |= vidcodec_debug(pf, vidcodec_list());

	return err;
}


static int dial_handler(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	int err = 0;

	(void)pf;

	err = ua_connect(ua_cur(), carg->prm, NULL, NULL, VIDMODE_ON);
	if (err) {
		DEBUG_WARNING("connect failed: %m\n", err);
	}

	return err;
}


static int cmd_answer(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;

	ua_answer(ua_cur());

	return 0;
}


static int cmd_hangup(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;

	ua_hangup(ua_cur());

	return 0;
}


static const struct cmd cmdv[] = {
	{'M',       0, "Main loop debug",          re_debug             },
	{'\n',      0, "Accept incoming call",     cmd_answer           },
	{'b',       0, "Hangup call",              cmd_hangup           },
	{'c',       0, "Call status",              ua_print_call_status },
	{'d', CMD_PRM, "Dial",                     dial_handler         },
	{'e',       0, "Codec status",             codec_status         },
	{'f',       0, "Audio Filters",            aufilt_debug         },
	{'h',       0, "Help menu",                cmd_print            },
	{'i',       0, "SIP debug",                ua_print_sip_status  },
	{'m',       0, "Module debug",             mod_debug            },
	{'n',       0, "Network debug",            net_debug            },
	{'r',       0, "Registration info",        ua_print_reg_status  },
	{'s',       0, "System info",              print_system_info    },
	{'t',       0, "Timer debug",              tmr_status           },
	{'y',       0, "Memory status",            mem_status           },
	{0x1b,      0, "Hangup call",              cmd_hangup           },

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


static int module_init(void)
{
	start_ticks = tmr_jiffies();
	(void)time(&start_time);

	return cmd_register(cmdv, ARRAY_SIZE(cmdv));
}


static int module_close(void)
{
	cmd_unregister(cmdv);
	return 0;
}


const struct mod_export DECL_EXPORTS(menu) = {
	"menu",
	"application",
	module_init,
	module_close
};
