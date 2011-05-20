/**
 * @file vidsrc.c Video Source
 *
 * Copyright (C) 2010 Creytiv.com
 */

#include <re.h>
#include <baresip.h>
#include "core.h"


struct vidsrc_st {
	struct vidsrc *vs;
};


static struct list vidsrcl = LIST_INIT;


static void destructor(void *arg)
{
	struct vidsrc *vs = arg;

	list_unlink(&vs->le);
}


int vidsrc_register(struct vidsrc **vsp, const char *name,
		    vidsrc_alloc_h *alloch, vidsrc_update_h *updateh)
{
	struct vidsrc *vs;

	if (!vsp)
		return EINVAL;

	vs = mem_zalloc(sizeof(*vs), destructor);
	if (!vs)
		return ENOMEM;

	list_append(&vidsrcl, &vs->le, vs);

	vs->name    = name;
	vs->alloch  = alloch;
	vs->updateh = updateh;

	(void)re_printf("vidsrc: %s\n", name);

	*vsp = vs;

	return 0;
}


const struct vidsrc *vidsrc_find(const char *name)
{
	struct le *le;

	for (le=vidsrcl.head; le; le=le->next) {

		struct vidsrc *vs = le->data;

		if (name && 0 != str_casecmp(name, vs->name))
			continue;

		return vs;
	}

	return NULL;
}


struct list *vidsrc_list(void)
{
	return &vidsrcl;
}


struct vidsrc *vidsrc_get(struct vidsrc_st *st)
{
	return st ? st->vs : NULL;
}
