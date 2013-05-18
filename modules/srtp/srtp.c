/**
 * @file srtp.c Secure Real-time Transport Protocol (RFC 3711)
 *
 * Copyright (C) 2010 Creytiv.com
 */
#if defined (__GNUC__) && !defined (asm)
#define asm __asm__  /* workaround */
#endif
#include <srtp/srtp.h>
#include <re.h>
#include <baresip.h>
#include "sdes.h"


#define DEBUG_MODULE "srtp"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct menc_st {
	/* one SRTP session per media line */
	uint8_t key_tx[32];  /* 32 for alignment, only 30 used */
	uint8_t key_rx[32];
	srtp_t srtp_tx, srtp_rx;
	srtp_policy_t policy_tx, policy_rx;
	bool use_srtp;

	void *rtpsock;
	void *rtcpsock;
	struct udp_helper *uh_rtp;   /**< UDP helper for RTP encryption    */
	struct udp_helper *uh_rtcp;  /**< UDP helper for RTCP encryption   */
	struct sdp_media *sdpm;
};


static const char aes_cm_128_hmac_sha1_32[] = "AES_CM_128_HMAC_SHA1_32";
static const char aes_cm_128_hmac_sha1_80[] = "AES_CM_128_HMAC_SHA1_80";


static void destructor(void *arg)
{
	struct menc_st *st = arg;

	mem_deref(st->sdpm);

	/* note: must be done before freeing socket */
	mem_deref(st->uh_rtp);
	mem_deref(st->uh_rtcp);
	mem_deref(st->rtpsock);
	mem_deref(st->rtcpsock);

	if (st->srtp_tx)
		srtp_dealloc(st->srtp_tx);
	if (st->srtp_rx)
		srtp_dealloc(st->srtp_rx);
}


static int setup_srtp(struct menc_st *st)
{
	err_status_t e;

	/* init SRTP */
	e = crypto_get_random(st->key_tx, SRTP_MASTER_KEY_LEN);
	if (err_status_ok != e) {
		DEBUG_WARNING("crypto_get_random() failed %d\n", e);
		return ENOSYS;
	}

	/* transmit policy */
	crypto_policy_set_rtp_default(&st->policy_tx.rtp);
	crypto_policy_set_rtcp_default(&st->policy_tx.rtcp);
	st->policy_tx.ssrc.type = ssrc_any_outbound;
	st->policy_tx.key = st->key_tx;
	st->policy_tx.next = NULL;

	/* receive policy */
	crypto_policy_set_rtp_default(&st->policy_rx.rtp);
	crypto_policy_set_rtcp_default(&st->policy_rx.rtcp);
	st->policy_rx.ssrc.type = ssrc_any_inbound;
	st->policy_rx.key = st->key_rx;
	st->policy_rx.next = NULL;

	/* allocate and initialize the SRTP session */
	srtp_create(&st->srtp_tx, &st->policy_tx);

	return 0;
}


static int rtp_enc(struct menc_st *st, struct mbuf *mb)
{
	uint32_t srtp_len;
	err_status_t e;
	int len = (int)mbuf_get_left(mb);

	if (!st->use_srtp)
		return 0;

	srtp_len = len + SRTP_MAX_TRAILER_LEN;
	if (srtp_len > mbuf_get_space(mb)) {
		mbuf_resize(mb, mb->pos + srtp_len);
	}

	e = srtp_protect(st->srtp_tx, mbuf_buf(mb), &len);
	if (err_status_ok != e) {
		DEBUG_WARNING("srtp_protect: err=%d\n", e);
		return EPROTO;
	}

	mbuf_set_end(mb, mb->pos + len);

	return 0;
}


static int rtp_dec(struct menc_st *st, struct mbuf *mb)
{
	err_status_t r;
	int len = (int)mbuf_get_left(mb);

	if (!st->use_srtp)
		return 0;

	r = srtp_unprotect(st->srtp_rx, mbuf_buf(mb), &len);

	switch (r) {

	case err_status_ok:
		mbuf_set_end(mb, mb->pos + len);
		break;

	case err_status_auth_fail:
		DEBUG_WARNING("srtp_unprotect: auth check fail\n");
		return EINVAL;

	case err_status_replay_fail:
		DEBUG_WARNING("srtp_unprotect: replay error\n");
		return ENOENT;

	default:
		DEBUG_WARNING("srtp_unprotect: unknown err %d\n", r);
		return ENOSYS;
	}

	return 0;
}


static int rtcp_enc(struct menc_st *st, struct mbuf *mb)
{
	uint32_t srtp_len;
	err_status_t e;
	int len = (int)mbuf_get_left(mb);

	if (!st->use_srtp)
		return 0;

	srtp_len = len + SRTP_MAX_TRAILER_LEN;
	if (srtp_len > mbuf_get_space(mb)) {
		mbuf_resize(mb, mb->pos + srtp_len);
	}

	e = srtp_protect_rtcp(st->srtp_tx, mbuf_buf(mb), &len);
	if (err_status_ok != e) {
		DEBUG_WARNING("srtp_protect_rtcp: err=%d\n", e);
		return EPROTO;
	}

	mbuf_set_end(mb, mb->pos + len);

	return 0;
}


static int rtcp_dec(struct menc_st *st, struct mbuf *mb)
{
	err_status_t r;
	int len = (int)mbuf_get_left(mb);

	if (!st->use_srtp)
		return 0;

	r = srtp_unprotect_rtcp(st->srtp_rx, mbuf_buf(mb), &len);

	switch (r) {

	case err_status_ok:
		mbuf_set_end(mb, mb->pos + len);
		break;

	case err_status_auth_fail:
		DEBUG_WARNING("srtp_unprotect_rtcp: auth check fail\n");
		return EINVAL;

	case err_status_replay_fail:
		DEBUG_WARNING("srtp_unprotect_rtcp: replay error\n");
		return ENOENT;

	default:
		DEBUG_WARNING("srtp_unprotect_rtcp: unknown err %d\n", r);
		return ENOSYS;
	}

	return 0;
}


/** Media Encryption - Encode */
static bool menc_send_handler(int *err, struct sa *dst,
			      struct mbuf *mb, void *arg)
{
	struct menc_st *st = arg;

	if (sa_cmp(dst, sdp_media_raddr(st->sdpm), SA_ALL))
		*err = rtp_enc(st, mb);

	return false;  /* continue processing */
}


/** Media Encryption - Decode */
static bool menc_recv_handler(struct sa *src, struct mbuf *mb, void *arg)
{
	struct menc_st *st = arg;

	(void)src;

	if (rtp_dec(st, mb))
		return true;  /* error - drop packet */

	return false;  /* continue processing */
}


static bool menc_send_rtcp(int *err, struct sa *dst,
			   struct mbuf *mb, void *arg)
{
	struct menc_st *st = arg;
	struct sa rrtcp;

	sdp_media_raddr_rtcp(st->sdpm, &rrtcp);

	if (sa_cmp(dst, &rrtcp, SA_ALL))
		*err = rtcp_enc(st, mb);

	return false;  /* continue processing */
}


static bool menc_recv_rtcp(struct sa *src,
			      struct mbuf *mb, void *arg)
{
	struct menc_st *st = arg;

	(void)src;

	if (rtcp_dec(st, mb))
		return true;  /* error - drop packet */

	return false;  /* continue processing */
}


/* a=crypto:<tag> <crypto-suite> <key-params> [<session-params>] */
static int sdp_enc(struct menc_st *st, struct sdp_media *m)
{
	char key[128] = "";
	size_t olen;
	int err;

	olen = sizeof(key);
	err = base64_encode(st->key_tx, SRTP_MASTER_KEY_LEN, key, &olen);
	if (err)
		return err;

	return sdes_encode_crypto(m, aes_cm_128_hmac_sha1_80, key, olen);
}


static int decode_crypto(struct menc_st *st, const char *value)
{
	struct crypto c;
	err_status_t e;
	size_t olen;
	int err;

	err = sdes_decode_crypto(&c, value);
	if (err)
		return err;

	/* key-info is BASE64 encoded */

	olen = sizeof(st->key_rx);
	err = base64_decode(c.key_info.p, c.key_info.l, st->key_rx, &olen);
	if (err)
		return err;

	if (SRTP_MASTER_KEY_LEN != olen) {
		DEBUG_WARNING("srtp keylen is %u (should be 30)\n", olen);
	}

	if (0 != pl_strcmp(&c.key_method, "inline")) {
		DEBUG_WARNING("only key method 'inline' supported\n");
		return EINVAL;
	}

	if (0 == pl_strcasecmp(&c.suite, aes_cm_128_hmac_sha1_32)) {
		crypto_policy_set_aes_cm_128_hmac_sha1_32(&st->policy_rx.rtp);
	}
	else if (0 == pl_strcasecmp(&c.suite, aes_cm_128_hmac_sha1_80)) {
		crypto_policy_set_aes_cm_128_hmac_sha1_80(&st->policy_rx.rtp);
	}
	else {
		DEBUG_WARNING("unknown SRTP crypto suite (%r)\n", &c.suite);
		return ENOENT;
	}

	e = srtp_create(&st->srtp_rx, &st->policy_rx);
	if (err_status_ok != e) {
		DEBUG_WARNING("srtp_create rx failed: %d\n", e);
	}

	/* use SRTP for this stream/session */
	st->use_srtp = true;

	(void)re_fprintf(stderr, "%s: SRTP is Enabled\n",
			 sdp_media_name(st->sdpm));

	return 0;
}


static int update(struct menc_st *st)
{
	const char *attr;

	attr = sdp_media_rattr(st->sdpm, sdp_attr_crypto);
	if (attr)
		return decode_crypto(st, attr);

	return 0;
}


static int alloc(struct menc_media **stp, struct menc_sess *sess,
		 int proto, void *rtpsock, void *rtcpsock,
		 struct sdp_media *sdpm)
{
	struct menc_st *st;
	int layer = 10; /* above zero */
	int err = 0;
	(void)sess;

	if (!stp || !sdpm)
		return EINVAL;
	if (proto != IPPROTO_UDP)
		return EPROTONOSUPPORT;

	if (*stp) {
		return update((struct menc_st *)*stp);
	}

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->sdpm = mem_ref(sdpm);

	if (rtpsock) {
		st->rtpsock = mem_ref(rtpsock);
		err |= udp_register_helper(&st->uh_rtp, rtpsock, layer,
					   menc_send_handler,
					   menc_recv_handler, st);
	}
	if (rtcpsock) {
		st->rtcpsock = mem_ref(rtcpsock);
		err |= udp_register_helper(&st->uh_rtcp, rtcpsock, layer,
					   menc_send_rtcp,
					   menc_recv_rtcp, st);
	}
	if (err)
		goto out;

	err = setup_srtp(st);
	if (err)
		goto out;

	err = sdp_enc(st, sdpm);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = (struct menc_media *)st;

	return err;
}


static struct menc menc_srtp_opt = {
	LE_INIT, "srtp", "RTP/AVP", NULL, alloc
};

static struct menc menc_srtp_mand = {
	LE_INIT, "srtp-mand", "RTP/SAVP", NULL, alloc
};

static struct menc menc_srtp_mandf = {
	LE_INIT, "srtp-mandf", "RTP/SAVPF", NULL, alloc
};


static int mod_srtp_init(void)
{
	if (err_status_ok != srtp_init()) {
		DEBUG_WARNING("srtp_init() failed\n");
		return ENOSYS;
	}

	menc_register(&menc_srtp_opt);
	menc_register(&menc_srtp_mand);
	menc_register(&menc_srtp_mandf);

	return 0;
}


static int mod_srtp_close(void)
{
	menc_unregister(&menc_srtp_mandf);
	menc_unregister(&menc_srtp_mand);
	menc_unregister(&menc_srtp_opt);

	crypto_kernel_shutdown();

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(srtp) = {
	"srtp",
	"menc",
	mod_srtp_init,
	mod_srtp_close
};
