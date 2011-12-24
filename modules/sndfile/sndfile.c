/**
 * @file sndfile.c  Audio dumper using libsndfile
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <sndfile.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "sndfile"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct aufilt_st {
	struct aufilt *af;  /* base class */
	SNDFILE *enc, *dec;
};


static struct aufilt *filt;
static uint32_t count = 0;


static void sndfile_destructor(void *arg)
{
	struct aufilt_st *st = arg;

	if (st->enc)
		sf_close(st->enc);
	if (st->dec)
		sf_close(st->dec);

	mem_deref(st->af);
}


static int alloc(struct aufilt_st **stp, struct aufilt *af,
		 const struct aufilt_prm *encprm,
		 const struct aufilt_prm *decprm)
{
	char filename_enc[128], filename_dec[128];
	SF_INFO sfinfo_enc, sfinfo_dec;
	struct aufilt_st *st;

	st = mem_zalloc(sizeof(*st), sndfile_destructor);
	if (!st)
		return EINVAL;

	st->af = mem_ref(af);

	(void)re_snprintf(filename_enc, sizeof(filename_enc),
			  "dump-%u-enc.wav", count);
	(void)re_snprintf(filename_dec, sizeof(filename_dec),
			  "dump-%u-dec.wav", count);

	sfinfo_enc.samplerate = encprm->srate;
	sfinfo_enc.channels   = encprm->ch;
	sfinfo_enc.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

	sfinfo_dec.samplerate = decprm->srate;
	sfinfo_dec.channels   = decprm->ch;
	sfinfo_dec.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

	st->enc = sf_open(filename_enc, SFM_WRITE, &sfinfo_enc);
	if (!st->enc) {
		DEBUG_WARNING("could not open: %s\n", filename_enc);
		puts(sf_strerror(NULL));
		goto error;
	}

	st->dec = sf_open(filename_dec, SFM_WRITE, &sfinfo_dec);
	if (!st->dec) {
		DEBUG_WARNING("could not open: %s\n", filename_dec);
		puts(sf_strerror(NULL));
		goto error;
	}

	DEBUG_NOTICE("dumping audio to %s and %s\n",
		     filename_enc, filename_dec);

	++count;
	*stp = st;
	return 0;

 error:
	mem_deref(st);
	return ENOMEM;
}


static int enc(struct aufilt_st *st, struct mbuf *mb)
{
	sf_write_short(st->enc, (short *)mbuf_buf(mb), mbuf_get_left(mb)/2);

	return 0;
}


static int dec(struct aufilt_st *st, struct mbuf *mb)
{
	sf_write_short(st->dec, (short *)mbuf_buf(mb), mbuf_get_left(mb)/2);

	return 0;
}


static int module_init(void)
{
	return aufilt_register(&filt, "sndfile", alloc, enc, dec, NULL);
}


static int module_close(void)
{
	filt = mem_deref(filt);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(sndfile) = {
	"sndfile",
	"filter",
	module_init,
	module_close
};
