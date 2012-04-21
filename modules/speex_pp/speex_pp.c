/**
 * @file speex_pp.c  Speex Pre-processor
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#include <speex/speex.h>
#include <speex/speex_preprocess.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "speex_pp"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct aufilt_st {
	struct aufilt *af;    /* base class */
	uint32_t psize;
	SpeexPreprocessState *state;
};


/** Speex configuration */
static struct {
	int denoise_enabled;
	int agc_enabled;
	int vad_enabled;
	int dereverb_enabled;
	spx_int32_t agc_level;
} pp_conf = {
	1,
	1,
	1,
	1,
	8000
};

static struct aufilt *filt;


static void speexpp_destructor(void *arg)
{
	struct aufilt_st *st = arg;

	if (st->state)
		speex_preprocess_state_destroy(st->state);

	mem_deref(st->af);
}


static int alloc(struct aufilt_st **stp, struct aufilt *af,
		 const struct aufilt_prm *encprm,
		 const struct aufilt_prm *decprm)
{
	struct aufilt_st *st;

	(void)decprm;

	if (!encprm || encprm->ch != 1)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), speexpp_destructor);
	if (!st)
		return ENOMEM;

	st->af = mem_ref(af);

	st->psize = 2 * encprm->frame_size;

	st->state = speex_preprocess_state_init(encprm->frame_size,
						encprm->srate);
	if (!st->state)
		goto error;

	speex_preprocess_ctl(st->state, SPEEX_PREPROCESS_SET_DENOISE,
			     &pp_conf.denoise_enabled);
	speex_preprocess_ctl(st->state, SPEEX_PREPROCESS_SET_AGC,
			     &pp_conf.agc_enabled);

#ifdef SPEEX_PREPROCESS_SET_AGC_TARGET
	if (pp_conf.agc_enabled) {
		speex_preprocess_ctl(st->state,
				     SPEEX_PREPROCESS_SET_AGC_TARGET,
				     &pp_conf.agc_level);
	}
#endif

	speex_preprocess_ctl(st->state, SPEEX_PREPROCESS_SET_VAD,
			     &pp_conf.vad_enabled);
	speex_preprocess_ctl(st->state, SPEEX_PREPROCESS_SET_DEREVERB,
			     &pp_conf.dereverb_enabled);

	DEBUG_NOTICE("Speex preprocessor loaded: enc=%uHz\n", encprm->srate);

	*stp = st;
	return 0;

 error:
	mem_deref(st);
	return ENOMEM;
}


static int enc(struct aufilt_st *st, struct mbuf *mb)
{
	int is_speech = 1;

	if (mbuf_get_left(mb) != st->psize) {
		DEBUG_WARNING("enc: expect %u bytes, got %u\n",
			      st->psize, mbuf_get_left(mb));
		return EINVAL;
	}

	/* NOTE: Using this macro to check libspeex version */
#ifdef SPEEX_PREPROCESS_SET_NOISE_SUPPRESS
	/* New API */
	is_speech = speex_preprocess_run(st->state, (int16_t *)mbuf_buf(mb));
#else
	/* Old API - not tested! */
	is_speech = speex_preprocess(st->state,
				     (int16_t *)mbuf_buf(mb), NULL);
#endif

	/* XXX: Handle is_speech and VAD */
	(void)is_speech;

	return 0;
}


static void config_parse(struct conf *conf)
{
	uint32_t v;

	if (0 == conf_get_u32(conf, "speex_agc_level", &v))
		pp_conf.agc_level = v;
}


static int module_init(void)
{
	config_parse(conf_cur());
	return aufilt_register(&filt, "speex_pp", alloc, enc, NULL, NULL);
}


static int module_close(void)
{
	filt = mem_deref(filt);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(speex_pp) = {
	"speex_pp",
	"filter",
	module_init,
	module_close
};
