/**
 * @file l16.c  16-bit linear codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>


enum {NR_CODECS = 8};


struct aucodec_st {
	struct aucodec *ac;  /* inheritance */
};


static struct aucodec *l16v[NR_CODECS];


static void destructor(void *arg)
{
	struct aucodec_st *st = arg;

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

	*stp = st;

	return 0;
}


static int encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	int err = 0;

	(void)st;

	while (mbuf_get_left(src) >= 2)
		err |= mbuf_write_u16(dst, htons(mbuf_read_u16(src)));

	return err;
}


static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	int err = 0;

	(void)st;

	while (mbuf_get_left(src))
		err |= mbuf_write_u16(dst, ntohs(mbuf_read_u16(src)));

	return err;
}


/* See RFC 3551 */
static const struct {
	const char *pt;
	uint32_t srate;
	uint8_t ch;
} codecv[NR_CODECS] = {
	{"10", 44100, 2},
	{NULL, 32000, 2},
	{NULL, 16000, 2},
	{NULL,  8000, 2},
	{"11", 44100, 1},
	{NULL, 32000, 1},
	{NULL, 16000, 1},
	{NULL,  8000, 1}
};


static int module_init(void)
{
	size_t i;
	int err = 0;

	for (i=0; i<NR_CODECS; i++) {
		err |= aucodec_register(&l16v[i], codecv[i].pt, "L16",
					codecv[i].srate, codecv[i].ch, NULL,
					alloc, encode, decode, NULL);
	}

	return err;
}


static int module_close(void)
{
	size_t i;

	for (i=0; i<NR_CODECS; i++) {
		l16v[i] = mem_deref(l16v[i]);
	}

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(l16) = {
	"l16",
	"codec",
	module_init,
	module_close
};
