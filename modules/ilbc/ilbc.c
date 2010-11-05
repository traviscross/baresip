/**
 * @file ilbc.c  Internet Low Bit Rate Codec (iLBC) audio codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include <iLBC_define.h>
#include <iLBC_decode.h>
#include <iLBC_encode.h>


#define DEBUG_MODULE "ilbc"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/*
 * This module implements the iLBC audio codec as defined in:
 *
 *     RFC 3951  Internet Low Bit Rate Codec (iLBC)
 *     RFC 3952  RTP Payload Format for iLBC Speech
 *
 * The iLBC source code is not included here, but can be downloaded from
 * http://ilbcfreeware.org/
 *
 * You can also use the source distributed by the Freeswitch project,
 * see www.freeswitch.org, and then freeswitch/libs/codec/ilbc.
 * Or you can look in the asterisk source code ...
 *
 *   mode=20  15.20 kbit/s  160samp  38bytes
 *   mode=30  13.33 kbit/s  240samp  50bytes
 *
 * TODO:
 *
 * - how to transfer mode/ptime to application?
 */

enum {
	DEFAULT_MODE = 20, /* 20ms or 30ms */
	USE_ENHANCER = 1
};

struct aucodec_st {
	struct aucodec *ac; /* inheritance */
	iLBC_Enc_Inst_t enc;
	iLBC_Dec_Inst_t dec;
	int mode;
	uint32_t enc_bytes;
	uint32_t dec_nsamp;
};


static struct aucodec *ilbc;
static char ilbc_fmtp[32];


static void set_mode(struct aucodec_st *st, int mode)
{
	if (st->mode == mode)
		return;

	(void)re_printf("set iLBC mode %dms\n", mode);

	st->mode = mode;

	switch (mode) {

	case 20:
		st->enc_bytes = NO_OF_BYTES_20MS;
		st->dec_nsamp = BLOCKL_20MS;
		break;

	case 30:
		st->enc_bytes = NO_OF_BYTES_30MS;
		st->dec_nsamp = BLOCKL_30MS;
		break;

	default:
		DEBUG_WARNING("unknown mode %d\n", mode);
		return;
	}

	st->enc_bytes = initEncode(&st->enc, mode);
	st->dec_nsamp = initDecode(&st->dec, mode, USE_ENHANCER);
}


static void fmtp_decode(struct aucodec_st *st, const struct pl *pl)
{
	struct pl mode;

	if (!pl)
		return;

	if (re_regex(pl->p, pl->l, "mode=[0-9]+", &mode))
		return;

	set_mode(st, pl_u32(&mode));
}


static void destructor(void *data)
{
	struct aucodec_st *st = data;
	mem_deref(st->ac);
}


static int alloc(struct aucodec_st **stp, struct aucodec *ac,
		 struct aucodec_prm *encp, struct aucodec_prm *decp,
		 const struct pl *sdp_fmtp)
{
	struct aucodec_st *st;

	(void)encp;
	(void)decp;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->ac = mem_ref(ac);
	set_mode(st, DEFAULT_MODE);
	fmtp_decode(st, sdp_fmtp);

	*stp = st;

	return 0;
}


static int encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	const size_t nsamp = mbuf_get_left(src)/2;
	float buf[nsamp];
	uint32_t i;

	/* Make sure there is enough space */
	if (mbuf_get_space(dst) < st->enc_bytes) {
		DEBUG_WARNING("encode: dst buffer is too small (%u bytes)\n",
			      mbuf_get_space(dst));
		return ENOMEM;
	}

	/* Convert from 16-bit samples to float */
	for (i=0; i<nsamp; i++) {
		const int16_t v = mbuf_read_u16(src);
		buf[i] = (float)v;
	}

	iLBC_encode(mbuf_buf(dst),  /* (o) encoded data bits iLBC */
		    buf,            /* (o) speech vector to encode */
		    &st->enc);      /* (i/o) the general encoder state */

	mbuf_set_end(dst, dst->end + st->enc_bytes);

	return 0;
}


static int do_dec(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	const uint32_t nsamp = st->dec_nsamp;
	const uint32_t n = 2*nsamp;
	float buf[st->dec_nsamp];
	const int mode = src ? 1 : 0;
	uint32_t i;
	int err;

	/* Make sure there is enough space in the buffer */
	if (mbuf_get_space(dst) < n) {
		DEBUG_NOTICE("decode: buffer too small (size=%u, need %u)\n",
			     mbuf_get_space(dst), n);
		err = mbuf_resize(dst, dst->pos + n);
		if (err)
			return err;
	}

	iLBC_decode(buf,            /* (o) decoded signal block */
		    mbuf_buf(src),  /* (i) encoded signal bits */
		    &st->dec,       /* (i/o) the decoder state structure */
		    mode);          /* (i) 0: bad packet, PLC, 1: normal */

	mbuf_advance(src, st->enc_bytes);

	/* Convert from float to 16-bit samples */
	for (i=0; i<nsamp; i++) {
		const int16_t s = buf[i];
		mbuf_write_u16(dst, s);
	}

	return 0;
}


/* src=NULL means lost packet */
static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	/* Try to detect mode */
	if (st->enc_bytes != mbuf_get_left(src)) {

		switch (mbuf_get_left(src)) {

		case NO_OF_BYTES_20MS:
			set_mode(st, 20);
			break;

		case NO_OF_BYTES_30MS:
			set_mode(st, 30);
			break;

		default:
			DEBUG_WARNING("decode: expect %u, got %u\n",
				      st->enc_bytes, mbuf_get_left(src));
			return EINVAL;
		}
	}

	return do_dec(st, dst, src);
}


static int module_init(void)
{
	(void)re_snprintf(ilbc_fmtp, sizeof(ilbc_fmtp),
			  "mode=%d", DEFAULT_MODE);

	return aucodec_register(&ilbc, NULL, "iLBC", 8000, 1, ilbc_fmtp,
			       alloc, encode, decode);
}


static int module_close(void)
{
	ilbc = mem_deref(ilbc);
	return 0;
}


/** Module exports */
EXPORT_SYM const struct mod_export DECL_EXPORTS(ilbc) = {
	"ilbc",
	"codec",
	module_init,
	module_close
};
