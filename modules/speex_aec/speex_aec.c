/**
 * @file speex_aec.c  Speex Acoustic Echo Cancellation
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#include <speex/speex.h>
#include <speex/speex_echo.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "speex_aec"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct aufilt_st {
	struct aufilt *af;    /* base class */
	uint32_t psize;
	int16_t *out;
	SpeexEchoState *state;
};


static struct aufilt *filt;


#ifdef SPEEX_SET_VBR_MAX_BITRATE
static void speex_aec_destructor(void *arg)
{
	struct aufilt_st *st = arg;

	if (st->state)
		speex_echo_state_destroy(st->state);

	mem_deref(st->out);

	mem_deref(st->af);
}


static int alloc(struct aufilt_st **stp, struct aufilt *af,
		 const struct aufilt_prm *encprm,
		 const struct aufilt_prm *decprm)
{
	struct aufilt_st *st;
	int err, tmp, fl;

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

	st->af = mem_ref(af);

	st->psize = 2 * encprm->ch * encprm->frame_size;

	st->out = mem_alloc(st->psize, NULL);
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
		*stp = st;

	return err;
}


static int enc(struct aufilt_st *st, struct mbuf *mb)
{
	size_t pos = mb->pos;
	int err;

	if (mbuf_get_left(mb) != st->psize) {
		DEBUG_WARNING("enc: expect %u bytes, got %u\n", st->psize,
			      mbuf_get_left(mb));
		return ENOMEM;
	}

	speex_echo_capture(st->state, (int16_t *)mbuf_buf(mb), st->out);
	err = mbuf_write_mem(mb, (uint8_t *)st->out, st->psize);
	mb->pos = pos;
	mb->end = st->psize;

	return err;
}


static int dec(struct aufilt_st *st, struct mbuf *mb)
{
	speex_echo_playback(st->state, (int16_t *)mbuf_buf(mb));
	return 0;
}
#endif


static int module_init(void)
{
	/* Note: Hack to check libspeex version */
#ifdef SPEEX_SET_VBR_MAX_BITRATE
	return aufilt_register(&filt, "speex_aec", alloc, enc, dec, NULL);
#else
	return ENOSYS;
#endif
}


static int module_close(void)
{
	filt = mem_deref(filt);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(speex_aec) = {
	"speex_aec",
	"filter",
	module_init,
	module_close
};
