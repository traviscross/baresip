/**
 * @file play.c  Audio-file player
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "play"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


enum {SILENCE_DUR = 2000};

/** Audio file player */
struct play {
	struct le le;
	struct play **playp;
	struct lock *lock;
	struct mbuf *what;
	struct auplay_st *auplay;
	struct tmr tmr;
	int repeat;
	bool eof;
};


static struct list playl;


static void tmr_polling(void *arg);


static void tmr_stop(void *arg)
{
	struct play *play = arg;
	mem_deref(play);
}


static void tmr_repeat(void *arg)
{
	struct play *play = arg;

	lock_write_get(play->lock);

	play->what->pos = 0;
	play->eof = false;

	tmr_start(&play->tmr, 1000, tmr_polling, arg);

	lock_rel(play->lock);
}


static void tmr_polling(void *arg)
{
	struct play *play = arg;

	lock_write_get(play->lock);

	tmr_start(&play->tmr, 1000, tmr_polling, arg);

	if (play->eof) {
		if (play->repeat > 0)
			play->repeat--;

		if (play->repeat == 0)
			tmr_start(&play->tmr, 1, tmr_stop, arg);
		else
			tmr_start(&play->tmr, SILENCE_DUR, tmr_repeat, arg);
	}

	lock_rel(play->lock);
}


/**
 * NOTE: DSP cannot be destroyed inside handler
 */
static bool write_handler(uint8_t *buf, size_t sz, void *arg)
{
	struct play *play = arg;

	lock_write_get(play->lock);

	if (play->eof)
		goto silence;

	if (mbuf_get_left(play->what) < sz) {
		play->eof = true;
	}
	else {
		(void)mbuf_read_mem(play->what, buf, sz);
	}

 silence:
	if (play->eof)
		memset(buf, 0, sz);

	lock_rel(play->lock);

	return true;
}


static void destructor(void *arg)
{
	struct play *play = arg;

	list_unlink(&play->le);
	tmr_cancel(&play->tmr);

	lock_write_get(play->lock);
	play->eof = true;
	lock_rel(play->lock);

	mem_deref(play->auplay);
	mem_deref(play->what);
	mem_deref(play->lock);

	if (play->playp)
		*play->playp = NULL;
}


static int play_alloc(struct play **playp, struct mbuf *what,
		      struct auplay_prm *wprm, int repeat)
{
	struct play *play;
	int err;

	play = mem_zalloc(sizeof(*play), destructor);
	if (!play)
		return ENOMEM;

	tmr_init(&play->tmr);
	play->repeat = repeat;
	play->what = mem_ref(what);

	err = lock_alloc(&play->lock);
	if (err)
		goto out;

	err = auplay_alloc(&play->auplay, config.audio.play_mod, wprm,
			   config.audio.play_dev, write_handler, play);
	if (err)
		goto out;

	list_append(&playl, &play->le, play);
	tmr_start(&play->tmr, 1000, tmr_polling, play);

 out:
	if (err) {
		mem_deref(play);
	}
	else if (playp) {
		play->playp = playp;
		*playp = play;
	}

	return err;
}


static int aufile_load(struct mbuf *mb, const char *filename,
		       uint32_t *srate, uint8_t *channels)
{
	struct aufile_prm prm;
	struct aufile *af;
	int err;

	err = aufile_open(&af, &prm, filename, AUFILE_READ);
	if (err)
		return err;

	while (!err) {
		uint8_t buf[4096];
		size_t i, n;

		n = sizeof(buf);

		err = aufile_read(af, buf, &n);
		if (err || !n)
			break;

		switch (prm.fmt) {

		case AUFMT_S16LE:
			err = mbuf_write_mem(mb, buf, n);
			break;

		case AUFMT_PCMA:
			for (i=0; i<n; i++) {
				err |= mbuf_write_u16(mb,
						      g711_alaw2pcm(buf[i]));
			}
			break;

		case AUFMT_PCMU:
			for (i=0; i<n; i++) {
				err |= mbuf_write_u16(mb,
						      g711_ulaw2pcm(buf[i]));
			}
			break;

		default:
			err = ENOSYS;
			break;
		}
	}

	mem_deref(af);

	if (!err) {
		mb->pos = 0;

		*srate    = prm.srate;
		*channels = (uint8_t)prm.channels;
	}

	return err;
}


/**
 * Play a tone from a PCM buffer
 *
 * @param playp    Pointer to allocated player object
 * @param tone     PCM buffer to play
 * @param srate    Sampling rate
 * @param ch       Number of channels
 * @param repeat   Number of times to repeat
 *
 * @return 0 if success, otherwise errorcode
 */
int play_tone(struct play **playp, struct mbuf *tone, uint32_t srate,
	      uint8_t ch, int repeat)
{
	struct auplay_prm wprm;

	if (playp && *playp)
		return EALREADY;

	wprm.fmt = AUFMT_S16LE;
	wprm.ch = ch;
	wprm.srate = srate;
	wprm.frame_size = calc_nsamp(wprm.srate, wprm.ch, 20);

	return play_alloc(playp, tone, &wprm, repeat);
}


/**
 * Play an audio file in WAV format
 *
 * @param playp    Pointer to allocated player object
 * @param filename Name of WAV file to play
 * @param repeat   Number of times to repeat
 *
 * @return 0 if success, otherwise errorcode
 */
int play_file(struct play **playp, const char *filename, int repeat)
{
	struct auplay_prm wprm;
	struct mbuf *mb;
	char path[256];
	int err;

	if (playp && *playp)
		return EALREADY;

#ifndef PREFIX
#define PREFIX "/usr"
#endif
	if (re_snprintf(path, sizeof(path), PREFIX "/share/baresip/%s",
			filename) < 0)
		return ENOMEM;

	wprm.fmt = AUFMT_S16LE;
	mb = mbuf_alloc(1024);
	if (!mb)
		return ENOMEM;

	err = aufile_load(mb, path, &wprm.srate, &wprm.ch);
	if (err) {
		DEBUG_WARNING("%s: %s\n", path, strerror(err));
		goto out;
	}

	wprm.frame_size = calc_nsamp(wprm.srate, wprm.ch, 20);

	err = play_alloc(playp, mb, &wprm, repeat);

 out:
	mem_deref(mb);

	return err;
}


/**
 * Close all active audio players
 */
void play_close(void)
{
	list_flush(&playl);
}
