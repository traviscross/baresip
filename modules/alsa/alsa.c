/**
 * @file alsa.c  ALSA sound driver
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _POSIX_SOURCE 1
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <re.h>
#include <baresip.h>
#include "alsa.h"


#define DEBUG_MODULE "alsa"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


char alsa_dev[64] = "default";

static struct ausrc *ausrc;
static struct auplay *auplay;


int alsa_reset(snd_pcm_t *pcm, uint32_t srate, uint8_t ch)
{
	snd_pcm_hw_params_t *hw_params = NULL;
	int err;

	err = snd_pcm_hw_params_malloc(&hw_params);
	if (err < 0) {
		DEBUG_WARNING("cannot allocate hw params (%s)\n",
			      snd_strerror(err));
		goto out;
	}

	err = snd_pcm_hw_params_any(pcm, hw_params);
	if (err < 0) {
		DEBUG_WARNING("cannot initialize hw params (%s)\n",
			      snd_strerror(err));
		goto out;
	}

	err = snd_pcm_hw_params_set_access(pcm, hw_params,
					   SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0) {
		DEBUG_WARNING("cannot set access type (%s)\n",
			      snd_strerror(err));
		goto out;
	}

	err = snd_pcm_hw_params_set_format(pcm, hw_params,
					   SND_PCM_FORMAT_S16_LE);
	if (err < 0) {
		DEBUG_WARNING("cannot set sample format (%s)\n",
			      snd_strerror(err));
		goto out;
	}

	err = snd_pcm_hw_params_set_rate(pcm, hw_params, srate, 0);
	if (err < 0) {
		DEBUG_WARNING("cannot set sample rate (%s)\n",
			      snd_strerror(err));
		goto out;
	}

	err = snd_pcm_hw_params_set_channels(pcm, hw_params, ch);
	if (err < 0) {
		DEBUG_WARNING("cannot set channel count to %d (%s)\n",
			      ch, snd_strerror(err));
		goto out;
	}

	err = snd_pcm_hw_params(pcm, hw_params);
	if (err < 0) {
		DEBUG_WARNING("cannot set parameters (%s)\n",
			      snd_strerror(err));
		goto out;
	}

	err = snd_pcm_prepare(pcm);
	if (err < 0) {
		DEBUG_WARNING("cannot prepare audio interface for use (%s)\n",
			      snd_strerror(err));
		goto out;
	}

	err = 0;

 out:
	snd_pcm_hw_params_free(hw_params);

	if (err) {
		DEBUG_WARNING("init failed: err=%d\n", err);
	}

	return err;
}


static int alsa_init(void)
{
	int err;

	err  = ausrc_register(&ausrc, "alsa", alsa_src_alloc);
	err |= auplay_register(&auplay, "alsa", alsa_play_alloc);

	return err;
}


static int alsa_close(void)
{
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);

	return 0;
}


const struct mod_export DECL_EXPORTS(alsa) = {
	"alsa",
	"sound",
	alsa_init,
	alsa_close
};
