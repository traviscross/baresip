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


enum stream_type {
	STREAM_UNKNOWN = 0,
	STREAM_AUDIO,
	STREAM_VIDEO
};


/** Defines a generic media stream */
struct stream {
	MAGIC_DECL

	struct le le;            /**< Linked list element                   */
	enum stream_type type;   /**< Type of stream (audio, video...)      */
	struct call *call;       /**< Ref. to call object                   */
	struct sdp_media *sdp;   /**< SDP Media line                        */
	struct rtp_sock *rtp;    /**< RTP Socket                            */
	struct rtpkeep *rtpkeep; /**< RTP Keepalive                         */
	struct jbuf *jbuf;       /**< Jitter Buffer for incoming RTP        */
	struct mnat_media *mns;  /**< Media NAT traversal state             */
	struct menc_st *menc;    /**< Media Encryption                      */
	uint32_t ssrc_rx;        /**< Incoming syncronizing source          */
	uint32_t pseq;           /**< Sequence number for incoming RTP      */
	bool nack_pli;           /**< Send NACK/PLI to peer                 */
	bool rtcp;               /**< Enable RTCP                           */
	bool rtcp_mux;           /**< RTP/RTCP multiplex supported by peer  */
	stream_recv_h *rh;       /**< Stream receive handler                */
	void *arg;               /**< Handler argument                      */

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


static inline int lostcalc(struct stream *s, uint16_t seq)
{
	const uint16_t delta = seq - s->pseq;
	int lostc;

	if (s->pseq == (uint32_t)-1)
		lostc = 0;
	else if (delta == 0)
		return -1;
	else if (delta < 3000)
		lostc = delta - 1;
	else if (delta < 0xff9c)
		lostc = 0;
	else
		return -2;

	s->pseq = seq;

	return lostc;
}


static void stream_destructor(void *arg)
{
	struct stream *s = arg;

	list_unlink(&s->le);
	tmr_cancel(&s->tmr_stats);
	mem_deref(s->rtpkeep);
	mem_deref(s->sdp);
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
	int err;

	if (!mbuf_get_left(mb))
		return;

	if (!(sdp_media_ldir(s->sdp) & SDP_RECVONLY))
		return;

	++s->stats.n_rx;
	s->stats.b_rx += mbuf_get_left(mb);

	if (hdr->ssrc != s->ssrc_rx) {
		if (s->ssrc_rx) {
			flush = true;
			DEBUG_NOTICE("%s: SSRC changed %x -> %x"
					" (%u bytes from %J)\n",
				     sdp_media_name(s->sdp),
				     s->ssrc_rx, hdr->ssrc,
				     mbuf_get_left(mb), src);
		}
		s->ssrc_rx = hdr->ssrc;
	}

	if (s->jbuf) {

		struct rtp_header hdr2;
		void *mb2 = NULL;

		/* Put frame in Jitter Buffer */
		if (flush)
			jbuf_flush(s->jbuf);

		err = jbuf_put(s->jbuf, hdr, mb);
		if (err) {
			(void)re_printf("%s: dropping %u bytes from %J (%s)\n",
					sdp_media_name(s->sdp), mb->end,
					src, strerror(err));
		}

		if (jbuf_get(s->jbuf, &hdr2, &mb2))
			memset(&hdr2, 0, sizeof(hdr2));

		if (lostcalc(s, hdr2.seq) > 0)
			s->rh(hdr, NULL, s->arg);

		s->rh(&hdr2, mb2, s->arg);

		mem_deref(mb2);
	}
	else {
		if (lostcalc(s, hdr->seq) > 0)
			s->rh(hdr, NULL, s->arg);

		s->rh(hdr, mb, s->arg);
	}
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
		if (s->type == STREAM_VIDEO)
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


int stream_alloc(struct stream **sp, struct call *call,
		 struct sdp_session *sdp_sess,
		 const char *name, int label,
		 const struct mnat *mnat, struct mnat_sess *mnat_sess,
		 const struct menc *menc,
		 stream_recv_h *rh, void *arg)
{
	struct stream *s;
	int err;

	if (!sp || !call || !rh)
		return EINVAL;

	s = mem_zalloc(sizeof(*s), stream_destructor);
	if (!s)
		return ENOMEM;

	MAGIC_INIT(s);

	tmr_init(&s->tmr_stats);

	s->call  = call;

	if (!str_casecmp(name, "audio"))
		s->type = STREAM_AUDIO;
	else if (!str_casecmp(name, "video"))
		s->type = STREAM_VIDEO;
	else
		s->type = STREAM_UNKNOWN;

	s->rh    = rh;
	s->arg   = arg;
	s->pseq  = -1;
	s->rtcp  = config.avt.rtcp_enable;

	err = stream_sock_alloc(s);
	if (err)
		goto out;

	/* Jitter buffer */
	if (config.avt.jbuf_del.min && config.avt.jbuf_del.max) {

		err = jbuf_alloc(&s->jbuf, config.avt.jbuf_del.min,
				 config.avt.jbuf_del.max);
		if (err)
			goto out;
	}

	err = sdp_media_add(&s->sdp, sdp_sess, name,
			    sa_port(rtp_local(s->rtp)),
			    menc2transp(menc));
	if (err)
		goto out;

	if (label) {
		err |= sdp_media_set_lattr(s->sdp, true,
					   "label", "%d", label);
	}

	/* RFC 5761 */
	if (config.avt.rtcp_mux)
		err |= sdp_media_set_lattr(s->sdp, true, "rtcp-mux", NULL);

	if (mnat) {
		err |= mnat->mediah(&s->mns, mnat_sess, IPPROTO_UDP,
				    rtp_sock(s->rtp),
				    (s->rtcp && !config.avt.rtcp_mux)
				    ? rtcp_sock(s->rtp) : NULL,
				    s->sdp);
	}

	if (menc) {
		err |= menc->alloch(&s->menc, (struct menc *)menc,
				    IPPROTO_UDP, rtp_sock(s->rtp),
				    s->rtcp ? rtcp_sock(s->rtp) : NULL,
				    s->sdp);
	}

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


struct sdp_media *stream_sdpmedia(const struct stream *s)
{
	return s ? s->sdp : NULL;
}


int stream_start(struct stream *s)
{
	if (!s)
		return EINVAL;

	tmr_start(&s->tmr_stats, 1, tmr_stats_handler, s);

	return 0;
}


void stream_start_keepalive(struct stream *s)
{
	const char *rtpkeep;

	if (!s)
		return;

	rtpkeep = ua_param(call_get_ua(s->call), "rtpkeep");

	s->rtpkeep = mem_deref(s->rtpkeep);

	if (rtpkeep && sdp_media_rformat(s->sdp, NULL)) {
		int err;
		err = rtpkeep_alloc(&s->rtpkeep, rtpkeep,
				    IPPROTO_UDP, s->rtp, s->sdp);
		if (err) {
			DEBUG_WARNING("rtpkeep_alloc failed: %s\n",
				      strerror(err));
		}
	}
}


int stream_send(struct stream *s, bool marker, uint8_t pt, uint32_t ts,
		struct mbuf *mb)
{
	int err;

	if (!s)
		return EINVAL;

	if (!sa_isset(sdp_media_raddr(s->sdp), SA_ALL))
		return 0;
	if (sdp_media_dir(s->sdp) != SDP_SENDRECV)
		return 0;

	s->stats.b_tx += mbuf_get_left(mb);

	err = rtp_send(s->rtp, sdp_media_raddr(s->sdp), marker, pt, ts, mb);

	rtpkeep_refresh(s->rtpkeep, ts);

	++s->stats.n_tx;

	return err;
}


void stream_remote_set(struct stream *s, const char *cname)
{
	struct sa rtcp;

	if (!s)
		return;

	/* RFC 5761 */
	if (config.avt.rtcp_mux && sdp_media_rattr(s->sdp, "rtcp-mux")) {

		if (!s->rtcp_mux)
			(void)re_printf("%s: RTP/RTCP multiplexing enabled\n",
					sdp_media_name(s->sdp));
		s->rtcp_mux = true;
	}

	rtcp_enable_mux(s->rtp, s->rtcp_mux);

	sdp_media_raddr_rtcp(s->sdp, &rtcp);

	rtcp_start(s->rtp, cname,
		   s->rtcp_mux ? sdp_media_raddr(s->sdp): &rtcp);
}


void stream_sdp_attr_decode(struct stream *s)
{
	const char *attr;
	int err;

	if (!s)
		return;

	if (s->menc && menc_get(s->menc)->updateh) {
		err = menc_get(s->menc)->updateh(s->menc);
		if (err) {
			DEBUG_WARNING("menc update: %s\n", strerror(err));
		}
	}

	/* RFC 4585 */
	attr = sdp_media_rattr(s->sdp, "rtcp-fb");
	if (attr) {
		if (0 == re_regex(attr, strlen(attr), "nack")) {
			if (!s->nack_pli) {
				DEBUG_NOTICE("Peer supports NACK PLI (%s)\n",
					     attr);
			}
			s->nack_pli = true;
		}
	}
}


int stream_jbuf_stat(struct re_printf *pf, const struct stream *s)
{
	struct jbuf_stat stat;
	int err;

	if (!s)
		return EINVAL;

	err  = re_hprintf(pf, " %s:", sdp_media_name(s->sdp));

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


void stream_hold(struct stream *s, bool hold)
{
	if (!s)
		return;

	sdp_media_set_ldir(s->sdp, hold ? SDP_SENDONLY : SDP_SENDRECV);
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


void stream_set_bw(struct stream *s, uint32_t bps)
{
	if (!s)
		return;

	sdp_media_set_lbandwidth(s->sdp, SDP_BANDWIDTH_AS, bps / 1024);
}


bool stream_has_media(const struct stream *s)
{
	bool has;

	if (!s)
		return false;

	has = sdp_media_rformat(s->sdp, NULL) != NULL;
	if (has)
		return sdp_media_rport(s->sdp) != 0;

	return false;
}


int stream_debug(struct re_printf *pf, const struct stream *s)
{
	struct sa rrtcp;
	int err;

	if (!s)
		return 0;

	err  = re_hprintf(pf, " %s dir=%s\n", sdp_media_name(s->sdp),
			  sdp_dir_name(sdp_media_dir(s->sdp)));

	sdp_media_raddr_rtcp(s->sdp, &rrtcp);
	err |= re_hprintf(pf, " remote: %J/%J\n",
			  sdp_media_raddr(s->sdp), &rrtcp);

	err |= rtp_debug(pf, s->rtp);
	err |= jbuf_debug(pf, s->jbuf);

	return err;
}


int stream_print(struct re_printf *pf, const struct stream *s)
{
	if (!s)
		return 0;

	return re_hprintf(pf, " %s=%u/%u", sdp_media_name(s->sdp),
			  s->stats.bitrate_tx, s->stats.bitrate_rx);
}
