/**
 * @file ua.c  User-Agent
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdio.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"
#include "version.h"


#define DEBUG_MODULE "ua"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

/** Magic number */
#define MAGIC 0x0a0a0a0a
#include "magic.h"


enum {
	REG_INTERVAL    = 3600,
};


/** Defines the answermodes */
enum answermode {
	ANSWERMODE_MANUAL = 0,
	ANSWERMODE_EARLY,
	ANSWERMODE_AUTO
};

/** Defines a SIP User Agent object */
struct ua {
	MAGIC_DECL                   /**< Magic number for struct ua         */
	struct le le;                /**< Linked list element                */
	struct sipreg *reg;          /**< SIP Register client                */
	struct call *call;           /**< SIP Call object                    */
	struct tmr tmr_alert;        /**< Incoming call alert timer          */
	struct tmr tmr_stat;         /**< Statistics refresh timer           */
	struct mbuf dialbuf;         /**< Buffer for dialled number          */
	char *addr;                  /**< Buffer for my SIP Address          */
	struct sip_addr aor;         /**< My SIP Address-Of-Record           */
	const struct mnat *mnat;     /**< Media NAT handling                 */
	const struct menc *menc;     /**< Media encryption type              */
	uint32_t regint;             /**< Registration interval in [seconds] */
	uint32_t ptime;              /**< Configured packet time in [ms]     */
	enum statmode statmode;      /**< Status mode                        */
	enum answermode answermode;  /**< Answermode for incoming calls      */
	bool reg_ok;                 /**< Registration OK flag               */
	ua_event_h *eh;              /**< Event handler                      */
	ua_message_h *msgh;          /**< Incoming message handler           */
	void *arg;                   /**< Handler argument                   */
	char local_uri[256];         /**< Local SIP uri                      */
	char *cuser;                 /**< SIP Contact username               */
	char *outbound;              /**< Optional SIP outbound proxy        */
	bool aucodecs;               /**< audio_codecs parameter is set      */
	struct list aucodecl;        /**< List of preferred audio-codecs     */
	bool vidcodecs;              /**< video_codecs parameter is set      */
	struct list vidcodecl;       /**< List of preferred video-codecs     */
	int sipfd;                   /**< Cached file-descr. for SIP conn    */
	char *sipnat;                /**< SIP Nat mechanism                  */
	char *rtpkeep;               /**< RTP Keepalive mechanism            */
	char *stun_user;             /**< STUN Username                      */
	char *stun_pass;             /**< STUN Password                      */
	char *stun_host;             /**< STUN Hostname                      */
	uint16_t stun_port;          /**< STUN Port number                   */

	/** Statistics */
	struct {
		uint16_t scode;
		char ua[64];
		uint32_t n_bindings;
	} stat;
};


static struct {
	struct list ual;
	struct sip *sip;
	struct sip_lsnr *lsnr;
	struct sipsess_sock *sock;
	struct ua *cur;
	exit_h *exith;
	char uuid[64];
	bool use_udp;
	bool use_tcp;
	bool use_tls;
#ifdef USE_TLS
	struct tls *tls;
#endif
	enum audio_mode aumode;
} uag = {
	LIST_INIT,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"",
	true,
	true,
	true,
#ifdef USE_TLS
	NULL,
#endif
	AUDIO_MODE_POLL,
};


/* prototypes */
static void register_handler(int err, const struct sip_msg *msg, void *arg);


static void sip_exit_handler(void *arg)
{
	(void)arg;

	if (uag.exith)
		uag.exith(0);
}


/* Find the correct UA from the contact user */
static struct ua *ua_find(const struct pl *cuser)
{
	struct le *le;

	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;

		if (0 == pl_strcasecmp(cuser, ua->cuser))
			return ua;
	}

	return NULL;
}


static uint32_t n_uas(void)
{
	return list_count(&uag.ual);
}


static void ua_printf(const struct ua *ua, const char *fmt, ...)
{
	va_list ap;

	if (!ua)
		return;

	va_start(ap, fmt);
	(void)re_fprintf(stderr, "%r@%r: ",
			 &ua->aor.uri.user, &ua->aor.uri.host);
	(void)re_vfprintf(stderr, fmt, ap);
	va_end(ap);
}


static void ua_cur_set(struct ua *ua)
{
	uag.cur = ua;

	(void)re_fprintf(stderr, "ua: %r@%r\n", &ua->aor.uri.user,
			 &ua->aor.uri.host);
}


static void ua_event(struct ua *ua, enum ua_event ev, const char *prm)
{
	if (ua->eh)
		ua->eh(ev, prm, ua->arg);
}


int ua_auth(struct ua *ua, char **username, char **password, const char *realm)
{
	int err;

	if (!ua)
		return EINVAL;

	(void)realm;

	err  = pl_strdup(username, &ua->aor.uri.user);
	err |= pl_strdup(password, &ua->aor.uri.password);

	return err;
}


static int sip_auth_handler(char **username, char **password,
			    const char *realm, void *arg)
{
	return ua_auth(arg, username, password, realm);
}


static int encode_uri_user(struct re_printf *pf, const struct uri *uri)
{
	struct uri uuri = *uri;

	uuri.password = uuri.params = uuri.headers = pl_null;

	return uri_encode(pf, &uuri);
}


/**
 * Start registration of a User-Agent
 *
 * @param ua User-Agent
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_register(struct ua *ua)
{
	struct uri uri;
	char reg_uri[64];
	const char *routev[1];
	char params[256] = "";

	if (!ua)
		return EINVAL;

	routev[0] = ua->outbound;

	uri = ua->aor.uri;
	uri.user = uri.password = pl_null;
	if (re_snprintf(reg_uri, sizeof(reg_uri), "%H", uri_encode, &uri) < 0)
		return ENOMEM;

	if (str_len(uag.uuid)) {
		if (re_snprintf(params, sizeof(params),
				";+sip.instance=\"<urn:uuid:%s>\"",
				uag.uuid) < 0)
			return ENOMEM;
	}

	if (ua->mnat && ua->mnat->ftag) {
		if (re_snprintf(&params[strlen(params)],
				sizeof(params) - strlen(params),
				";%s", ua->mnat->ftag) < 0)
			return ENOMEM;
	}

	ua_event(ua, UA_EVENT_REGISTERING, NULL);

	ua->reg = mem_deref(ua->reg);
	return sipreg_register(&ua->reg, uag.sip, reg_uri, ua->local_uri,
			       ua->local_uri, ua->regint, ua->cuser,
			       routev[0] ? routev : NULL,
			       routev[0] ? 1 : 0,
			       str_casecmp(ua->sipnat, "outbound") ? 0 : 1,
			       sip_auth_handler, ua, false,
			       register_handler, ua,
			       params[0] ? &params[1] : NULL,
			       NULL);
}


static uint32_t ua_nreg_get(void)
{
	struct le *le;
	uint32_t n = 0;

	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;
		if (ua->reg_ok)
			++n;
	}

	return n;
}


static void ua_check_registrations(void)
{
	static bool ual_ready = false;

	if (ual_ready)
		return;

	if (ua_nreg_get() < n_uas())
		return;

	/* We are ready */
	ui_splash(n_uas());

	ual_ready = true;
}


static int sipmsg_fd(const struct sip_msg *msg)
{
	if (!msg)
		return -1;

	switch (msg->tp) {

	case SIP_TRANSP_UDP:
		return udp_sock_fd(msg->sock, AF_UNSPEC);

	case SIP_TRANSP_TCP:
	case SIP_TRANSP_TLS:
		return tcp_conn_fd(sip_msg_tcpconn(msg));

	default:
		return -1;
	}
}


static void register_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct ua *ua = arg;
	const struct sip_hdr *hdr;
	char buf[128];

	MAGIC_CHECK(ua);

	if (err) {
		ua->reg_ok = false;
		DEBUG_WARNING("%r@%r: Register: %s\n",
			      &ua->aor.uri.user, &ua->aor.uri.host,
			      strerror(err));
		(void)re_snprintf(buf, sizeof(buf), "%s", strerror(err));
		ua_event(ua, UA_EVENT_REGISTER_FAIL, buf);
		ua->stat.scode = 999;
		return;
	}

	ua->stat.scode = msg->scode;

	hdr = sip_msg_hdr(msg, SIP_HDR_SERVER);
	if (hdr)
		(void)pl_strcpy(&hdr->val, ua->stat.ua, sizeof(ua->stat.ua));

	(void)re_snprintf(buf, sizeof(buf), "%u %r", msg->scode, &msg->reason);

	if (200 <= msg->scode && msg->scode <= 299) {

		ua->stat.n_bindings = sip_msg_hdr_count(msg, SIP_HDR_CONTACT);

		if (!ua->reg_ok) {
			ua->reg_ok = true;
			ua_printf(ua, "%u %r (%s) [%u binding%s]\n",
				  msg->scode, &msg->reason,
				  ua->stat.ua, ua->stat.n_bindings,
				  1==ua->stat.n_bindings?"":"s");
		}

		ua->sipfd = sipmsg_fd(msg);

		ua_event(ua, UA_EVENT_REGISTER_OK, buf);
	}
	else if (msg->scode >= 300) {
		DEBUG_WARNING("%r@%r: %u %r (%s)\n",
			      &ua->aor.uri.user, &ua->aor.uri.host, msg->scode,
			      &msg->reason, ua->stat.ua);
		ua->reg_ok = false;

		ua_event(ua, UA_EVENT_REGISTER_FAIL, buf);
	}

	ua_check_registrations();
}


static bool ua_iscur(const struct ua *ua)
{
	return ua == uag.cur;
}


static void call_stat(void *arg)
{
	struct ua *ua = arg;

	MAGIC_CHECK(ua);

	if (STATMODE_OFF == ua->statmode)
		return;

	tmr_start(&ua->tmr_stat, 100, call_stat, ua);

	if (!ua_iscur(ua))
		return;

	switch (ua->statmode) {

	case STATMODE_CALL:
		call_enable_vumeter(ua->call, true);
		(void)re_fprintf(stderr, "%H\r", call_status, ua->call);
		break;

	case STATMODE_JBUF:
		call_enable_vumeter(ua->call, false);
		(void)re_fprintf(stderr, "%H\r", call_jbuf_stat, ua->call);
		break;

	case STATMODE_OFF:
	default:
		call_enable_vumeter(ua->call, false);
		break;
	}
}


static void alert_start(void *arg)
{
	struct ua *ua = arg;

	ui_out("\033[10;1000]\033[11;1000]\a");

	tmr_start(&ua->tmr_alert, 1000, alert_start, ua);
}


static void alert_stop(struct ua *ua)
{
	ui_out("\r");
	tmr_cancel(&ua->tmr_alert);
}


static void print_call_summary(const struct ua *ua)
{
	const uint32_t dur = call_duration(ua->call);

	if (!dur)
		return;

	(void)re_fprintf(stderr, "\n");

	ua_printf(ua, "Call terminated (duration: %H)\n",
		  fmt_human_time, &dur);
}


static void call_event_handler(enum call_event ev, const char *prm, void *arg)
{
	struct ua *ua = arg;
	const char *peeruri;

	MAGIC_CHECK(ua);

	peeruri = call_peeruri(ua->call);

	switch (ev) {

	case CALL_EVENT_INCOMING:
		switch (ua->answermode) {

		case ANSWERMODE_EARLY:
			(void)call_progress(ua->call);
			break;

		case ANSWERMODE_AUTO:
			(void)call_answer(ua->call, 200);
			break;

		case ANSWERMODE_MANUAL:
		default:
			/* Alert user */
			alert_start(ua);
			(void)call_ringtone(ua->call, "ring.wav");

			ua_printf(ua, "Incoming call from: %s -"
				  " (press ENTER to accept)\n", peeruri);
			ua_event(ua, UA_EVENT_CALL_INCOMING, peeruri);
			break;
		}
		break;

	case CALL_EVENT_RINGING:
		ua_event(ua, UA_EVENT_CALL_RINGING, peeruri);
		break;

	case CALL_EVENT_PROGRESS:
		ui_set_incall(true);
		ua_printf(ua, "Call in-progress: %s\n", peeruri);
		call_stat(ua);
		ua_event(ua, UA_EVENT_CALL_PROGRESS, peeruri);
		break;

	case CALL_EVENT_ESTABLISHED:
		alert_stop(ua);
		ui_set_incall(true);
		ua_printf(ua, "Call established: %s\n", peeruri);
		call_stat(ua);
		ua_event(ua, UA_EVENT_CALL_ESTABLISHED, peeruri);
		break;

	case CALL_EVENT_CLOSED:
		alert_stop(ua);
		ui_set_incall(false);
		tmr_cancel(&ua->tmr_stat);
		print_call_summary(ua);
		ua_event(ua, UA_EVENT_CALL_CLOSED, prm);
		ua->call = mem_deref(ua->call);
		break;

	default:
		DEBUG_WARNING("call event: unhandled event %d\n", ev);
		alert_stop(ua);
		ua->call = mem_deref(ua->call);
		tmr_cancel(&ua->tmr_stat);
		break;
	}
}


static int ua_call_alloc(struct ua *ua, struct call **callp,
			 const struct mnat *mnat, const struct menc *menc,
			 enum vidmode vidmode, const struct sip_msg *msg)
{
	struct call_prm prm;
	char dname[128] = "";

	if (*callp) {
		DEBUG_WARNING("call_alloc: call is already allocated\n");
		return EALREADY;
	}

	prm.ptime   = ua->ptime;
	prm.aumode  = uag.aumode;
	prm.vidmode = vidmode;

	(void)pl_strcpy(&ua->aor.dname, dname, sizeof(dname));

	return call_alloc(callp, ua, &prm, mnat,
			  ua->stun_user, ua->stun_pass,
			  ua->stun_host, ua->stun_port,
			  menc, dname, ua->local_uri, ua->cuser, msg,
			  call_event_handler, ua);
}


static void handle_options(struct ua *ua, const struct sip_msg *msg)
{
	struct call *call = NULL;
	struct mbuf *desc = NULL;
	int err;

	err = ua_call_alloc(ua, &call, NULL, NULL, VIDMODE_ON, NULL);
	if (err) {
		(void)sip_treply(NULL, uag.sip, msg, 500, "Call Error");
		return;
	}

	err = call_sdp_get(call, &desc, true);
	if (err)
		goto out;

	err = sip_treplyf(NULL, NULL, uag.sip,
			  msg, true, 200, "OK",
			  "Contact: <sip:%s@%J%s>\r\n"
			  "Content-Type: application/sdp\r\n"
			  "Content-Length: %u\r\n"
			  "\r\n"
			  "%b",
			  ua->cuser, &msg->dst, sip_transp_param(msg->tp),
			  mbuf_get_left(desc),
			  mbuf_buf(desc),
			  mbuf_get_left(desc));
	if (err) {
		DEBUG_WARNING("options: sip_treplyf: %s\n", strerror(err));
	}

 out:
	mem_deref(desc);
	mem_deref(call);
}


/* RFC 3428 */
static void handle_message(struct ua *ua, const struct sip_msg *msg)
{
	static const char *ctype_text = "text/plain";

	if (ua->msgh) {
		ua->msgh(&msg->from.auri, &msg->ctype, msg->mb, ua->arg);
		(void)sip_reply(uag.sip, msg, 200, "OK");
	}
	else if (!pl_strcasecmp(&msg->ctype, ctype_text)) {
		(void)re_fprintf(stderr, "\r%r: \"%b\"\n", &msg->from.auri,
				 mbuf_buf(msg->mb), mbuf_get_left(msg->mb));
		(void)play_file(NULL, "message.wav", 0);
		(void)sip_reply(uag.sip, msg, 200, "OK");
	}
	else {
		(void)sip_replyf(uag.sip, msg, 415, "Unsupported Media Type",
				 "Accept: %s\r\n"
				 "Content-Length: 0\r\n"
				 "\r\n",
				 ctype_text);
	}
}


static void ua_destructor(void *arg)
{
	struct ua *ua = arg;

	list_unlink(&ua->le);

	tmr_cancel(&ua->tmr_stat);
	tmr_cancel(&ua->tmr_alert);

	mbuf_reset(&ua->dialbuf);

	mem_deref(ua->addr);
	mem_deref(ua->call);
	mem_deref(ua->reg);
	mem_deref(ua->cuser);
	mem_deref(ua->outbound);
	mem_deref(ua->stun_user);
	mem_deref(ua->stun_pass);
	mem_deref(ua->stun_host);
	mem_deref(ua->sipnat);
	mem_deref(ua->rtpkeep);

	list_flush(&ua->aucodecl);
	list_flush(&ua->vidcodecl);
}


/**
 * Decode STUN Server parameter. We use the SIP parameters as default,
 * and override with an STUN parameters present.
 *
 * \verbatim
 *   ;stunserver=stun:username:password@host:port
 * \endverbatim
 */
static int stunsrv_decode(struct ua *ua)
{
	struct pl srv;
	struct uri uri;
	int err;

	memset(&uri, 0, sizeof(uri));

	if (0 == sip_param_decode(&ua->aor.params, "stunserver", &srv)) {

		DEBUG_NOTICE("got stunserver: '%r'\n", &srv);

		err = uri_decode(&uri, &srv);
		if (err) {
			DEBUG_WARNING("%r: decode failed: %s\n",
				      &srv, strerror(err));
			memset(&uri, 0, sizeof(uri));
		}

		if (0 != pl_strcasecmp(&uri.scheme, "stun")) {
			DEBUG_WARNING("unknown scheme: %r\n", &uri.scheme);
			return EINVAL;
		}
	}

	err = 0;
	if (pl_isset(&uri.user))
		err |= pl_strdup(&ua->stun_user, &uri.user);
	else
		err |= pl_strdup(&ua->stun_user, &ua->aor.uri.user);

	if (pl_isset(&uri.password))
		err |= pl_strdup(&ua->stun_pass, &uri.password);
	else
		err |= pl_strdup(&ua->stun_pass, &ua->aor.uri.password);

	if (pl_isset(&uri.host))
		err |= pl_strdup(&ua->stun_host, &uri.host);
	else
		err |= pl_strdup(&ua->stun_host, &ua->aor.uri.host);

	ua->stun_port = uri.port;

	return err;
}


/** Decode media parameters */
static int media_decode(struct ua *ua)
{
	struct pl mnat, menc, ptime, rtpkeep;
	int err = 0;

	/* Decode media nat parameter */
	ua->mnat = NULL;
	if (0 == sip_param_decode(&ua->aor.params, "medianat", &mnat)) {

		char mnatid[64];

		ua_printf(ua, "Using medianat: %r\n", &mnat);

		(void)pl_strcpy(&mnat, mnatid, sizeof(mnatid));

		ua->mnat = mnat_find(mnatid);
		if (!ua->mnat) {
			DEBUG_WARNING("medianat not found: %r\n", &mnat);
		}
	}

	/* Media encryption */
	ua->menc = NULL;
	if (0 == sip_param_decode(&ua->aor.params, "mediaenc", &menc)) {

		char mencid[64];

		ua_printf(ua, "Using media encryption `%r'\n", &menc);

		(void)pl_strcpy(&menc, mencid, sizeof(mencid));

		ua->menc = menc_find(mencid);
		if (!ua->menc) {
			DEBUG_WARNING("mediaenc not found: %r\n", &menc);
		}
	}

	/* Decode ptime parameter */
	if (0 == sip_param_decode(&ua->aor.params, "ptime", &ptime)) {
		ua->ptime = pl_u32(&ptime);
		if (!ua->ptime) {
			DEBUG_WARNING("ptime must be greater than zero\n");
			return EINVAL;
		}
		DEBUG_NOTICE("setting ptime=%u\n", ua->ptime);
	}

	if (!sip_param_decode(&ua->aor.params, "rtpkeep", &rtpkeep)) {

		ua_printf(ua, "Using RTP keepalive: %r\n", &rtpkeep);

		err = pl_strdup(&ua->rtpkeep, &rtpkeep);
	}

	return err;
}


/* Decode answermode parameter */
static void answermode_decode(struct ua *ua)
{
	struct pl amode;

	if (0 == sip_param_decode(&ua->aor.params, "answermode", &amode)) {

		if (0 == pl_strcasecmp(&amode, "manual")) {
			ua->answermode = ANSWERMODE_MANUAL;
		}
		else if (0 == pl_strcasecmp(&amode, "early")) {
			ua->answermode = ANSWERMODE_EARLY;
		}
		else if (0 == pl_strcasecmp(&amode, "auto")) {
			ua->answermode = ANSWERMODE_AUTO;
		}
		else {
			DEBUG_WARNING("answermode: unknown (%r)\n", &amode);
			ua->answermode = ANSWERMODE_MANUAL;
		}
	}
}


static int csl_parse(struct pl *pl, char *str, size_t sz)
{
	struct pl ws = PL_INIT, val, ws2 = PL_INIT, cma = PL_INIT;
	int err;

	err = re_regex(pl->p, pl->l, "[ \t]*[^, \t]+[ \t]*[,]*",
		       &ws, &val, &ws2, &cma);
	if (err)
		return err;

	pl_advance(pl, ws.l + val.l + ws2.l + cma.l);

	(void)pl_strcpy(&val, str, sz);

	return 0;
}


static int audio_codecs_decode(struct ua *ua)
{
	struct pl tmp;
	int err;

	if (0 == sip_param_exists(&ua->aor.params, "audio_codecs", &tmp)) {
		struct pl acs;
		char cname[64];

		ua->aucodecs = true;

		if (sip_param_decode(&ua->aor.params, "audio_codecs", &acs))
			return 0;

		while (0 == csl_parse(&acs, cname, sizeof(cname))) {
			struct aucodec *ac;
			struct pl pl_cname, pl_srate, pl_ch = PL_INIT;
			uint32_t srate = 8000;
			uint8_t ch = 1;

			/* Format: "codec/srate/ch" */
			if (0 == re_regex(cname, strlen(cname),
					  "[^/]+/[0-9]+[/]*[0-9]*",
					  &pl_cname, &pl_srate,
					  NULL, &pl_ch)) {
				(void)pl_strcpy(&pl_cname, cname,
						sizeof(cname));
				srate = pl_u32(&pl_srate);
				if (pl_isset(&pl_ch))
					ch = pl_u32(&pl_ch);
			}

			ac = (struct aucodec *)aucodec_find(cname, srate, ch);
			if (!ac) {
				DEBUG_WARNING("audio codec not found:"
					      " %s/%u/%d\n",
					      cname, srate, ch);
				continue;
			}

			err = aucodec_clone(&ua->aucodecl, ac);
			if (err)
				return err;
		}
	}

	return 0;
}


static int video_codecs_decode(struct ua *ua)
{
	struct pl tmp;
	int err;

	if (0 == sip_param_exists(&ua->aor.params, "video_codecs", &tmp)) {
		struct pl vcs;
		char cname[64];

		ua->vidcodecs = true;

		if (sip_param_decode(&ua->aor.params, "video_codecs", &vcs))
			return 0;

		while (0 == csl_parse(&vcs, cname, sizeof(cname))) {
			struct vidcodec *vc;

			vc = (struct vidcodec *)vidcodec_find(cname);
			if (!vc) {
				DEBUG_WARNING("video codec not found: %s\n",
					      cname);
				continue;
			}

			err = vidcodec_clone(&ua->vidcodecl, vc);
			if (err)
				return err;
		}
	}

	return 0;
}


/** Construct my AOR */
static int mk_aor(struct ua *ua, const char *aor)
{
	struct pl pl;
	int err;

	err = str_dup(&ua->addr, aor);
	if (err)
		return err;

	pl_set_str(&pl, ua->addr);

	err = sip_addr_decode(&ua->aor, &pl);
	if (err)
		return err;

	if (re_snprintf(ua->local_uri, sizeof(ua->local_uri),
			"%H", encode_uri_user, &ua->aor.uri) < 0)
		return ENOMEM;

	return re_sdprintf(&ua->cuser, "%p", ua);
}


static bool request_handler(const struct sip_msg *msg, void *arg)
{
	struct ua *ua;
	bool opt;

	(void)arg;

	if (!pl_strcmp(&msg->met, "OPTIONS"))
		opt = true;
	else if (!pl_strcmp(&msg->met, "MESSAGE"))
		opt = false;
	else
		return false;

	ua = ua_find(&msg->uri.user);
	if (!ua) {
		(void)sip_treply(NULL, uag_sip(), msg, 404, "Not Found");
		return true;
	}

	if (opt)
		handle_options(ua, msg);
	else
		handle_message(ua, msg);

	return true;
}


static int sip_params_decode(struct ua *ua)
{
	struct pl regint, ob, sipnat;
	int err = 0;

	ua->regint = REG_INTERVAL + (rand_u32()&0xff);
	if (0 == sip_param_decode(&ua->aor.params, "regint", &regint)) {
		ua->regint = pl_u32(&regint);
	}

	if (0 == sip_param_decode(&ua->aor.params, "outbound", &ob)) {
		err |= pl_strdup(&ua->outbound, &ob);
	}

	if (0 == sip_param_decode(&ua->aor.params, "sipnat", &sipnat)) {
		DEBUG_NOTICE("sipnat: %r\n", &sipnat);
		err = pl_strdup(&ua->sipnat, &sipnat);
	}

	return err;
}


/**
 * Allocate a SIP User-Agent
 *
 * @param uap   Pointer to allocated User-Agent object
 * @param aor   SIP Address-of-Record (AOR)
 * @param eh    Event handler
 * @param msgh  SIP Message handler
 * @param arg   Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_alloc(struct ua **uap, const char *aor,
	     ua_event_h *eh, ua_message_h *msgh, void *arg)
{
	struct ua *ua;
	int err;

	if (!aor)
		return EINVAL;

	ua = mem_zalloc(sizeof(*ua), ua_destructor);
	if (!ua)
		return ENOMEM;

	MAGIC_INIT(ua);

	list_append(&uag.ual, &ua->le, ua);

	list_init(&ua->aucodecl);
	list_init(&ua->vidcodecl);

	tmr_init(&ua->tmr_stat);
	tmr_init(&ua->tmr_alert);

	mbuf_init(&ua->dialbuf);

	ua->eh   = eh;
	ua->msgh = msgh;
	ua->arg  = arg;

	err = mk_aor(ua, aor);
	if (err)
		goto out;

	/* Decode address parameters */
	err |= sip_params_decode(ua);
	answermode_decode(ua);
	err |= audio_codecs_decode(ua);
	err |= video_codecs_decode(ua);
	err |= media_decode(ua);
	if (err)
		goto out;

	if (ua->sipnat || ua->mnat)
		err = stunsrv_decode(ua);

	/* Set current UA to this */
	ua_cur_set(ua);

 out:
	if (err)
		mem_deref(ua);
	else if (uap)
		*uap = ua;

	return err;
}


static int ua_start(struct ua *ua)
{
	if (!ua->regint)
		return 0;

	return ua_register(ua);
}


/**
 * Connect an outgoing call to a given SIP uri
 *
 * @param ua      User-Agent
 * @param uri     SIP uri to connect to
 * @param params  Optional URI parameters
 * @param mnatid  Optional MNAT id to override default settings
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_connect(struct ua *ua, const char *uri, const char *params,
	       const char *mnatid, enum vidmode vidmode)
{
	const struct mnat *mnat;
	struct pl pl;
	size_t len;
	int err = 0;

	if (!ua || !uri)
		return EINVAL;

	len = strlen(uri);
	if (len > 0) {
		mbuf_reset(&ua->dialbuf);

		if (params)
			err |= mbuf_printf(&ua->dialbuf, "<");

		/* Append sip: scheme if missing */
		if (0 != re_regex(uri, len, "sip:"))
			err |= mbuf_printf(&ua->dialbuf, "sip:");

		err |= mbuf_write_str(&ua->dialbuf, uri);

		/* Append domain if missing */
		if (0 != re_regex(uri, len, "[^@]+@[^]+", NULL, NULL)) {
#if HAVE_INET6
			if (AF_INET6 == ua->aor.uri.af)
				err |= mbuf_printf(&ua->dialbuf, "@[%r]",
						   &ua->aor.uri.host);
			else
#endif
				err |= mbuf_printf(&ua->dialbuf, "@%r",
						   &ua->aor.uri.host);

			/* Also append port if specified and not 5060 */
			switch (ua->aor.uri.port) {

			case 0:
			case SIP_PORT:
				break;

			default:
				err |= mbuf_printf(&ua->dialbuf, ":%u",
						   ua->aor.uri.port);

				break;
			}
		}

		if (params) {
			err |= mbuf_printf(&ua->dialbuf, ";%s", params);
		}

		/* Append any optional parameters */
		err |= mbuf_write_pl(&ua->dialbuf, &ua->aor.uri.params);

		if (params)
			err |= mbuf_printf(&ua->dialbuf, ">");
	}

	if (err)
		return err;

	/* override medianat per call */
	if (mnatid) {
		mnat = mnat_find(mnatid);
	}
	else
		mnat = ua->mnat;

	err = ua_call_alloc(ua, &ua->call, mnat, ua->menc, vidmode, NULL);
	if (err)
		return err;

	pl.p = (char *)ua->dialbuf.buf;
	pl.l = ua->dialbuf.end;

	return call_connect(ua->call, &pl);
}


/**
 * Hangup the current call
 *
 * @param ua User-Agent
 */
void ua_hangup(struct ua *ua)
{
	int err;

	if (!ua || !ua->call)
		return;

	err = call_hangup(ua->call);
	if (err) {
		DEBUG_WARNING("call hangup failed (%s)\n", strerror(err));
	}

	tmr_cancel(&ua->tmr_stat);

	print_call_summary(ua);

	ua->call = mem_deref(ua->call);
	ui_set_incall(false);
}


/**
 * Answer an incoming call
 *
 * @param ua User-Agent
 */
void ua_answer(struct ua *ua)
{
	if (!ua || !ua->call)
		return;

	(void)call_answer(ua->call, 200);
	call_stat(ua);
}


static int ua_print_status(struct re_printf *pf, const struct ua *ua)
{
	char userhost[64];
	int err;

	if (!ua)
		return 0;

	if (re_snprintf(userhost, sizeof(userhost), "%H",
			encode_uri_user, &ua->aor.uri) < 0)
		return ENOMEM;
	err = re_hprintf(pf, "%-42s ", userhost);

	if (0 == ua->stat.scode) {
		err |= re_hprintf(pf, "\x1b[33m" "zzz" "\x1b[;m");
	}
	else if (200 == ua->stat.scode) {
		err |= re_hprintf(pf, "\x1b[32m" "OK " "\x1b[;m");
	}
	else {
		err |= re_hprintf(pf, "\x1b[31m" "ERR" "\x1b[;m");
	}

	err |= re_hprintf(pf, " (%2u) %s\n", ua->stat.n_bindings, ua->stat.ua);

	return err;
}


/**
 * Send SIP OPTIONS message to a peer
 *
 * @param ua      User-Agent object
 * @param uri     Peer SIP Address
 * @param resph   Response handler
 * @param arg     Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_options_send(struct ua *ua, const char *uri,
		    options_resp_h *resph, void *arg)
{
	int err;

	if (!ua)
		return EINVAL;

	err = sip_req_send(ua, "OPTIONS", uri, resph, arg,
			   "Accept: application/sdp\r\n"
			   "Content-Length: 0\r\n"
			   "\r\n");
	if (err) {
		DEBUG_WARNING("send options: (%s)\n", strerror(err));
	}

	return err;
}


static void im_resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct ua *ua = arg;

	(void)ua;

	if (err) {
		(void)re_fprintf(stderr, " \x1b[31m%s\x1b[;m\n",
				 strerror(err));
		return;
	}

	if (msg->scode >= 300) {
		(void)re_fprintf(stderr, " \x1b[31m%u %r\x1b[;m\n",
				 msg->scode, &msg->reason);
	}
}


/**
 * Send SIP instant MESSAGE to a peer
 *
 * @param ua    User-Agent object
 * @param peer  Peer SIP Address
 * @param msg   Message to send
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_im_send(struct ua *ua, struct mbuf *peer, struct mbuf *msg)
{
	char *uri = NULL;
	int err = 0;

	if (!ua || !msg)
		return EINVAL;

	msg->pos = 0;

	if (ua->call) {
		err = str_dup(&uri, call_peeruri(ua->call));
	}
	else if (peer) {
		struct sip_addr addr;
		struct pl pl;

		pl_set_mbuf(&pl, peer);

		err = sip_addr_decode(&addr, &pl);
		if (err)
			return err;

		err = pl_strdup(&uri, &addr.auri);
	}
	if (err)
		return err;

	err = sip_req_send(ua, "MESSAGE", uri, im_resp_handler, ua,
			   "Accept: text/plain\r\n"
			   "Content-Type: text/plain\r\n"
			   "Content-Length: %u\r\n"
			   "\r\n%b",
			   mbuf_get_left(msg),
			   mbuf_buf(msg), mbuf_get_left(msg));

	mem_deref(uri);

	return err;
}


/**
 * Toggle status mode
 *
 * @param ua User-Agent object
 */
void ua_toggle_statmode(struct ua *ua)
{
	if (!ua)
		return;

	if (++ua->statmode >= STATMODE_N)
		ua->statmode = (enum statmode)0;

	ua_set_statmode(ua, ua->statmode);

	(void)re_fprintf(stderr, "\r                                   ");
	(void)re_fprintf(stderr, "                                   \r");
}


/**
 * Set the current UA status mode
 *
 * @param ua   User-Agent object
 * @param mode Status mode
 */
void ua_set_statmode(struct ua *ua, enum statmode mode)
{
	if (!ua)
		return;

	ua->statmode = mode;

	/* kick-start it */
	call_stat(ua);
}


/**
 * Get the AOR of a User-Agent
 *
 * @param ua User-Agent object
 *
 * @return AOR
 */
const char *ua_aor(const struct ua *ua)
{
	return ua ? ua->local_uri : NULL;
}


/**
 * Get the outbound SIP proxy of a User-Agent
 *
 * @param ua User-Agent object
 *
 * @return Outbound SIP proxy uri
 */
char *ua_outbound(const struct ua *ua)
{
	return ua ? ua->outbound : NULL;
}


/**
 * Get the current call object of a User-Agent
 *
 * @param ua User-Agent object
 *
 * @return Current call, NULL if no active calls
 */
struct call *ua_call(const struct ua *ua)
{
	return ua ? ua->call : NULL;
}


int ua_debug(struct re_printf *pf, const struct ua *ua)
{
	int err;

	if (!ua)
		return 0;

	err  = re_hprintf(pf, "--- %H ---\n", uri_encode, &ua->aor.uri);
	err |= re_hprintf(pf, " Contact user: %s\n", ua->cuser);

	if (ua->aucodecs)
		err |= aucodec_debug(pf, &ua->aucodecl);
	if (ua->vidcodecs)
		err |= vidcodec_debug(pf, &ua->vidcodecl);

	return err;
}


/* One instance */


static int ua_add_transp(void)
{
	struct sa local;
	int err = 0;

	if (config.sip.local[0]) {
		err = sa_decode(&local, config.sip.local,
				strlen(config.sip.local));
		if (err) {
			err = sa_set_str(&local, config.sip.local, 0);
			if (err) {
				DEBUG_WARNING("decode failed: %s\n",
					      config.sip.local);
				return err;
			}
		}
	}
	else {
		sa_cpy(&local, net_laddr());
		sa_set_port(&local, 0);
	}

	if (uag.use_udp)
		err |= sip_transp_add(uag.sip, SIP_TRANSP_UDP, &local);
	if (uag.use_tcp)
		err |= sip_transp_add(uag.sip, SIP_TRANSP_TCP, &local);
	if (err) {
		DEBUG_WARNING("SIP Transport failed: %s\n", strerror(err));
		return err;
	}

#ifdef USE_TLS
	if (uag.use_tls) {
		/* Build our SSL context*/
		if (!uag.tls) {
			err = tls_alloc(&uag.tls, TLS_METHOD_SSLV23,
					NULL, NULL);
			if (err) {
				DEBUG_WARNING("tls_alloc() failed: %s\n",
					      strerror(err));
				return err;
			}
		}

		if (sa_isset(&local, SA_PORT))
			sa_set_port(&local, sa_port(&local) + 1);

		err = sip_transp_add(uag.sip, SIP_TRANSP_TLS, &local, uag.tls);
		if (err) {
			DEBUG_WARNING("SIP/TLS transport failed: %s\n",
				      strerror(err));
			return err;
		}
	}
#endif

	return err;
}


static int ua_setup_transp(const char *software, bool udp, bool tcp, bool tls)
{
	int err;

	uag.use_udp = udp;
	uag.use_tcp = tcp;
	uag.use_tls = tls;

	err = sip_alloc(&uag.sip, net_dnsc(),
			config.sip.trans_bsize,
			config.sip.trans_bsize,
			config.sip.trans_bsize,
			software, sip_exit_handler, NULL);
	if (err) {
		DEBUG_WARNING("sip stack failed: %s\n", strerror(err));
		return err;
	}

	err = ua_add_transp();

	return err;
}


/* Handle incoming calls */
static void sipsess_conn_handler(const struct sip_msg *msg, void *arg)
{
	struct ua *ua;
	char str[256];
	int err;

	(void)arg;

	ua = ua_find(&msg->uri.user);
	if (!ua) {
		DEBUG_WARNING("%r: UA not found: %r\n",
			      &msg->from.auri, &msg->uri.user);
		(void)sip_treply(NULL, uag_sip(), msg, 404, "Not Found");
		return;
	}

	/* TODO: handle multiple calls */
	if (ua->call) {
		(void)sip_treply(NULL, uag.sip, msg, 486, "Busy Here");
		return;
	}

	err = ua_call_alloc(ua, &ua->call, ua->mnat, ua->menc,
			    VIDMODE_ON, msg);
	if (err) {
		DEBUG_WARNING("call_alloc: %s\n", strerror(err));
		goto error;
	}

	err = call_accept(ua->call, uag.sock, msg, ua->cuser);
	if (err)
		goto error;

	return;

 error:
	ua->call = mem_deref(ua->call);
	(void)re_snprintf(str, sizeof(str), "Error (%s)", strerror(err));
	(void)sip_treply(NULL, uag.sip, msg, 500, str);
}


/**
 * Initialise the User-Agents
 *
 * @param software SIP User-Agent string
 * @param udp      Enable UDP transport
 * @param tcp      Enable TCP transport
 * @param tls      Enable TLS transport
 * @param exith    SIP Exit handler
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_init(const char *software, bool udp, bool tcp, bool tls, exit_h *exith)
{
	int err;

	list_init(&uag.ual);

	err = ua_setup_transp(software, udp, tcp, tls);
	if (err)
		goto out;

	err = sip_listen(&uag.lsnr, uag.sip, true, request_handler, NULL);
	if (err)
		goto out;

	err = sipsess_listen(&uag.sock, uag.sip, config.sip.trans_bsize,
			     sipsess_conn_handler, NULL);

	uag.exith = exith;

 out:
	if (err) {
		DEBUG_WARNING("init failed (%s)\n", strerror(err));
		ua_close();
	}
	return err;
}


/**
 * Set the device UUID for all User-Agents
 *
 * @param uuid Device UUID
 */
void ua_set_uuid(const char *uuid)
{
	uag.uuid[0] = '\0';

	if (str_len(uuid))
		str_ncpy(uag.uuid, uuid, sizeof(uag.uuid));
}


/**
 * Set the Audio-transmit mode for all User-Agents
 *
 * @param aumode Audio transmit mode
 */
void ua_set_aumode(enum audio_mode aumode)
{
	uag.aumode = aumode;
}


/**
 * Close all active User-Agents
 */
void ua_close(void)
{
	uag.sock = mem_deref(uag.sock);
	uag.lsnr = mem_deref(uag.lsnr);
	uag.sip = mem_deref(uag.sip);

#ifdef USE_TLS
	uag.tls = mem_deref(uag.tls);
#endif

	list_flush(&uag.ual);
}


/**
 * Suspend the SIP stack
 */
void ua_stack_suspend(void)
{
	struct le *le;

	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;

		ua->reg = mem_deref(ua->reg);
	}

	sip_close(uag.sip, false);
}


/**
 * Resume the SIP stack
 *
 * @param software SIP User-Agent string
 * @param udp      Enable UDP transport
 * @param tcp      Enable TCP transport
 * @param tls      Enable TLS transport
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_stack_resume(const char *software, bool udp, bool tcp, bool tls)
{
	struct le *le;
	int err = 0;

	DEBUG_NOTICE("STACK RESUME: %s%s%s\n",
		     udp ? " UDP" : "",
		     tcp ? " TCP" : "",
		     tls ? " TLS" : "");

	/* Destroy SIP stack */
	uag.sock = mem_deref(uag.sock);
	uag.sip = mem_deref(uag.sip);

#ifdef USE_TLS
	uag.tls = mem_deref(uag.tls);
#endif

	err = net_reset();
	if (err)
		return err;

	err = ua_setup_transp(software, udp, tcp, tls);
	if (err)
		return err;

	err = sipsess_listen(&uag.sock, uag.sip, config.sip.trans_bsize,
			     sipsess_conn_handler, NULL);
	if (err)
		return err;

	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;

		ua->reg_ok = false;

		err |= ua_start(ua);
	}

	return err;
}


/**
 * Start all User-Agents
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_start_all(void)
{
	struct le *le;
	int err = 0;

	for (le = uag.ual.head; le; le = le->next)
		err |= ua_start(le->data);

	return err;
}


/**
 * Start all User-Agents
 *
 * @param forced True to force, otherwise false
 */
void ua_stop_all(bool forced)
{
	(void)re_fprintf(stderr, "Un-registering %u useragents.. %s\n",
			 n_uas(), forced ? "(Forced)" : "");

	if (forced)
		sipsess_close_all(uag.sock);
	else
		list_flush(&uag.ual);

	uag.cur = NULL;
	sip_close(uag.sip, forced);
}


/**
 * Reset the SIP transports for all User-Agents
 *
 * @param reg      True to reset registration
 * @param reinvite True to update active calls
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_reset_transp(bool reg, bool reinvite)
{
	struct le *le;
	int err;

	/* Update SIP transports */
	sip_transp_flush(uag.sip);

	(void)net_check();
	err = ua_add_transp();
	if (err)
		return err;

	/* Re-REGISTER all User-Agents */
	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;

		if (reg) {
			ua->reg_ok = false;
			err |= ua_register(ua);
		}

		/* update all active calls */
		if (reinvite && ua->call)
			err |= call_reset_transp(ua->call);
	}

	return err;
}


void ua_next(void)
{
	struct ua *ua = ua_cur();
	struct le *le;

	if (!ua)
		return;

	le = &ua->le;

	le = le->next ? le->next : uag.ual.head;

	ua_cur_set(list_ledata(le));
}


/**
 * Return the current User-Agent in focus
 *
 * @return Current User-Agent
 */
struct ua *ua_cur(void)
{
	return uag.cur ? uag.cur : list_ledata(uag.ual.head);
}


/**
 * Print the SIP Status for all User-Agents
 *
 * @param pf     Print handler for debug output
 * @param unused Unused parameter
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_print_sip_status(struct re_printf *pf, void *unused)
{
	(void)unused;
	return sip_debug(pf, uag.sip);
}


/**
 * Print the SIP Registration for all User-Agents
 *
 * @param pf     Print handler for debug output
 * @param unused Unused parameter
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_print_reg_status(struct re_printf *pf, void *unused)
{
	struct le *le;
	int err;

	(void)unused;

	err = re_hprintf(pf, "\n--- Useragents: %u/%u ---\n", ua_nreg_get(),
			 n_uas());

	for (le = uag.ual.head; le && !err; le = le->next) {
		const struct ua *ua = le->data;

		err  = re_hprintf(pf, "%s", ua == ua_cur() ? ">" : " ");
		err |= ua_print_status(pf, ua);
	}

	err |= re_hprintf(pf, "\n");

	return err;
}


/**
 * Print the SIP Call status for all User-Agents
 *
 * @param pf     Print handler for debug output
 * @param unused Unused parameter
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_print_call_status(struct re_printf *pf, void *unused)
{
	int err;

	(void)unused;

	err  = re_hprintf(pf, "\n--- Call status: ---\n");
	err |= call_debug(pf, ua_cur()->call);
	err |= re_hprintf(pf, "\n");

	return err;
}


/**
 * Get the global SIP Stack
 *
 * @return SIP Stack
 */
struct sip *uag_sip(void)
{
	return uag.sip;
}


/**
 * Get the global SIP Session socket
 *
 * @return SIP Session socket
 */
struct sipsess_sock *uag_sipsess_sock(void)
{
	return uag.sock;
}


struct tls *uag_tls(void)
{
#ifdef USE_TLS
	return uag.tls;
#else
	return NULL;
#endif
}


/**
 * Get the name of the User-Agent event
 *
 * @param ev User-Agent event
 *
 * @return Name of the event
 */
const char *ua_event_str(enum ua_event ev)
{
	switch (ev) {

	case UA_EVENT_REGISTERING:      return "REGISTERING";
	case UA_EVENT_REGISTER_OK:      return "REGISTER_OK";
	case UA_EVENT_REGISTER_FAIL:    return "REGISTER_FAIL";
	case UA_EVENT_UNREGISTERING:    return "UNREGISTERING";
	case UA_EVENT_UNREGISTER_OK:    return "UNREGISTER_OK";
	case UA_EVENT_UNREGISTER_FAIL:  return "UNREGISTER_FAIL";
	case UA_EVENT_CALL_INCOMING:    return "CALL_INCOMING";
	case UA_EVENT_CALL_RINGING:     return "CALL_RINGING";
	case UA_EVENT_CALL_PROGRESS:    return "CALL_PROGRESS";
	case UA_EVENT_CALL_ESTABLISHED: return "CALL_ESTABLISHED";
	case UA_EVENT_CALL_CLOSED:      return "CALL_CLOSED";
	default: return "?";
	}
}


struct list *ua_aucodecl(struct ua *ua)
{
	return (ua && ua->aucodecs) ? &ua->aucodecl : aucodec_list();
}


struct list *ua_vidcodecl(struct ua *ua)
{
	return (ua && ua->vidcodecs) ? &ua->vidcodecl : vidcodec_list();
}


/**
 * Get the current SIP socket file descriptor for a User-Agent
 *
 * @param ua User-Agent
 *
 * @return File descriptor, or -1 if not available
 */
int ua_sipfd(const struct ua *ua)
{
	return ua ? ua->sipfd : -1;
}


const char *ua_param(const struct ua *ua, const char *key)
{
	if (!ua)
		return NULL;

	if (!str_casecmp(key, "rtpkeep"))
		return ua->rtpkeep;

	return NULL;
}
