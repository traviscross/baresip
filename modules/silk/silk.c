/**
 * @file silk.c  Skype SILK audio codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include <silk/SKP_Silk_SDK_API.h>


/*
 * References:  https://developer.skype.com/silk
 */


enum {
	MAX_BYTES_PER_FRAME = 250,
	MAX_FRAME_SIZE      = 2*480,
};


struct aucodec_st {
	struct aucodec *ac;  /* inheritance */
	void *enc;
	SKP_SILK_SDK_EncControlStruct encControl;
	void *dec;
	SKP_SILK_SDK_DecControlStruct decControl;
};

static struct aucodec *silk[4];


static void destructor(void *arg)
{
	struct aucodec_st *st = arg;

	mem_deref(st->dec);
	mem_deref(st->enc);

	mem_deref(st->ac);
}


static int alloc(struct aucodec_st **stp, struct aucodec *ac,
		 struct aucodec_prm *encp, struct aucodec_prm *decp,
		 const char *fmtp)
{
	struct aucodec_st *st;
	int ret, err = 0;
	int32_t enc_size, dec_size;

	(void)decp;
	(void)fmtp;

	ret  = SKP_Silk_SDK_Get_Encoder_Size(&enc_size);
	ret |= SKP_Silk_SDK_Get_Decoder_Size(&dec_size);
	if (ret || enc_size <= 0 || dec_size <= 0)
		return EINVAL;

	st = mem_alloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->ac = mem_ref(ac);

	st->enc = mem_alloc(enc_size, NULL);
	st->dec = mem_alloc(dec_size, NULL);
	if (!st->enc || !st->dec) {
		err = ENOMEM;
		goto out;
	}

	ret = SKP_Silk_SDK_InitEncoder(st->enc, &st->encControl);
	if (ret) {
		err = EPROTO;
		goto out;
	}

	ret = SKP_Silk_SDK_InitDecoder(st->dec);
	if (ret) {
		err = EPROTO;
		goto out;
	}

	st->encControl.API_sampleRate = aucodec_srate(ac);
	st->encControl.maxInternalSampleRate = aucodec_srate(ac);
	st->encControl.packetSize = encp->ptime * aucodec_srate(ac) / 1000;
	st->encControl.bitRate = 64000;
	st->encControl.complexity = 2;
	st->encControl.useInBandFEC = 0;
	st->encControl.useDTX = 0;

	st->decControl.API_sampleRate = aucodec_srate(ac);

	re_printf("SILK: %dHz, psize=%d, bitrate=%d, complex=%d,"
		  " fec=%d, dtx=%d\n",
		  st->encControl.API_sampleRate,
		  st->encControl.packetSize,
		  st->encControl.bitRate,
		  st->encControl.complexity,
		  st->encControl.useInBandFEC,
		  st->encControl.useDTX);


 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	int ret;
	int16_t nBytesOut;

	/* Make sure there is enough space */
	if (mbuf_get_space(dst) < MAX_BYTES_PER_FRAME) {
		int err = mbuf_resize(dst, dst->pos + MAX_BYTES_PER_FRAME);
		if (err)
			return err;
	}

	nBytesOut = mbuf_get_space(dst);
	ret = SKP_Silk_SDK_Encode(st->enc,
				  &st->encControl,
				  (int16_t *)mbuf_buf(src),
				  (int)mbuf_get_left(src)/2,
				  mbuf_buf(dst),
				  &nBytesOut);
	if (ret) {
		re_printf("SKP_Silk_SDK_Encode: ret=%d\n", ret);
	}

	src->pos = src->end;
	mbuf_set_end(dst, dst->end + nBytesOut);

	return 0;
}


/* src=NULL means lost packet */
static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	int16_t nsamp;
	int ret;

	/* Make sure there is enough space in the buffer */
	if (mbuf_get_space(dst) < MAX_FRAME_SIZE) {
		int err = mbuf_resize(dst, MAX_FRAME_SIZE);
		if (err)
			return err;
	}

	nsamp = mbuf_get_space(dst) / 2;

	ret = SKP_Silk_SDK_Decode(st->dec,
				  &st->decControl,
				  mbuf_get_left(src) == 0,
				  mbuf_buf(src),
				  (int)mbuf_get_left(src),
				  (int16_t *)mbuf_buf(dst),
				  &nsamp);
	if (ret) {
		re_printf("SKP_Silk_SDK_Decode: ret=%d\n", ret);
	}

	if (src)
		mbuf_skip_to_end(src);
	if (nsamp > 0)
		mbuf_set_end(dst, dst->end + nsamp*2);

	return 0;
}


static int module_init(void)
{
	int err = 0;

	re_printf("SILK %s\n", SKP_Silk_SDK_get_version());

	err |= aucodec_register(&silk[0], NULL, "SILK", 24000, 1,
				NULL, alloc, encode, decode, NULL);
	err |= aucodec_register(&silk[1], NULL, "SILK", 16000, 1,
				NULL, alloc, encode, decode, NULL);
	err |= aucodec_register(&silk[2], NULL, "SILK", 12000, 1,
				NULL, alloc, encode, decode, NULL);
	err |= aucodec_register(&silk[3], NULL, "SILK", 8000, 1,
				NULL, alloc, encode, decode, NULL);

	return err;
}


static int module_close(void)
{
	int i = ARRAY_SIZE(silk);

	while (i--)
		silk[i] = mem_deref(silk[i]);

	return 0;
}


/** Module exports */
EXPORT_SYM const struct mod_export DECL_EXPORTS(silk) = {
	"silk",
	"codec",
	module_init,
	module_close
};
