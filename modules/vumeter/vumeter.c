/**
 * @file vumeter.c  VU-meter
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <stdlib.h>
#include <re.h>
#include <baresip.h>


struct aufilt_st {
	struct aufilt *af; /* inheritance */
	struct tmr tmr;
	int16_t avg_rec;
	int16_t avg_play;
};


static struct aufilt *filt;


static void destructor(void *arg)
{
	struct aufilt_st *st = arg;

	tmr_cancel(&st->tmr);
	mem_deref(st->af);
}


static int16_t calc_avg_s16(const int16_t *sampv, size_t sampc)
{
	int32_t v = 0;
	size_t i;

	if (!sampv || !sampc)
		return 0;

	for (i=0; i<sampc; i++)
		v += abs(sampv[i]);

	return v/sampc;
}


static int audio_print_vu(struct re_printf *pf, int16_t *avg)
{
	char avg_buf[16];
	size_t i, res;

	res = min(2 * sizeof(avg_buf) * (*avg)/0x8000,
		  sizeof(avg_buf)-1);
	memset(avg_buf, 0, sizeof(avg_buf));
	for (i=0; i<res; i++) {
		avg_buf[i] = '=';
	}

	return re_hprintf(pf, "[%-16s]", avg_buf);
}


static void tmr_handler(void *arg)
{
	struct aufilt_st *st = arg;

	tmr_start(&st->tmr, 100, tmr_handler, st);

	/* move cursor to a fixed position */
	re_fprintf(stderr, "\x1b[66G");

	/* print VU-meter in Nice colors */
	re_fprintf(stderr, " \x1b[31m%H\x1b[;m \x1b[32m%H\x1b[;m\r",
		   audio_print_vu, &st->avg_rec,
		   audio_print_vu, &st->avg_play);
}


static int alloc(struct aufilt_st **stp, struct aufilt *af,
		 const struct aufilt_prm *encprm,
		 const struct aufilt_prm *decprm)
{
	struct aufilt_st *st;

	(void)encprm;
	(void)decprm;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->af = mem_ref(af);

	tmr_start(&st->tmr, 10, tmr_handler, st);

	*stp = st;

	return 0;
}


static int enc(struct aufilt_st *st, struct mbuf *mb)
{
	st->avg_rec = calc_avg_s16((int16_t *)mbuf_buf(mb),
				   mbuf_get_left(mb)/2);
	return 0;
}


static int dec(struct aufilt_st *st, struct mbuf *mb)
{
	st->avg_play = calc_avg_s16((int16_t *)mbuf_buf(mb),
				    mbuf_get_left(mb)/2);
	return 0;
}


static int module_init(void)
{
	return aufilt_register(&filt, "vumeter", alloc, enc, dec, NULL);
}


static int module_close(void)
{
	filt = mem_deref(filt);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(vumeter) = {
	"vumeter",
	"filter",
	module_init,
	module_close
};
