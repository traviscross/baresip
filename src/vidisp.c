/**
 * @file vidisp.c  Video Display
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


struct vidisp_st {
	struct vidisp *vd;
};


static struct list vidispl = LIST_INIT;


static void destructor(void *data)
{
	struct vidisp *vd = data;

	list_unlink(&vd->le);
}


int vidisp_register(struct vidisp **vp, const char *name,
		    vidisp_alloc_h *alloch, vidisp_disp_h *disph,
		    vidisp_hide_h *hideh)
{
	struct vidisp *vd;

	if (!vp)
		return EINVAL;

	vd = mem_zalloc(sizeof(*vd), destructor);
	if (!vd)
		return ENOMEM;

	list_append(&vidispl, &vd->le, vd);

	vd->name   = name;
	vd->alloch = alloch;
	vd->disph  = disph;
	vd->hideh  = hideh;

	(void)re_printf("vidisp: %s\n", name);

	*vp = vd;
	return 0;
}


const struct vidisp *vidisp_find(const char *name)
{
	struct le *le;

	for (le = vidispl.head; le; le = le->next) {
		struct vidisp *vd = le->data;

		if (name && 0 != str_casecmp(name, vd->name))
			continue;

		/* Found */
		return vd;
	}

	return NULL;
}


int vidisp_alloc(struct vidisp_st **stp, struct vidisp_prm *prm,
		 const char *name, const char *dev, vidisp_input_h *input,
		 vidisp_resize_h *resizeh, void *arg)
{
	struct le *le;

	for (le = vidispl.head; le; le = le->next) {
		struct vidisp *vd = le->data;

		if (name && 0 != str_casecmp(name, vd->name))
			continue;

		/* Found */
		return vd->alloch(stp, vd, prm, dev, input, resizeh, arg);
	}

	return ENOENT;
}


int vidisp_display(struct vidisp_st *st, const char *title,
		   const struct vidframe *frame)
{
	if (!st || !frame)
		return EINVAL;

	return st->vd->disph(st, title, frame);
}
