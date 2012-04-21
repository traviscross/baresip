/**
 * @file g711.c  ITU G.711 codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <rem.h>
#include <baresip.h>


typedef uint8_t (enc_h)(int16_t samp);
typedef int16_t (dec_h)(uint8_t octet);

struct aucodec_st {
	struct aucodec *ac;  /* inheritance */
	enc_h *enc;
	dec_h *dec;
};


static struct aucodec *acv[4];


static void destructor(void *arg)
{
	struct aucodec_st *as = arg;

	mem_deref(as->ac);
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

	if (0 == str_casecmp(aucodec_name(ac), "PCMA")) {
		st->enc = g711_pcm2alaw;
		st->dec = g711_alaw2pcm;
	}
	else if (0 == str_casecmp(aucodec_name(ac), "PCMU")) {
		st->enc = g711_pcm2ulaw;
		st->dec = g711_ulaw2pcm;
	}
	else {
		err = EINVAL;
	}

	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	size_t nsamp;
	uint8_t *d;

	nsamp = mbuf_get_left(src) / 2;

	if (mbuf_get_space(dst) < nsamp) {
		int err = mbuf_resize(dst, dst->size + nsamp);
		if (err)
			return err;
	}

	d = dst->buf + dst->pos;
	dst->end = dst->pos = (dst->pos + nsamp);

	while (nsamp--)
		*d++ = st->enc(mbuf_read_u16(src));

	return 0;
}


static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	size_t nsamp;
	uint8_t *s;

	nsamp = mbuf_get_left(src);
	if (!nsamp)
		return 0;

	if (mbuf_get_space(dst) < 2*nsamp) {
		int err = mbuf_resize(dst, dst->size + 2*nsamp);
		if (err)
			return err;
	}

	s = src->buf + src->pos;
	src->pos = src->end;

	while (nsamp--)
		(void)mbuf_write_u16(dst, st->dec(*s++));

	return 0;
}


static int module_init(void)
{
	int err = 0;

#ifdef G711_EXPERIMENTAL
	/* Non-standard codecs - enable at own risk */
	err |= aucodec_register(&acv[0], NULL, "PCMA", 16000, 1, NULL,
				alloc, encode, decode, NULL);
	err |= aucodec_register(&acv[1], NULL, "PCMU", 16000, 1, NULL,
				alloc, encode, decode, NULL);
#endif
	err |= aucodec_register(&acv[2], "8", "PCMA", 8000, 1, NULL,
				alloc, encode, decode, NULL);
	err |= aucodec_register(&acv[3], "0", "PCMU", 8000, 1, NULL,
				alloc, encode, decode, NULL);

	return err;
}


static int module_close(void)
{
	size_t i;

	for (i=0; i<ARRAY_SIZE(acv); i++)
		acv[i] = mem_deref(acv[i]);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(g711) = {
	"g711",
	"codec",
	module_init,
	module_close,
};
