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
	MAX_CALLS       =    4
};


/** Defines a SIP User Agent object */
struct ua {
	MAGIC_DECL                   /**< Magic number for struct ua         */
	struct ua **uap;             /**< Pointer to application's ua        */
	struct le le;                /**< Linked list element                */
	struct account *acc;         /**< Account Parameters                 */
	struct list regl;            /**< List of Register clients           */
	struct list calls;           /**< List of active calls (struct call) */
	char *cuser;                 /**< SIP Contact username               */
	int af;                      /**< Preferred Address Family           */
};

struct ua_eh {
	struct le le;
	ua_event_h *h;
	void *arg;
};

static struct {
	struct config_sip *cfg;        /**< SIP configuration               */
	struct list ual;               /**< List of User-Agents (struct ua) */
	struct list ehl;               /**< Event handlers (struct ua_eh)   */
	struct sip *sip;               /**< SIP Stack                       */
	struct sip_lsnr *lsnr;         /**< SIP Listener                    */
	struct sipsess_sock *sock;     /**< SIP Session socket              */
	struct sipevent_sock *evsock;  /**< SIP Event socket                */
	bool use_udp;                  /**< Use UDP transport               */
	bool use_tcp;                  /**< Use TCP transport               */
	bool use_tls;                  /**< Use TLS transport               */
	bool prefer_ipv6;              /**< Force IPv6 transport            */
#ifdef USE_TLS
	struct tls *tls;               /**< TLS Context                     */
#endif
} uag = {
	NULL,
	LIST_INIT,
	LIST_INIT,
	NULL,
	NULL,
	NULL,
	NULL,
	true,
	true,
	true,
	false,
#ifdef USE_TLS
	NULL,
#endif
};


/* List of supported SIP extensions (option tags) */
static const struct pl sip_extensions[] = {
	PL("ice"),
	PL("outbound"),
};


/* prototypes */
static int  ua_call_alloc(struct call **callp, struct ua *ua,
			  enum vidmode vidmode, const struct sip_msg *msg,
			  struct call *xcall);


/* This function is called when all SIP transactions are done */
static void exit_handler(void *arg)
{
	(void)arg;

	re_cancel();
}


void ua_printf(const struct ua *ua, const char *fmt, ...)
{
	va_list ap;

	if (!ua)
		return;

	va_start(ap, fmt);
	(void)re_fprintf(stderr, "%r@%r: ",
			 &ua->acc->luri.user, &ua->acc->luri.host);
	(void)re_vfprintf(stderr, fmt, ap);
	va_end(ap);
}


void ua_event(struct ua *ua, enum ua_event ev, const char *fmt, ...)
{
	struct le *le;
	char buf[256];
	va_list ap;

	if (!ua)
		return;

	va_start(ap, fmt);
	(void)re_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	/* send event to all clients */
	for (le = uag.ehl.head; le; le = le->next) {

		struct ua_eh *eh = le->data;

		eh->h(ua, ev, buf, eh->arg);
	}
}


/**
 * Start registration of a User-Agent
 *
 * @param ua User-Agent
 *
 * @return 0 if success, otherwise errorcode
 */
static int ua_register(struct ua *ua)
{
	struct account *acc;
	struct le *le;
	struct uri uri;
	char reg_uri[64];
	char params[256] = "";
	unsigned i;
	int err;

	if (!ua)
		return EINVAL;

	acc = ua->acc;
	uri = ua->acc->luri;
	uri.user = uri.password = pl_null;
	if (re_snprintf(reg_uri, sizeof(reg_uri), "%H", uri_encode, &uri) < 0)
		return ENOMEM;

	if (str_isset(uag.cfg->uuid)) {
		if (re_snprintf(params, sizeof(params),
				";+sip.instance=\"<urn:uuid:%s>\"",
				uag.cfg->uuid) < 0)
			return ENOMEM;
	}

	if (acc->regq) {
		if (re_snprintf(&params[strlen(params)],
				sizeof(params) - strlen(params),
				";q=%s", acc->regq) < 0)
			return ENOMEM;
	}

	if (acc->mnat && acc->mnat->ftag) {
		if (re_snprintf(&params[strlen(params)],
				sizeof(params) - strlen(params),
				";%s", acc->mnat->ftag) < 0)
			return ENOMEM;
	}

	ua_event(ua, UA_EVENT_REGISTERING, NULL);

	for (le = ua->regl.head, i=0; le; le = le->next, i++) {
		struct reg *reg = le->data;

		err = reg_register(reg, reg_uri, params,
				   acc->regint, acc->outbound[i]);
		if (err) {
			DEBUG_WARNING("SIP register failed: %m\n", err);
			return err;
		}
	}

	return 0;
}


bool ua_isregistered(const struct ua *ua)
{
	struct le *le;

	for (le = ua->regl.head; le; le = le->next) {

		const struct reg *reg = le->data;

		/* it is enough if one of the registrations work */
		if (reg_isok(reg))
			return true;
	}

	return false;
}


static void call_event_handler(struct call *call, enum call_event ev,
			       const char *str, void *arg)
{
	struct ua *ua = arg;
	const char *peeruri;
	struct call *call2 = NULL;
	int err;

	MAGIC_CHECK(ua);

	peeruri = call_peeruri(call);

	switch (ev) {

	case CALL_EVENT_INCOMING:
		switch (ua->acc->answermode) {

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
				(void)call_ringtone(call, "ring.wav", -1);
			}

			ua_printf(ua, "Incoming call from: %s %s -"
				  " (press ENTER to accept)\n",
				  call_peername(call), peeruri);

			ua_event(ua, UA_EVENT_CALL_INCOMING, peeruri);
			break;
		}
		break;

	case CALL_EVENT_RINGING:
		ua_event(ua, UA_EVENT_CALL_RINGING, peeruri);
		break;

	case CALL_EVENT_PROGRESS:
		ua_printf(ua, "Call in-progress: %s\n", peeruri);
		ua_event(ua, UA_EVENT_CALL_PROGRESS, peeruri);
		break;

	case CALL_EVENT_ESTABLISHED:
		ua_printf(ua, "Call established: %s\n", peeruri);
		ua_event(ua, UA_EVENT_CALL_ESTABLISHED, peeruri);
		break;

	case CALL_EVENT_CLOSED:
		mem_deref(call);
		ua_event(ua, UA_EVENT_CALL_CLOSED, str);
		break;

	case CALL_EVENT_TRANSFER:

		/*
		 * Create a new call to transfer target.
		 *
		 * NOTE: we will automatically connect a new call to the
		 *       transfer target
		 */

		ua_printf(ua, "transferring call to %s\n", str);

		err = ua_call_alloc(&call2, ua, VIDMODE_ON, NULL, call);
		if (!err) {
			struct pl pl;

			pl_set_str(&pl, str);

			err = call_connect(call2, &pl);
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
}


static int ua_call_alloc(struct call **callp, struct ua *ua,
			 enum vidmode vidmode, const struct sip_msg *msg,
			 struct call *xcall)
{
	struct call_prm cprm;

	if (*callp) {
		DEBUG_WARNING("call_alloc: call is already allocated\n");
		return EALREADY;
	}

	cprm.vidmode = vidmode;
	cprm.af      = ua->af;

	return call_alloc(callp, conf_config(),
			  &ua->calls, ua->acc, ua, &cprm,
			  msg, xcall, call_event_handler, ua);
}


static void handle_options(struct ua *ua, const struct sip_msg *msg)
{
	struct call *call = NULL;
	struct mbuf *desc = NULL;
	int err;

	err = ua_call_alloc(&call, ua, VIDMODE_ON, NULL, NULL);
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
			  "Content-Length: %zu\r\n"
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


static void ua_destructor(void *arg)
{
	struct ua *ua = arg;

	if (ua->uap) {
		*ua->uap = NULL;
		ua->uap = NULL;
	}

	ua_event(ua, UA_EVENT_UNREGISTERING, NULL);

	list_unlink(&ua->le);

	list_flush(&ua->calls);
	list_flush(&ua->regl);
	mem_deref(ua->cuser);
	mem_deref(ua->acc);
}


static bool request_handler(const struct sip_msg *msg, void *arg)
{
	struct ua *ua;

	(void)arg;

	if (pl_strcmp(&msg->met, "OPTIONS"))
		return false;

	ua = uag_find(&msg->uri.user);
	if (!ua) {
		(void)sip_treply(NULL, uag_sip(), msg, 404, "Not Found");
		return true;
	}

	handle_options(ua, msg);

	return true;
}


/**
 * Allocate a SIP User-Agent
 *
 * @param uap   Pointer to allocated User-Agent object
 * @param aor   SIP Address-of-Record (AOR)
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_alloc(struct ua **uap, const char *aor)
{
	struct ua *ua;
	int err;

	if (!aor)
		return EINVAL;

	ua = mem_zalloc(sizeof(*ua), ua_destructor);
	if (!ua)
		return ENOMEM;

	MAGIC_INIT(ua);

	list_init(&ua->calls);

#if HAVE_INET6
	ua->af   = uag.prefer_ipv6 ? AF_INET6 : AF_INET;
#else
	ua->af   = AF_INET;
#endif

	err = re_sdprintf(&ua->cuser, "%p", ua);
	if (err)
		goto out;

	/* Decode SIP address */

	err = account_alloc(&ua->acc, aor);
	if (err)
		goto out;

	if (ua->acc->sipnat) {
		ua_printf(ua, "Using sipnat: `%s'\n", ua->acc->sipnat);
	}

	if (ua->acc->mnat) {
		ua_printf(ua, "Using medianat `%s'\n",
			  ua->acc->mnat->id);
	}

	if (ua->acc->menc) {
		ua_printf(ua, "Using media encryption `%s'\n",
			  ua->acc->menc->id);
	}

	/* Register clients */
	if (0 == str_casecmp(ua->acc->sipnat, "outbound")) {

		size_t i;

		if (!str_isset(uag.cfg->uuid)) {

			DEBUG_WARNING("outbound requires valid UUID!\n");
			err = ENOSYS;
			goto out;
		}

		for (i=0; i<ARRAY_SIZE(ua->acc->outbound); i++) {

			if (ua->acc->outbound[i]) {
				err = reg_add(&ua->regl, ua, (int)i+1);
				if (err)
					break;
			}
		}
	}
	else {
		err = reg_add(&ua->regl, ua, 0);
	}
	if (err)
		goto out;

	list_append(&uag.ual, &ua->le, ua);

	if (ua->acc->regint) {
		err = ua_register(ua);
	}

 out:
	if (err)
		mem_deref(ua);
	else if (uap) {
		*uap = ua;

		ua->uap = uap;
	}

	return err;
}


/**
 * Connect an outgoing call to a given SIP uri
 *
 * @param ua      User-Agent
 * @param uri     SIP uri to connect to
 * @param params  Optional URI parameters
 * @param vmode   Video mode
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_connect(struct ua *ua, const char *uri, const char *params,
	       enum vidmode vmode)
{
	struct call *call = NULL;
	struct mbuf *dialbuf;
	struct pl pl;
	size_t len;
	int err = 0;

	if (!ua || !str_isset(uri))
		return EINVAL;

	len = str_len(uri);

	dialbuf = mbuf_alloc(64);
	if (!dialbuf)
		return ENOMEM;

	if (params)
		err |= mbuf_printf(dialbuf, "<");

	/* Append sip: scheme if missing */
	if (0 != re_regex(uri, len, "sip:"))
		err |= mbuf_printf(dialbuf, "sip:");

	err |= mbuf_write_str(dialbuf, uri);

	/* Append domain if missing */
	if (0 != re_regex(uri, len, "[^@]+@[^]+", NULL, NULL)) {
#if HAVE_INET6
		if (AF_INET6 == ua->acc->luri.af)
			err |= mbuf_printf(dialbuf, "@[%r]",
					   &ua->acc->luri.host);
		else
#endif
			err |= mbuf_printf(dialbuf, "@%r",
					   &ua->acc->luri.host);

		/* Also append port if specified and not 5060 */
		switch (ua->acc->luri.port) {

		case 0:
		case SIP_PORT:
			break;

		default:
			err |= mbuf_printf(dialbuf, ":%u", ua->acc->luri.port);
			break;
		}
	}

	if (params) {
		err |= mbuf_printf(dialbuf, ";%s", params);
	}

	/* Append any optional URI parameters */
	err |= mbuf_write_pl(dialbuf, &ua->acc->luri.params);

	if (params)
		err |= mbuf_printf(dialbuf, ">");

	if (err)
		goto out;

	err = ua_call_alloc(&call, ua, vmode, NULL, NULL);
	if (err)
		goto out;

	pl.p = (char *)dialbuf->buf;
	pl.l = dialbuf->end;

	err = call_connect(call, &pl);

	if (err)
		mem_deref(call);

 out:
	mem_deref(dialbuf);

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

	call = ua_call(ua);
	if (!call)
		return;

	(void)call_hangup(call);

	mem_deref(call);
}


/**
 * Answer an incoming call
 *
 * @param ua User-Agent
 *
 * @return 0 if success, otherwise errorcode
 */
int ua_answer(struct ua *ua)
{
	struct call *call;

	if (!ua)
		return EINVAL;

	call = ua_call(ua);
	if (!call) {
		DEBUG_NOTICE("answer: no incoming calls found\n");
		return ENOENT;
	}

	/* todo: put previous call on-hold (if configured) */

	return call_answer(call, 200);
}


int ua_print_status(struct re_printf *pf, const struct ua *ua)
{
	struct le *le;
	int err;

	if (!ua)
		return 0;

	err = re_hprintf(pf, "%-42s", ua->acc->aor);

	for (le = ua->regl.head; le; le = le->next)
		err |= reg_status(pf, le->data);

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


/**
 * Get the AOR of a User-Agent
 *
 * @param ua User-Agent object
 *
 * @return AOR
 */
const char *ua_aor(const struct ua *ua)
{
	return ua ? ua->acc->aor : NULL;
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
	return ua ? ua->acc->outbound[0] : NULL;
}


/**
 * Get the current call object of a User-Agent
 *
 * @param ua User-Agent object
 *
 * @return Current call, NULL if no active calls
 *
 *
 * Current call strategy:
 *
 * We can only have 1 current call. The current call is the one that was
 * added last (end of the list), which is not on-hold
 */
struct call *ua_call(const struct ua *ua)
{
	struct le *le;

	if (!ua)
		return NULL;

	for (le = ua->calls.tail; le; le = le->prev) {

		struct call *call = le->data;

		/* todo: check if call is on-hold */

		return call;
	}

	return NULL;
}


int ua_debug(struct re_printf *pf, const struct ua *ua)
{
	struct le *le;
	int err;

	if (!ua)
		return 0;

	err  = re_hprintf(pf, "--- %s ---\n", ua->acc->aor);
	err |= re_hprintf(pf, " cuser:     %s\n", ua->cuser);
	err |= re_hprintf(pf, " af:        %s\n", net_af2name(ua->af));

	err |= account_debug(pf, ua->acc);

	for (le = ua->regl.head; le; le = le->next)
		err |= reg_debug(pf, le->data);

	return err;
}


/* One instance */


static int add_transp_af(const struct sa *laddr)
{
	struct sa local;
	int err = 0;

	if (str_isset(uag.cfg->local)) {
		err = sa_decode(&local, uag.cfg->local,
				str_len(uag.cfg->local));
		if (err) {
			err = sa_set_str(&local, uag.cfg->local, 0);
			if (err) {
				DEBUG_WARNING("decode failed: %s\n",
					      uag.cfg->local);
				return err;
			}
		}

		if (!sa_isset(&local, SA_ADDR)) {
			uint16_t port = sa_port(&local);
			(void)sa_set_sa(&local, &laddr->u.sa);
			sa_set_port(&local, port);
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
			const char *cert = NULL;

			if (str_isset(uag.cfg->cert)) {
				cert = uag.cfg->cert;
				(void)re_printf("SIP Certificate: %s\n", cert);
			}

			err = tls_alloc(&uag.tls, TLS_METHOD_SSLV23,
					cert, NULL);
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

	ua = uag_find(&msg->uri.user);
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

	err = ua_call_alloc(&call, ua, VIDMODE_ON, msg, NULL);
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

	(void)uag_reset_transp(true, true);
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
	struct config *cfg = conf_config();
	uint32_t bsize;
	int err;

	uag.cfg = &cfg->sip;
	bsize = cfg->sip.trans_bsize;
	ui_init(&cfg->input);

	play_init(cfg);

	/* Initialise Network */
	err = net_init(&cfg->net, prefer_ipv6 ? AF_INET6 : AF_INET);
	if (err) {
		DEBUG_WARNING("network init failed: %m\n", err);
		return err;
	}

	uag.use_udp = udp;
	uag.use_tcp = tcp;
	uag.use_tls = tls;
	uag.prefer_ipv6 = prefer_ipv6;

	list_init(&uag.ual);

	err = sip_alloc(&uag.sip, net_dnsc(), bsize, bsize, bsize,
			software, exit_handler, NULL);
	if (err) {
		DEBUG_WARNING("sip stack failed: %m\n", err);
		goto out;
	}

	err = ua_add_transp();
	if (err)
		goto out;

	err = sip_listen(&uag.lsnr, uag.sip, true, request_handler, NULL);
	if (err)
		goto out;

	err = sipsess_listen(&uag.sock, uag.sip, bsize,
			     sipsess_conn_handler, NULL);
	if (err)
		goto out;

	err = sipevent_listen(&uag.evsock, uag.sip, bsize, bsize, NULL, NULL);
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
 * Close all active User-Agents
 */
void ua_close(void)
{
	cmd_unregister(cmdv);
	net_close();
	play_close();

	uag.evsock   = mem_deref(uag.evsock);
	uag.sock     = mem_deref(uag.sock);
	uag.lsnr     = mem_deref(uag.lsnr);
	uag.sip      = mem_deref(uag.sip);

#ifdef USE_TLS
	uag.tls = mem_deref(uag.tls);
#endif

	list_flush(&uag.ual);
	list_flush(&uag.ehl);
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
				 list_count(&uag.ual),
				 forced ? "(Forced)" : "");
	}

	if (forced)
		sipsess_close_all(uag.sock);
	else
		list_flush(&uag.ual);

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
int uag_reset_transp(bool reg, bool reinvite)
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

		if (reg && ua->acc->regint) {
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
const char *uag_event_str(enum ua_event ev)
{
	switch (ev) {

	case UA_EVENT_REGISTERING:      return "REGISTERING";
	case UA_EVENT_REGISTER_OK:      return "REGISTER_OK";
	case UA_EVENT_REGISTER_FAIL:    return "REGISTER_FAIL";
	case UA_EVENT_UNREGISTERING:    return "UNREGISTERING";
	case UA_EVENT_CALL_INCOMING:    return "CALL_INCOMING";
	case UA_EVENT_CALL_RINGING:     return "CALL_RINGING";
	case UA_EVENT_CALL_PROGRESS:    return "CALL_PROGRESS";
	case UA_EVENT_CALL_ESTABLISHED: return "CALL_ESTABLISHED";
	case UA_EVENT_CALL_CLOSED:      return "CALL_CLOSED";
	default: return "?";
	}
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

		struct reg *reg = le->data;
		int fd;

		fd = reg_sipfd(reg);
		if (fd != -1)
			return fd;
	}

	return -1;
}


/**
 * Find the correct UA from the contact user
 *
 * @param cuser Contact username
 *
 * @return Matching UA if found, NULL if not found
 */
struct ua *uag_find(const struct pl *cuser)
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

		if (0 == pl_casecmp(cuser, &ua->acc->luri.user))
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
struct ua *uag_find_aor(const char *aor)
{
	struct le *le;

	for (le = uag.ual.head; le; le = le->next) {
		struct ua *ua = le->data;

		if (str_isset(aor) && str_cmp(ua->acc->aor, aor))
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


struct list *uag_list(void)
{
	return &uag.ual;
}


/**
 * Return list of methods supported by the UA
 *
 * @return String of supported methods
 */
const char *uag_allowed_methods(void)
{
	/* todo: create a dynamic list of supported methods */
	return "INVITE,ACK,BYE,CANCEL,REFER,NOTIFY,SUBSCRIBE,INFO";
}


int ua_print_supported(struct re_printf *pf, const struct ua *ua)
{
	int err;

	err = re_hprintf(pf, "Supported: path");

	if (0 == str_casecmp(ua->acc->sipnat, "outbound"))
		err |= re_hprintf(pf, ", outbound");
	if (ua->acc->mnat && 0 == str_casecmp(ua->acc->mnat->id, "ice"))
		err |= re_hprintf(pf, ", ice");

	err |= re_hprintf(pf, "\r\n");

	return err;
}


struct account *ua_prm(const struct ua *ua)
{
	return ua ? ua->acc : NULL;
}


static void eh_destructor(void *arg)
{
	struct ua_eh *eh = arg;
	list_unlink(&eh->le);
}


int uag_event_register(ua_event_h *h, void *arg)
{
	struct ua_eh *eh;

	if (!h)
		return EINVAL;

	eh = mem_zalloc(sizeof(*eh), eh_destructor);
	if (!eh)
		return ENOMEM;

	eh->h = h;
	eh->arg = arg;

	list_append(&uag.ehl, &eh->le, eh);

	return 0;
}


void uag_event_unregister(ua_event_h *h)
{
	struct le *le;

	for (le = uag.ehl.head; le; le = le->next) {

		struct ua_eh *eh = le->data;

		if (eh->h == h) {
			mem_deref(eh);
			break;
		}
	}
}
