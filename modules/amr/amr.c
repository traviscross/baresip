/**
 * @file amr.c Adaptive Multi-Rate (AMR) audio codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#ifdef AMR_NB
#include <interf_enc.h>
#include <interf_dec.h>
#endif
#ifdef AMR_WB
#ifdef _TYPEDEF_H
#define typedef_h
#endif
#include <enc_if.h>
#include <dec_if.h>
#endif
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "amr"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/*
 * This module supports both AMR Narrowband (8000 Hz) and
 * AMR Wideband (16000 Hz) audio codecs.
 *
 * Reference:
 *
 *     http://tools.ietf.org/html/rfc4867
 *
 *     http://www.penguin.cz/~utx/amr
 */


#ifndef L_FRAME16k
#define L_FRAME16k 320
#endif

#ifndef NB_SERIAL_MAX
#define NB_SERIAL_MAX 61
#endif


struct aucodec_st {
	struct aucodec *ac;         /**< Inheritance - base class */
	void *enc;                  /**< Encoder state            */
	void *dec;                  /**< Decoder state            */
};


static struct aucodec *codecv[2];


static void destructor(void *arg)
{
	struct aucodec_st *st = arg;

	switch (aucodec_srate(st->ac)) {

#ifdef AMR_NB
	case 8000:
		Encoder_Interface_exit(st->enc);
		Decoder_Interface_exit(st->dec);
		break;
#endif

#ifdef AMR_WB
	case 16000:
		E_IF_exit(st->enc);
		D_IF_exit(st->dec);
		break;
#endif
	}

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

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->ac = mem_ref(ac);

	switch (aucodec_srate(ac)) {

#ifdef AMR_NB
	case 8000:
		st->enc = Encoder_Interface_init(0);
		st->dec = Decoder_Interface_init();
		break;
#endif

#ifdef AMR_WB
	case 16000:
		st->enc = E_IF_init();
		st->dec = D_IF_init();
		break;
#endif
	}

	if (!st->enc || !st->dec)
		err = ENOMEM;

	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


#ifdef AMR_WB
static int encode_wb(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	int len;

	if (!mbuf_get_left(src))
		return 0;

	if (mbuf_get_left(src) != 2*L_FRAME16k) {
		DEBUG_WARNING("encode: got %d bytes, expected %d\n",
			      mbuf_get_left(src), 2*L_FRAME16k);
		return EINVAL;
	}

	if (mbuf_get_space(dst) < NB_SERIAL_MAX) {
		int err = mbuf_resize(dst, dst->pos + NB_SERIAL_MAX);
		if (err)
			return err;
	}

	len = IF2E_IF_encode(st->enc, 8, (void *)mbuf_buf(src),
			     mbuf_buf(dst), 0);
	if (len <= 0) {
		DEBUG_WARNING("encode error: %d\n", len);
		return EPROTO;
	}

	src->pos = src->end;
	dst->end = dst->pos + len;

	return 0;
}


/* src=NULL means lost packet */
static int decode_wb(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	if (!src)
		return 0;

	/* Make sure there is enough space in the buffer */
	if (mbuf_get_space(dst) < 2*L_FRAME16k) {
		int err = mbuf_resize(dst, dst->pos + 2*L_FRAME16k);
		if (err)
			return err;
	}

	IF2D_IF_decode(st->dec, mbuf_buf(src), (void *)mbuf_buf(dst), 0);

	if (src)
		src->pos = src->end;

	dst->end += 2*L_FRAME16k;

	return 0;
}
#endif


#ifdef AMR_NB
static int encode_nb(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	enum Mode mode = MR475;
	int len;

	if (!mbuf_get_left(src))
		return 0;

	if (mbuf_get_left(src) != 320) {
		DEBUG_WARNING("encode: got %d bytes, expected %d\n",
			      mbuf_get_left(src), 320);
		return EINVAL;
	}

	if (mbuf_get_space(dst) < NB_SERIAL_MAX) {
		int err = mbuf_resize(dst, dst->pos + NB_SERIAL_MAX);
		if (err)
			return err;
	}

	len = Encoder_Interface_Encode(st->enc, mode, (void *)mbuf_buf(src),
				       mbuf_buf(dst), 0);
	if (len <= 0) {
		DEBUG_WARNING("encode error: %d\n", len);
		return EPROTO;
	}

	src->pos = src->end;
	dst->end = dst->pos + len;

	return 0;
}


/* src=NULL means lost packet */
static int decode_nb(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	if (!src)
		return 0;

	/* Make sure there is enough space in the buffer */
	if (mbuf_get_space(dst) < 2*L_FRAME16k) {
		int err = mbuf_resize(dst, dst->pos + 2*L_FRAME16k);
		if (err)
			return err;
	}

	Decoder_Interface_Decode(st->dec, mbuf_buf(src),
				 (void *)mbuf_buf(dst), 0);

	if (src)
		src->pos = src->end;

	dst->end += 2*L_FRAME16k;

	return 0;
}
#endif


static int module_init(void)
{
	int err = 0;

#ifdef AMR_WB
	err |= aucodec_register(&codecv[0], NULL, "AMR-WB", 16000, 1, NULL,
				alloc, encode_wb, decode_wb, NULL);
#endif
#ifdef AMR_NB
	err |= aucodec_register(&codecv[1], NULL, "AMR", 8000, 1, NULL,
				alloc, encode_nb, decode_nb, NULL);
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
EXPORT_SYM const struct mod_export DECL_EXPORTS(amr) = {
	"amr",
	"codec",
	module_init,
	module_close
};
