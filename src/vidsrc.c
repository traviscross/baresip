/**
 * @file vidsrc.c  Video Source
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


static struct list vidsrcl = LIST_INIT;


static void destructor(void *data)
{
	struct vidsrc *vs = data;

	list_unlink(&vs->le);
}


int vidsrc_register(struct vidsrc **vsp, const char *name,
		    vidsrc_alloc_h *alloch)
{
	struct vidsrc *vs;

	if (!vsp)
		return EINVAL;

	vs = mem_zalloc(sizeof(*vs), destructor);
	if (!vs)
		return ENOMEM;

	list_append(&vidsrcl, &vs->le, vs);

	vs->name   = name;
	vs->alloch = alloch;

	(void)re_printf("vidsrc: %s\n", name);

	*vsp = vs;
	return 0;
}


const struct vidsrc *vidsrc_find(const char *name)
{
	struct le *le;

	for (le = vidsrcl.head; le; le = le->next) {
		struct vidsrc *vs = le->data;

		if (name && 0 != str_casecmp(name, vs->name))
			continue;

		/* Found */
		return vs;
	}

	return NULL;
}


int vidsrc_alloc(struct vidsrc_st **stp, const char *name,
		 struct vidsrc_prm *prm, const char *fmt, const char *dev,
		 vidsrc_frame_h *frameh, vidsrc_error_h *errorh, void *arg)
{
	struct vidsrc *vs = (struct vidsrc *)vidsrc_find(name);
	if (!vs)
		return ENOENT;

	return vs->alloch(stp, vs, prm, fmt, dev, frameh, errorh, arg);
}


struct list *vidsrc_list(void)
{
	return &vidsrcl;
}
