/**
 * @file ua.c  User-Agent
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "ua"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

/** Magic number */
#define MAGIC 0x0a0a0a0a
#include "magic.h"


enum {
	REG_INTERVAL    = 3600,
	MAX_CALLS       =    4
};


/** Defines the answermodes */
enum answermode {
	ANSWERMODE_MANUAL = 0,
	ANSWERMODE_EARLY,
	ANSWERMODE_AUTO
};

/** User-Agent Parameters */
struct ua_prm {
	enum answermode answermode;  /**< Answermode for incoming calls      */
	bool aucodecs;               /**< audio_codecs parameter is set      */
	struct list aucodecl;        /**< List of preferred audio-codecs     */
	char *auth_user;             /**< Authentication username            */
	char *auth_pass;             /**< Authentication password            */
	const struct menc *menc;     /**< Media encryption type              */
	const struct mnat *mnat;     /**< Media NAT handling                 */
	char *outbound[2];           /**< Optional SIP outbound proxies      */
	uint32_t ptime;              /**< Configured packet time in [ms]     */
	uint32_t regint;             /**< Registration interval in [seconds] */
	char *regq;                  /**< Registration Q-value               */
	char *rtpkeep;               /**< RTP Keepalive mechanism            */
	char *sipnat;                /**< SIP Nat mechanism                  */
	char *stun_user;             /**< STUN Username                      */
	char *stun_pass;             /**< STUN Password                      */
	char *stun_host;             /**< STUN Hostname                      */
	uint16_t stun_port;          /**< STUN Port number                   */
	bool vidcodecs;              /**< video_codecs parameter is set      */
	struct list vidcodecl;       /**< List of preferred video-codecs     */
};

/** User-Agent Register client */
struct ua_reg {
	struct le le;                /**< Linked list element                */
	struct ua *ua;               /**< Pointer to parent UA object        */
	struct sipreg *reg;          /**< SIP Register client                */
	int id;                      /**< Registration ID (for SIP outbound) */
	int sipfd;                   /**< Cached file-descr. for SIP conn    */
	char *srv;                   /**< SIP Server id                      */
	uint16_t scode;              /**< Registration status code           */
};

/** Defines a SIP User Agent object */
struct ua {
	MAGIC_DECL                   /**< Magic number for struct ua         */
	struct le le;                /**< Linked list element                */
	struct ua_prm *prm;          /**< UA Parameters                      */
	struct list regl;            /**< List of Register clients           */
	struct list calls;           /**< List of active calls (struct call) */
	struct tmr tmr_alert;        /**< Incoming call alert timer          */
	struct tmr tmr_stat;         /**< Statistics refresh timer           */
	struct mbuf *dialbuf;        /**< Buffer for dialled number          */
	struct sip_addr aor;         /**< My SIP Address-Of-Record           */
	enum statmode statmode;      /**< Status mode                        */
	char *addr;                  /**< Buffer for my SIP Address          */
	char *local_uri;             /**< Local SIP uri                      */
	char *cuser;                 /**< SIP Contact username               */
	uint32_t n_bindings;         /**< Number of bindings for this AOR    */
	int af;                      /**< Preferred Address Family           */
	ua_event_h *eh;              /**< Event handler                      */
	ua_message_h *msgh;          /**< Incoming message handler           */
	void *arg;                   /**< Handler argument                   */
};


static struct {
	struct list ual;
	struct sip *sip;
	struct sip_lsnr *lsnr;
	struct sipsess_sock *sock;
	struct sipevent_sock *evsock;
	struct ua *cur;
	char uuid[64];
	bool use_udp;
	bool use_tcp;
	bool use_tls;
#ifdef USE_TLS
	struct tls *tls;
#endif
	enum audio_mode aumode;
	uint64_t start_ticks;          /**< Ticks when UA started         */
	bool prefer_ipv6;              /**< Force IPv6 transport          */
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
	0UL,
	false,
};


/* List of supported SIP extensions (option tags) */
static const struct pl sip_extensions[] = {
	PL("ice"),
	PL("outbound"),
};


/* prototypes */
static void menu_set_incall(bool incall);
static void register_handler(int err, const struct sip_msg *msg, void *arg);
static int  ua_call_alloc(struct call **callp, struct ua *ua,
			  const struct ua_prm *prm, const struct mnat *mnat,
			  enum vidmode vidmode, const struct sip_msg *msg,
			  struct call *xcall);


/* This function is called when all SIP transactions are done */
static void exit_handler(void *arg)
{
	(void)arg;

	re_cancel();
}


/*
 * Current call strategy.
 *
 * We can only have 1 current call. The current call is the one that was
 * added last (end of the list), which is not on-hold
 */
static struct call *current_call(const struct ua *ua)
{
	struct le *le;

	for (le = ua->calls.tail; le; le = le->prev) {

		struct call *call = le->data;

		/* todo: check if call is on-hold */

		return call;
	}

	return NULL;
}


/* Return TRUE if there any calls active */
static bool active_calls(const struct ua *ua)
{
	return !list_isempty(&ua->calls);
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


static int password_prompt(struct ua *ua)
{
	char pwd[64];
	const char *nl;
	int err;

	(void)re_printf("Please enter password for %r@%r: ",
			&ua->aor.uri.user, &ua->aor.uri.host);

	/* note: blocking UI call */
	fgets(pwd, sizeof(pwd), stdin);
	pwd[sizeof(pwd) - 1] = '\0';

	err = str_dup(&ua->prm->auth_pass, pwd);
	if (err)
		return err;

	nl = strchr(ua->prm->auth_pass, '\n');
	if (nl == NULL) {
		(void)re_printf("Invalid password (0 - 63 characters"
				" followed by newline)\n");
		return EINVAL;
	}

	return 0;
}


/**
 * Authenticate a User-Agent (UA)
 *
 * @param prm      User-Agent parameters
 * @param username Pointer to allocated username string
 * @param password Pointer to allocated password string
 * @param realm    Realm string
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_auth(struct ua_prm *prm, char **username, char **password,
	    const char *realm)
{
	if (!prm)
		return EINVAL;

	(void)realm;

	*username = mem_ref(prm->auth_user);
	*password = mem_ref(prm->auth_pass);

	return 0;
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


static int uareg_register(struct ua_reg *reg, struct ua *ua,
			  const char *reg_uri, const char *params)
{
	const char *routev[1] = {NULL};
	struct ua_prm *prm = ua->prm;
	int err;

	if (!reg)
		return EINVAL;

	reg->scode = 0;

	if (reg->id && (reg->id - 1) < (int)ARRAY_SIZE(prm->outbound))
		routev[0] = prm->outbound[reg->id - 1];

	reg->reg = mem_deref(reg->reg);
	err = sipreg_register(&reg->reg, uag.sip, reg_uri, ua->local_uri,
			      ua->local_uri, prm->regint, ua->cuser,
			      routev[0] ? routev : NULL,
			      routev[0] ? 1 : 0,
			      reg->id,
			      sip_auth_handler, ua->prm, true,
			      register_handler, reg,
			      params[0] ? &params[1] : NULL,
			      "Allow: %s\r\n", ua_allowed_methods());
	if (err) {
		DEBUG_WARNING("SIP register failed: %m\n", err);
		return err;
	}

	return 0;
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
	struct ua_prm *prm;
	struct le *le;
	struct uri uri;
	char reg_uri[64];
	char params[256] = "";
	int err;

	if (!ua)
		return EINVAL;

	prm = ua->prm;
	uri = ua->aor.uri;
	uri.user = uri.password = pl_null;
	if (re_snprintf(reg_uri, sizeof(reg_uri), "%H", uri_encode, &uri) < 0)
		return ENOMEM;

	if (str_isset(uag.uuid)) {
		if (re_snprintf(params, sizeof(params),
				";+sip.instance=\"<urn:uuid:%s>\"",
				uag.uuid) < 0)
			return ENOMEM;
	}

	if (prm->regq) {
		if (re_snprintf(&params[strlen(params)],
				sizeof(params) - strlen(params),
				";q=%s", prm->regq) < 0)
			return ENOMEM;
	}

	if (prm->mnat && prm->mnat->ftag) {
		if (re_snprintf(&params[strlen(params)],
				sizeof(params) - strlen(params),
				";%s", prm->mnat->ftag) < 0)
			return ENOMEM;
	}

	ua_event(ua, UA_EVENT_REGISTERING, NULL);

	for (le = ua->regl.head; le; le = le->next) {
		struct ua_reg *reg = le->data;

		err = uareg_register(reg, ua, reg_uri, params);
		if (err)
			return err;
	}

	return 0;
}


static inline bool ua_regok(const struct ua *ua)
{
	struct le *le;

	for (le = ua->regl.head; le; le = le->next) {

		const struct ua_reg *reg = le->data;

		if (200 <= reg->scode && reg->scode <= 299)
			return true;
	}

	return false;
}


static uint32_t ua_nreg_get(void)
{
	struct le *le;
	uint32_t n = 0;

	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;

		if (ua_regok(ua))
			++n;
	}

	return n;
}


static void ua_check_registrations(void)
{
	static bool ual_ready = false;
	uint32_t n = n_uas();

	if (ual_ready)
		return;

	if (ua_nreg_get() < n)
		return;

	/* We are ready */
	(void)re_printf("\x1b[32mAll %u useragent%s registered successfully!"
			" (%u ms)\x1b[;m\n",
			n, n==1 ? "" : "s",
			(uint32_t)(tmr_jiffies() - uag.start_ticks));

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


static int sipmsg_af(const struct sip_msg *msg)
{
	struct sa laddr;
	int err = 0;

	if (!msg)
		return AF_UNSPEC;

	switch (msg->tp) {

	case SIP_TRANSP_UDP:
		err = udp_local_get(msg->sock, &laddr);
		break;

	case SIP_TRANSP_TCP:
	case SIP_TRANSP_TLS:
		err = tcp_conn_local_get(sip_msg_tcpconn(msg), &laddr);
		break;

	default:
		return AF_UNSPEC;
	}

	return err ? AF_UNSPEC : sa_af(&laddr);
}


static void register_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct ua_reg *reg = arg;
	struct ua *ua = reg->ua;
	const struct sip_hdr *hdr;
	char buf[128];

	MAGIC_CHECK(ua);

	if (err) {
		DEBUG_WARNING("%r@%r: Register: %m\n",
			      &ua->aor.uri.user, &ua->aor.uri.host, err);

		reg->scode = 999;
		ua_event(ua, UA_EVENT_REGISTER_FAIL, strerror(err));
		return;
	}

	hdr = sip_msg_hdr(msg, SIP_HDR_SERVER);
	if (hdr) {
		reg->srv = mem_deref(reg->srv);
		(void)pl_strdup(&reg->srv, &hdr->val);
	}

	(void)re_snprintf(buf, sizeof(buf), "%u %r", msg->scode, &msg->reason);

	if (200 <= msg->scode && msg->scode <= 299) {

		ua->n_bindings = sip_msg_hdr_count(msg, SIP_HDR_CONTACT);

		if (msg->scode != reg->scode) {
			ua_printf(ua, "{%d/%s} %u %r (%s) [%u binding%s]\n",
				  reg->id, sip_transp_name(msg->tp),
				  msg->scode, &msg->reason,
				  reg->srv, ua->n_bindings,
				  1==ua->n_bindings?"":"s");
		}

		reg->scode = msg->scode;
		reg->sipfd = sipmsg_fd(msg);

		ua->af = sipmsg_af(msg);

		ua_event(ua, UA_EVENT_REGISTER_OK, buf);
	}
	else if (msg->scode >= 300) {

		DEBUG_WARNING("%s: %u %r (%s)\n", ua->local_uri,
			      msg->scode, &msg->reason, reg->srv);

		reg->scode = msg->scode;
		reg->sipfd = -1;

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
	struct call *call;

	MAGIC_CHECK(ua);

	if (STATMODE_OFF == ua->statmode)
		return;

	if (!ua_iscur(ua))
		return;

	/* the UI will only show the current active call */
	call = current_call(ua);
	if (!call)
		return;

	tmr_start(&ua->tmr_stat, 100, call_stat, ua);

	(void)re_fprintf(stderr, "%H\r", call_status, call);
}


static void alert_start(void *arg)
{
	struct ua *ua = arg;

	ui_output("\033[10;1000]\033[11;1000]\a");

	tmr_start(&ua->tmr_alert, 1000, alert_start, ua);
}


static void alert_stop(struct ua *ua)
{
	ui_output("\r");
	tmr_cancel(&ua->tmr_alert);
}


static void call_event_handler(struct call *call, enum call_event ev,
			       const void *prm, void *arg)
{
	struct ua *ua = arg;
	const char *peeruri;
	const struct pl *pl = prm;
	struct call *call2 = NULL;
	int err;

	MAGIC_CHECK(ua);

	peeruri = call_peeruri(call);

	switch (ev) {

	case CALL_EVENT_INCOMING:
		switch (ua->prm->answermode) {

		case ANSWERMODE_EARLY:
			(void)call_progress(call);
			break;

		case ANSWERMODE_AUTO:
			(void)call_answer(call, 200);
			break;

		case ANSWERMODE_MANUAL:
		default:
			if (list_count(&ua->calls) > 1) {
				(void)call_ringtone(call,
						    "callwaiting.wav", 3);
			}
			else {
				/* Alert user */
				alert_start(ua);
				(void)call_ringtone(call, "ring.wav", -1);
			}

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
		ua_printf(ua, "Call in-progress: %s\n", peeruri);
		call_stat(ua);
		ua_event(ua, UA_EVENT_CALL_PROGRESS, peeruri);
		break;

	case CALL_EVENT_ESTABLISHED:
		alert_stop(ua);
		ua_printf(ua, "Call established: %s\n", peeruri);
		call_stat(ua);
		ua_event(ua, UA_EVENT_CALL_ESTABLISHED, peeruri);
		break;

	case CALL_EVENT_CLOSED:
		alert_stop(ua);
		ua_event(ua, UA_EVENT_CALL_CLOSED, prm);
		mem_deref(call);
		break;

	case CALL_EVENT_TRANSFER:

		/*
		 * Create a new call to transfer target.
		 *
		 * NOTE: we will automatically connect a new call to the
		 *       transfer target
		 */

		ua_printf(ua, "transferring call to %r\n", pl);

		err = ua_call_alloc(&call2, ua, ua->prm, ua->prm->mnat,
				    VIDMODE_ON, NULL, call);
		if (!err) {
			err = call_connect(call2, pl);
			if (err) {
				DEBUG_WARNING("transfer: connect error: %m\n",
					      err);
			}
		}

		if (err) {
			(void)call_notify_sipfrag(call, 500, "%m", err);
			mem_deref(call2);
		}
		break;
	}

	menu_set_incall(active_calls(ua));
}


static int ua_call_alloc(struct call **callp, struct ua *ua,
			 const struct ua_prm *prm, const struct mnat *mnat,
			 enum vidmode vidmode, const struct sip_msg *msg,
			 struct call *xcall)
{
	struct call_prm cprm;
	char dname[128] = "";

	if (*callp) {
		DEBUG_WARNING("call_alloc: call is already allocated\n");
		return EALREADY;
	}

	cprm.ptime   = prm->ptime;
	cprm.aumode  = uag.aumode;
	cprm.vidmode = vidmode;
	cprm.af      = ua->af;

	(void)pl_strcpy(&ua->aor.dname, dname, sizeof(dname));

	return call_alloc(callp, &ua->calls, ua, &cprm, mnat,
			  prm->stun_user, prm->stun_pass,
			  prm->stun_host, prm->stun_port,
			  prm->menc, dname, ua->local_uri, msg, xcall,
			  call_event_handler, ua);
}


static void handle_options(struct ua *ua, const struct sip_msg *msg)
{
	struct call *call = NULL;
	struct mbuf *desc = NULL;
	int err;

	err = ua_call_alloc(&call, ua, ua->prm, NULL,
			    VIDMODE_ON, NULL, NULL);
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
		DEBUG_WARNING("options: sip_treplyf: %m\n", err);
	}

 out:
	mem_deref(desc);
	mem_deref(call);
}


/* RFC 3428 */
static void handle_message(struct ua *ua, const struct sip_msg *msg)
{
	static const char *ctype_text = "text/plain";
	struct pl mtype;

	if (re_regex(msg->ctype.p, msg->ctype.l, "[^;]+", &mtype))
		mtype = msg->ctype;

	if (ua->msgh) {
		ua->msgh(&msg->from.auri, &msg->ctype, msg->mb, ua->arg);
		(void)sip_reply(uag.sip, msg, 200, "OK");
	}
	else if (!pl_strcasecmp(&mtype, ctype_text)) {
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


static void prm_destructor(void *arg)
{
	struct ua_prm *prm = arg;
	size_t i;

	list_flush(&prm->aucodecl);
	list_flush(&prm->vidcodecl);
	mem_deref(prm->auth_user);
	mem_deref(prm->auth_pass);
	for (i=0; i<ARRAY_SIZE(prm->outbound); i++)
		mem_deref(prm->outbound[i]);
	mem_deref(prm->regq);
	mem_deref(prm->rtpkeep);
	mem_deref(prm->sipnat);
	mem_deref(prm->stun_user);
	mem_deref(prm->stun_pass);
	mem_deref(prm->stun_host);
}


static void uareg_destructor(void *arg)
{
	struct ua_reg *reg = arg;

	list_unlink(&reg->le);
	mem_deref(reg->srv);
	mem_deref(reg->reg);
}


static void ua_destructor(void *arg)
{
	struct ua *ua = arg;

	list_unlink(&ua->le);

	tmr_cancel(&ua->tmr_stat);
	tmr_cancel(&ua->tmr_alert);

	mem_deref(ua->dialbuf);
	mem_deref(ua->addr);
	list_flush(&ua->calls);
	mem_deref(ua->cuser);
	mem_deref(ua->local_uri);

	list_flush(&ua->regl);
	mem_deref(ua->prm);
}


/**
 * Decode STUN Server parameter. We use the SIP parameters as default,
 * and override with any STUN parameters present.
 *
 * \verbatim
 *   ;stunserver=stun:username:password@host:port
 * \endverbatim
 */
static int stunsrv_decode(struct ua_prm *prm, const struct ua *ua)
{
	struct pl srv;
	struct uri uri;
	int err;

	memset(&uri, 0, sizeof(uri));

	if (0 == sip_param_decode(&ua->aor.params, "stunserver", &srv)) {

		DEBUG_NOTICE("got stunserver: '%r'\n", &srv);

		err = uri_decode(&uri, &srv);
		if (err) {
			DEBUG_WARNING("%r: decode failed: %m\n", &srv, err);
			memset(&uri, 0, sizeof(uri));
		}

		if (0 != pl_strcasecmp(&uri.scheme, "stun")) {
			DEBUG_WARNING("unknown scheme: %r\n", &uri.scheme);
			return EINVAL;
		}
	}

	err = 0;
	if (pl_isset(&uri.user))
		err |= pl_strdup(&prm->stun_user, &uri.user);
	else
		err |= pl_strdup(&prm->stun_user, &ua->aor.uri.user);

	if (pl_isset(&uri.password))
		err |= pl_strdup(&prm->stun_pass, &uri.password);
	else
		err |= pl_strdup(&prm->stun_pass, &ua->aor.uri.password);

	if (pl_isset(&uri.host))
		err |= pl_strdup(&prm->stun_host, &uri.host);
	else
		err |= pl_strdup(&prm->stun_host, &ua->aor.uri.host);

	prm->stun_port = uri.port;

	return err;
}


/** Decode media parameters */
static int media_decode(struct ua_prm *prm, const struct ua *ua)
{
	struct pl mnat, menc, ptime, rtpkeep;
	int err = 0;

	/* Decode media nat parameter */
	if (0 == sip_param_decode(&ua->aor.params, "medianat", &mnat)) {

		char mnatid[64];

		ua_printf(ua, "Using medianat: %r\n", &mnat);

		(void)pl_strcpy(&mnat, mnatid, sizeof(mnatid));

		prm->mnat = mnat_find(mnatid);
		if (!prm->mnat) {
			DEBUG_WARNING("medianat not found: %r\n", &mnat);
		}
	}

	/* Media encryption */
	if (0 == sip_param_decode(&ua->aor.params, "mediaenc", &menc)) {

		char mencid[64];

		ua_printf(ua, "Using media encryption `%r'\n", &menc);

		(void)pl_strcpy(&menc, mencid, sizeof(mencid));

		prm->menc = menc_find(mencid);
		if (!prm->menc) {
			DEBUG_WARNING("mediaenc not found: %r\n", &menc);
		}
	}

	/* Decode ptime parameter */
	if (0 == sip_param_decode(&ua->aor.params, "ptime", &ptime)) {
		prm->ptime = pl_u32(&ptime);
		if (!prm->ptime) {
			DEBUG_WARNING("ptime must be greater than zero\n");
			return EINVAL;
		}
		DEBUG_NOTICE("setting ptime=%u\n", prm->ptime);
	}

	if (!sip_param_decode(&ua->aor.params, "rtpkeep", &rtpkeep)) {

		ua_printf(ua, "Using RTP keepalive: %r\n", &rtpkeep);

		err = pl_strdup(&prm->rtpkeep, &rtpkeep);
	}

	return err;
}


/* Decode answermode parameter */
static void answermode_decode(struct ua_prm *prm, const struct pl *pl)
{
	struct pl amode;

	if (0 == sip_param_decode(pl, "answermode", &amode)) {

		if (0 == pl_strcasecmp(&amode, "manual")) {
			prm->answermode = ANSWERMODE_MANUAL;
		}
		else if (0 == pl_strcasecmp(&amode, "early")) {
			prm->answermode = ANSWERMODE_EARLY;
		}
		else if (0 == pl_strcasecmp(&amode, "auto")) {
			prm->answermode = ANSWERMODE_AUTO;
		}
		else {
			DEBUG_WARNING("answermode: unknown (%r)\n", &amode);
			prm->answermode = ANSWERMODE_MANUAL;
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


static int audio_codecs_decode(struct ua_prm *prm, const struct ua *ua)
{
	struct pl tmp;
	int err;

	list_init(&prm->aucodecl);

	if (0 == sip_param_exists(&ua->aor.params, "audio_codecs", &tmp)) {
		struct pl acs;
		char cname[64];

		prm->aucodecs = true;

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

			err = aucodec_clone(&prm->aucodecl, ac);
			if (err)
				return err;
		}
	}

	return 0;
}


static int video_codecs_decode(struct ua_prm *prm, const struct ua *ua)
{
	struct pl tmp;
	int err;

	list_init(&prm->vidcodecl);

	if (0 == sip_param_exists(&ua->aor.params, "video_codecs", &tmp)) {
		struct pl vcs;
		char cname[64];

		prm->vidcodecs = true;

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

			err = vidcodec_clone(&prm->vidcodecl, vc);
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

	err = re_sdprintf(&ua->local_uri, "%H", encode_uri_user, &ua->aor.uri);
	if (err)
		return err;

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


static int sip_params_decode(struct ua_prm *prm, const struct ua *ua)
{
	struct pl regint, regq, ob, sipnat, auth_user;
	size_t i;
	int err = 0;

	prm->regint = REG_INTERVAL + (rand_u32()&0xff);
	if (0 == sip_param_decode(&ua->aor.params, "regint", &regint)) {
		prm->regint = pl_u32(&regint);
	}

	prm->regq = 0;
	if (0 == sip_param_decode(&ua->aor.params, "regq", &regq)) {
	        err |= pl_strdup(&prm->regq, &regq);
	}

	for (i=0; i<ARRAY_SIZE(prm->outbound); i++) {

		char expr[16] = "outbound";

		expr[8] = i + 1 + 0x30;
		expr[9] = '\0';

		if (0 == sip_param_decode(&ua->aor.params, expr, &ob)) {
			err |= pl_strdup(&prm->outbound[i], &ob);
		}
	}

	/* backwards compat */
	if (!prm->outbound[0]) {
		if (0 == sip_param_decode(&ua->aor.params, "outbound", &ob)) {
			err |= pl_strdup(&prm->outbound[0], &ob);
		}
	}

	if (0 == sip_param_decode(&ua->aor.params, "sipnat", &sipnat)) {
		DEBUG_NOTICE("sipnat: %r\n", &sipnat);
		err |= pl_strdup(&prm->sipnat, &sipnat);
	}

	if (0 == sip_param_decode(&ua->aor.params, "auth_user", &auth_user))
		err |= pl_strdup(&prm->auth_user, &auth_user);
	else
		err |= pl_strdup(&prm->auth_user, &ua->aor.uri.user);

	return err;
}


static int uareg_add(struct list *lst, struct ua *ua, int regid)
{
	struct ua_reg *reg;

	reg = mem_zalloc(sizeof(*reg), uareg_destructor);
	if (!reg)
		return ENOMEM;

	reg->ua    = ua;
	reg->id    = regid;
	reg->sipfd = -1;

	list_append(lst, &reg->le, reg);

	return 0;
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

	ua->prm = mem_zalloc(sizeof(*ua->prm), prm_destructor);
	if (!ua->prm) {
		err = ENOMEM;
		goto out;
	}

	list_append(&uag.ual, &ua->le, ua);

	list_init(&ua->calls);

	tmr_init(&ua->tmr_stat);
	tmr_init(&ua->tmr_alert);

	ua->dialbuf = mbuf_alloc(64);
	if (!ua->dialbuf) {
		err = ENOMEM;
		goto out;
	}

#if HAVE_INET6
	ua->af   = uag.prefer_ipv6 ? AF_INET6 : AF_INET;
#else
	ua->af   = AF_INET;
#endif
	ua->eh   = eh;
	ua->msgh = msgh;
	ua->arg  = arg;

	err = mk_aor(ua, aor);
	if (err)
		goto out;

	/* Decode address parameters */
	err |= sip_params_decode(ua->prm, ua);
	answermode_decode(ua->prm, &ua->aor.params);
	err |= audio_codecs_decode(ua->prm, ua);
	err |= video_codecs_decode(ua->prm, ua);
	err |= media_decode(ua->prm, ua);
	if (ua->prm->mnat)
		err |= stunsrv_decode(ua->prm, ua);
	if (err)
		goto out;

	/* optional password prompt */
	if (!pl_isset(&ua->aor.uri.password)) {
		err = password_prompt(ua);
		if (err)
			goto out;
	}
	else {
		err = pl_strdup(&ua->prm->auth_pass, &ua->aor.uri.password);
		if (err)
			goto out;
	}

	/* Register clients */
	if (0 == str_casecmp(ua->prm->sipnat, "outbound")) {

		size_t i;

		if (!str_isset(uag.uuid)) {

			DEBUG_WARNING("outbound requires valid UUID!\n");
			err = ENOSYS;
			goto out;
		}

		for (i=0; i<ARRAY_SIZE(ua->prm->outbound); i++) {

			if (ua->prm->outbound[i]) {
				err = uareg_add(&ua->regl, ua, (int)i+1);
				if (err)
					break;
			}
		}
	}
	else {
		err = uareg_add(&ua->regl, ua, 0);
	}
	if (err)
		goto out;

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
	if (!ua->prm->regint)
		return 0;

	return ua_register(ua);
}


/**
 * Add a User-Agent (UA)
 *
 * @param addr SIP Address string
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_add(const struct pl *addr)
{
	struct ua *ua;
	char buf[512];
	int err;

	(void)pl_strcpy(addr, buf, sizeof(buf));

	err = ua_alloc(&ua, buf, NULL, NULL, NULL);
	if (err)
		return err;

#if 0
	/* todo: move statmode to a global state */
	if (app.run_daemon)
		ua_set_statmode(ua, STATMODE_OFF);
#endif

	err = ua_start(ua);
	if (err)
		return err;

	return err;
}


/**
 * Connect an outgoing call to a given SIP uri
 *
 * @param ua      User-Agent
 * @param uri     SIP uri to connect to
 * @param params  Optional URI parameters
 * @param mnatid  Optional MNAT id to override default settings
 * @param vmode   Video mode
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_connect(struct ua *ua, const char *uri, const char *params,
	       const char *mnatid, enum vidmode vmode)
{
	const struct mnat *mnat;
	struct call *call = NULL;
	struct pl pl;
	size_t len;
	int err = 0;

	if (!ua || !uri)
		return EINVAL;

	len = strlen(uri);
	if (len > 0) {
		mbuf_rewind(ua->dialbuf);

		if (params)
			err |= mbuf_printf(ua->dialbuf, "<");

		/* Append sip: scheme if missing */
		if (0 != re_regex(uri, len, "sip:"))
			err |= mbuf_printf(ua->dialbuf, "sip:");

		err |= mbuf_write_str(ua->dialbuf, uri);

		/* Append domain if missing */
		if (0 != re_regex(uri, len, "[^@]+@[^]+", NULL, NULL)) {
#if HAVE_INET6
			if (AF_INET6 == ua->aor.uri.af)
				err |= mbuf_printf(ua->dialbuf, "@[%r]",
						   &ua->aor.uri.host);
			else
#endif
				err |= mbuf_printf(ua->dialbuf, "@%r",
						   &ua->aor.uri.host);

			/* Also append port if specified and not 5060 */
			switch (ua->aor.uri.port) {

			case 0:
			case SIP_PORT:
				break;

			default:
				err |= mbuf_printf(ua->dialbuf, ":%u",
						   ua->aor.uri.port);

				break;
			}
		}

		if (params) {
			err |= mbuf_printf(ua->dialbuf, ";%s", params);
		}

		/* Append any optional parameters */
		err |= mbuf_write_pl(ua->dialbuf, &ua->aor.uri.params);

		if (params)
			err |= mbuf_printf(ua->dialbuf, ">");
	}

	if (err)
		return err;

	/* override medianat per call */
	if (mnatid) {
		mnat = mnat_find(mnatid);
	}
	else
		mnat = ua->prm->mnat;

	err = ua_call_alloc(&call, ua, ua->prm, mnat, vmode, NULL, NULL);
	if (err)
		return err;

	pl.p = (char *)ua->dialbuf->buf;
	pl.l = ua->dialbuf->end;

	err = call_connect(call, &pl);

	if (err)
		mem_deref(call);

	return err;
}


/**
 * Hangup the current call
 *
 * @param ua User-Agent
 */
void ua_hangup(struct ua *ua)
{
	struct call *call;

	if (!ua)
		return;

	call = current_call(ua);
	if (!call)
		return;

	(void)call_hangup(call);

	mem_deref(call);
	menu_set_incall(active_calls(ua));
}


/**
 * Answer an incoming call
 *
 * @param ua User-Agent
 */
void ua_answer(struct ua *ua)
{
	struct call *call;

	if (!ua)
		return;

	call = current_call(ua);
	if (!call) {
		DEBUG_NOTICE("answer: no incoming calls found\n");
		return;
	}

	/* todo: put previous call on-hold (if configured) */

	(void)call_answer(call, 200);
}


static const char *uareg_status(uint16_t scode)
{
	if (0 == scode)        return "\x1b[33m" "zzz" "\x1b[;m";
	else if (200 == scode) return "\x1b[32m" "OK " "\x1b[;m";
	else                   return "\x1b[31m" "ERR" "\x1b[;m";
}


static int ua_print_status(struct re_printf *pf, const struct ua *ua)
{
	struct le *le;
	char userhost[64];
	int err;

	if (!ua)
		return 0;

	if (re_snprintf(userhost, sizeof(userhost), "%H",
			encode_uri_user, &ua->aor.uri) < 0)
		return ENOMEM;
	err = re_hprintf(pf, "%-42s (%2u)", userhost, ua->n_bindings);

	for (le = ua->regl.head; le; le = le->next) {
		struct ua_reg *reg = le->data;

		err |= re_hprintf(pf, " %s %s",
				  uareg_status(reg->scode), reg->srv);
	}
	err |= re_hprintf(pf, "\n");

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
		DEBUG_WARNING("send options: (%m)\n", err);
	}

	return err;
}


static void im_resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct ua *ua = arg;

	(void)ua;

	if (err) {
		(void)re_fprintf(stderr, " \x1b[31m%m\x1b[;m\n", err);
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
int ua_im_send(struct ua *ua, const char *peer, const char *msg)
{
	struct sip_addr addr;
	struct pl pl;
	char *uri = NULL;
	int err = 0;

	if (!ua || !peer || !msg)
		return EINVAL;

	pl_set_str(&pl, peer);

	err = sip_addr_decode(&addr, &pl);
	if (err)
		return err;

	err = pl_strdup(&uri, &addr.auri);
	if (err)
		return err;

	err = sip_req_send(ua, "MESSAGE", uri, im_resp_handler, ua,
			   "Accept: text/plain\r\n"
			   "Content-Type: text/plain\r\n"
			   "Content-Length: %u\r\n"
			   "\r\n%s",
			   strlen(msg), msg);

	mem_deref(uri);

	return err;
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
const char *ua_outbound(const struct ua *ua)
{
	/* NOTE: we pick the first outbound server, should be rotated? */
	return ua ? ua->prm->outbound[0] : NULL;
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
	return ua ? current_call(ua) : NULL;
}


static int uaprm_debug(struct re_printf *pf, const struct ua_prm *prm)
{
	struct le *le;
	size_t i;
	int err = 0;

	if (!prm)
		return 0;

	err |= re_hprintf(pf, "\nUA Parameters:\n");
	err |= re_hprintf(pf, " answermode:   %d\n", prm->answermode);
	if (prm->aucodecs) {
		err |= re_hprintf(pf, " audio_codecs:");
		for (le = list_head(&prm->aucodecl); le; le = le->next) {
			const struct aucodec *ac = le->data;
			err |= re_hprintf(pf, " %s/%u/%u",
					  ac->name, ac->srate, ac->ch);
		}
		err |= re_hprintf(pf, "\n");
	}
	err |= re_hprintf(pf, " auth_user:    %s\n", prm->auth_user);
	err |= re_hprintf(pf, " mediaenc:     %s\n",
			  prm->menc ? prm->menc->id : "none");
	err |= re_hprintf(pf, " medianat:     %s\n",
			  prm->mnat ? prm->mnat->id : "none");
	for (i=0; i<ARRAY_SIZE(prm->outbound); i++) {
		if (prm->outbound[i]) {
			err |= re_hprintf(pf, " outbound%d:    %s\n",
					  i+1, prm->outbound[i]);
		}
	}
	err |= re_hprintf(pf, " ptime:        %u\n", prm->ptime);
	err |= re_hprintf(pf, " regint:       %u\n", prm->regint);
	err |= re_hprintf(pf, " regq:         %s\n", prm->regq);
	err |= re_hprintf(pf, " rtpkeep:      %s\n", prm->rtpkeep);
	err |= re_hprintf(pf, " sipnat:       %s\n", prm->sipnat);
	err |= re_hprintf(pf, " stunserver:   stun:%s@%s:%u\n",
			  prm->stun_user, prm->stun_host, prm->stun_port);
	if (prm->vidcodecs) {
		err |= re_hprintf(pf, " video_codecs:");
		for (le = list_head(&prm->vidcodecl); le; le = le->next) {
			const struct vidcodec *vc = le->data;
			err |= re_hprintf(pf, " %s", vc->name);
		}
		err |= re_hprintf(pf, "\n");
	}

	return err;
}


static int uareg_debug(struct re_printf *pf, const struct ua_reg *reg)
{
	int err = 0;

	if (!reg)
		return 0;

	err |= re_hprintf(pf, "\nRegister client:\n");
	err |= re_hprintf(pf, " id:     %d\n", reg->id);
	err |= re_hprintf(pf, " scode:  %u (%s)\n",
			  reg->scode, uareg_status(reg->scode));
	err |= re_hprintf(pf, " sipfd:  %d\n", reg->sipfd);
	err |= re_hprintf(pf, " srv:    %s\n", reg->srv);

	return err;
}


int ua_debug(struct re_printf *pf, const struct ua *ua)
{
	struct le *le;
	int err;

	if (!ua)
		return 0;

	err  = re_hprintf(pf, "--- %H ---\n", uri_encode, &ua->aor.uri);
	err |= re_hprintf(pf, " addr:      %s\n", ua->addr);
	err |= re_hprintf(pf, " local_uri: %s\n", ua->local_uri);
	err |= re_hprintf(pf, " cuser:     %s\n", ua->cuser);
	err |= re_hprintf(pf, " af:        %s\n", net_af2name(ua->af));

	err |= uaprm_debug(pf, ua->prm);

	for (le = ua->regl.head; le; le = le->next)
		err |= uareg_debug(pf, le->data);

	return err;
}


/* One instance */


static int add_transp_af(const struct sa *laddr)
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

		if (sa_af(laddr) != sa_af(&local))
			return 0;
	}
	else {
		sa_cpy(&local, laddr);
		sa_set_port(&local, 0);
	}

	if (uag.use_udp)
		err |= sip_transp_add(uag.sip, SIP_TRANSP_UDP, &local);
	if (uag.use_tcp)
		err |= sip_transp_add(uag.sip, SIP_TRANSP_TCP, &local);
	if (err) {
		DEBUG_WARNING("SIP Transport failed: %m\n", err);
		return err;
	}

#ifdef USE_TLS
	if (uag.use_tls) {
		/* Build our SSL context*/
		if (!uag.tls) {
			err = tls_alloc(&uag.tls, TLS_METHOD_SSLV23,
					NULL, NULL);
			if (err) {
				DEBUG_WARNING("tls_alloc() failed: %m\n", err);
				return err;
			}
		}

		if (sa_isset(&local, SA_PORT))
			sa_set_port(&local, sa_port(&local) + 1);

		err = sip_transp_add(uag.sip, SIP_TRANSP_TLS, &local, uag.tls);
		if (err) {
			DEBUG_WARNING("SIP/TLS transport failed: %m\n", err);
			return err;
		}
	}
#endif

	return err;
}


static int ua_add_transp(void)
{
	int err = 0;

	if (!uag.prefer_ipv6) {
		if (sa_isset(net_laddr_af(AF_INET), SA_ADDR))
			err |= add_transp_af(net_laddr_af(AF_INET));
	}

#if HAVE_INET6
	if (sa_isset(net_laddr_af(AF_INET6), SA_ADDR))
		err |= add_transp_af(net_laddr_af(AF_INET6));
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
			software, exit_handler, NULL);
	if (err) {
		DEBUG_WARNING("sip stack failed: %m\n", err);
		return err;
	}

	err = ua_add_transp();

	return err;
}


static bool require_handler(const struct sip_hdr *hdr,
			    const struct sip_msg *msg, void *arg)
{
	bool supported = false;
	size_t i;

	(void)msg;
	(void)arg;

	/* XXX: potential slow lookup, use dynamic stringmap instead */
	for (i=0; i<ARRAY_SIZE(sip_extensions); i++) {

		if (!pl_casecmp(&hdr->val, &sip_extensions[i])) {
			supported = true;
			break;
		}
	}

	return !supported;
}


/* Handle incoming calls */
static void sipsess_conn_handler(const struct sip_msg *msg, void *arg)
{
	const struct sip_hdr *hdr;
	struct ua *ua;
	struct call *call = NULL;
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

	/* handle multiple calls */
	if (list_count(&ua->calls) + 1 > MAX_CALLS) {
		DEBUG_NOTICE("rejected call from %r (maximum %d calls)\n",
			     &msg->from.auri, MAX_CALLS);
		(void)sip_treply(NULL, uag.sip, msg, 486, "Busy Here");
		return;
	}

	/* Handle Require: header, check for any required extensions */
	hdr = sip_msg_hdr_apply(msg, true, SIP_HDR_REQUIRE,
				require_handler, ua);
	if (hdr) {
		DEBUG_NOTICE("call from %r rejected with 420"
			     " -- option-tag '%r' not supported\n",
			     &msg->from.auri, &hdr->val);

		(void)sip_treplyf(NULL, NULL, uag.sip, msg, false,
				  420, "Bad Extension",
				  "Unsupported: %r\r\n"
				  "Content-Length: 0\r\n\r\n",
				  &hdr->val);
		return;
	}

	err = ua_call_alloc(&call, ua, ua->prm, ua->prm->mnat,
			    VIDMODE_ON, msg, NULL);
	if (err) {
		DEBUG_WARNING("call_alloc: %m\n", err);
		goto error;
	}

	err = call_accept(call, uag.sock, msg);
	if (err)
		goto error;

	return;

 error:
	mem_deref(call);
	(void)re_snprintf(str, sizeof(str), "Error (%m)", err);
	(void)sip_treply(NULL, uag.sip, msg, 500, str);
}


static void net_change_handler(void *arg)
{
	(void)arg;

	(void)re_printf("IP-address changed: %j\n", net_laddr_af(AF_INET));

	(void)ua_reset_transp(true, true);
}


static int cmd_ua_next(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;
	ua_next();
	return 0;
}


static int cmd_ua_debug(struct re_printf *pf, void *unused)
{
	(void)unused;
	return ua_debug(pf, ua_cur());
}


static int cmd_print_calls(struct re_printf *pf, void *unused)
{
	(void)unused;
	return ua_print_calls(pf, ua_cur());
}


static int cmd_quit(struct re_printf *pf, void *unused)
{
	int err;

	(void)unused;

	err = re_hprintf(pf, "Quit\n");

	ua_stop_all(false);

	return err;
}


static const struct cmd cmdv[] = {
	{' ',       0, "Toggle UAs",               cmd_ua_next          },
	{'u',       0, "UA debug",                 cmd_ua_debug         },
	{'l',       0, "List active calls",        cmd_print_calls      },
	{'q',       0, "Quit",                     cmd_quit             },
};


/**
 * Initialise the User-Agents
 *
 * @param software    SIP User-Agent string
 * @param udp         Enable UDP transport
 * @param tcp         Enable TCP transport
 * @param tls         Enable TLS transport
 * @param prefer_ipv6 Prefer IPv6 flag
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_init(const char *software, bool udp, bool tcp, bool tls,
	    bool prefer_ipv6)
{
	int err;

	/* Initialise Network */
	err = net_init();
	if (err) {
		DEBUG_WARNING("network init failed: %m\n", err);
		return err;
	}

	uag.start_ticks = tmr_jiffies();
	uag.prefer_ipv6 = prefer_ipv6;
	list_init(&uag.ual);

	err = ua_setup_transp(software, udp, tcp, tls);
	if (err)
		goto out;

	err = sip_listen(&uag.lsnr, uag.sip, true, request_handler, NULL);
	if (err)
		goto out;

	err = sipsess_listen(&uag.sock, uag.sip, config.sip.trans_bsize,
			     sipsess_conn_handler, NULL);
	if (err)
		goto out;

	err = sipevent_listen(&uag.evsock, uag.sip,
			      config.sip.trans_bsize, config.sip.trans_bsize,
			      NULL, NULL);
	if (err)
		goto out;

	err = cmd_register(cmdv, ARRAY_SIZE(cmdv));
	if (err)
		goto out;

	net_change(60, net_change_handler, NULL);

 out:
	if (err) {
		DEBUG_WARNING("init failed (%m)\n", err);
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

	if (str_isset(uuid))
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
	menu_set_incall(false);
	cmd_unregister(cmdv);
	net_close();
	play_close();

	uag.evsock = mem_deref(uag.evsock);
	uag.sock = mem_deref(uag.sock);
	uag.lsnr = mem_deref(uag.lsnr);
	uag.sip = mem_deref(uag.sip);

#ifdef USE_TLS
	uag.tls = mem_deref(uag.tls);
#endif

	list_flush(&uag.ual);
}


static void ua_unregister(struct ua *ua)
{
	struct le *le;

	for (le = ua->regl.head; le; le = le->next) {
		struct ua_reg *reg = le->data;

		reg->reg = mem_deref(reg->reg);
	}
}


/**
 * Suspend the SIP stack
 */
void ua_stack_suspend(void)
{
	struct le *le;

	for (le = uag.ual.head; le; le = le->next)
		ua_unregister(le->data);

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
 * Stop all User-Agents
 *
 * @param forced True to force, otherwise false
 */
void ua_stop_all(bool forced)
{
	module_app_unload();

	if (!list_isempty(&uag.ual)) {
		(void)re_fprintf(stderr, "Un-registering %u useragents.. %s\n",
				 n_uas(), forced ? "(Forced)" : "");
	}

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
			err |= ua_register(ua);
		}

		/* update all active calls */
		if (reinvite) {
			struct le *lec;

			for (lec = ua->calls.head; lec; lec = lec->next) {
				struct call *call = lec->data;

				err |= call_reset_transp(call);
			}
		}
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
 * Print the current SIP Call status for the current User-Agent
 *
 * @param pf     Print handler for debug output
 * @param unused Unused parameter
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_print_call_status(struct re_printf *pf, void *unused)
{
	struct call *call;
	int err;

	(void)unused;

	call = current_call(ua_cur());
	if (call) {
		err  = re_hprintf(pf, "\n--- Call status: ---\n");
		err |= call_debug(pf, call);
		err |= re_hprintf(pf, "\n");
	}
	else {
		err  = re_hprintf(pf, "\n(no active calls)\n");
	}

	return err;
}


/**
 * Print all calls for a given User-Agent
 */
int ua_print_calls(struct re_printf *pf, const struct ua *ua)
{
	struct le *le;
	int err = 0;

	err |= re_hprintf(pf, "\n--- List of active calls (%u): ---\n",
			  list_count(&ua->calls));

	for (le = ua->calls.head; le; le = le->next) {

		const struct call *call = le->data;

		err |= re_hprintf(pf, "  %H\n", call_info, call);
	}

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


/**
 * Get the global SIP Event socket
 *
 * @return SIP Event socket
 */
struct sipevent_sock *uag_sipevent_sock(void)
{
	return uag.evsock;
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


struct list *ua_aucodecl(const struct ua *ua)
{
	return (ua && ua->prm->aucodecs)
		? (struct list *)&ua->prm->aucodecl : aucodec_list();
}


struct list *ua_vidcodecl(const struct ua *ua)
{
	return (ua && ua->prm->vidcodecs)
		? (struct list *)&ua->prm->vidcodecl : vidcodec_list();
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
	struct le *le;

	if (!ua)
		return -1;

	for (le = ua->regl.head; le; le = le->next) {

		struct ua_reg *reg = le->data;

		if (reg->sipfd != -1)
			return reg->sipfd;
	}

	return -1;
}


const char *ua_param(const struct ua *ua, const char *key)
{
	if (!ua)
		return NULL;

	if (!str_casecmp(key, "rtpkeep"))
		return ua->prm->rtpkeep;

	return NULL;
}


/**
 * Find the correct UA from the contact user
 *
 * @param cuser Contact username
 *
 * @return Matching UA if found, NULL if not found
 */
struct ua *ua_find(const struct pl *cuser)
{
	struct le *le;

	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;

		if (0 == pl_strcasecmp(cuser, ua->cuser))
			return ua;
	}

	/* Try also matching by AOR, for better interop */
	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;

		if (0 == pl_casecmp(cuser, &ua->aor.uri.user))
			return ua;
	}

	return NULL;
}


/**
 * Find a User-Agent (UA) from an Address-of-Record (AOR)
 *
 * @param aor Address-of-Record string
 *
 * @return User-Agent (UA) if found, otherwise NULL
 */
struct ua *ua_find_aor(const char *aor)
{
	struct le *le;

	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;

		if (str_isset(aor) && strcmp(ua->local_uri, aor))
			continue;

		return ua;
	}

	return NULL;
}


/**
 * Get the contact user of a User-Agent (UA)
 *
 * @param ua User-Agent
 *
 * @return Contact user
 */
const char *ua_cuser(const struct ua *ua)
{
	return ua ? ua->cuser : NULL;
}


static int call_audio_debug(struct re_printf *pf, void *unused)
{
	(void)unused;
	return audio_debug(pf, call_audio(ua_call(ua_cur())));
}


static int call_audioenc_cycle(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;
	call_audioencoder_cycle(ua_call(ua_cur()));
	return 0;
}


static int call_reinvite(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;
	return call_modify(ua_call(ua_cur()));
}


static int call_mute(struct re_printf *pf, void *unused)
{
	static bool muted = false;
	(void)unused;

	muted = !muted;
	(void)re_hprintf(pf, "\ncall %smuted\n", muted ? "" : "un-");
	audio_mute(call_audio(ua_call(ua_cur())), muted);

	return 0;
}


static int call_xfer(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	(void)pf;

	ua_set_statmode(ua_cur(), STATMODE_OFF);

	return call_transfer(ua_call(ua_cur()), carg->prm);
}


static int call_holdresume(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	(void)pf;

	return call_hold(ua_call(ua_cur()), 'x' == carg->key);
}


#ifdef USE_VIDEO
static int call_videoenc_cycle(struct re_printf *pf, void *unused)
{
	(void)pf;
	(void)unused;
	call_videoencoder_cycle(ua_call(ua_cur()));
	return 0;
}


static int call_video_debug(struct re_printf *pf, void *unused)
{
	(void)unused;
	return video_debug(pf, call_video(ua_call(ua_cur())));
}
#endif


static int digit_handler(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	struct call *call;
	int err = 0;

	(void)pf;

	call = ua_call(ua_cur());
	if (call)
		err = call_send_digit(call, carg->key);

	return err;
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


struct list *uag_list(void)
{
	return &uag.ual;
}


/**
 * Return list of methods supported by the UA
 *
 * @return String of supported methods
 */
const char *ua_allowed_methods(void)
{
	return "INVITE,ACK,BYE,CANCEL,REFER,NOTIFY,SUBSCRIBE,INFO";
}


struct ua_prm *ua_prm(const struct ua *ua)
{
	return ua ? ua->prm : NULL;
}
