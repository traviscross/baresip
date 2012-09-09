/**
 * @file aucodec.c Audio Codec
 *
 * Copyright (C) 2010 Creytiv.com
 */

#include <re.h>
#include <baresip.h>
#include "core.h"


/** Audio Codec state */
struct aucodec_st {
	struct aucodec *ac;  /**< Audio Codec */
};

static struct list aucodecl = LIST_INIT;


static void destructor(void *arg)
{
	struct aucodec *ac = arg;

	list_unlink(&ac->le);
}


/**
 * Register an Audio Codec
 *
 * @param ap     Pointer to allocated Audio Codec object
 * @param pt     Payload Type
 * @param name   Audio Codec name
 * @param srate  Sampling rate
 * @param ch     Number of channels
 * @param fmtp   Optional format parameters
 * @param alloch Allocation handler
 * @param ench   Encode handler
 * @param dech   Decode handler
 * @param cmph   SDP compare handler
 *
 * @return 0 if success, otherwise errorcode
 */
int aucodec_register(struct aucodec **ap, const char *pt, const char *name,
		     uint32_t srate, uint8_t ch, const char *fmtp,
		     aucodec_alloc_h *alloch, aucodec_enc_h *ench,
		     aucodec_dec_h *dech, sdp_fmtp_cmp_h *cmph)
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
	ac->cmph   = cmph;

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


const struct aucodec *aucodec_find(const char *name, uint32_t srate,
				   uint8_t ch)
{
	struct le *le;

	for (le=aucodecl.head; le; le=le->next) {

		struct aucodec *ac = le->data;

		if (name && 0 != str_casecmp(name, ac->name))
			continue;

		if (srate && srate != ac->srate)
			continue;

		if (ch && ch != ac->ch)
			continue;

		return ac;
	}

	return NULL;
}


/**
 * Get the list of Audio Codecs
 */
struct list *aucodec_list(void)
{
	return &aucodecl;
}


/**
 * Allocate an Audio Codec state
 *
 * @param sp        Pointer to allocated state
 * @param name      Audio Codec name
 * @param srate     Audio Codec sampling rate
 * @param channels  Audio Codec channels
 * @param encp      Optional encoding parameters
 * @param decp      Optional decoding parameters
 * @param fmtp      Optional SDP format parameters
 *
 * @return 0 if success, otherwise errorcode
 */
int aucodec_alloc(struct aucodec_st **sp, const char *name, uint32_t srate,
		  uint8_t channels, struct aucodec_prm *encp,
		  struct aucodec_prm *decp, const char *fmtp)
{
	struct aucodec *ac;

	ac = (struct aucodec *)aucodec_find(name, srate, channels);
	if (!ac)
		return ENOENT;

	return ac->alloch(sp, ac, encp, decp, fmtp);
}


/**
 * Audio Codec encoder
 *
 * @param st  Audio Codec state
 * @param dst Destination buffer
 * @param src Source buffer of PCM audio
 *
 * @return 0 if success, otherwise errorcode
 */
int aucodec_encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	if (!st || !st->ac)
		return EINVAL;

	return st->ac->ench ? st->ac->ench(st, dst, src) : 0;
}


/**
 * Audio Codec decoder
 *
 * @param st  Audio Codec state
 * @param dst Destination buffer for PCM audio
 * @param src Source buffer
 *
 * @return 0 if success, otherwise errorcode
 */
int aucodec_decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	if (!st || !st->ac)
		return EINVAL;

	return st->ac->dech ? st->ac->dech(st, dst, src) : 0;
}


/**
 * Get the Payload Type of an Audio Codec
 */
const char *aucodec_pt(const struct aucodec *ac)
{
	return ac ? ac->pt : NULL;
}


/**
 * Get the name of an Audio Codec
 */
const char *aucodec_name(const struct aucodec *ac)
{
	return ac ? ac->name : NULL;
}


/**
 * Get the Sampling Rate of an Audio Codec
 */
uint32_t aucodec_srate(const struct aucodec *ac)
{
	return ac ? ac->srate : 0;
}


/**
 * Get the number of channels for an Audio Codec
 */
uint8_t aucodec_ch(const struct aucodec *ac)
{
	return ac ? ac->ch : 0;
}


/**
 * Get the Audio Codec from an Audio Codec state
 */
struct aucodec *aucodec_get(const struct aucodec_st *st)
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
