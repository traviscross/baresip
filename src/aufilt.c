/**
 * @file aufilt.c Audio Filter API
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


#ifndef RELEASE
#define AUFILT_DEBUG 1
#endif


/*
 - operate on linear PCM samples
 - both encode and decode directions
 - list of preproc's for each dir
 - latency calculations
 - some orthogonal enc/dec, some not

  Module        type  encode   decode  ch    config

o sndfile       orth  in       in      1,2   srate,ch, file names
o speex_aec     comb  in->out  in      1,2?  srate, Tail length etc.
o speex_pp      orth  in->out  -       1,2?  srate, ch, preproc denoise,agc,vad
o speex_resamp  orth  in->out  in->out 1,2   srate in/out, q

RTP <--- [Audio Encoder] <--- [PROC Encode] <--- [Audio input]

RTP ---> [Audio Decoder] ---> [PROC Decode] ---> [Audio output]
 */


/* Base type */
struct aufilt_st {
	struct aufilt *af;
};

struct aufilt {
	struct le le;
	const char *name;
	aufilt_alloc_h *alloch;
	aufilt_enc_h *ench;
	aufilt_dec_h *dech;
	aufilt_dbg_h *dbgh;
};

/* One filter element */
struct aufilt_elem {
	struct le le;
	struct aufilt_st *st;
};

/* A chain of filters */
struct aufilt_chain {
	struct list filtl;  /* struct aufilt_elem */
};


static struct list aufiltl = LIST_INIT;  /* struct aufilt */

#if AUFILT_DEBUG
static struct {
	uint32_t enc_min;
	uint32_t enc_avg;
	uint32_t enc_max;
	uint32_t dec_min;
	uint32_t dec_avg;
	uint32_t dec_max;
} stat = {
	~0, 0, 0, ~0, 0, 0
};
#endif


static inline struct aufilt *aufilt_get(struct aufilt_st *st)
{
	return st ? st->af : NULL;
}


static void aufilt_elem_destructor(void *data)
{
	struct aufilt_elem *f = data;
	list_unlink(&f->le);
	mem_deref(f->st);
}


static void aufilt_chain_destructor(void *data)
{
	struct aufilt_chain *fc = data;
	list_flush(&fc->filtl);
}


/* Allocate a filter-chain */
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

	if (fc->filtl.head) {
		(void)re_printf("audio-filter chain: enc=%uHz/%dch"
				" dec=%uHz/%dch (%u filters)\n",
				encprm->srate, encprm->ch,
				decprm->srate, decprm->ch,
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
#if AUFILT_DEBUG
	uint64_t t = tmr_jiffies();
	uint32_t diff;
#endif

	if (!fc)
		return EINVAL;

	for (le = fc->filtl.head; !err && le; le = le->next) {
		struct aufilt_elem *f = le->data;
		struct aufilt *af = aufilt_get(f->st);

		if (af->ench)
			err = af->ench(f->st, mb);
	}

#if AUFILT_DEBUG
	diff = (uint32_t) (tmr_jiffies() - t);
	stat.enc_min = min(stat.enc_min, diff);
	stat.enc_avg = avg(stat.enc_avg, diff);
	stat.enc_max = max(stat.enc_max, diff);
#endif

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
#if AUFILT_DEBUG
	uint64_t t = tmr_jiffies();
	uint32_t diff;
#endif

	if (!fc)
		return EINVAL;

	for (le = fc->filtl.head; !err && le; le = le->next) {
		struct aufilt_elem *f = le->data;
		struct aufilt *af = aufilt_get(f->st);

		if (af->dech)
			err = af->dech(f->st, mb);
	}

#if AUFILT_DEBUG
	diff = (uint32_t) (tmr_jiffies() - t);
	stat.dec_min = min(stat.dec_min, diff);
	stat.dec_avg = avg(stat.dec_avg, diff);
	stat.dec_max = max(stat.dec_max, diff);
#endif

	return err;
}


static void destructor(void *arg)
{
	struct aufilt *af = arg;

	list_unlink(&af->le);
}


int aufilt_register(struct aufilt **afp, const char *name,
		    aufilt_alloc_h *alloch, aufilt_enc_h *ench,
		    aufilt_dec_h *dech, aufilt_dbg_h *dbgh)
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
	af->dbgh   = dbgh;

	(void)re_printf("aufilt: %s\n", name);

	*afp = af;

	return 0;
}


struct list *aufilt_list(void)
{
	return &aufiltl;
}


int aufilt_chain_debug(struct re_printf *pf, const struct aufilt_chain *fc)
{
	struct le *le;
	int err;

	if (!fc)
		return 0;

	err = re_hprintf(pf, "Audio filter chain: (%u)\n",
			 list_count(&fc->filtl));
	for (le = fc->filtl.head; le && !err; le = le->next) {
		struct aufilt_elem *ae = le->data;
		struct aufilt_st *st = ae->st;

		if (aufilt_get(st)->dbgh)
			err = aufilt_get(st)->dbgh(pf, st);
	}

	return err;
}


int aufilt_debug(struct re_printf *pf, void *unused)
{
	struct le *le;
	uint32_t i = 0;
	int err;

	(void)unused;

	err = re_hprintf(pf, "Audio filter chain:\n");
	for (le = aufiltl.head; !err && le; le = le->next, i++) {
		const struct aufilt *af = le->data;

		err = re_hprintf(pf, " %u: %s\n", i, af->name);
	}

#if AUFILT_DEBUG
	err |= re_hprintf(pf, " Encoder min/avg/max = %u/%u/%u [ms]\n",
			  stat.enc_min, stat.enc_avg, stat.enc_max);
	err |= re_hprintf(pf, " Decoder min/avg/max = %u/%u/%u [ms]\n",
			  stat.dec_min, stat.dec_avg, stat.dec_max);
#endif

	return err;
}
