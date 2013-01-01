/**
 * @file ice.c ICE Module
 *
 * Copyright (C) 2010 Creytiv.com
 */
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCNetworkReachability.h>
#endif
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "ice"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct mnat_sess {
	struct list medial;
	struct sa srv;
	struct stun_dns *dnsq;
	struct sdp_session *sdp;
	struct ice *ice;
	char *user;
	char *pass;
	int mediac;
	bool started;
	bool send_reinvite;
	mnat_estab_h *estabh;
	void *arg;
};

struct mnat_media {
	struct le le;
	struct sa addr1;
	struct sa addr2;
	struct mnat_sess *sess;
	struct sdp_media *sdpm;
	struct icem *icem;
	void *sock1;
	void *sock2;
	bool complete;
};


static struct mnat *mnat;
static struct {
	char ifc[64];
	enum ice_mode mode;
	enum ice_nomination nom;
	bool turn;
	bool debug;
} ice = {
	"",
	ICE_MODE_FULL,
	ICE_NOMINATION_AGGRESSIVE,
	true,
	false
};


static void gather_handler(int err, uint16_t scode, const char *reason,
			   void *arg);


static bool is_cellular(const struct sa *laddr)
{
#if TARGET_OS_IPHONE
	SCNetworkReachabilityRef r;
	SCNetworkReachabilityFlags flags = 0;
	bool cell = false;

	r = SCNetworkReachabilityCreateWithAddressPair(NULL,
						       &laddr->u.sa, NULL);
	if (!r)
		return false;

	if (SCNetworkReachabilityGetFlags(r, &flags)) {

		if (flags & kSCNetworkReachabilityFlagsIsWWAN)
			cell = true;
	}

	CFRelease(r);

	return cell;
#else
	(void)laddr;
	return false;
#endif
}


static void ice_printf(struct mnat_media *m, const char *fmt, ...)
{
	va_list ap;

	if (!ice.debug)
		return;

	va_start(ap, fmt);
	re_printf("%s: %v", m ? sdp_media_name(m->sdpm) : "ICE", fmt, &ap);
	va_end(ap);
}


static bool stream_has_media(const struct sdp_media *sdpm)
{
	bool has;

	has = sdp_media_rformat(sdpm, NULL) != NULL;
	if (has)
		return sdp_media_rport(sdpm) != 0;

	return false;
}


static void session_destructor(void *arg)
{
	struct mnat_sess *sess = arg;

	list_flush(&sess->medial);
	mem_deref(sess->dnsq);
	mem_deref(sess->user);
	mem_deref(sess->pass);
	mem_deref(sess->ice);
	mem_deref(sess->sdp);
}


static void media_destructor(void *arg)
{
	struct mnat_media *m = arg;

	list_unlink(&m->le);
	mem_deref(m->sdpm);
	mem_deref(m->icem);
	mem_deref(m->sock1);
	mem_deref(m->sock2);
}


static int set_session_attributes(struct mnat_sess *s)
{
	int err = 0;

	if (ICE_MODE_LITE == ice.mode) {
		err |= sdp_session_set_lattr(s->sdp, true,
					     ice_attr_lite, NULL);
	}

	err |= sdp_session_set_lattr(s->sdp, true,
				     ice_attr_ufrag, ice_ufrag(s->ice));
	err |= sdp_session_set_lattr(s->sdp, true,
				     ice_attr_pwd, ice_pwd(s->ice));

	return err;
}


static bool candidate_handler(struct le *le, void *arg)
{
	return 0 != sdp_media_set_lattr(arg, false, ice_attr_cand, "%H",
					ice_cand_encode, le->data);
}


/**
 * Update the local SDP attributes, this can be called multiple times
 * when the state of the ICE machinery changes
 */
static int set_media_attributes(struct mnat_media *m)
{
	int err = 0;

	if (icem_mismatch(m->icem)) {
		err = sdp_media_set_lattr(m->sdpm, true,
					  ice_attr_mismatch, NULL);
		return err;
	}
	else {
		sdp_media_del_lattr(m->sdpm, ice_attr_mismatch);
	}

	/* Encode all my candidates */
	sdp_media_del_lattr(m->sdpm, ice_attr_cand);
	if (list_apply(icem_lcandl(m->icem), true, candidate_handler, m->sdpm))
		return ENOMEM;

	if (ice_remotecands_avail(m->icem)) {
		err |= sdp_media_set_lattr(m->sdpm, true,
					   ice_attr_remote_cand, "%H",
					   ice_remotecands_encode, m->icem);
	}

	return err;
}


static bool if_handler(const char *ifname, const struct sa *sa, void *arg)
{
	struct mnat_media *m = arg;
	uint16_t lprio;
	int err = 0;

	/* Skip loopback and link-local addresses */
	if (sa_is_loopback(sa) || sa_is_linklocal(sa))
		return false;

	/* Interface filter */
	if (str_isset(ice.ifc)) {

		if (0 != str_casecmp(ifname, ice.ifc)) {
			re_printf("ICE: skip interface: %s\n", ifname);
			return false;
		}
	}

	lprio = is_cellular(sa) ? 0 : 10;

	DEBUG_NOTICE("%s: added interface: %s:%j (local prio %u)\n",
		     sdp_media_name(m->sdpm), ifname, sa, lprio);

	if (m->sock1)
		err |= icem_cand_add(m->icem, 1, lprio, ifname, sa);
	if (m->sock2)
		err |= icem_cand_add(m->icem, 2, lprio, ifname, sa);

	if (err) {
		DEBUG_WARNING("%s:%j: icem_cand_add: %m\n", ifname, sa, err);
	}

	return false;
}


static int media_start(struct mnat_sess *sess, struct mnat_media *m)
{
	int err = 0;

	net_if_apply(if_handler, m);

	switch (ice.mode) {

	default:
	case ICE_MODE_FULL:
		if (ice.turn) {
			err = icem_gather_relay(m->icem, &sess->srv,
						sess->user, sess->pass);
		}
		else {
			err = icem_gather_srflx(m->icem, &sess->srv);
		}
		break;

	case ICE_MODE_LITE:
		gather_handler(0, 0, NULL, m);
		break;
	}

	return err;
}


static void dns_handler(int err, const struct sa *srv, void *arg)
{
	struct mnat_sess *sess = arg;
	struct le *le;

	if (err)
		goto out;

	sess->srv = *srv;

	for (le=sess->medial.head; le; le=le->next) {

		struct mnat_media *m = le->data;

		err = media_start(sess, m);
		if (err)
			goto out;
	}

	return;

 out:
	sess->estabh(err, 0, NULL, sess->arg);
}


static int session_alloc(struct mnat_sess **sessp, struct dnsc *dnsc,
			 const char *srv, uint16_t port,
			 const char *user, const char *pass,
			 struct sdp_session *ss, bool offerer,
			 mnat_estab_h *estabh, void *arg)
{
	struct mnat_sess *sess;
	const char *usage;
	int err;

	if (!sessp || !dnsc || !srv || !user || !pass || !ss || !estabh)
		return EINVAL;

	sess = mem_zalloc(sizeof(*sess), session_destructor);
	if (!sess)
		return ENOMEM;

	sess->sdp    = mem_ref(ss);
	sess->estabh = estabh;
	sess->arg    = arg;

	err  = str_dup(&sess->user, user);
	err |= str_dup(&sess->pass, pass);
	if (err)
		goto out;

	err = ice_alloc(&sess->ice, ice.mode, offerer);
	if (err)
		goto out;

	ice_conf(sess->ice)->nom = ice.nom;
	ice_conf(sess->ice)->debug = ice.debug;

	err = set_session_attributes(sess);
	if (err)
		goto out;

	usage = ice.turn ? stun_usage_relay : stun_usage_binding;

	err = stun_server_discover(&sess->dnsq, dnsc, usage, stun_proto_udp,
				   AF_INET, srv, port, dns_handler, sess);

 out:
	if (err)
		mem_deref(sess);
	else
		*sessp = sess;

	return err;
}


static bool verify_peer_ice(struct mnat_sess *ms)
{
	struct le *le;

	for (le = ms->medial.head; le; le = le->next) {
		struct mnat_media *m = le->data;
		const struct sa *raddr1;
		struct sa raddr2;

		if (!stream_has_media(m->sdpm)) {
			DEBUG_NOTICE("stream '%s' is disabled -- ignore\n",
				     sdp_media_name(m->sdpm));
			continue;
		}

		raddr1 = sdp_media_raddr(m->sdpm);
		sdp_media_raddr_rtcp(m->sdpm, &raddr2);

		if (m->sock1 && !icem_verify_support(m->icem, 1, raddr1)) {
			DEBUG_WARNING("%s.1: no remote candidates found"
				      " (address = %J)\n",
				      sdp_media_name(m->sdpm), raddr1);
			return false;
		}

		if (m->sock2 && !icem_verify_support(m->icem, 2, &raddr2)) {
			DEBUG_WARNING("%s.2: no remote candidates found"
				      " (address = %J)\n",
				      sdp_media_name(m->sdpm), &raddr2);
			return false;
		}
	}

	return true;
}


/*
 * Update SDP Media with local addresses
 */
static bool refresh_laddr(struct mnat_media *m,
			  const struct sa *addr1,
			  const struct sa *addr2)
{
	bool changed = false;

	if (m->sock1 && addr1) {

		if (!sa_cmp(&m->addr1, addr1, SA_ALL)) {
			changed = true;

			if (sa_isset(&m->addr1, SA_ALL)) {
				ice_printf(m,
					   "comp1 local changed: %J ---> %J\n",
					   &m->addr1, addr1);
			}
			else {
				ice_printf(m,
					   "comp1 setting local: %J\n",
					   addr1);
			}
		}

		sa_cpy(&m->addr1, addr1);
		sdp_media_set_laddr(m->sdpm, &m->addr1);
	}

	if (m->sock2 && addr2) {

		if (!sa_cmp(&m->addr2, addr2, SA_ALL)) {
			changed = true;

			if (sa_isset(&m->addr2, SA_ALL)) {
				ice_printf(m,
					   "comp2 local changed: %J ---> %J\n",
					   &m->addr2, addr2);
			}
			else {
				ice_printf(m,
					   "comp2 setting local: %J\n",
					   addr2);
			}
		}

		sa_cpy(&m->addr2, addr2);
		sdp_media_set_laddr_rtcp(m->sdpm, &m->addr2);
	}

	return changed;
}


static void gather_handler(int err, uint16_t scode, const char *reason,
			   void *arg)
{
	struct mnat_media *m = arg;

	if (err || scode) {
		DEBUG_WARNING("gather error: %m (%u %s)\n",
			      err, scode, reason);
	}
	else {
		refresh_laddr(m,
			      icem_cand_default(m->icem, 1),
			      icem_cand_default(m->icem, 2));

		DEBUG_NOTICE("%s: Default local candidates: %J / %J\n",
			     sdp_media_name(m->sdpm), &m->addr1, &m->addr2);

		set_media_attributes(m);

		if (--m->sess->mediac)
			return;
	}

	m->sess->estabh(err, scode, reason, m->sess->arg);
}


static void conncheck_handler(int err, bool update, void *arg)
{
	struct mnat_media *m = arg;
	struct mnat_sess *sess = m->sess;
	struct le *le;

	DEBUG_NOTICE("%s: Conncheck is complete\n", sdp_media_name(m->sdpm));

	if (err) {
		DEBUG_WARNING("conncheck failed: %m\n", err);

		(void)re_printf("Dumping state: %H\n", ice_debug, sess->ice);
	}
	else {
		bool changed;

		m->complete = true;

		changed = refresh_laddr(m,
					icem_selected_laddr(m->icem, 1),
					icem_selected_laddr(m->icem, 2));
		if (changed)
			sess->send_reinvite = true;

		set_media_attributes(m);

		/* Check all conncheck flags */
		LIST_FOREACH(&sess->medial, le) {
			struct mnat_media *mx = le->data;
			if (!mx->complete)
				return;
		}
	}

	/* call estab-handler and send re-invite */
	if (sess->send_reinvite && update) {
		sess->estabh(0, 0, NULL, sess->arg);
		sess->send_reinvite = false;
	}
}


static int ice_start(struct mnat_sess *sess)
{
	struct le *le;
	int err;

	ice_printf(NULL, "ICE Start: %H", ice_debug, sess->ice);

	/* Update SDP media */
	if (sess->started) {

		LIST_FOREACH(&sess->medial, le) {
			struct mnat_media *m = le->data;

			icem_update(m->icem);

			refresh_laddr(m,
				      icem_selected_laddr(m->icem, 1),
				      icem_selected_laddr(m->icem, 2));

			set_media_attributes(m);
		}

		return 0;
	}

	/* Clear all conncheck flags */
	LIST_FOREACH(&sess->medial, le) {
		struct mnat_media *m = le->data;

		if (stream_has_media(m->sdpm)) {
			m->complete = false;

			if (ice.mode == ICE_MODE_FULL) {
				err = icem_conncheck_start(m->icem);
				if (err)
					return err;
			}
		}
		else {
			m->complete = true;
		}
	}

	sess->started = true;

	return 0;
}


static int media_alloc(struct mnat_media **mp, struct mnat_sess *sess,
		       int proto, void *sock1, void *sock2,
		       struct sdp_media *sdpm)
{
	struct mnat_media *m;
	int err = 0;

	if (!mp || !sess || !sdpm)
		return EINVAL;

	m = mem_zalloc(sizeof(*m), media_destructor);
	if (!m)
		return ENOMEM;

	list_append(&sess->medial, &m->le, m);
	m->sdpm  = mem_ref(sdpm);
	m->sess  = sess;
	m->sock1 = mem_ref(sock1);
	m->sock2 = mem_ref(sock2);

	err = icem_alloc(&m->icem, sess->ice, proto, 0,
			 gather_handler, conncheck_handler, m);
	if (err)
		goto out;

	icem_set_name(m->icem, sdp_media_name(sdpm));

	if (sock1)
		err |= icem_comp_add(m->icem, 1, sock1);
	if (sock2)
		err |= icem_comp_add(m->icem, 2, sock2);

	if (sa_isset(&sess->srv, SA_ALL))
		err = media_start(sess, m);

 out:
	if (err)
		mem_deref(m);
	else {
		*mp = m;
		++sess->mediac;
	}

	return err;
}


static bool sdp_attr_handler(const char *name, const char *value, void *arg)
{
	struct mnat_sess *sess = arg;
	return 0 != ice_sdp_decode(sess->ice, name, value);
}


static bool media_attr_handler(const char *name, const char *value, void *arg)
{
	struct mnat_media *m = arg;
	return 0 != icem_sdp_decode(m->icem, name, value);
}


static int enable_turn_channels(struct mnat_sess *sess)
{
	struct le *le;
	int err = 0;

	for (le = sess->medial.head; le; le = le->next) {

		struct mnat_media *m = le->data;
		struct sa raddr1, raddr2;

		set_media_attributes(m);

		raddr1 = *sdp_media_raddr(m->sdpm);
		sdp_media_raddr_rtcp(m->sdpm, &raddr2);

		if (m->sock1 && sa_isset(&raddr1, SA_ALL))
			err |= icem_add_chan(m->icem, 1, &raddr1);
		if (m->sock2 && sa_isset(&raddr2, SA_ALL))
			err |= icem_add_chan(m->icem, 2, &raddr2);
	}

	return err;
}


/** This can be called several times */
static int update(struct mnat_sess *sess)
{
	struct le *le;
	int err = 0;

	/* SDP session */
	(void)sdp_session_rattr_apply(sess->sdp, NULL, sdp_attr_handler, sess);

	/* SDP medialines */
	for (le = sess->medial.head; le; le = le->next) {
		struct mnat_media *m = le->data;

		sdp_media_rattr_apply(m->sdpm, NULL, media_attr_handler, m);
	}

	/* 5.1.  Verifying ICE Support */
	if (verify_peer_ice(sess)) {
		err = ice_start(sess);
	}
	else if (ice.turn) {
		DEBUG_NOTICE("ICE not supported by peer, fallback to TURN\n");
		err = enable_turn_channels(sess);
	}
	else {
		DEBUG_WARNING("ICE not supported by peer\n");

		LIST_FOREACH(&sess->medial, le) {
			struct mnat_media *m = le->data;

			set_media_attributes(m);
		}
	}

	return err;
}


static int module_init(void)
{
#ifdef MODULE_CONF
	struct pl pl;

	conf_get_str(conf_cur(), "ice_interface", ice.ifc, sizeof(ice.ifc));
	conf_get_bool(conf_cur(), "ice_turn", &ice.turn);
	conf_get_bool(conf_cur(), "ice_debug", &ice.debug);

	if (!conf_get(conf_cur(), "ice_nomination", &pl)) {
		if (0 == pl_strcasecmp(&pl, "regular"))
			ice.nom = ICE_NOMINATION_REGULAR;
		else if (0 == pl_strcasecmp(&pl, "aggressive"))
			ice.nom = ICE_NOMINATION_AGGRESSIVE;
		else {
			DEBUG_WARNING("unknown nomination: %r\n", &pl);
		}
	}
	if (!conf_get(conf_cur(), "ice_mode", &pl)) {
		if (!pl_strcasecmp(&pl, "full"))
			ice.mode = ICE_MODE_FULL;
		else if (!pl_strcasecmp(&pl, "lite"))
			ice.mode = ICE_MODE_LITE;
		else {
			DEBUG_WARNING("unknown mode: %r\n", &pl);
		}
	}
#endif

	return mnat_register(&mnat, "ice", "+sip.ice",
			     session_alloc, media_alloc, update);
}


static int module_close(void)
{
	mnat = mem_deref(mnat);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(ice) = {
	"ice",
	"mnat",
	module_init,
	module_close,
};
