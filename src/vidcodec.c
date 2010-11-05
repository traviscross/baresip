/**
 * @file vidcodec.c  Video codecs
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


/* Base type */
struct vidcodec_st {
	struct vidcodec *vc;
};

static struct list vidcodecl = LIST_INIT;


static void destructor(void *data)
{
	struct vidcodec *vc = data;

	list_unlink(&vc->le);
}


int vidcodec_register(struct vidcodec **vp, const char *pt, const char *name,
		      const char *fmtp, vidcodec_alloc_h *alloch,
		      vidcodec_enc_h *ench, vidcodec_dec_h *dech)
{
	struct vidcodec *vc;

	if (!vp)
		return EINVAL;

	vc = mem_zalloc(sizeof(*vc), destructor);
	if (!vc)
		return ENOMEM;

	list_append(&vidcodecl, &vc->le, vc);

	vc->pt     = pt;
	vc->name   = name;
	vc->fmtp   = fmtp;
	vc->alloch = alloch;
	vc->ench   = ench;
	vc->dech   = dech;

	(void)re_printf("vidcodec: %s\n", name);

	*vp = vc;
	return 0;
}


int vidcodec_clone(struct list *l, const struct vidcodec *src)
{
	struct vidcodec *vc;

	if (!l || !src)
		return EINVAL;

	vc = mem_zalloc(sizeof(*vc), destructor);
	if (!vc)
		return ENOMEM;

	*vc = *src;

	vc->le.list = NULL;
	list_append(l, &vc->le, vc);

	return 0;
}


const struct vidcodec *vidcodec_find(const char *name)
{
	struct le *le;

	for (le = vidcodecl.head; le; le = le->next) {
		struct vidcodec *vc = le->data;

		if (name && 0 != str_casecmp(name, vc->name))
			continue;

		/* Found */
		return vc;
	}

	return NULL;
}


int vidcodec_alloc(struct vidcodec_st **sp, const char *name,
		   struct vidcodec_prm *encp, struct vidcodec_prm *decp,
		   const struct pl *sdp_fmtp,
		   vidcodec_send_h *sendh, void *arg)
{
	struct vidcodec *vc = (struct vidcodec *)vidcodec_find(name);
	if (!vc)
		return ENOENT;

	return vc->alloch(sp, vc, name, encp, decp, sdp_fmtp,
			  sendh, arg);
}


struct list *vidcodec_list(void)
{
	return &vidcodecl;
}


struct vidcodec *vidcodec_get(struct vidcodec_st *st)
{
	return st ? st->vc : NULL;
}


const char *vidcodec_pt(const struct vidcodec *vc)
{
	return vc ? vc->pt : NULL;
}


const char *vidcodec_name(const struct vidcodec *vc)
{
	return vc ? vc->name : NULL;
}


void vidcodec_set_fmtp(struct vidcodec *vc, const char *fmtp)
{
	if (vc)
		vc->fmtp = fmtp;
}


bool vidcodec_cmp(const struct vidcodec *l, const struct vidcodec *r)
{
	if (!l || !r)
		return false;

	if (l == r)
		return true;

	if (0 != str_casecmp(l->name, r->name))
		return false;

	return true;
}


int vidcodec_debug(struct re_printf *pf, const struct list *vcl)
{
	struct le *le;
	int err;

	err = re_hprintf(pf, "Video codecs: (%u)\n", list_count(vcl));
	for (le = list_head(vcl); le; le = le->next) {
		const struct vidcodec *vc = le->data;
		err |= re_hprintf(pf, " %3s %-8s\n", vc->pt, vc->name);
	}

	return err;
}
