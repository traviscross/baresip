/**
 * @file auloop.c  Audio loop
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>


#define DEBUG_MODULE "auloop"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/* Configurable items */
#define PTIME 20


/** Audio Loop */
struct audio_loop {
	uint32_t index;
	struct aubuf *ab;
	struct ausrc_st *ausrc;
	struct auplay_st *auplay;
	const struct aucodec *ac;
	struct auenc_state *enc;
	struct audec_state *dec;
	uint32_t srate;
	uint32_t ch;
	uint32_t fs;
	uint32_t n_read;
	uint32_t n_write;
};

static const struct {
	uint32_t srate;
	uint32_t ch;
} configv[] = {
	{ 8000, 1},
	{16000, 1},
	{32000, 1},
	{48000, 1},
	{ 8000, 2},
	{16000, 2},
	{32000, 2},
	{48000, 2},
};

static struct audio_loop *gal = NULL;
static char aucodec[64];


static void auloop_destructor(void *arg)
{
	struct audio_loop *al = arg;

	mem_deref(al->ausrc);
	mem_deref(al->auplay);
	mem_deref(al->ab);
	mem_deref(al->enc);
	mem_deref(al->dec);
}


static void print_stats(struct audio_loop *al)
{
	(void)re_fprintf(stderr, "\r%uHz %dch frame_size=%u"
			 " n_read=%u n_write=%u"
			 " aubuf=%5u codec=%s",
			 al->srate, al->ch, al->fs,
			 al->n_read, al->n_write,
			 aubuf_cur_size(al->ab), aucodec);
}


static int codec_read(struct audio_loop *al, uint8_t *buf, size_t sz)
{
	int16_t *sampv;
	uint8_t x[1024];
	size_t xlen = sizeof(x), sampc = sz/2;
	int err;

	sampv = mem_alloc(sz, NULL);
	if (!sampv) {
		err = ENOMEM;
		goto out;
	}

	aubuf_read_samp(al->ab, sampv, sampc);

	err = al->ac->ench(al->enc, x, &xlen, sampv, sampc);
	if (err)
		goto out;

	err = al->ac->dech(al->dec, (void *)buf, &sampc, x, xlen);
	if (err)
		goto out;

 out:
	mem_deref(sampv);

	return err;
}


static void read_handler(const uint8_t *buf, size_t sz, void *arg)
{
	struct audio_loop *al = arg;
	int err;

	++al->n_read;

	err = aubuf_write(al->ab, buf, sz);
	if (err) {
		DEBUG_WARNING("aubuf_write: %m\n", err);
	}

	print_stats(al);
}


static bool write_handler(uint8_t *buf, size_t sz, void *arg)
{
	struct audio_loop *al = arg;

	++al->n_write;

	/* read from beginning */
	if (al->ac) {
		(void)codec_read(al, buf, sz);
	}
	else {
		aubuf_read(al->ab, buf, sz);
	}

	return true;
}


static void error_handler(int err, const char *str, void *arg)
{
	(void)arg;
	DEBUG_WARNING("error: %m (%s)\n", err, str);
	gal = mem_deref(gal);
}


static void start_codec(struct audio_loop *al, const char *name)
{
	struct auenc_param prm = {PTIME};
	int err;

	al->ac = aucodec_find(name,
			      configv[al->index].srate,
			      configv[al->index].ch);
	if (!al->ac) {
		DEBUG_WARNING("could not find codec: %s\n", name);
		return;
	}

	if (al->ac->encupdh) {
		err = al->ac->encupdh(&al->enc, al->ac, &prm, NULL);
		if (err) {
			DEBUG_WARNING("encoder update failed: %m\n", err);
		}
	}

	if (al->ac->decupdh) {
		err = al->ac->decupdh(&al->dec, al->ac, NULL);
		if (err) {
			DEBUG_WARNING("decoder update failed: %m\n", err);
		}
	}
}


static int auloop_reset(struct audio_loop *al)
{
	struct auplay_prm auplay_prm;
	struct ausrc_prm ausrc_prm;
	const struct config *cfg = conf_config();
	int err;

	if (!cfg)
		return ENOENT;

	/* Optional audio codec */
	if (str_isset(aucodec))
		start_codec(al, aucodec);

	al->auplay = mem_deref(al->auplay);
	al->ausrc = mem_deref(al->ausrc);
	al->ab = mem_deref(al->ab);

	al->srate = configv[al->index].srate;
	al->ch    = configv[al->index].ch;
	al->fs    = al->srate * al->ch * PTIME / 1000;

	(void)re_printf("Audio-loop: %uHz, %dch\n", al->srate, al->ch);

	err = aubuf_alloc(&al->ab, 320, 0);
	if (err)
		return err;

	auplay_prm.fmt        = AUFMT_S16LE;
	auplay_prm.srate      = al->srate;
	auplay_prm.ch         = al->ch;
	auplay_prm.frame_size = al->fs;
	err = auplay_alloc(&al->auplay, cfg->audio.play_mod, &auplay_prm,
			   cfg->audio.play_dev, write_handler, al);
	if (err) {
		DEBUG_WARNING("auplay %s,%s failed: %m\n",
			      cfg->audio.play_mod, cfg->audio.play_dev,
			      err);
		return err;
	}

	ausrc_prm.fmt        = AUFMT_S16LE;
	ausrc_prm.srate      = al->srate;
	ausrc_prm.ch         = al->ch;
	ausrc_prm.frame_size = al->fs;
	err = ausrc_alloc(&al->ausrc, NULL, cfg->audio.src_mod,
			  &ausrc_prm, cfg->audio.src_dev,
			  read_handler, error_handler, al);
	if (err) {
		DEBUG_WARNING("ausrc %s,%s failed: %m\n", cfg->audio.src_mod,
			      cfg->audio.src_dev, err);
		return err;
	}

	return err;
}


static int audio_loop_alloc(struct audio_loop **alp)
{
	struct audio_loop *al;
	int err;

	al = mem_zalloc(sizeof(*al), auloop_destructor);
	if (!al)
		return ENOMEM;

	err = auloop_reset(al);
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(al);
	else
		*alp = al;

	return err;
}


static int audio_loop_cycle(struct audio_loop *al)
{
	int err;

	++al->index;

	if (al->index >= ARRAY_SIZE(configv)) {
		gal = mem_deref(gal);
		(void)re_printf("\nAudio-loop stopped\n");
		return 0;
	}

	err = auloop_reset(al);
	if (err)
		return err;

	(void)re_printf("\nAudio-loop started: %uHz, %dch\n",
			al->srate, al->ch);

	return 0;
}


/**
 * Start the audio loop (for testing)
 */
static int auloop_start(struct re_printf *pf, void *arg)
{
	int err;

	(void)pf;
	(void)arg;

	if (gal) {
		err = audio_loop_cycle(gal);
		if (err) {
			DEBUG_WARNING("cycle: %m\n", err);
		}
	}
	else {
		err = audio_loop_alloc(&gal);
		if (err) {
			DEBUG_WARNING("auloop: %m\n", err);
		}
	}

	return err;
}


static int auloop_stop(struct re_printf *pf, void *arg)
{
	(void)arg;

	if (gal) {
		(void)re_hprintf(pf, "audio-loop stopped\n");
		gal = mem_deref(gal);
	}

	return 0;
}


static const struct cmd cmdv[] = {
	{'a', 0, "Start audio-loop", auloop_start },
	{'A', 0, "Stop audio-loop",  auloop_stop  },
};


static int module_init(void)
{
	conf_get_str(conf_cur(), "auloop_codec", aucodec, sizeof(aucodec));

	return cmd_register(cmdv, ARRAY_SIZE(cmdv));
}


static int module_close(void)
{
	auloop_stop(NULL, NULL);
	cmd_unregister(cmdv);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(auloop) = {
	"auloop",
	"application",
	module_init,
	module_close,
};
