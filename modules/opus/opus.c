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


/*
 * Latest supported version: libopus 1.0.0
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
};


struct auenc_state {
	OpusEncoder *enc;           /**< Encoder state                   */
	int frame_size;             /**< Num samples, excluding channels */
	int ch;
};

struct audec_state {
	OpusDecoder *dec;           /**< Decoder state                */
	uint8_t ch;
};


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


static void encode_destructor(void *arg)
{
	struct auenc_state *st = arg;

	if (st->enc)
		opus_encoder_destroy(st->enc);
}


static void decode_destructor(void *arg)
{
	struct audec_state *st = arg;

	if (st->dec)
		opus_decoder_destroy(st->dec);
}


static int encode_update(struct auenc_state **aesp,
			 const struct aucodec *ac,
			 struct auenc_param *prm, const char *fmtp)
{
	struct auenc_state *st;
	int use_inbandfec;
	int use_dtx;
	int opuserr;
	int err = 0;
	(void)prm;
	(void)fmtp;

	if (!aesp || !ac)
		return EINVAL;
	if (*aesp)
		return 0;

	st = mem_zalloc(sizeof(*st), encode_destructor);
	if (!st)
		return ENOMEM;

	st->frame_size = ac->srate * prm->ptime / 1000;
	st->ch = ac->ch;

	/* Encoder */
	st->enc = opus_encoder_create(ac->srate, ac->ch, opus.app, &opuserr);
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

 out:
	if (err)
		mem_deref(st);
	else
		*aesp = st;

	return err;
}


static int decode_update(struct audec_state **adsp,
			 const struct aucodec *ac, const char *fmtp)
{
	struct audec_state *st;
	int err = 0;
	int opuserr;
	(void)fmtp;

	if (!adsp || !ac)
		return EINVAL;
	if (*adsp)
		return 0;

	st = mem_zalloc(sizeof(*st), decode_destructor);
	if (!st)
		return ENOMEM;

	st->ch = ac->ch;

	/* Decoder */
	st->dec = opus_decoder_create(ac->srate, ac->ch, &opuserr);
	if (!st->dec) {
		err = ENOMEM;
		goto out;
	}

 out:
	if (err)
		mem_deref(st);
	else
		*adsp = st;

	return err;
}


static int encode(struct auenc_state *st, uint8_t *buf,
		  size_t *len, const int16_t *sampv, size_t sampc)
{
	int n;

	if (!st || !buf || !len || !sampv || !sampc)
		return EINVAL;

	/* verify sample-count */
	if ((int)sampc != st->ch * st->frame_size)
		return EINVAL;

	n = opus_encode(st->enc, sampv, st->frame_size, buf, (int)*len);
	if (n < 0)
		return EPROTO;

	if (n > (int)*len) {
		DEBUG_WARNING("opus overwrite buffer: %d > %d\n",
			      n, *len);
		return ENOMEM;
	}

	*len = n;

	return 0;
}


static int decode(struct audec_state *st, int16_t *sampv,
		  size_t *sampc, const uint8_t *buf, size_t len)
{
	int n;

	n = opus_decode(st->dec, buf, (int)len, sampv, (int)*sampc, 0);
	if (n < 0)
		return EPROTO;

	*sampc = n * st->ch;

	return 0;
}


static int pkloss(struct audec_state *st, int16_t *sampv, size_t *sampc)
{
	int n;

	n = opus_decode(st->dec, NULL, 0, sampv, (int)*sampc, 0);
	if (n < 0)
		return EPROTO;

	*sampc = n * st->ch;

	return 0;
}


static struct aucodec opus0 = {
	LE_INIT, 0, "opus", 48000, 2, NULL,
	encode_update, encode,
	decode_update, decode, pkloss,
	NULL, NULL
};

static struct aucodec opus1 = {
	LE_INIT, 0, "opus", 48000, 1, NULL,
	encode_update, encode,
	decode_update, decode, pkloss,
	NULL, NULL
};


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

	aucodec_register(&opus0);
	aucodec_register(&opus1);

	return err;
}


static int module_close(void)
{
	aucodec_unregister(&opus1);
	aucodec_unregister(&opus0);

	return 0;
}


/** Module exports */
EXPORT_SYM const struct mod_export DECL_EXPORTS(opus) = {
	"opus",
	"codec",
	module_init,
	module_close
};
