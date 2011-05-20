/**
 * @file resampler.c  Speex resampler
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <speex/speex_resampler.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "speex_resamp"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct aufilt_st {
	struct aufilt *af; /* base class */
	SpeexResamplerState *enc, *dec;
};


static struct aufilt *filt;
static const int quality = 0;  /* 0-10 */


static void resamp_destructor(void *arg)
{
	struct aufilt_st *st = arg;

	if (st->enc)
		speex_resampler_destroy(st->enc);
	if (st->dec)
		speex_resampler_destroy(st->dec);

	mem_deref(st->af);
}


static int alloc(struct aufilt_st **stp, struct aufilt *af,
		 const struct aufilt_prm *encprm,
		 const struct aufilt_prm *decprm)
{
	struct aufilt_st *st;
	int err;

	st = mem_zalloc(sizeof(*st), resamp_destructor);
	if (!st)
		return ENOMEM;

	st->af = mem_ref(af);

	if (encprm->srate != encprm->srate_out) {
		st->enc = speex_resampler_init(encprm->ch, encprm->srate,
					       encprm->srate_out,
					       quality,	&err);
		if (!st->enc) {
			DEBUG_WARNING("speex_resampler_init: %s\n",
				      speex_resampler_strerror(err));
			goto error;
		}
	}

	if (decprm->srate != decprm->srate_out) {
		st->dec = speex_resampler_init(decprm->ch, decprm->srate,
					       decprm->srate_out,
					       quality, &err);
		if (!st->dec) {
			DEBUG_WARNING("speex_resampler_init: %s\n",
				      speex_resampler_strerror(err));
			goto error;
		}
	}

	*stp = st;
	return 0;

 error:
	mem_deref(st);
	return ENOMEM;
}


static int process(SpeexResamplerState *state, struct mbuf *mb)
{
	uint32_t in_len = (uint32_t)mbuf_get_left(mb)/2;
	int16_t out[4096];
	uint32_t out_len = (uint32_t)sizeof(out)/2;
	size_t pos = mb->pos;
	int err;

	err = speex_resampler_process_interleaved_int(state,
					  (int16_t *)mbuf_buf(mb), &in_len,
					  out, &out_len);
	if (err) {
		DEBUG_NOTICE("speex_resampler_process_int: err=%d\n", err);
		return EINVAL;
	}
	if (in_len < mbuf_get_left(mb)/2) {
		DEBUG_NOTICE("short read: %u of %u samples\n",
			     in_len, mbuf_get_left(mb)/2);
	}

	err = mbuf_write_mem(mb, (uint8_t *)out, 2*out_len);
	mb->pos = pos;
	mb->end = 2*out_len;

	return err;
}


static int dec(struct aufilt_st *st, struct mbuf *mb)
{
	return st->dec ? process(st->dec, mb) : 0;
}


static int enc(struct aufilt_st *st, struct mbuf *mb)
{
	return st->enc ? process(st->enc, mb) : 0;
}


static int module_init(void)
{
	return aufilt_register(&filt, "speex_resamp", alloc, enc, dec, NULL);
}


static int module_close(void)
{
	filt = mem_deref(filt);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(speex_resamp) = {
	"speex_resamp",
	"filter",
	module_init,
	module_close
};
