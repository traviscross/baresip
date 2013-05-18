/**
 * @file plc.c  PLC -- Packet Loss Concealment
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <spandsp.h>
#include <re.h>
#include <baresip.h>


struct plc_st {
	struct aufilt_st af; /* base class */
	plc_state_t plc;
	size_t nsamp;
};


static void destructor(void *arg)
{
	struct plc_st *st = arg;

	list_unlink(&st->af.le);
}


static int update(struct aufilt_st **stp, struct aufilt *af,
		  const struct aufilt_prm *encprm,
		  const struct aufilt_prm *decprm)
{
	struct plc_st *st;
	int err = 0;

	(void)af;
	(void)encprm;

	if (!stp || !decprm)
		return EINVAL;

	if (*stp)
		return 0;

	/* XXX: add support for stereo PLC */
	if (decprm->ch != 1) {
		re_fprintf(stderr, "plc: only mono supported (ch=%u)\n",
			   decprm->ch);
		return ENOSYS;
	}

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	if (!plc_init(&st->plc)) {
		err = ENOMEM;
		goto out;
	}

	st->nsamp = decprm->frame_size;

 out:
	if (err)
		mem_deref(st);
	else
		*stp = (struct aufilt_st *)st;

	return err;
}


/**
 * PLC is only valid for Decoding (RX)
 *
 * NOTE: sampc_in==0 , means Packet loss
 */
static int decode(struct aufilt_st *st, int16_t *sampv, size_t *sampc)
{
	struct plc_st *plc = (struct plc_st *)st;

	if (*sampc)
		plc_rx(&plc->plc, sampv, (int)*sampc);
	else
		*sampc = plc_fillin(&plc->plc, sampv, (int)plc->nsamp);

	return 0;
}


static struct aufilt plc = {
	LE_INIT, "plc", update, NULL, decode
};


static int module_init(void)
{
	aufilt_register(&plc);
	return 0;
}


static int module_close(void)
{
	aufilt_unregister(&plc);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(plc) = {
	"plc",
	"filter",
	module_init,
	module_close
};
