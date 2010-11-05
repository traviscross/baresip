/**
 * @file stream.c  Generic Media Stream
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <time.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "stream"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

#define MAGIC 0x00814ea5
#include "magic.h"


enum {
	RTP_RECV_SIZE    = 8192,  /**< Receive buffer for incoming RTP     */
	RTP_KEEPALIVE_Tr = 15,    /**< RTP keepalive interval in [seconds] */
};

/** Defines a generic media stream */
struct stream {
	MAGIC_DECL

	struct le le;            /**< Linked list element                   */
	const char *name;        /**< Stream name                           */
	struct sdp_media *media; /**< SDP Media line                        */
	struct rtp_sock *rtp;    /**< RTP Socket                            */
	struct jbuf *jbuf;       /**< Jitter Buffer for incoming RTP        */
	struct sa rtp_remote;    /**< Remote RTP IP address and port        */
	struct sa rtcp_remote;   /**< Remote RTCP address                   */
	struct mnat_media *mns;  /**< Media NAT traversal state             */
	struct menc_st *menc;    /**< Media Encryption                      */
	uint32_t ts_tx;          /**< Timestamp for outgoing RTP            */
	uint32_t ssrc_rx;        /**< Incoming syncronizing source          */
	uint32_t pseq;           /**< Sequence number for incoming RTP      */
	bool active;             /**< Stream is active flag                 */
	bool nack_pli;           /**< Send NACK/PLI to peer                 */
	bool rtcp;               /**< Enable RTCP                           */
	int label;               /**< Media stream label                    */
	stream_recv_h *rh;       /**< Stream receive handler                */
	void *arg;               /**< Handler argument                      */

#ifdef DEPRECATED_KEEPALIVE
	struct tmr tmr_ka;       /**< Keep-alive timer                      */
	uint8_t pt_ka;           /**< Payload type for keepalive            */
	bool ka_flag;            /**< Keep-alive flag, set by stream_send() */
#endif

	struct tmr tmr_stats;
	struct {
		uint32_t n_tx;
		uint32_t n_rx;
		size_t b_tx;
		size_t b_rx;
		size_t bitrate_tx;
		size_t bitrate_rx;
		uint64_t ts;
	} stats;
};


static inline uint16_t lostcalc(struct stream *s, uint16_t seq)
{
	uint16_t lostc;

	if (s->pseq == (uint32_t)-1)
		lostc = 0;
	else if (seq > s->pseq)
		lostc = seq - s->pseq - 1;
	else if ((uint32_t)(seq + 0x8000) < s->pseq)
		lostc = seq + 0xffff - s->pseq;
	else
		lostc = 0;

	s->pseq = seq;

	return lostc;
}


static void stream_poll(struct stream *s)
{
	struct rtp_header hdr;
	void *mb = NULL;
	int err;

	if (!s)
		return;

	err = jbuf_get(s->jbuf, &hdr, &mb);
	if (err)
		return;

	s->rh(&hdr, mb, s->arg);

	mem_deref(mb);
}


#ifdef DEPRECATED_KEEPALIVE
static void ka_send(struct stream *s)
{
	struct mbuf *mb;
	int err = ENOMEM;

	if (s->pt_ka < PT_DYN_MIN || s->pt_ka > PT_DYN_MAX)
		return;

	mb = mbuf_alloc(RTP_HEADER_SIZE);
	if (!mb)
		goto out;

	mb->pos = mb->end = RTP_HEADER_SIZE;
	err = stream_send(s, false, s->pt_ka, s->ts_tx, mb);

 out:
	mem_deref(mb);

	if (err) {
		DEBUG_WARNING("ka_send: %s\n", strerror(err));
	}
}


/**
 * Find a dynamic payload type that is not used
 *
 * @param m SDP Media
 *
 * @return Unused payload type, -1 if no found
 */
static int find_unused_pt(const struct sdp_media *m)
{
	int pt;

	for (pt = PT_DYN_MAX; pt>=PT_DYN_MIN; pt--) {

		if (!sdp_media_format(m, false, NULL, pt, NULL, -1, -1))
			return pt;
	}

	return -1;
}


/**
 * See draft-ietf-avt-app-rtp-keepalive
 *
 *   Keepalive packets MUST be sent every Tr seconds.  Tr SHOULD be
 *   configurable, and otherwise MUST default to 15 seconds.
 *
 *   The agent SHOULD only send RTP keepalive when it does not send
 *   regular RTP paquets.
 *
 * Logic:
 *
 * We check for RTP activity every 7.5 seconds, and clear the ka_flag.
 * The ka_flag is set for every transmitted RTP packet. If the ka_flag
 * is not set, it means that we have not sent any RTP packet in the
 * last period of 7.5 - 15 seconds. Start transmitting RTP keepalives
 * every 15 seconds after that.
 */
static void keepalive_handler(void *arg)
{
	struct stream *s = arg;
	uint64_t delay = (1000 * RTP_KEEPALIVE_Tr) / 2;

	/* Between 7.5 - 15 seconds of RTP inactivity? */
	if (!s->ka_flag) {
		ka_send(s);
		delay = 1000 * RTP_KEEPALIVE_Tr;
	}

	/* always clear keepalive-flag */
	s->ka_flag = false;

	tmr_start(&s->tmr_ka, delay, keepalive_handler, s);
}
#endif


static void stream_destructor(void *data)
{
	struct stream *s = data;

	list_unlink(&s->le);
#ifdef DEPRECATED_KEEPALIVE
	tmr_cancel(&s->tmr_ka);
#endif
	tmr_cancel(&s->tmr_stats);

	/* note: must be done before freeing socket */
	mem_deref(s->menc);
	mem_deref(s->mns);

	mem_deref(s->jbuf);
	mem_deref(s->rtp);
}


static void rtp_recv(const struct sa *src, const struct rtp_header *hdr,
		     struct mbuf *mb, void *arg)
{
	struct stream *s = arg;
	bool flush = false;
	uint16_t lostc;
	int err;

	if (!mbuf_get_left(mb))
		return;

	if (!(sdp_media_ldir(s->media) & SDP_RECVONLY))
		return;

	lostc = lostcalc(s, hdr->seq);
	if (lostc > 0) {
		DEBUG_NOTICE("lost/oos %s packets: (%u)   \n", s->name, lostc);
	}

	++s->stats.n_rx;
	s->stats.b_rx += mbuf_get_left(mb);

	if (hdr->ssrc != s->ssrc_rx) {
		if (s->ssrc_rx) {
			flush = true;
			DEBUG_NOTICE("%p: SSRC changed %x -> %x"
					" (%u bytes from %J)\n",
				     s, s->ssrc_rx, hdr->ssrc,
				     mbuf_get_left(mb), src);
		}
		s->ssrc_rx = hdr->ssrc;
	}

	/* Put frame in Jitter Buffer */
	if (flush)
		jbuf_flush(s->jbuf);
	err = jbuf_put(s->jbuf, hdr, mb);
	if (err) {
		(void)re_printf("recv: dropping %u bytes from %J\n",
				mb->end, src);
	}

	/* Poll the jitter buffer */
	stream_poll(s);
}


static void rtcp_handler(const struct sa *src, struct rtcp_msg *msg, void *arg)
{
	struct stream *s = arg;

	(void)src;
	(void)s;

	switch (msg->hdr.pt) {

#ifdef USE_VIDEO
	case RTCP_FIR:
		DEBUG_NOTICE("got RTCP FIR from %J\n", src);
		if (!str_casecmp(s->name, "video"))
			video_update_picture((struct video *)s->arg);
		break;

	case RTCP_PSFB:
		if (msg->hdr.count == RTCP_PSFB_PLI) {
			DEBUG_NOTICE("got RTCP PLI from %J\n", src);
			video_update_picture((struct video *)s->arg);
		}
		break;
#endif

	default:
		break;
	}
}


static int stream_sock_alloc(struct stream *s)
{
	struct sa laddr;
	int tos, err;

	if (!s)
		return EINVAL;

	/* we listen on all interfaces */
	sa_init(&laddr, sa_af(net_laddr()));
	sa_init(&s->rtcp_remote, sa_af(net_laddr()));

	err = rtp_listen(&s->rtp, IPPROTO_UDP, &laddr,
			 config.avt.rtp_ports.min, config.avt.rtp_ports.max,
			 s->rtcp, rtp_recv, rtcp_handler, s);
	if (err)
		return err;

	tos = config.avt.rtp_tos;
	err = udp_setsockopt(rtp_sock(s->rtp), IPPROTO_IP, IP_TOS,
			     &tos, sizeof(tos));
	err |= udp_setsockopt(rtcp_sock(s->rtp), IPPROTO_IP, IP_TOS,
			      &tos, sizeof(tos));
	if (err) {
		DEBUG_INFO("alloc: udp_setsockopt IP_TOS: %s\n",
			   strerror(err));
	}

	udp_rxsz_set(rtp_sock(s->rtp), RTP_RECV_SIZE);

	return 0;
}


enum {TMR_INTERVAL = 3};
static void tmr_stats_handler(void *arg)
{
	struct stream *s = arg;
	const uint64_t now = tmr_jiffies();
	uint32_t diff;

	tmr_start(&s->tmr_stats, TMR_INTERVAL * 1000, tmr_stats_handler, s);

 	if (now <= s->stats.ts)
		return;

	if (s->stats.ts) {
		diff = (uint32_t)(now - s->stats.ts);
		s->stats.bitrate_tx = 1000 * 8 * s->stats.b_tx / diff;
		s->stats.bitrate_rx = 1000 * 8 * s->stats.b_rx / diff;
	}

	/* Reset counters */
	s->stats.b_tx = s->stats.b_rx = 0;
	s->stats.ts = now;
}


int stream_alloc(struct stream **sp, struct call *call, const char *name,
		 int label, stream_recv_h *rh, void *arg)
{
	struct stream *s;
	int err;

	if (!sp || !rh)
		return EINVAL;

	s = mem_zalloc(sizeof(*s), stream_destructor);
	if (!s)
		return ENOMEM;

	MAGIC_INIT(s);

#ifdef DEPRECATED_KEEPALIVE
	tmr_init(&s->tmr_ka);
#endif
	tmr_init(&s->tmr_stats);

	s->name  = name;
	s->label = label;
	s->rh    = rh;
	s->arg   = arg;
#ifdef DEPRECATED_KEEPALIVE
	s->pt_ka = PT_NONE;
#endif
	s->pseq  = -1;
	s->rtcp  = config.avt.rtcp_enable;

	err = stream_sock_alloc(s);
	if (err)
		goto out;

	list_append(call_streaml(call), &s->le, s);

 out:
	if (err)
		mem_deref(s);
	else
		*sp = s;

	return err;
}


void stream_set_sdpmedia(struct stream *s, struct sdp_media *m)
{
	if (!s)
		return;

	s->media = m;
}


struct sdp_media *stream_sdpmedia(const struct stream *s)
{
	return s ? s->media : NULL;
}


int stream_menc_set(struct stream *s, const char *type)
{
	int err;

	if (!s)
		return EINVAL;

	if (!type)
		return 0;

	err = menc_alloc(&s->menc, type, IPPROTO_UDP, rtp_sock(s->rtp),
			 s->rtcp ? rtcp_sock(s->rtp) : NULL, s->media);
	if (err) {
		DEBUG_WARNING("media-encryption not found: %s\n", type);
		return err;
	}

	return err;
}


int stream_mnat_init(struct stream *s, const struct mnat *mnat,
		     struct mnat_sess *mnat_sess)
{
	if (!s)
		return EINVAL;

	return mnat->mediah(&s->mns, mnat_sess, IPPROTO_UDP,
			    rtp_sock(s->rtp),
			    s->rtcp ? rtcp_sock(s->rtp) : NULL,
			    s->media);
}


int stream_start(struct stream *s)
{
	if (!s)
		return EINVAL;

	/* Jitter buffer */
	if (!s->jbuf) {
		int err = jbuf_alloc(&s->jbuf, config.jbuf.delay.min,
				     config.jbuf.delay.max);
		if (err)
			return err;
	}

	tmr_start(&s->tmr_stats, 1, tmr_stats_handler, s);

	s->active = true;

	return 0;
}


void stream_stop(struct stream *s)
{
	if (!s)
		return;

	s->active = false;
#ifdef DEPRECATED_KEEPALIVE
	tmr_cancel(&s->tmr_ka);
#endif
}


void stream_start_keepalive(struct stream *s)
{
	if (!s)
		return;

#ifdef DEPRECATED_KEEPALIVE
	if (sdp_media_rformat(s->media, NULL)) {
		const int pt = find_unused_pt(s->media);
		if (pt >= 0) {
			s->pt_ka = pt;
			tmr_start(&s->tmr_ka, 0, keepalive_handler, s);
		}
	}
#endif
}


int stream_send(struct stream *s, bool marker, uint8_t pt, uint32_t ts,
		struct mbuf *mb)
{
	int err;

	if (!s || !sa_isset(&s->rtp_remote, SA_ALL))
		return EINVAL;

	if (sdp_media_dir(s->media) != SDP_SENDRECV) {

#ifdef DEPRECATED_KEEPALIVE
		if (pt != s->pt_ka)
			return 0;
#else
		return 0;
#endif
	}

	s->stats.b_tx += mbuf_get_left(mb);

	err = rtp_send(s->rtp, &s->rtp_remote, marker, pt, ts, mb);

	s->ts_tx = ts;
#ifdef DEPRECATED_KEEPALIVE
	s->ka_flag = true;
#endif
	++s->stats.n_tx;

	return err;
}


const struct sa *stream_local(const struct stream *s)
{
	return s ? rtp_local(s->rtp) : NULL;
}


void stream_remote_set(struct stream *s, const char *cname)
{
	if (!s)
		return;

	sa_cpy(&s->rtp_remote, sdp_media_raddr(s->media));
	sdp_media_raddr_rtcp(s->media, &s->rtcp_remote);

	rtcp_start(s->rtp, cname, &s->rtcp_remote);
}


int stream_sdp_attr_encode(const struct stream *s, struct sdp_media *m)
{
	int err = 0;

	if (!s)
		return EINVAL;

	if (s->label)
		err |= sdp_media_set_lattr(m, true, "label", "%d", s->label);

	return err;
}


void stream_sdp_attr_decode(struct stream *s)
{
	const char *attr;
	int err = 0;

	if (!s)
		return;

	if (s->menc && menc_get(s->menc)->updateh) {
		err |= menc_get(s->menc)->updateh(s->menc);
	}

	/* RFC 4585 */
	attr = sdp_media_rattr(s->media, "rtcp-fb");
	if (attr) {
		if (0 == re_regex(attr, strlen(attr), "nack")) {
			DEBUG_NOTICE("Peer supports NACK PLI (%s)\n", attr);
			s->nack_pli = true;
		}
	}
}


void *stream_arg(const struct stream *s)
{
	return s ? s->arg : NULL;
}


int stream_jbuf_stat(struct re_printf *pf, const struct stream *s)
{
	struct jbuf_stat stat;
	int err;

	if (!s)
		return EINVAL;

	err  = re_hprintf(pf, " %s:", s->name);

	err |= jbuf_stats(s->jbuf, &stat);
	if (err) {
		err = re_hprintf(pf, "Jbuf stat: (not available)");
	}
	else {
		err = re_hprintf(pf, "Jbuf stat: put=%u get=%u or=%u ur=%u",
				  stat.n_put, stat.n_get,
				  stat.n_overflow, stat.n_underflow);
	}

	return err;
}


void stream_set_active(struct stream *s, bool active)
{
	if (!s)
		return;

	s->active = active;
}


bool stream_is_active(const struct stream *s)
{
	return s ? s->active : false;
}


void stream_hold(struct stream *s, bool hold)
{
	if (!s)
		return;

	sdp_media_set_ldir(s->media, hold ? SDP_SENDONLY : SDP_SENDRECV);
}


void stream_set_srate(struct stream *s, uint32_t srate_tx, uint32_t srate_rx)
{
	if (!s)
		return;

	rtcp_set_srate(s->rtp, srate_tx, srate_rx);
}


void stream_send_fir(struct stream *s)
{
	int err;

	if (!s)
		return;

	if (s->nack_pli)
		err = rtcp_send_pli(s->rtp, s->ssrc_rx);
	else
		err = rtcp_send_fir(s->rtp, rtp_sess_ssrc(s->rtp));

	if (err) {
		DEBUG_WARNING("Send FIR: %s\n", strerror(err));
	}
}


void stream_reset(struct stream *s)
{
	if (!s)
		return;

	jbuf_flush(s->jbuf);
}


int stream_debug(struct re_printf *pf, const struct stream *s)
{
	int err;

	if (!s)
		return 0;

	err  = re_hprintf(pf, " %s RTP remote=%J, RTCP remote=%J dir=%s\n",
			  s->name, &s->rtp_remote, &s->rtcp_remote,
			  sdp_dir_name(sdp_media_dir(s->media)));
	err |= rtp_debug(pf, s->rtp);
	err |= jbuf_debug(pf, s->jbuf);

	return err;
}


int stream_print(struct re_printf *pf, const struct stream *s)
{
	if (!s)
		return 0;

	return re_hprintf(pf, " %s=%u/%u", s->name,
			  s->stats.bitrate_tx, s->stats.bitrate_rx);
}
