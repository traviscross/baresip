/**
 * @file alsa_play.c  ALSA sound driver - player
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _POSIX_SOURCE 1
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "alsa.h"


#define DEBUG_MODULE "alsa_play"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct auplay_st {
	struct auplay *ap;      /* inheritance */
	pthread_t thread;
	bool run;
	int frame_size;
	int sample_size;
	snd_pcm_t *write;
	struct mbuf *mbw;
	auplay_write_h *wh;
	void *arg;
};


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	/* Wait for termination of other thread */
	if (st->run) {
		st->run = false;
		(void)pthread_join(st->thread, NULL);
	}

	if (st->write)
		snd_pcm_close(st->write);

	mem_deref(st->mbw);
	mem_deref(st->ap);
}


static void *write_thread(void *arg)
{
	struct auplay_st *st = arg;
	int n;

	while (st->run) {
		const int samples = st->frame_size;

		st->wh(st->mbw->buf, st->mbw->size, st->arg);

		n = snd_pcm_writei(st->write, st->mbw->buf, samples);
		if (-EPIPE == n) {
			snd_pcm_prepare(st->write);

			n = snd_pcm_writei(st->write, st->mbw->buf, samples);
			if (n != samples) {
				DEBUG_WARNING("Write error: %s\n",
					      snd_strerror(n));
			}
			else if (n < 0) {
				DEBUG_WARNING("Write error %s\n",
					      snd_strerror(n));
			}
		}
		else if (n < 0) {
			DEBUG_WARNING("write: %s\n", snd_strerror(n));
		}
		else if (n != samples) {
			DEBUG_WARNING("write: wrote %d of %d bytes\n",
				      n, samples);
		}
	}

	return NULL;
}


int alsa_play_alloc(struct auplay_st **stp, struct auplay *ap,
		    struct auplay_prm *prm, const char *device,
		    auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err;

	if (!str_isset(device))
		device = alsa_dev;

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->ap  = mem_ref(ap);
	st->wh  = wh;
	st->arg = arg;
	st->sample_size = prm->ch * (prm->fmt == AUFMT_S16LE ? 2 : 1);
	st->frame_size = prm->frame_size;

	err = snd_pcm_open(&st->write, device, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		DEBUG_WARNING("open: %s %s\n", device, snd_strerror(err));
		goto out;
	}

	st->mbw = mbuf_alloc(st->sample_size * prm->frame_size);
	if (!st->mbw) {
		err = ENOMEM;
		goto out;
	}

	err = alsa_reset(st->write, prm->srate, prm->ch, prm->fmt,
			 prm->frame_size);
	if (err)
		goto out;

	st->run = true;
	err = pthread_create(&st->thread, NULL, write_thread, st);
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
