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


struct auenc_state {
	ISACStruct *inst;
};

struct audec_state {
	ISACStruct *inst;
};


static void encode_destructor(void *arg)
{
	struct auenc_state *st = arg;

	if (st->inst)
		WebRtcIsac_Free(st->inst);
}


static void decode_destructor(void *arg)
{
	struct audec_state *st = arg;

	if (st->inst)
		WebRtcIsac_Free(st->inst);
}


static int encode_update(struct auenc_state **aesp,
			 const struct aucodec *ac, const char *fmtp)
{
	struct auenc_state *st;
	int err = 0;
	(void)fmtp;

	if (!aesp || !ac)
		return EINVAL;

	if (*aesp)
		return 0;

	st = mem_alloc(sizeof(*st), encode_destructor);
	if (!st)
		return ENOMEM;

	if (WebRtcIsac_Create(&st->inst) < 0) {
		err = ENOMEM;
		goto out;
	}

	WebRtcIsac_EncoderInit(st->inst, 0);

	if (ac->srate == 32000)
		WebRtcIsac_SetEncSampRate(st->inst, kIsacSuperWideband);

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
	(void)fmtp;

	if (!adsp || !ac)
		return EINVAL;

	if (*adsp)
		return 0;

	st = mem_alloc(sizeof(*st), decode_destructor);
	if (!st)
		return ENOMEM;

	if (WebRtcIsac_Create(&st->inst) < 0) {
		err = ENOMEM;
		goto out;
	}

	WebRtcIsac_DecoderInit(st->inst);

	if (ac->srate == 32000)
		WebRtcIsac_SetDecSampRate(st->inst, kIsacSuperWideband);

 out:
	if (err)
		mem_deref(st);
	else
		*adsp = st;

	return err;
}


static int encode(struct auenc_state *st, uint8_t *buf, size_t *len,
		  const int16_t *sampv, size_t sampc)
{
	WebRtc_Word16 len1, len2;

	/* 10 ms audio blocks */
	len1 = WebRtcIsac_Encode(st->inst, sampv,           (void *)buf);
	len2 = WebRtcIsac_Encode(st->inst, &sampv[sampc/2], (void *)buf);

	*len = len1 ? len1 : len2;

	return 0;
}


static int decode(struct audec_state *st, int16_t *sampv,
		  size_t *sampc, const uint8_t *buf, size_t len)
{
	WebRtc_Word16 speechType;
	int n;

	n = WebRtcIsac_Decode(st->inst, (void *)buf, len,
			      (void *)sampv, &speechType);
	if (n < 0)
		return EPROTO;

	*sampc = n;

	return 0;
}


static int plc(struct audec_state *st, int16_t *sampv, size_t *sampc)
{
	int n;

	n = WebRtcIsac_DecodePlc(st->inst, (void *)sampv, 1);
	if (n < 0)
		return EPROTO;

	*sampc = n;

	return 0;
}


static struct aucodec isac[2] = {
	{
	LE_INIT, 0, "iSAC", 32000, 1, NULL,
	encode_update, encode, decode_update, decode, plc, NULL, NULL
	},
	{
	LE_INIT, 0, "iSAC", 16000, 1, NULL,
	encode_update, encode, decode_update, decode, plc, NULL, NULL
	}
};


static int module_init(void)
{
	aucodec_register(&isac[0]);
	aucodec_register(&isac[1]);

	return 0;
}


static int module_close(void)
{
	int i = ARRAY_SIZE(isac);

	while (i--)
		aucodec_unregister(&isac[i]);

	return 0;
}


/** Module exports */
EXPORT_SYM const struct mod_export DECL_EXPORTS(isac) = {
	"isac",
	"codec",
	module_init,
	module_close
};
