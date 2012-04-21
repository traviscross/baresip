/**
 * @file ui.c  User Interface
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <time.h>
#include <re.h>
#include <baresip.h>
#include "core.h"
#include "version.h"


#define DEBUG_MODULE "ui"
#define DEBUG_LEVEL 6      /* keep 6 for green splash */
#include <re_dbg.h>


/** User Interface */
struct ui {
	struct le le;
	const char *name;
	struct ui_st *st; /* only one instance */
	ui_alloc_h  *alloch;
	ui_output_h *outputh;
};

typedef int (ui_state_h)(char key, struct re_printf *pf);
typedef int (ui_help_h)(struct re_printf *pf);


static struct {
	struct list l;                 /**< List of UIs (struct ui)        */
	struct mbuf dialbuf;           /**< Buffer for dial string         */
	struct mbuf contactbuf;        /**< Buffer for contact search      */
	struct mbuf chatpeer;          /**< Buffer for chat mode peer      */
	struct mbuf chatmsg;           /**< Buffer for chat message        */
	uint64_t start_ticks;          /**< Ticks when app started         */
	time_t start_time;             /**< Start time of application      */
	exit_h *exith;                 /**< Exit handler                   */
	bool run_daemon;               /**< Run as daemon                  */
	bool terminated;               /**< App is terminated              */
} uig;


/* UI State machine */
static int ui_idle(char key, struct re_printf *pf);
static int ui_edit(char key, struct re_printf *pf);
static int ui_chat(char key, struct re_printf *pf);
static int ui_contact(char key, struct re_printf *pf);
static int ui_call(char key, struct re_printf *pf);
static int help_idle(struct re_printf *pf);
static int help_call(struct re_printf *pf);

/** User Interface view */
struct ui_view {
	ui_state_h *stateh;
	ui_help_h *helph;
};

enum view {
	VIEW_IDLE = 0,
	VIEW_EDIT,
	VIEW_CHAT,
	VIEW_CONTACT,
	VIEW_CALL,

	VIEW_NR
};

static const struct ui_view ui_views[VIEW_NR] = {
	{ui_idle,    help_idle},
	{ui_edit,    help_idle},
	{ui_chat,    help_idle},
	{ui_contact, help_idle},
	{ui_call,    help_call}
};
static const struct ui_view *ui_view = &ui_views[VIEW_IDLE];


static void ui_print_editor(void)
{
	(void)re_fprintf(stderr, "\r> %32b", uig.dialbuf.buf, uig.dialbuf.end);
}


static void ui_view_set(enum view view)
{
	if (view >= VIEW_NR)
		return;

	ui_view = &ui_views[view];
}


static int help_idle(struct re_printf *pf)
{
	int err;

	err  = re_hprintf(pf, "--- Help ---\n");
	err |= re_hprintf(pf, " /     - Search contacts\n");
	err |= re_hprintf(pf, " a  A  - Audio-loop toggle\n");
	err |= re_hprintf(pf, " b ESC - Hangup\n");
	err |= re_hprintf(pf, " C     - List contacts\n");
	err |= re_hprintf(pf, " SPACE - Toggle UA\n");
	err |= re_hprintf(pf, " ENTER - Accept incoming call\n");
	err |= re_hprintf(pf, " c     - Call status\n");
	err |= re_hprintf(pf, " d     - Dial\n");
	err |= re_hprintf(pf, " e     - Codec status\n");
	err |= re_hprintf(pf, " f     - Audio Filter Chain\n");
	err |= re_hprintf(pf, " h     - Help menu\n");
	err |= re_hprintf(pf, " i     - SIP debug\n");
	err |= re_hprintf(pf, " m     - Module debug\n");
	err |= re_hprintf(pf, " M     - Main loop debug\n");
	err |= re_hprintf(pf, " n     - Network debug\n");
	err |= re_hprintf(pf, " o  O  - Send OPTIONS to all contacts\n");
	err |= re_hprintf(pf, " P     - Polling method toggle\n");
	err |= re_hprintf(pf, " q     - Quit\n");
	err |= re_hprintf(pf, " r     - Registration status\n");
	err |= re_hprintf(pf, " s     - System info\n");
	err |= re_hprintf(pf, " t     - Timer debug\n");
	err |= re_hprintf(pf, " u     - UA debug\n");
#ifdef USE_VIDEO
	err |= re_hprintf(pf, " v  V  - Video-loop toggle\n");
#endif
	err |= re_hprintf(pf, " w     - Play sine wave\n");
	err |= re_hprintf(pf, " y     - Memory status\n");
	err |= re_hprintf(pf, "\n");

	return err;
}


static int help_call(struct re_printf *pf)
{
	int err;

	err  = re_hprintf(pf, "--- Help ---\n");
	err |= re_hprintf(pf, " SPACE - Toggle UA\n");
	err |= re_hprintf(pf, " a     - Audio stream\n");
	err |= re_hprintf(pf, " b ESC - Hangup\n");
	err |= re_hprintf(pf, " c     - Call status\n");
	err |= re_hprintf(pf, " e     - Cycle audio encoder\n");
#ifdef USE_VIDEO
	err |= re_hprintf(pf, " E     - Cycle video encoder\n");
#endif
	err |= re_hprintf(pf, " f     - Audio Filter Chain\n");
	err |= re_hprintf(pf, " h     - Help menu\n");
	err |= re_hprintf(pf, " i     - SIP debug\n");
	err |= re_hprintf(pf, " I     - Send re-INVITE\n");
	err |= re_hprintf(pf, " m     - Call mute/un-mute\n");
	err |= re_hprintf(pf, " M     - Main loop debug\n");
	err |= re_hprintf(pf, " n     - Network debug\n");
	err |= re_hprintf(pf, " q     - Quit\n");
	err |= re_hprintf(pf, " S     - Toggle status-mode\n");
	err |= re_hprintf(pf, " t     - Timer debug\n");
	err |= re_hprintf(pf, " u     - UA debug\n");
	err |= re_hprintf(pf, " v     - Video stream\n");
	err |= re_hprintf(pf, " x     - Call hold\n");
	err |= re_hprintf(pf, " X     - Call resume\n");
	err |= re_hprintf(pf, " y     - Memory status\n");
	err |= re_hprintf(pf, "\n");

	return err;
}


static int ui_edit(char key, struct re_printf *pf)
{
	char str[256];
	int err;

	(void)pf;

	switch (key) {

	case '\n':
		(void)re_fprintf(stderr, "\n");
		ui_view_set(VIEW_IDLE);
		uig.dialbuf.pos = 0;
		(void)mbuf_read_str(&uig.dialbuf, str, uig.dialbuf.end);
		str[uig.dialbuf.end] = '\0';
		err = ua_connect(ua_cur(), str, NULL, NULL, VIDMODE_ON);
		if (err) {
			DEBUG_WARNING("connect failed: %s\n", strerror(err));
		}
		return 0;

	case '\b':
	case 0x7f:
		if (uig.dialbuf.pos > 0) {
			--uig.dialbuf.pos;
			--uig.dialbuf.end;
		}
		break;

	case 0x1b:
		(void)re_fprintf(stderr, "\r\n");
		ui_view_set(VIEW_IDLE);
		return 0;

	case 0x00:
		/* ignore key release */
		break;

	default:
		(void)mbuf_write_u8(&uig.dialbuf, key);
		break;
	}

	ui_print_editor();

	return 0;
}


static int ui_chat(char key, struct re_printf *pf)
{
	int err = 0;

	(void)pf;

	switch (key) {

	case '\n':
		(void)re_fprintf(stderr, "\n");

		if (uig.chatpeer.buf && uig.chatpeer.end) {
			uig.chatpeer.pos = 0;
			err = ua_im_send(ua_cur(), &uig.chatpeer,
					 &uig.chatmsg);
		}
		else {
			err = ua_im_send(ua_cur(), NULL, &uig.chatmsg);
		}

		if (err) {
			DEBUG_WARNING("chat: ua_im_send() failed (%s)\n",
				      strerror(err));
		}

		mbuf_reset(&uig.chatmsg);
		return 0;

		/* Backspace or Delete */
	case '\b':
	case 0x7f:
		if (uig.chatmsg.pos > 0) {
			--uig.chatmsg.pos;
			--uig.chatmsg.end;
		}
		break;

		/* Escape */
	case 0x1b:
		(void)re_fprintf(stderr, "\nCancel\n");
		ui_view_set(VIEW_IDLE);
		return 0;

	case 0x00:
		/* ignore key release */
		break;

	default:
		(void)mbuf_write_u8(&uig.chatmsg, key);
		break;
	}

	(void)re_fprintf(stderr, "\r> %b", uig.chatmsg.buf, uig.chatmsg.end);

	return 0;
}


static int ui_contact(char key, struct re_printf *pf)
{
	char str[256];
	struct pl pl;
	int err;

	(void)pf;

	switch (key) {

	case '\n':
		if (uig.dialbuf.buf && uig.dialbuf.end) {
			(void)re_fprintf(stderr, "\n");
			ui_view_set(VIEW_IDLE);
			uig.dialbuf.pos = 0;
			(void)mbuf_read_str(&uig.dialbuf, str,
					    uig.dialbuf.end);
			str[uig.dialbuf.end] = '\0';
			err = ua_connect(ua_cur(), str, NULL,
					 NULL, VIDMODE_ON);
			if (err) {
				DEBUG_WARNING("connect failed: %s\n",
					      strerror(err));
			}
			return 0;
		}
		break;

		/* Backspace or Delete */
	case '\b':
	case 0x7f:
		if (uig.contactbuf.pos > 0) {
			--uig.contactbuf.pos;
			--uig.contactbuf.end;
		}
		break;

		/* Escape */
	case 0x1b:
		(void)re_fprintf(stderr, "\nCancel\n");
		ui_view_set(VIEW_IDLE);
		return 0;

	case '#':
		if (uig.dialbuf.buf && uig.dialbuf.end) {
			uig.dialbuf.pos = 0;
			(void)mbuf_read_str(&uig.dialbuf, str,
					    uig.dialbuf.end);
			str[uig.dialbuf.end] = '\0';
			contact_send_options(ua_cur(), str, true);
			ui_view_set(VIEW_IDLE);
			return 0;
		}
		break;

	case '=':
		if (!uig.dialbuf.buf || !uig.dialbuf.end)
			break;

		mbuf_reset(&uig.chatpeer);
		mbuf_reset(&uig.chatmsg);
		(void)mbuf_write_mem(&uig.chatpeer,
				     uig.dialbuf.buf, uig.dialbuf.end);

		(void)re_fprintf(stderr, "\nChat mode with %b - ESC to quit\n",
				 uig.chatpeer.buf, uig.chatpeer.end);

		ui_view_set(VIEW_CHAT);
		return 0;

	case 0x00:
		/* ignore key release */
		break;

	default:
		(void)mbuf_write_u8(&uig.contactbuf, key);
		break;
	}

	mbuf_reset(&uig.dialbuf);
	pl.p = (char *)uig.contactbuf.buf;
	pl.l = uig.contactbuf.end;
	if (pl_isset(&pl) && 0 == contact_find(&pl, &uig.dialbuf)) {
		(void)re_fprintf(stderr, "\r> %10r: %b         "
				 "                  ",
				 &pl, uig.dialbuf.buf, uig.dialbuf.end);
	}
	else {
		(void)re_fprintf(stderr, "\r> %10r: (no match) "
				 "                     "
				 "                                 ",
				 pl.l ? &pl : NULL);
	}

	return 0;
}


static int ui_call(char key, struct re_printf *pf)
{
	switch (key) {

	case '\n':
		ua_answer(ua_cur());
		break;

	case 'a':
		return audio_debug(pf, call_audio(ua_call(ua_cur())));

	case 'b':
	case 0x1b:
		ua_hangup(ua_cur());
		break;

	case 'c':
		return ua_print_call_status(pf, NULL);

	case 'h':
	default:
		return ui_view->helph(pf);

	case 'e':
		call_audioencoder_cycle(ua_call(ua_cur()));
		break;

#ifdef USE_VIDEO
	case 'E':
		call_videoencoder_cycle(ua_call(ua_cur()));
		break;
#endif

	case 'f':
		return aufilt_debug(pf, NULL);

	case 'i':
		return ua_print_sip_status(pf, NULL);

	case 'I':
		(void)call_modify(ua_call(ua_cur()));
		break;

	case 'm': {
		static bool muted = false;
		muted = !muted;
		(void)re_printf("\ncall %smuted\n", muted ? "" : "un-");
		audio_mute(call_audio(ua_call(ua_cur())), muted);
	}
		break;

	case 'M':
		return re_debug(pf, NULL);

	case 'n':
		return net_debug(pf, NULL);

	case 'q':
		uig.terminated = true;
		if (uig.exith)
			uig.exith(1);
		break;

	case 'S':
		ua_toggle_statmode(ua_cur());
		break;

	case 't':
		tmr_debug();
		break;

	case 'x':
		(void)call_hold(ua_call(ua_cur()), true);
		break;

	case 'X':
		(void)call_hold(ua_call(ua_cur()), false);
		break;

	case 'u':
		return ua_debug(pf, ua_cur());

#ifdef USE_VIDEO
	case 'v':
		return video_debug(pf, call_video(ua_call(ua_cur())));
#endif

	case 'y':
		return mem_status(pf, NULL);

	case ' ':
		ua_next();
		break;

	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case '*':
	case '#':
	case 0x00: /* key release */
		(void)call_send_digit(ua_call(ua_cur()), key);
		break;
	}

	return 0;
}


static int print_system_info(struct re_printf *pf)
{
	uint32_t uptime;
	int err = 0;

	uptime = (uint32_t)((long long)(tmr_jiffies() - uig.start_ticks)/1000);

	err |= re_hprintf(pf, "\n--- System info: ---\n");

	err |= re_hprintf(pf, " Machine:  %s/%s\n", sys_arch_get(),
			  sys_os_get());
	err |= re_hprintf(pf, " Version:  %s\n", sys_libre_version_get());
	err |= re_hprintf(pf, " Build:    %H\n", sys_build_get, NULL);
	err |= re_hprintf(pf, " Kernel:   %H\n", sys_kernel_get, NULL);
	err |= re_hprintf(pf, " Uptime:   %H\n", fmt_human_time, &uptime);
	err |= re_hprintf(pf, " Started:  %s", ctime(&uig.start_time));

#ifdef __VERSION__
	err |= re_hprintf(pf, " Compiler: %s\n", __VERSION__);
#endif

	return err;
}


static int codec_status(struct re_printf *pf)
{
	int err;

	err  = aucodec_debug(pf, aucodec_list());
	err |= vidcodec_debug(pf, vidcodec_list());

	return err;
}


static int ui_idle(char key, struct re_printf *pf)
{
	switch (key) {

	case '\n':
		ua_answer(ua_cur());
		break;

	case 'a':
	case 'A':
		audio_loop_test('A' == key);
		break;

	case 'b':
	case 0x1b:
		ua_hangup(ua_cur());
		break;

	case 'c':
		return ua_print_call_status(pf, NULL);

	case 'C':
		return contact_debug(pf, NULL);

	case 'h':
	default:
		return ui_view->helph(pf);

	case 'd':
		mbuf_reset(&uig.dialbuf);
		ui_view_set(VIEW_EDIT);
		ui_print_editor();
		break;

	case 'e':
		return codec_status(pf);

	case 'f':
		return aufilt_debug(pf, NULL);

	case 'i':
		return ua_print_sip_status(pf, NULL);

	case 'm':
		return mod_debug(pf, NULL);

	case 'M':
		return re_debug(pf, NULL);

	case 'n':
		return net_debug(pf, NULL);

	case 'o':
	case 'O':
		contact_send_options_to_all(ua_cur(), 'O' == key);
		break;

	case 'q':
		uig.terminated = true;
		if (uig.exith)
			uig.exith(1);
		break;

	case 'r':
		return ua_print_reg_status(pf, NULL);

	case 's':
		return print_system_info(pf);

	case 't':
		tmr_debug();
		break;

	case 'u':
		return ua_debug(pf, ua_cur());

	case 'y':
		return mem_status(pf, NULL);


	case ' ':
		ua_next();
		break;

	case '/':
		mbuf_reset(&uig.contactbuf);
		ui_view_set(VIEW_CONTACT);
		(void)re_fprintf(stderr, "Contact search:\n"
				 "  ENTER  Dial\n"
				 "  ESC    Cancel search\n"
				 "  #      Send OPTIONS\n"
				 "  =      Chatmode\n"
				 );
		break;

	case 'P':
		{
			static enum poll_method poll_method = METHOD_NULL;

			(void)poll_method_set(poll_method);

			if (++poll_method >= METHOD_MAX)
				poll_method = METHOD_NULL;
		}
		break;

	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case '*':
	case '#':
		mbuf_reset(&uig.dialbuf);
		ui_view_set(VIEW_EDIT);
		(void)ui_edit(key, pf);
		break;

	case 0x00:
		/* ignore key release */
		break;

#ifdef USE_VIDEO
	case 'v':
	case 'V':
		video_loop_test('V' == key);
		break;
#endif

	case 'w':
		(void)play_file(NULL, "sine.wav", 0);
		break;
	}

	return 0;
}


static void ui_handler(char key, struct re_printf *pf, void *arg)
{
	(void)arg;

	if (!ui_view || !ui_view->stateh)
		return;

	if (uig.terminated)
		return;

	(void)ui_view->stateh(key, pf);
}


static void destructor(void *arg)
{
	struct ui *ui = arg;

	list_unlink(&ui->le);
}


int ui_register(struct ui **uip, const char *name,
		ui_alloc_h *alloch, ui_output_h *outh)
{
	struct ui *ui;
	int err = 0;

	if (!uip)
		return EINVAL;

	/* Do not load stdio module in daemon mode */
	if (uig.run_daemon && 0 == strcmp(name, "stdio"))
		return 0;

	ui = mem_zalloc(sizeof(*ui), destructor);
	if (!ui)
		return ENOMEM;

	list_append(&uig.l, &ui->le, ui);

	ui->name    = name;
	ui->alloch  = alloch;
	ui->outputh = outh;

	if (err)
		mem_deref(ui);
	else
		*uip = ui;

	return err;
}


int ui_start(void)
{
	struct ui_prm prm;
	struct le *le;
	int err = 0;

	prm.device = config.input.device;
	prm.port   = config.input.port;

	for (le = uig.l.head; le; le = le->next) {
		struct ui *ui = le->data;

		if (!ui->alloch)
			continue;

		err = ui->alloch(&ui->st, &prm, ui_handler, NULL);
		if (err) {
			DEBUG_WARNING("start: alloc fail: %s (%s)\n",
				      ui->name, strerror(err));
			break;
		}
	}

	return err;
}


static struct ui *ui_find(const char *name)
{
	struct le *le;

	for (le = uig.l.head; le; le = le->next) {
		struct ui *ui = le->data;

		if (name && 0 != str_casecmp(name, ui->name))
			continue;

		return ui;
	}

	return NULL;
}


int ui_allocate(const char *name, ui_input_h *inputh, void *arg)
{
	struct ui_prm prm;
	struct ui *ui = ui_find(name);
	if (!ui)
		return ENOENT;

	prm.device = config.input.device;
	prm.port   = config.input.port;

	return ui->alloch ? ui->alloch(&ui->st, &prm, inputh, arg) : 0;
}


int ui_init(exit_h *exith, bool rdaemon)
{
	list_init(&uig.l);
	mbuf_init(&uig.dialbuf);
	mbuf_init(&uig.contactbuf);
	mbuf_init(&uig.chatpeer);
	mbuf_init(&uig.chatmsg);

	uig.start_ticks = tmr_jiffies();
	(void)time(&uig.start_time);
	uig.exith = exith;
	uig.run_daemon = rdaemon;

	ui_view_set(VIEW_IDLE);

	return 0;
}


static void ui_flush(void)
{
	struct le *le;

	for (le = uig.l.head; le; le = le->next) {
		struct ui *ui = le->data;
		ui->st = mem_deref(ui->st);
	}
}


void ui_close(void)
{
	ui_flush();

	mbuf_reset(&uig.dialbuf);
	mbuf_reset(&uig.contactbuf);
	mbuf_reset(&uig.chatpeer);
	mbuf_reset(&uig.chatmsg);
}


void ui_input(char key, struct re_printf *pf)
{
	ui_handler(key, pf, NULL);
}


void ui_out(const char *str)
{
	struct le *le;

	for (le = uig.l.head; le; le = le->next) {
		const struct ui *ui = le->data;

		if (ui->outputh)
			ui->outputh(ui->st, str);
	}
}


void ui_splash(uint32_t n_uas)
{
	const uint32_t t = (uint32_t)(tmr_jiffies() - uig.start_ticks);
	DEBUG_INFO("All %u useragents registered successfully! (%u ms)\n",
		   n_uas, t);
}


void ui_set_incall(bool incall)
{
	ui_view_set(incall ? VIEW_CALL : VIEW_IDLE);
}
