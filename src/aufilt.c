/**
 * @file aufilt.c Audio Filter API
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


/*
 * \page AudioFilter Audio Filter

 - operate on linear PCM samples
 - both encode and decode directions
 - list of preproc's for each dir
 - some orthogonal enc/dec, some not

RTP <--- [Audio Encoder] <--- [PROC Encode] <--- [Audio input]

RTP ---> [Audio Decoder] ---> [PROC Decode] ---> [Audio output]
 */


/** Audio Filter state */
struct aufilt_st {
	struct aufilt *af;
};

/** Audio Filter */
struct aufilt {
	struct le le;
	const char *name;
	aufilt_alloc_h *alloch;
	aufilt_enc_h *ench;
	aufilt_dec_h *dech;
	aufilt_update_h *updh;
};

/** Audio Filter element */
struct aufilt_elem {
	struct le le;
	struct aufilt_st *st;
};

/** A chain of Audio Filters */
struct aufilt_chain {
	struct list filtl;  /* struct aufilt_elem */
};


static struct list aufiltl = LIST_INIT;  /* struct aufilt */


static inline struct aufilt *aufilt_get(struct aufilt_st *st)
{
	return st ? st->af : NULL;
}


static void destructor(void *arg)
{
	struct aufilt *af = arg;

	list_unlink(&af->le);
}


static void aufilt_elem_destructor(void *arg)
{
	struct aufilt_elem *f = arg;
	list_unlink(&f->le);
	mem_deref(f->st);
}


static void aufilt_chain_destructor(void *arg)
{
	struct aufilt_chain *fc = arg;
	list_flush(&fc->filtl);
}


/**
 * Allocate an audio filter-chain
 */
int aufilt_chain_alloc(struct aufilt_chain **fcp,
		       const struct aufilt_prm *encprm,
		       const struct aufilt_prm *decprm)
{
	struct aufilt_chain *fc;
	struct le *le;
	int err = 0;

	if (!fcp || !encprm || !decprm)
		return EINVAL;

	fc = mem_zalloc(sizeof(*fc), aufilt_chain_destructor);
	if (!fc)
		return ENOMEM;

	list_init(&fc->filtl);

	/* Loop through all filter modules */
	for (le = aufiltl.head; le; le = le->next) {
		struct aufilt *af = le->data;
		struct aufilt_elem *f = mem_zalloc(sizeof(*f),
						   aufilt_elem_destructor);
		if (!f)	{
			err = ENOMEM;
			goto out;
		}
		list_append(&fc->filtl, &f->le, f);

		err = af->alloch(&f->st, af, encprm, decprm);
		if (err) {
			mem_deref(f);
			goto out;
		}
	}

	if (!list_isempty(&fc->filtl)) {
		(void)re_printf("audio-filter chain: enc=%u-%uHz/%dch"
				" dec=%u-%uHz/%dch (%u filters)\n",
				encprm->srate, encprm->srate_out, encprm->ch,
				decprm->srate, decprm->srate_out, decprm->ch,
				list_count(&fc->filtl));
	}

 out:
	if (err)
		mem_deref(fc);
	else
		*fcp = fc;

	return err;
}


/**
 * Process PCM-data on encode-path
 *
 * @param fc  Filter-chain
 * @param mb  Buffer with PCM data. NULL==silence
 *
 * @return 0 for success, otherwise error code
 */
int aufilt_chain_encode(struct aufilt_chain *fc, struct mbuf *mb)
{
	struct le *le;
	int err = 0;

	if (!fc)
		return EINVAL;

	for (le = fc->filtl.head; !err && le; le = le->next) {
		const struct aufilt_elem *f = le->data;
		const struct aufilt *af = aufilt_get(f->st);

		if (af->ench)
			err = af->ench(f->st, mb);
	}

	return err;
}


/**
 * Process PCM-data on decode-path
 *
 * @param fc  Filter-chain
 * @param mb  Buffer with PCM data - NULL if no packets received
 *
 * @return 0 for success, otherwise error code
 */
int aufilt_chain_decode(struct aufilt_chain *fc, struct mbuf *mb)
{
	struct le *le;
	int err = 0;

	if (!fc)
		return EINVAL;

	for (le = fc->filtl.head; !err && le; le = le->next) {
		const struct aufilt_elem *f = le->data;
		const struct aufilt *af = aufilt_get(f->st);

		if (af->dech)
			err = af->dech(f->st, mb);
	}

	return err;
}


/**
 * Update audio-filter chain
 *
 * @param fc Filter-chain
 *
 * @return 0 for success, otherwise error code
 */
int aufilt_chain_update(struct aufilt_chain *fc)
{
	struct le *le;
	int err = 0;

	if (!fc)
		return EINVAL;

	for (le = fc->filtl.head; !err && le; le = le->next) {
		const struct aufilt_elem *f = le->data;
		const struct aufilt *af = aufilt_get(f->st);

		if (af->updh)
			err = af->updh(f->st);
	}

	return err;
}


/**
 * Register a new Audio Filter
 *
 * @param afp     Pointer to allocated Audio Filter
 * @param name    Name of the Audio Filter
 * @param alloch  Allocation handler
 * @param ench    Encode handler
 * @param dech    Decode handler
 * @param updh    Update handler
 *
 * @return 0 if success, otherwise errorcode
 */
int aufilt_register(struct aufilt **afp, const char *name,
		    aufilt_alloc_h *alloch, aufilt_enc_h *ench,
		    aufilt_dec_h *dech, aufilt_update_h *updh)
{
	struct aufilt *af;

	if (!afp || !name || !alloch)
		return EINVAL;

	af = mem_zalloc(sizeof(*af), destructor);
	if (!af)
		return ENOMEM;

	list_append(&aufiltl, &af->le, af);

	af->name   = name;
	af->alloch = alloch;
	af->ench   = ench;
	af->dech   = dech;
	af->updh   = updh;

	(void)re_printf("aufilt: %s\n", name);

	*afp = af;

	return 0;
}


/**
 * Get the list of registered Audio filters
 *
 * @return List of Audio filters
 */
struct list *aufilt_list(void)
{
	return &aufiltl;
}


/**
 * Print debug information about the audio filter chain
 *
 * @param pf     Print handler for debug output
 * @param unused Unused parameter
 *
 * @return 0 if success, otherwise errorcode
 */
int aufilt_debug(struct re_printf *pf, void *unused)
{
	struct le *le;
	uint32_t i = 0;
	int err;

	(void)unused;

	err = re_hprintf(pf, "Audio filter chain:\n");
	for (le = aufiltl.head; le; le = le->next, i++) {
		const struct aufilt *af = le->data;

		err |= re_hprintf(pf, " %u: %s\n", i, af->name);
	}

	return err;
}
