/**
 * @file gsm.c  GSM Audio Codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <gsm.h> /* please report if you have problems finding this file */
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "gsm"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


enum {
	SRATE = 8000,
	FRAME_SIZE = sizeof(gsm_signal[160])
};

struct aucodec_st {
	struct aucodec *ac;  /* inheritance */
	gsm enc, dec;
};


static struct aucodec *ac_gsm;


static void gsm_destructor(void *arg)
{
	struct aucodec_st *st = arg;

	gsm_destroy(st->enc);
	gsm_destroy(st->dec);

	mem_deref(st->ac);
}


static int alloc(struct aucodec_st **stp, struct aucodec *ac,
		 struct aucodec_prm *encp, struct aucodec_prm *decp,
		 const char *fmtp)
{
	struct aucodec_st *st;
	int err = 0;

	(void)encp;
	(void)decp;
	(void)fmtp;

	st = mem_zalloc(sizeof(*st), gsm_destructor);
	if (!st)
		return ENOMEM;

	st->ac = mem_ref(ac);

	st->enc = gsm_create();
	if (!st->enc) {
		DEBUG_WARNING("gsm_create() encoder failed\n");
		err = EPROTO;
		goto out;
	}

	st->dec = gsm_create();
	if (!st->dec) {
		DEBUG_WARNING("gsm_create() decoder failed\n");
		err = EPROTO;
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
	gsm_signal *sig;
	gsm_byte *byte;

	/* Make sure we have enough samples */
	if (mbuf_get_left(src) < FRAME_SIZE) {
		DEBUG_WARNING("encode: expected %d bytes, got %u\n",
			      FRAME_SIZE, mbuf_get_left(src));
		return ENOMEM;
	}

	/* Make sure there is enough space */
	if (mbuf_get_space(dst) < sizeof(gsm_frame)) {
		DEBUG_WARNING("encode: dest buffer is too small (%u bytes)\n",
			      mbuf_get_space(dst));
		return ENOMEM;
	}

	sig = (gsm_signal *)mbuf_buf(src);
	byte = mbuf_buf(dst);

	gsm_encode(st->enc, sig, byte);

	mbuf_advance(src, FRAME_SIZE);
	dst->end += sizeof(gsm_frame);

	return 0;
}


static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	gsm_signal *sig;
	gsm_byte *byte;
	int err;

	if (!mbuf_get_left(src))
		return 0;

	/* Make sure we have a complete GSM frame */
	if (mbuf_get_left(src) < sizeof(gsm_frame)) {
		DEBUG_INFO("decode: not enough input (%u bytes)\n",
			   mbuf_get_left(src));
		return EINVAL;
	}

	/* Make sure there is enough space in the buffer */
	if (mbuf_get_space(dst) < FRAME_SIZE) {
		DEBUG_WARNING("decode: buffer too small (size=%u, need %u)\n",
			      mbuf_get_space(dst), FRAME_SIZE);
		return ENOMEM;
	}

	byte = mbuf_buf(src);
	sig = (gsm_signal *)mbuf_buf(dst);

	err = gsm_decode(st->dec, byte, sig);
	if (err) {
		DEBUG_WARNING("decode: gsm_decode() failed (err=%d)\n", err);
		return ENOENT;
	}

	mbuf_advance(src, sizeof(gsm_frame));
	dst->end += FRAME_SIZE;

	return 0;
}


static int module_init(void)
{
	DEBUG_INFO("GSM v%u.%u.%u\n", GSM_MAJOR, GSM_MINOR, GSM_PATCHLEVEL);

	return aucodec_register(&ac_gsm, "3", "GSM", 8000, 1, NULL,
				alloc, encode, decode, NULL);
}


static int module_close(void)
{
	ac_gsm = mem_deref(ac_gsm);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(gsm) = {
	"gsm",
	"codec",
	module_init,
	module_close
};
