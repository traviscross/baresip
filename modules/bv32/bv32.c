/**
 * @file bv32.c  BroadVoice32 audio codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include <bv32/bv32.h>
#include <bv32/bitpack.h>


/*
 * BroadVoice32 Wideband Audio codec (RFC 4298)
 *
 * http://www.broadcom.com/support/broadvoice/downloads.php
 * http://files.freeswitch.org/downloads/libs/libbv32-0.1.tar.gz
 */


enum {
	NSAMP        = 80,
	CODED_OCTETS = 20
};

struct aucodec_st {
	struct aucodec *ac;            /**< Inheritance - base class */
	struct BV32_Encoder_State cs;
	struct BV32_Decoder_State ds;
	struct BV32_Bit_Stream bsc;
	struct BV32_Bit_Stream bsd;
};


static struct aucodec *bv32;


static void destructor(void *arg)
{
	struct aucodec_st *st = arg;

	Reset_BV32_Coder(&st->cs);
	Reset_BV32_Decoder(&st->ds);

	mem_deref(st->ac);
}


static int alloc(struct aucodec_st **stp, struct aucodec *ac,
		 struct aucodec_prm *encp, struct aucodec_prm *decp,
		 const char *fmtp)
{
	struct aucodec_st *st;

	(void)encp;
	(void)decp;
	(void)fmtp;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->ac = mem_ref(ac);

	Reset_BV32_Coder(&st->cs);
	Reset_BV32_Decoder(&st->ds);

	*stp = st;

	return 0;
}


static int encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	size_t nframe, pos;
	int err;

	pos = dst->pos;
	nframe = (mbuf_get_left(src)/2) / NSAMP;

	if (mbuf_get_space(dst) < nframe * CODED_OCTETS) {
		err = mbuf_resize(dst, dst->size + nframe * CODED_OCTETS);
		if (err)
			return err;
	}

	for (; nframe--;) {
		BV32_Encode(&st->bsc, &st->cs, (short *)mbuf_buf(src));
		BV32_BitPack((unsigned short *)mbuf_buf(dst), &st->bsc);

		mbuf_advance(src, NSAMP*2);
		dst->pos = dst->end = (dst->pos + CODED_OCTETS);
	}

	dst->pos = pos;

	return 0;
}


/* src=NULL means lost packet */
static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	size_t nframe, pos;
	int err;

	nframe = mbuf_get_left(src) / CODED_OCTETS;
	pos = dst->pos;

	if (mbuf_get_space(dst) < NSAMP*2*nframe) {
		err = mbuf_resize(dst, dst->size + NSAMP*2*nframe);
		if (err)
			return err;
	}

	if (!mbuf_get_left(src)) {
		BV32_PLC(&st->ds, (short *)mbuf_buf(dst));
		dst->end += NSAMP*2;
		return 0;
	}

	for (;nframe--;) {
		BV32_BitUnPack((unsigned short *)mbuf_buf(src), &st->bsd);
		BV32_Decode(&st->bsd, &st->ds, (short *)mbuf_buf(dst));

		mbuf_advance(src, CODED_OCTETS);
		dst->pos = dst->end = (dst->pos + NSAMP*2);
	}

	dst->pos = pos;

	return 0;
}


static int module_init(void)
{
	return aucodec_register(&bv32, 0, "BV32", 16000, 1, NULL,
				alloc, encode, decode, NULL);
}


static int module_close(void)
{
	bv32 = mem_deref(bv32);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(bv32) = {
	"bv32",
	"codec",
	module_init,
	module_close
};
