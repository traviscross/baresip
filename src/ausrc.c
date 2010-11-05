/**
 * @file ausrc.c  Audio Source
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


static struct list ausrcl = LIST_INIT;


static void destructor(void *data)
{
	struct ausrc *as = data;

	list_unlink(&as->le);
}


int ausrc_register(struct ausrc **asp, const char *name, ausrc_alloc_h *alloch)
{
	struct ausrc *as;

	if (!asp)
		return EINVAL;

	as = mem_zalloc(sizeof(*as), destructor);
	if (!as)
		return ENOMEM;

	list_append(&ausrcl, &as->le, as);

	as->name   = name;
	as->alloch = alloch;

	(void)re_printf("ausrc: %s\n", name);

	*asp = as;
	return 0;
}


const struct ausrc *ausrc_find(const char *name)
{
	struct le *le;

	for (le = ausrcl.head; le; le = le->next) {
		struct ausrc *as = le->data;

		if (name && 0 != str_casecmp(name, as->name))
			continue;

		/* Found */
		return as;
	}

	return NULL;
}


int ausrc_alloc(struct ausrc_st **stp, const char *name,
		struct ausrc_prm *prm, const char *device,
		ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc *as;

	as = (struct ausrc *)ausrc_find(name);
	if (!as)
		return ENOENT;

	return as->alloch(stp, as, prm, device, rh, errh, arg);
}
