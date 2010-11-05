/**
 * @file g7221.c  G.722.1 audio codec using Polycom implementation
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "g722_1.h"


/*
 * RFC 5577 -- RTP Payload Format for ITU-T Recommendation G.722.1
 *
 * Source: http://fisheye.freeswitch.org/browse/FreeSWITCH/libs/libg722_1
 */

enum {DEFAULT_BITRATE = 32000};

struct aucodec_st {
	struct aucodec *ac;  /* inheritance */
	g722_1_encode_state_t enc;
	g722_1_decode_state_t dec;
};

static struct aucodec *g7221[2];


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
	struct pl br;
	int bitrate = DEFAULT_BITRATE;
	int err = 0;

	(void)encp;
	(void)decp;

	if (fmt_param_get(sdp_fmtp, "bitrate", &br)) {
		bitrate = pl_u32(&br);
	}

	st = mem_alloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->ac = mem_ref(ac);

	if (!g722_1_encode_init(&st->enc, bitrate, aucodec_srate(ac))) {
		err = EPROTO;
		goto out;
	}

	if (!g722_1_decode_init(&st->dec, bitrate, aucodec_srate(ac))) {
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
	size_t n;
	int len;

	/* Make sure there is enough space */
	if (mbuf_get_space(dst) < MAX_FRAME_SIZE) {
		int err = mbuf_resize(dst, 2 * (dst->pos + MAX_FRAME_SIZE));
		if (err)
			return err;
	}

	n = mbuf_get_left(src);
	len = g722_1_encode(&st->enc, mbuf_buf(dst),
			    (int16_t *)mbuf_buf(src), (int)n/2);
	if (len <= 0) {
		re_printf("g722_encode: len=%d\n", len);
	}
	else if (len > (int)mbuf_get_space(dst)) {
		return EBADMSG;
	}

	mbuf_advance(src, n);
	mbuf_set_end(dst, dst->end + len);

	return 0;
}


/* src=NULL means lost packet */
static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	const size_t n = sizeof(uint16_t) * st->dec.frame_size;
	int nsamp;

	/* Make sure there is enough space in the buffer */
	if (mbuf_get_space(dst) < n) {
		int err = mbuf_resize(dst, 2 * (dst->pos + n));
		if (err)
			return err;
	}

	if (mbuf_get_left(src)) {
		nsamp = g722_1_decode(&st->dec, (int16_t *)mbuf_buf(dst),
				      mbuf_buf(src), (int)mbuf_get_left(src));
	}
	else {
		nsamp = g722_1_fillin(&st->dec, (int16_t *)mbuf_buf(dst),
				      NULL, 0);
	}
	if (nsamp <= 0) {
		re_printf("g722_decode: nsamp=%d\n", nsamp);
	}

	mbuf_skip_to_end(src);
	if (nsamp > 0)
		mbuf_set_end(dst, dst->end + nsamp*2);

	return 0;
}


static int module_init(void)
{
	int err = 0;

	err |= aucodec_register(&g7221[0], NULL, "G7221", 32000, 1,
				"bitrate=48000", alloc, encode, decode);
	err |= aucodec_register(&g7221[1], NULL, "G7221", 16000, 1,
				"bitrate=32000", alloc, encode, decode);

	return err;
}


static int module_close(void)
{
	g7221[1] = mem_deref(g7221[1]);
	g7221[0] = mem_deref(g7221[0]);
	return 0;
}


/** Module exports */
EXPORT_SYM const struct mod_export DECL_EXPORTS(g7221) = {
	"g7221",
	"codec",
	module_init,
	module_close
};
