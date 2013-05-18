/**
 * @file vumeter.c  VU-meter
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <stdlib.h>
#include <re.h>
#include <baresip.h>


struct vumeter {
	struct aufilt_st af;  /* inheritance */
	struct tmr tmr;
	int16_t avg_rec;
	int16_t avg_play;
};


static void destructor(void *arg)
{
	struct vumeter *st = arg;

	list_unlink(&st->af.le);
	tmr_cancel(&st->tmr);
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
	struct vumeter *st = arg;

	tmr_start(&st->tmr, 100, tmr_handler, st);

	/* move cursor to a fixed position */
	re_fprintf(stderr, "\x1b[66G");

	/* print VU-meter in Nice colors */
	re_fprintf(stderr, " \x1b[31m%H\x1b[;m \x1b[32m%H\x1b[;m\r",
		   audio_print_vu, &st->avg_rec,
		   audio_print_vu, &st->avg_play);
}


static int update(struct aufilt_st **stp, struct aufilt *af,
		  const struct aufilt_prm *encprm,
		  const struct aufilt_prm *decprm)
{
	struct vumeter *st;

	if (!stp || !af)
		return EINVAL;

	if (*stp)
		return 0;

	(void)encprm;
	(void)decprm;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	tmr_start(&st->tmr, 10, tmr_handler, st);

	*stp = (struct aufilt_st *)st;

	return 0;
}


static int encode(struct aufilt_st *st, int16_t *sampv, size_t *sampc)
{
	struct vumeter *vu = (struct vumeter *)st;
	vu->avg_rec = calc_avg_s16(sampv, *sampc);
	return 0;
}


static int decode(struct aufilt_st *st, int16_t *sampv, size_t *sampc)
{
	struct vumeter *vu = (struct vumeter *)st;
	vu->avg_play = calc_avg_s16(sampv, *sampc);
	return 0;
}


static struct aufilt vumeter = {
	LE_INIT, "vumeter", update, encode, decode
};


static int module_init(void)
{
	aufilt_register(&vumeter);
	return 0;
}


static int module_close(void)
{
	aufilt_unregister(&vumeter);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(vumeter) = {
	"vumeter",
	"filter",
	module_init,
	module_close
};
