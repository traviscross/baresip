/**
 * @file aufile.c Audio file (WAV-file) reader
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdio.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "aufile"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


enum {
	WAVE_FMT_UNKNOWN  = 0x0000,
	WAVE_FMT_PCM      = 0x0001,
	WAVE_FMT_ALAW     = 0x0006,
	WAVE_FMT_ULAW     = 0x0007,
	WAVE_FMT_GSM610   = 0x0031,
};

struct wav_chunk {
	uint8_t id[4]; /* "RIFF" */
	uint32_t size;
};

/* Total 30 bytes */
struct wav_header {
	/* 12 bytes */
	struct wav_chunk header;
	char rifftype[4];        /* "WAVE" */

	/* 18 bytes */
	struct wav_chunk fmt;
	uint16_t comp_code;
	uint16_t channels;
	uint32_t srate;
	uint32_t avg_bps;
	uint16_t block_align;
	uint16_t sign_bps;
	uint16_t extra_fmt;
};


/** Convert from little endian to host order */
static void wav_header_convert(struct wav_header *hdr)
{
	if (!hdr)
		return;

	hdr->header.size = sys_ltohl(hdr->header.size);
	hdr->fmt.size    = sys_ltohl(hdr->fmt.size);
	hdr->comp_code   = sys_ltohs(hdr->comp_code);
	hdr->channels    = sys_ltohs(hdr->channels);
	hdr->srate       = sys_ltohl(hdr->srate);
	hdr->avg_bps     = sys_ltohl(hdr->avg_bps);
	hdr->block_align = sys_ltohs(hdr->block_align);
	hdr->sign_bps    = sys_ltohs(hdr->sign_bps);
	hdr->extra_fmt   = sys_ltohs(hdr->extra_fmt);
}


static int wav_header_check(const struct wav_header *hdr)
{
	if (!hdr)
		return EINVAL;

	if (memcmp(hdr->header.id, "RIFF", 4)) {
		DEBUG_WARNING("wav: expected RIFF (%c%c%c%c)\n",
			      hdr->header.id[0], hdr->header.id[1],
			      hdr->header.id[2], hdr->header.id[3]);
		return EINVAL;
	}

	if (memcmp(hdr->rifftype, "WAVE", 4)) {
		DEBUG_WARNING("wav: expected WAVE (%c%c%c%c)\n",
			      hdr->rifftype[0], hdr->rifftype[1],
			      hdr->rifftype[2], hdr->rifftype[3]);
		return EINVAL;
	}

	if (memcmp(hdr->fmt.id, "fmt ", 4)) {
		DEBUG_WARNING("wav: expected fmt (%c%c%c%c)\n",
			      hdr->fmt.id[0], hdr->fmt.id[1],
			      hdr->fmt.id[2], hdr->fmt.id[3]);
		return EINVAL;
	}

	return 0;
}


static int wav_header_read(FILE *f, struct wav_header *hdr)
{
	const size_t n = fread((void *)hdr, sizeof(*hdr), 1, f);
	if (1 != n) {
		DEBUG_WARNING("error reading wav header (n=%d)\n", n);
		return EINVAL;
	}

	wav_header_convert(hdr);

	return wav_header_check(hdr);
}


int aufile_load(struct mbuf *mbf, const char *filename,
		uint32_t *srate, uint8_t *channels)
{
	char path[256] = "", file[256] = "";
	struct wav_header hdr;
	const char *cname = NULL;
	struct aucodec_st *codec = NULL;
	FILE *f = NULL;
	int err;

	if (!mbf || !filename)
		return EINVAL;

	err = conf_path_get(path, sizeof(path));
	if (err)
		return err;

	/* First try conf path */
	if (re_snprintf(file, sizeof(file), "%s/%s", path, filename) < 0)
		return ENOMEM;

	f = fopen(file, "r");
	if (!f) {
		/* Then try /usr/share */
		if (re_snprintf(file, sizeof(file), "/usr/share/baresip/%s",
				filename) < 0)
			return ENOMEM;
		f = fopen(file, "r");
		if (!f) {
			err = errno;
			DEBUG_WARNING("alloc: %s: %s\n", filename,
				      strerror(err));
			goto out;
		}
	}

	err = wav_header_read(f, &hdr);
	if (err)
		goto out;

	/* check format, find decoder */
	switch (hdr.comp_code) {

	case WAVE_FMT_PCM:
		/* No decoder needed */
		break;

	case WAVE_FMT_ALAW:
		cname = "pcma";
		break;

	case WAVE_FMT_ULAW:
		cname = "pcmu";
		break;

	case WAVE_FMT_GSM610:
		cname = "gsm";
		break;

	default:
		err = ENOENT;
		break;
	}

	if (cname) {
		err = aucodec_alloc(&codec, cname, hdr.srate,
				    hdr.channels, NULL, NULL, NULL);
		if (err) {
			DEBUG_WARNING("could not find decoder: %u %uHz %dch\n",
				      hdr.comp_code, hdr.srate, hdr.channels);
			goto out;
		}
	}

	for (;;) {
		struct mbuf *mb;
		size_t n;

		mb = mbuf_alloc(8192);
		if (!mb) {
			err = ENOMEM;
			break;
		}

		n = fread(mb->buf, 1, mb->size, f);
		if (ferror(f)) {
			err = errno;
		}
		else if (n > 0) {
			mb->end = n;

			if (codec) {
				err = aucodec_get(codec)->dech(codec, mbf, mb);
			}
			else {
				err = mbuf_write_mem(mbf, mb->buf, mb->end);
			}
		}

		mem_deref(mb);

		if (err || !n)
			break;
	}

	mbf->pos = 0;

	if (srate)
		*srate = hdr.srate;

	if (channels)
		*channels = (uint8_t) (hdr.channels & 0xff);

 out:
	mem_deref(codec);
	if (f)
		(void)fclose(f);

	return err;
}
