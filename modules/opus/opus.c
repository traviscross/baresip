/**
 * @file opus.c OPUS audio codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#include <opus/opus.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "opus"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/* NOTE: This is experimental code!
 *
 * Latest supported version: 0.9.8
 *
 * References:
 *
 *    draft-ietf-codec-opus-10
 *    draft-spittka-payload-rtp-opus-00
 *
 *    http://opus-codec.org/downloads/
 */


enum {
	DEFAULT_BITRATE    = 64000, /**< 32-128 kbps               */
	DEFAULT_PTIME      = 20,    /**< Packet time in [ms]       */
	MAX_PACKET         = 1500,  /**< Maximum bytes per packet  */
};


struct aucodec_st {
	struct aucodec *ac;         /**< Inheritance - base class     */
	OpusEncoder *enc;           /**< Encoder state                */
	OpusDecoder *dec;           /**< Decoder state                */
	uint32_t frame_size;        /**< Frame size in [samples]      */
	uint32_t fsize;             /**< PCM Frame size in bytes      */
	bool got_packet;            /**< We have received a packet    */
};


static struct aucodec *codecv[4];

static struct {
	int app;
	int bandwidth;
	uint32_t bitrate;
	uint32_t complex;
	bool vbr;
} opus = {
	OPUS_APPLICATION_AUDIO,
	OPUS_BANDWIDTH_FULLBAND,
	DEFAULT_BITRATE,
	10,
	0,
};


static void destructor(void *arg)
{
	struct aucodec_st *st = arg;

	if (st->enc)
		opus_encoder_destroy(st->enc);
	if (st->dec)
		opus_decoder_destroy(st->dec);

	mem_deref(st->ac);
}


static int alloc(struct aucodec_st **stp, struct aucodec *ac,
		 struct aucodec_prm *encp, struct aucodec_prm *decp,
		 const char *fmtp)
{
	struct aucodec_st *st;
	const uint32_t srate = aucodec_srate(ac);
	const uint8_t ch = aucodec_ch(ac);
	uint32_t ptime = DEFAULT_PTIME;
	int use_inbandfec;
	int use_dtx;
	int err = 0;
	int opuserr;

	(void)decp;
	(void)fmtp;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	if (encp && encp->ptime)
		ptime = encp->ptime;

	st->ac         = mem_ref(ac);
	st->frame_size = srate * ptime / 1000;
	st->fsize      = 2 * st->frame_size * ch;

	/* Encoder */
	st->enc = opus_encoder_create(srate, ch, opus.app, &opuserr);
	if (!st->enc) {
		err = ENOMEM;
		goto out;
	}

	use_inbandfec = 1;
	use_dtx = 1;

	opus_encoder_ctl(st->enc, OPUS_SET_BITRATE(opus.bitrate));
	opus_encoder_ctl(st->enc, OPUS_SET_BANDWIDTH(opus.bandwidth));
	opus_encoder_ctl(st->enc, OPUS_SET_VBR(opus.vbr));
	opus_encoder_ctl(st->enc, OPUS_SET_COMPLEXITY(opus.complex));
	opus_encoder_ctl(st->enc, OPUS_SET_INBAND_FEC(use_inbandfec));
	opus_encoder_ctl(st->enc, OPUS_SET_DTX(use_dtx));

	/* Decoder */
	st->dec = opus_decoder_create(srate, ch, &opuserr);
	if (!st->dec) {
		err = ENOMEM;
		goto out;
	}

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	int len;

	if (!mbuf_get_left(src))
		return 0;

	if (mbuf_get_left(src) != st->fsize) {
		DEBUG_WARNING("encode: got %d bytes, expected %d\n",
			      mbuf_get_left(src), st->fsize);
		return EINVAL;
	}

	if (mbuf_get_space(dst) < MAX_PACKET) {
		int err = mbuf_resize(dst, dst->pos + MAX_PACKET);
		if (err)
			return err;
	}

	len = opus_encode(st->enc, (short *)mbuf_buf(src), st->frame_size,
			  mbuf_buf(dst), (int)mbuf_get_space(dst));
	if (len < 0) {
		DEBUG_WARNING("encode error: %d (%u bytes)\n", len,
			      mbuf_get_left(src));
		return EPROTO;
	}

	src->pos = src->end;
	dst->end = dst->pos + len;

	return 0;
}


/* src=NULL means lost packet */
static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	int r;

	if (!mbuf_get_left(src) && !st->got_packet)
		return 0;

	/* Make sure there is enough space in the buffer */
	if (mbuf_get_space(dst) < st->fsize) {
		int err = mbuf_resize(dst, dst->pos + st->fsize);
		if (err)
			return err;
	}

	r = opus_decode(st->dec, mbuf_buf(src), (int)mbuf_get_left(src),
			(short *)mbuf_buf(dst), st->frame_size, 0);
	if (r <= 0) {
		DEBUG_WARNING("opus_decode: r=%d (%u bytes)\n",
			      r, mbuf_get_left(src));
		return EBADMSG;
	}

	if (src)
		src->pos = src->end;

	dst->end += 2 * r * aucodec_ch(st->ac);

	st->got_packet = true;

	return 0;
}


static int module_init(void)
{
	int err = 0;

#ifdef MODULE_CONF
	struct pl pl;

	if (!conf_get(conf_cur(), "opus_application", &pl)) {

		if (!pl_strcasecmp(&pl, "voip"))
			opus.app = OPUS_APPLICATION_VOIP;
		else if (!pl_strcasecmp(&pl, "audio"))
			opus.app = OPUS_APPLICATION_AUDIO;
		else {
			DEBUG_WARNING("unknown application: %r\n", &pl);
		}
	}

	if (!conf_get(conf_cur(), "opus_bandwidth", &pl)) {

		if (!pl_strcasecmp(&pl, "narrowband"))
			opus.bandwidth = OPUS_BANDWIDTH_NARROWBAND;
		else if (!pl_strcasecmp(&pl, "mediumband"))
			opus.bandwidth = OPUS_BANDWIDTH_MEDIUMBAND;
		else if (!pl_strcasecmp(&pl, "wideband"))
			opus.bandwidth = OPUS_BANDWIDTH_WIDEBAND;
		else if (!pl_strcasecmp(&pl, "superwideband"))
			opus.bandwidth = OPUS_BANDWIDTH_SUPERWIDEBAND;
		else if (!pl_strcasecmp(&pl, "fullband"))
			opus.bandwidth = OPUS_BANDWIDTH_FULLBAND;
		else {
			DEBUG_WARNING("unknown bandwidth: %r\n", &pl);
		}
	}

	conf_get_u32(conf_cur(),  "opus_complexity", &opus.complex);
	conf_get_u32(conf_cur(),  "opus_bitrate",    &opus.bitrate);
	conf_get_bool(conf_cur(), "opus_vbr",        &opus.vbr);
#endif

	err |= aucodec_register(&codecv[0], NULL, "opus", 48000, 2, NULL,
				alloc, encode, decode, NULL);

	err |= aucodec_register(&codecv[1], NULL, "opus", 48000, 1, NULL,
				alloc, encode, decode, NULL);
#if 0
	err |= aucodec_register(&codecv[2], NULL, "opus", 32000, 1, NULL,
				alloc, encode, decode, NULL);
#endif

	return err;
}


static int module_close(void)
{
	size_t i;

	for (i=0; i<ARRAY_SIZE(codecv); i++)
		codecv[i] = mem_deref(codecv[i]);

	return 0;
}


/** Module exports */
EXPORT_SYM const struct mod_export DECL_EXPORTS(opus) = {
	"opus",
	"codec",
	module_init,
	module_close
};
