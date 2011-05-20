/**
 * @file plc.c  PLC -- Packet Loss Concealment
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <spandsp.h>
#include <re.h>
#include <baresip.h>


struct aufilt_st {
	struct aufilt *af; /* base class */
	plc_state_t plc;
	size_t psize;
};


static struct aufilt *filt;


static void destructor(void *arg)
{
	struct aufilt_st *st = arg;

	mem_deref(st->af);
}


static int alloc(struct aufilt_st **stp, struct aufilt *af,
		 const struct aufilt_prm *encprm,
		 const struct aufilt_prm *decprm)
{
	struct aufilt_st *st;
	int err = 0;

	(void)encprm;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->af = mem_ref(af);

	if (!plc_init(&st->plc)) {
		err = ENOMEM;
		goto out;
	}

	if (decprm)
		st->psize = 2 * decprm->frame_size * decprm->ch;
	else
		st->psize = 320;

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


/* PLC is only valid for Decoding (RX) */
static int dec(struct aufilt_st *st, struct mbuf *mb)
{
	int nsamp = (int)mbuf_get_left(mb) / 2;

	if (nsamp) {
		nsamp = plc_rx(&st->plc, (int16_t *)mbuf_buf(mb), nsamp);
		if (nsamp >= 0)
			mb->end = mb->pos + (2*nsamp);
	}
	else {
		nsamp = (int)st->psize / 2;

		re_printf("plc: concealing %u bytes\n", st->psize);

		if (mbuf_get_space(mb) < st->psize) {

			int err = mbuf_resize(mb, st->psize);
			if (err)
				return err;
		}

		nsamp = plc_fillin(&st->plc, (int16_t *)mbuf_buf(mb), nsamp);

		mb->end = mb->pos + 2 * nsamp;
	}

	return 0;
}


static int module_init(void)
{
	return aufilt_register(&filt, "plc", alloc, NULL, dec, NULL);
}


static int module_close(void)
{
	filt = mem_deref(filt);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(plc) = {
	"plc",
	"filter",
	module_init,
	module_close
};
