/**
 * @file speex_aec.c  Speex Acoustic Echo Cancellation
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#include <string.h>
#include <speex/speex.h>
#include <speex/speex_echo.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "speex_aec"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct speex_st {
	struct aufilt af;    /* base class */
	uint32_t nsamp;
	int16_t *out;
	SpeexEchoState *state;
};


#ifdef SPEEX_SET_VBR_MAX_BITRATE
static void speex_aec_destructor(void *arg)
{
	struct speex_st *st = arg;

	if (st->state)
		speex_echo_state_destroy(st->state);

	mem_deref(st->out);
	list_unlink(&st->af.le);
}


static int update(struct aufilt_st **stp, struct aufilt *af,
		  const struct aufilt_prm *encprm,
		  const struct aufilt_prm *decprm)
{
	struct speex_st *st;
	int err, tmp, fl;

	(void)af;

	if (!stp)
		return EINVAL;

	if (*stp)
		return 0;

	/* Check config */
	if (encprm->srate != decprm->srate) {
		DEBUG_WARNING("symm srate required for AEC\n");
		return EINVAL;
	}
	if (encprm->ch != decprm->ch) {
		DEBUG_WARNING("symm channels required for AEC\n");
		return EINVAL;
	}

	st = mem_zalloc(sizeof(*st), speex_aec_destructor);
	if (!st)
		return ENOMEM;

	st->nsamp = encprm->ch * encprm->frame_size;

	st->out = mem_alloc(2 * st->nsamp, NULL);
	if (!st->out) {
		err = ENOMEM;
		goto out;
	}

	/* Echo canceller with 200 ms tail length */
	fl = 10 * encprm->frame_size;
	st->state = speex_echo_state_init(encprm->frame_size, fl);
	if (!st->state) {
		err = ENOMEM;
		goto out;
	}

	tmp = encprm->srate;
	err = speex_echo_ctl(st->state, SPEEX_ECHO_SET_SAMPLING_RATE, &tmp);
	if (err < 0) {
		DEBUG_WARNING("speex_echo_ctl: err=%d\n", err);
	}

	DEBUG_NOTICE("Speex AEC loaded: enc=%uHz\n", encprm->srate);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = (struct aufilt_st *)st;

	return err;
}


static int encode(struct aufilt_st *st, int16_t *sampv, size_t *sampc)
{
	struct speex_st *sp = (struct speex_st *)st;

	if (*sampc) {
		speex_echo_capture(sp->state, sampv, sp->out);
		memcpy(sampv, sp->out, 2 * sp->nsamp);
	}

	return 0;
}


static int decode(struct aufilt_st *st, int16_t *sampv, size_t *sampc)
{
	struct speex_st *sp = (struct speex_st *)st;

	if (*sampc)
		speex_echo_playback(sp->state, sampv);

	return 0;
}
#endif


static struct aufilt speex_aec = {
	LE_INIT, "speex_aec", update, encode, decode
};


static int module_init(void)
{
	/* Note: Hack to check libspeex version */
#ifdef SPEEX_SET_VBR_MAX_BITRATE
	aufilt_register(&speex_aec);
	return 0;
#else
	return ENOSYS;
#endif
}


static int module_close(void)
{
	aufilt_unregister(&speex_aec);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(speex_aec) = {
	"speex_aec",
	"filter",
	module_init,
	module_close
};
