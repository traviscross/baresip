/**
 * @file isac.c iSAC audio codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "isac.h"


/*
 * draft-ietf-avt-rtp-isac-01
 */


struct aucodec_st {
	struct aucodec *ac;  /* inheritance */
	ISACStruct *inst;
};


static struct aucodec *isac[2];


static void destructor(void *arg)
{
	struct aucodec_st *st = arg;

	if (st->inst)
		WebRtcIsac_Free(st->inst);

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

	st = mem_alloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->ac = mem_ref(ac);

	if (WebRtcIsac_Create(&st->inst) < 0) {
		err = ENOMEM;
		goto out;
	}

	WebRtcIsac_EncoderInit(st->inst, 0);
	WebRtcIsac_DecoderInit(st->inst);

	if (aucodec_srate(ac) == 32000) {
		WebRtcIsac_SetDecSampRate(st->inst, kIsacSuperWideband);
		WebRtcIsac_SetEncSampRate(st->inst, kIsacSuperWideband);
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
	WebRtc_Word16 encoded[2048];
	WebRtc_Word16 len1, len2, len;
	WebRtc_Word16 *in;
	size_t n;

	in = (void *)mbuf_buf(src);
	n = mbuf_get_left(src);

	/* 10 ms audio blocks */
	len1 = WebRtcIsac_Encode(st->inst, in, encoded);
	len2 = WebRtcIsac_Encode(st->inst, &in[n/4], encoded);

	src->pos = src->end;

	len = len1 ? len1 : len2;
	if (len > 0)
		return mbuf_write_mem(dst, (void *)encoded, len);

	return 0;
}


/* src=NULL means lost packet */
static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	WebRtc_Word16 decoded[2048];
	WebRtc_Word16 speechType;
	int ret;

	if (!mbuf_get_left(src)) {
		ret = WebRtcIsac_DecodePlc(st->inst, decoded, 1);
	}
	else {
		ret = WebRtcIsac_Decode(st->inst, (void *)mbuf_buf(src),
					mbuf_get_left(src),
					decoded, &speechType);
	}
	if (ret < 0)
		return EPROTO;

	if (src)
		src->pos = src->end;

	return mbuf_write_mem(dst, (void *)decoded, ret * 2);
}


static int module_init(void)
{
	int err = 0;

	err |= aucodec_register(&isac[0], NULL, "iSAC", 32000, 1,
				NULL, alloc, encode, decode, NULL);
	err |= aucodec_register(&isac[1], NULL, "iSAC", 16000, 1,
				NULL, alloc, encode, decode, NULL);

	return err;
}


static int module_close(void)
{
	int i = ARRAY_SIZE(isac);

	while (i--)
		isac[i] = mem_deref(isac[i]);

	return 0;
}


/** Module exports */
EXPORT_SYM const struct mod_export DECL_EXPORTS(isac) = {
	"isac",
	"codec",
	module_init,
	module_close
};
