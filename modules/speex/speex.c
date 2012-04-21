/**
 * @file speex.c  Speex audio codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#include <speex/speex.h>
#include <speex/speex_stereo.h>
#include <speex/speex_callbacks.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "speex"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


enum {
	MIN_FRAME_SIZE = 43,
	SPEEX_PTIME    = 20,
};

struct aucodec_st {
	struct aucodec *ac;  /* inheritance */
	uint32_t frame_size;  /* Number of samples */
	uint8_t channels;
	struct {
		void *st;
		SpeexBits bits;
	} enc;
	struct {
		void *st;
		SpeexBits bits;
		SpeexStereoState stereo;
		SpeexCallback callback;
	} dec;
};


static struct aucodec *speexv[6];
static char speex_fmtp[128];


/** Speex configuration */
static struct {
	int quality;
	int complexity;
	int enhancement;
	int vbr;
	int vad;
} sconf = {
	3,  /* 0-10   */
	2,  /* 0-10   */
	0,  /* 0 or 1 */
	0,  /* 0 or 1 */
	0   /* 0 or 1 */
};


static void speex_destructor(void *arg)
{
	struct aucodec_st *st = arg;

	/* Encoder */
	speex_bits_destroy(&st->enc.bits);
	speex_encoder_destroy(st->enc.st);

	/* Decoder */
	speex_bits_destroy(&st->dec.bits);
	speex_decoder_destroy(st->dec.st);

	mem_deref(st->ac);
}


static void encoder_config(void *st)
{
	int ret;

	ret = speex_encoder_ctl(st, SPEEX_SET_QUALITY, &sconf.quality);
	if (ret) {
		DEBUG_WARNING("SPEEX_SET_QUALITY: %d\n", ret);
	}

	ret = speex_encoder_ctl(st, SPEEX_SET_COMPLEXITY, &sconf.complexity);
	if (ret) {
		DEBUG_WARNING("SPEEX_SET_COMPLEXITY: %d\n", ret);
	}

	ret = speex_encoder_ctl(st, SPEEX_SET_VBR, &sconf.vbr);
	if (ret) {
		DEBUG_WARNING("SPEEX_SET_VBR: %d\n", ret);
	}

	ret = speex_encoder_ctl(st, SPEEX_SET_VAD, &sconf.vad);
	if (ret) {
		DEBUG_WARNING("SPEEX_SET_VAD: %d\n", ret);
	}
}


static void decoder_config(void *st)
{
	int ret;

	ret = speex_decoder_ctl(st, SPEEX_SET_ENH, &sconf.enhancement);
	if (ret) {
		DEBUG_WARNING("SPEEX_SET_ENH: %d\n", ret);
	}
}


static int decode_param(struct aucodec_st *st, const struct pl *name,
			const struct pl *val)
{
	int ret;

	DEBUG_INFO("speex param: \"%r\" = \"%r\"\n", name, val);

	/* mode: List supported Speex decoding modes.  The valid modes are
	   different for narrowband and wideband, and are defined as follows:

	   {1,2,3,4,5,6,7,8,any} for narrowband
	   {0,1,2,3,4,5,6,7,8,9,10,any} for wideband and ultra-wideband
	 */
	if (0 == pl_strcasecmp(name, "mode")) {
		struct pl v;
		int mode;

		/* parameter is quoted */
		if (re_regex(val->p, val->l, "\"[^\"]+\"", &v))
			v = *val;

		if (0 == pl_strcasecmp(&v, "any"))
			return 0;

		mode = pl_u32(&v);

		DEBUG_NOTICE("SPEEX_SET_MODE: mode=%d\n", mode);
		ret = speex_encoder_ctl(st->enc.st, SPEEX_SET_MODE, &mode);
		if (ret) {
			DEBUG_WARNING("SPEEX_SET_MODE: ret=%d\n", ret);
		}
	}
	/* vbr: variable bit rate - either 'on' 'off' or 'vad' */
	else if (0 == pl_strcasecmp(name, "vbr")) {
		int vbr = 0, vad = 0;

		if (0 == pl_strcasecmp(val, "on"))
			vbr = 1;
		else if (0 == pl_strcasecmp(val, "off"))
			vbr = 0;
		else if (0 == pl_strcasecmp(val, "vad"))
			vad = 1;
		else {
			DEBUG_WARNING("invalid vbr value %r\n", val);
		}

		DEBUG_NOTICE("Setting VBR=%d VAD=%d\n", vbr, vad);
		ret = speex_encoder_ctl(st->enc.st, SPEEX_SET_VBR, &vbr);
		if (ret) {
			DEBUG_WARNING("SPEEX_SET_VBR: ret=%d\n", ret);
		}
		ret = speex_encoder_ctl(st->enc.st, SPEEX_SET_VAD, &vad);
		if (ret) {
			DEBUG_WARNING("SPEEX_SET_VAD: ret=%d\n", ret);
		}
	}
	else if (0 == pl_strcasecmp(name, "cng")) {
		int dtx = 0;

		if (0 == pl_strcasecmp(val, "on"))
			dtx = 0;
		else if (0 == pl_strcasecmp(val, "off"))
			dtx = 1;

		ret = speex_encoder_ctl(st->enc.st, SPEEX_SET_DTX, &dtx);
		if (ret) {
			DEBUG_WARNING("SPEEX_SET_DTX: ret=%d\n", ret);
		}
	}
	else {
		DEBUG_NOTICE("unknown Speex param: %r=%r\n", name, val);
	}

	return 0;
}


static void param_handler(const struct pl *name, const struct pl *val,
			  void *arg)
{
	struct aucodec_st *st = arg;

	decode_param(st, name, val);
}


static int alloc(struct aucodec_st **stp, struct aucodec *ac,
		 struct aucodec_prm *encp, struct aucodec_prm *decp,
		 const char *fmtp)
{
	struct aucodec_st *st;
	const uint32_t srate = aucodec_srate(ac);
	const SpeexMode *mode = &speex_nb_mode;
	const char *speex_ver;
	int ret, err = 0;

	switch (srate) {

	case 8000:
		mode = &speex_nb_mode;
		break;

	case 16000:
		mode = &speex_wb_mode;
		break;

	case 32000:
		mode = &speex_uwb_mode;
		break;

	default:
		DEBUG_WARNING("alloc: unsupported srate %lu\n", srate);
		return EINVAL;
	}

	if ((encp && encp->ptime % 20) || (decp && decp->ptime % 20)) {
		DEBUG_WARNING("alloc: ptime must be a multiple of 20\n");
		return EINVAL;
	}

	st = mem_zalloc(sizeof(*st), speex_destructor);
	if (!st)
		return ENOMEM;

	st->ac = mem_ref(ac);
	st->frame_size = 160 * srate/8000;
	st->channels = aucodec_ch(ac);

	if (0 == speex_lib_ctl(SPEEX_LIB_GET_VERSION_STRING, &speex_ver)) {
		DEBUG_NOTICE("using Speex version %s\n", speex_ver);
	}

	/* Encoder */
	st->enc.st = speex_encoder_init((SpeexMode *)mode);
	if (!st->enc.st) {
		DEBUG_WARNING("alloc: speex_encoder_init() failed\n");
		err = EPROTO;
		goto out;
	}

	speex_bits_init(&st->enc.bits);

	encoder_config(st->enc.st);

	ret = speex_encoder_ctl(st->enc.st, SPEEX_GET_FRAME_SIZE,
				&st->frame_size);
	if (ret) {
		DEBUG_WARNING("SPEEX_GET_FRAME_SIZE: %d\n", ret);
	}

	if (str_isset(fmtp)) {
		struct pl params;

		pl_set_str(&params, fmtp);

		fmt_param_apply(&params, param_handler, st);
	}

	/* Decoder */
	st->dec.st = speex_decoder_init((SpeexMode *)mode);
	if (!st->dec.st) {
		err = EPROTO;
		goto out;
	}

	speex_bits_init(&st->dec.bits);

	if (2 == st->channels) {
		DEBUG_NOTICE("decoder: Stereo enabled\n");

		/* Stereo. */
		st->dec.stereo.balance = 1;
		st->dec.stereo.e_ratio = .5f;
		st->dec.stereo.smooth_left = 1;
		st->dec.stereo.smooth_right = 1;

		st->dec.callback.callback_id = SPEEX_INBAND_STEREO;
		st->dec.callback.func = speex_std_stereo_request_handler;
		st->dec.callback.data = &st->dec.stereo;
		speex_decoder_ctl(st->dec.st, SPEEX_SET_HANDLER,
				  &st->dec.callback);
	}

	decoder_config(st->dec.st);

out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int enc(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	const size_t n = sizeof(uint16_t) * st->channels * st->frame_size;
	int ret;
	int len;

	/* Make sure there is enough space */
	if (mbuf_get_space(dst) < 128) {
		DEBUG_WARNING("encode: dst buffer is too small (%u bytes)\n",
			      mbuf_get_space(dst));
		return ENOMEM;
	}

	/* VAD */
	if (0 == mbuf_get_left(src)) {
		/* 5 zeros interpreted by Speex as silence (submode 0) */
		speex_bits_pack(&st->enc.bits, 0, 5);
		goto out;
	}

	/* Handle multiple Speex frames in one RTP packet */
	while (mbuf_get_left(src) >= n) {

		/* Assume stereo */
		if (2 == st->channels) {
			speex_encode_stereo_int((int16_t *)mbuf_buf(src),
						st->frame_size, &st->enc.bits);
		}

		ret = speex_encode_int(st->enc.st, (int16_t *)mbuf_buf(src),
				       &st->enc.bits);
		if (1 != ret) {
			DEBUG_WARNING("speex_encode_int: ret=%d\n", ret);
		}

		mbuf_advance(src, n);
	}

 out:
	/* Terminate bit stream */
	speex_bits_pack(&st->enc.bits, 15, 5);

	len = speex_bits_write(&st->enc.bits, (char *)mbuf_buf(dst),
			       (int)(dst->size - dst->pos));
	dst->end += len;

	speex_bits_reset(&st->enc.bits);

	return 0;
}


/* src=NULL means lost packet */
static int dec(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	const size_t n = sizeof(uint16_t) * st->channels * st->frame_size;
	int err;

	/* Make sure there is enough space in the buffer */
	if (mbuf_get_space(dst) < n) {
		err = mbuf_resize(dst, dst->size + n);
		if (err)
			return err;
	}

	/* Silence */
	if (0 == mbuf_get_left(src)) {
		speex_decode_int(st->dec.st, NULL, (int16_t *)mbuf_buf(dst));
		dst->end += n;
		return 0;
	}

	/* Read into bit-stream */
	speex_bits_read_from(&st->dec.bits, (char *)mbuf_buf(src),
			     (int)mbuf_get_left(src));
	mbuf_skip_to_end(src);

	/* Handle multiple Speex frames in one RTP packet */
	while (speex_bits_remaining(&st->dec.bits) >= MIN_FRAME_SIZE) {
		int ret;

		if (mbuf_get_space(dst) < n) {
			err = mbuf_resize(dst, dst->size + n);
			if (err)
				return err;
		}

		ret = speex_decode_int(st->dec.st, &st->dec.bits,
				       (int16_t *)mbuf_buf(dst));
		if (ret < 0) {
			if (-1 == ret) {
			}
			else if (-2 == ret) {
				DEBUG_WARNING("decode: corrupt stream\n");
			}
			else {
				DEBUG_WARNING("decode: speex_decode_int:"
					      " ret=%d\n", ret);
			}
			break;
		}

		/* Transforms a mono frame into a stereo frame
		   using intensity stereo info */
		if (2 == st->channels) {
			speex_decode_stereo_int((int16_t *)mbuf_buf(dst),
						st->frame_size,
						&st->dec.stereo);
		}

		dst->end += n;
		mbuf_advance(dst, n);
	}

	return 0;
}


static void config_parse(struct conf *conf)
{
	uint32_t v;

	if (0 == conf_get_u32(conf, "speex_quality", &v))
		sconf.quality = v;
	if (0 == conf_get_u32(conf, "speex_complexity", &v))
		sconf.complexity = v;
	if (0 == conf_get_u32(conf, "speex_enhancement", &v))
		sconf.enhancement = v;
	if (0 == conf_get_u32(conf, "speex_vbr", &v))
		sconf.vbr = v;
	if (0 == conf_get_u32(conf, "speex_vad", &v))
		sconf.vad = v;
}


static int speex_init(void)
{
	int err;

	config_parse(conf_cur());

	(void)re_snprintf(speex_fmtp, sizeof(speex_fmtp),
			  "mode=\"7\";vbr=%s;cng=on",
			  sconf.vad ? "vad" : (sconf.vbr ? "on" : "off"));

	/* Stereo Speex */
	err  = aucodec_register(&speexv[0], NULL, "speex", 32000, 2,
				speex_fmtp, alloc, enc, dec, NULL);
	err |= aucodec_register(&speexv[1], NULL, "speex", 16000, 2,
				speex_fmtp, alloc, enc, dec, NULL);
	err |= aucodec_register(&speexv[2], NULL, "speex",  8000, 2,
				speex_fmtp, alloc, enc, dec, NULL);

	/* Standard Speex */
	err |= aucodec_register(&speexv[3], NULL, "speex", 32000, 1,
				speex_fmtp, alloc, enc, dec, NULL);
	err |= aucodec_register(&speexv[4], NULL, "speex", 16000, 1,
				speex_fmtp, alloc, enc, dec, NULL);
	err |= aucodec_register(&speexv[5], NULL, "speex",  8000, 1,
				speex_fmtp, alloc, enc, dec, NULL);

	return err;
}


static int speex_close(void)
{
	size_t i;
	for (i=0; i<ARRAY_SIZE(speexv); i++)
		speexv[i] = mem_deref(speexv[i]);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(speex) = {
	"speex",
	"codec",
	speex_init,
	speex_close
};
