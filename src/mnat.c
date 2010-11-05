/**
 * @file mnat.c Media NAT
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


static struct list mnatl = LIST_INIT;


static void destructor(void *arg)
{
	struct mnat *mnat = arg;

	list_unlink(&mnat->le);
}


int mnat_register(struct mnat **mnatp, const char *id, const char *ftag,
		  mnat_sess_h *sessh, mnat_media_h *mediah,
		  mnat_update_h *updateh)
{
	struct mnat *mnat;

	if (!mnatp || !id || !sessh || !mediah)
		return EINVAL;

	mnat = mem_zalloc(sizeof(*mnat), destructor);
	if (!mnat)
		return ENOMEM;

	list_append(&mnatl, &mnat->le, mnat);

	mnat->id      = id;
	mnat->ftag    = ftag;
	mnat->sessh   = sessh;
	mnat->mediah  = mediah;
	mnat->updateh = updateh;

	(void)re_printf("mnat: %s\n", id);

	*mnatp = mnat;

	return 0;
}


const struct mnat *mnat_find(const char *id)
{
	struct mnat *mnat;
	struct le *le;

	for (le=mnatl.head; le; le=le->next) {

		mnat = le->data;

		if (str_casecmp(mnat->id, id))
			continue;

		return mnat;
	}

	return NULL;
}
