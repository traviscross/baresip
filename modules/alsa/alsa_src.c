/**
 * @file alsa_src.c  ALSA sound driver - recorder
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _POSIX_SOURCE 1
#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "alsa.h"


#define DEBUG_MODULE "alsa_src"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct ausrc_st {
	struct ausrc *as;      /* inheritance */
	pthread_t thread;
	bool run;
	snd_pcm_t *read;
	int sample_size;
	int frame_size;
	struct mbuf *mbr;
	ausrc_read_h *rh;
	ausrc_error_h *errh;
	void *arg;
};


static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	/* Wait for termination of other thread */
	if (st->run) {
		st->run = false;
		(void)pthread_join(st->thread, NULL);
	}

	if (st->read)
		snd_pcm_close(st->read);

	mem_deref(st->mbr);
	mem_deref(st->as);
}


static void *read_thread(void *arg)
{
	struct ausrc_st *st = arg;
	int err;

	while (st->run) {
		err = snd_pcm_readi(st->read, st->mbr->buf, st->frame_size);
		if (err == -EPIPE) {
			snd_pcm_prepare(st->read);
		}
		else if (err <= 0) {
			if (EAGAIN != err) {
				DEBUG_WARNING("read: %s\n", snd_strerror(err));
			}
			continue;
		}

		st->rh(st->mbr->buf, err * st->sample_size, st->arg);
	}

	return NULL;
}


int alsa_src_alloc(struct ausrc_st **stp, struct ausrc *as,
		   struct media_ctx **ctx,
		   struct ausrc_prm *prm, const char *device,
		   ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err;

	(void)ctx;
	(void)errh;

	if (!str_isset(device))
		device = alsa_dev;

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->as  = mem_ref(as);
	st->rh  = rh;
	st->arg = arg;
	st->sample_size = prm->ch * (prm->fmt == AUFMT_S16LE ? 2 : 1);
	st->frame_size = prm->frame_size;

	err = snd_pcm_open(&st->read, device, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		DEBUG_WARNING("read open: %s %s\n", device, snd_strerror(err));
		goto out;
	}

	st->mbr = mbuf_alloc(st->sample_size * st->frame_size);
	if (!st->mbr) {
		err = ENOMEM;
		goto out;
	}

	err = alsa_reset(st->read, prm->srate, prm->ch, prm->fmt,
			 prm->frame_size);
	if (err)
		goto out;

	/* Start */
	err = snd_pcm_start(st->read);
	if (err) {
		DEBUG_WARNING("snd_pcm_start on read: %s\n",
			      snd_strerror(err));
		goto out;
	}

	st->run = true;
	err = pthread_create(&st->thread, NULL, read_thread, st);
	if (err) {
		st->run = false;
		goto out;
	}

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
