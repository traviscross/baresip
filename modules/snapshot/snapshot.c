/**
 * @file snapshot.c  Snapshot Video-Filter
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "png_vf.h"


struct snapshot {
	struct vidfilt_st vf;    /**< Inheritance      */
};


static bool flag_enc, flag_dec;


static void destructor(void *arg)
{
	struct snapshot *st = arg;

	list_unlink(&st->vf.le);
}


static int update(struct vidfilt_st **stp, struct vidfilt *vf)
{
	struct snapshot *st;
	int err = 0;

	if (!stp || !vf)
		return EINVAL;

	if (*stp)
		return 0;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	if (err)
		mem_deref(st);
	else
		*stp = (struct vidfilt_st *)st;

	return err;
}


static int encode(struct vidfilt_st *st, struct vidframe *frame)
{
	(void)st;

	if (!frame)
		return 0;

	if (flag_enc) {
		flag_enc = false;
		png_save_vidframe(frame, "snapshot-send");
	}

	return 0;
}


static int decode(struct vidfilt_st *st, struct vidframe *frame)
{
	(void)st;

	if (!frame)
		return 0;

	if (flag_dec) {
		flag_dec = false;
		png_save_vidframe(frame, "snapshot-recv");
	}

	return 0;
}


static int do_snapshot(struct re_printf *pf, void *arg)
{
	(void)pf;
	(void)arg;

	/* NOTE: not re-entrant */
	flag_enc = flag_dec = true;

	return 0;
}


static struct vidfilt snapshot = {
	LE_INIT, "snapshot", update, encode, decode,
};


static const struct cmd cmdv[] = {
	{'o', 0, "Take video snapshot", do_snapshot },
};


static int module_init(void)
{
	vidfilt_register(&snapshot);
	return cmd_register(cmdv, ARRAY_SIZE(cmdv));
}


static int module_close(void)
{
	vidfilt_unregister(&snapshot);
	cmd_unregister(cmdv);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(snapshot) = {
	"snapshot",
	"vidfilt",
	module_init,
	module_close
};
