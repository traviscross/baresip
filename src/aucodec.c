/**
 * @file aucodec.c  Audio codecs
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


/* Base type */
struct aucodec_st {
	struct aucodec *ac;
};

static struct list aucodecl = LIST_INIT;


static void destructor(void *data)
{
	struct aucodec *ac = data;

	list_unlink(&ac->le);
}


int aucodec_register(struct aucodec **ap, const char *pt, const char *name,
		     uint32_t srate, uint8_t ch, const char *fmtp,
		     aucodec_alloc_h *alloch,
		     aucodec_enc_h *ench, aucodec_dec_h *dech)
{
	struct aucodec *ac;

	if (!ap)
		return EINVAL;

	ac = mem_zalloc(sizeof(*ac), destructor);
	if (!ac)
		return ENOMEM;

	list_append(&aucodecl, &ac->le, ac);

	ac->pt     = pt;
	ac->name   = name;
	ac->srate  = srate;
	ac->ch     = ch;
	ac->fmtp   = fmtp;
	ac->alloch = alloch;
	ac->ench   = ench;
	ac->dech   = dech;

	(void)re_printf("aucodec: %s %uHz %uch\n", name, srate, ch);

	*ap = ac;
	return 0;
}


int aucodec_clone(struct list *l, const struct aucodec *src)
{
	struct aucodec *ac;

	if (!l || !src)
		return EINVAL;

	ac = mem_zalloc(sizeof(*ac), destructor);
	if (!ac)
		return ENOMEM;

	*ac = *src;

	ac->le.list = NULL;
	list_append(l, &ac->le, ac);

	return 0;
}


const struct aucodec *aucodec_find(const char *name, uint32_t srate, int ch)
{
	struct le *le;

	for (le = aucodecl.head; le; le = le->next) {
		struct aucodec *ac = le->data;

		if (name && 0 != str_casecmp(name, ac->name))
			continue;

		if (srate && srate != ac->srate)
			continue;

		if (ch && ch != ac->ch)
			continue;

		/* Found */
		return ac;
	}

	return NULL;
}


struct list *aucodec_list(void)
{
	return &aucodecl;
}


int aucodec_alloc(struct aucodec_st **sp, const char *name, uint32_t srate,
		  int channels, struct aucodec_prm *encp,
		  struct aucodec_prm *decp, const struct pl *sdp_fmtp)
{
	struct le *le;

	for (le = aucodecl.head; le; le = le->next) {
		struct aucodec *ac = le->data;

		if (name && 0 != str_casecmp(name, ac->name))
			continue;

		if (srate && srate != ac->srate)
			continue;

		if (channels && channels != ac->ch)
			continue;

		/* Found */
		return ac->alloch(sp, ac, encp, decp, sdp_fmtp);
	}

	return ENOENT;
}


int aucodec_encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	return st->ac->ench ? st->ac->ench(st, dst, src) : 0;
}


int aucodec_decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	return st->ac->dech ? st->ac->dech(st, dst, src) : 0;
}


const char *aucodec_pt(const struct aucodec *ac)
{
	return ac ? ac->pt : NULL;
}


const char *aucodec_name(const struct aucodec *ac)
{
	return ac ? ac->name : NULL;
}


uint32_t aucodec_srate(const struct aucodec *ac)
{
	return ac ? ac->srate : 0;
}


uint8_t  aucodec_ch(const struct aucodec *ac)
{
	return ac ? ac->ch : 0;
}


struct aucodec *aucodec_get(struct aucodec_st *st)
{
	return st ? st->ac : NULL;
}


bool aucodec_cmp(const struct aucodec *l, const struct aucodec *r)
{
	if (!l || !r)
		return false;

	if (l == r)
		return true;

	if (0 != str_casecmp(l->name, r->name))
		return false;

	if (l->srate != r->srate)
		return false;

	if (l->ch != r->ch)
		return false;

	return true;
}


int aucodec_debug(struct re_printf *pf, const struct list *acl)
{
	struct le *le;
	int err;

	err = re_hprintf(pf, "Audio codecs: (%u)\n", list_count(acl));
	for (le = list_head(acl); le; le = le->next) {
		const struct aucodec *ac = le->data;
		err |= re_hprintf(pf, " %3s %-8s %uHz/%u\n",
				  ac->pt, ac->name, ac->srate, ac->ch);
	}

	return err;
}
