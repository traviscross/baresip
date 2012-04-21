/**
 * @file h263.c  H.263 video codec (RFC 4629)
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <libavcodec/avcodec.h>
#ifdef USE_X264
#include <x264.h>
#endif
#include "h26x.h"
#include "avcodec.h"


#define DEBUG_MODULE "avcodec_h263"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


int h263_hdr_encode(const struct h263_hdr *hdr, struct mbuf *mb)
{
	uint32_t v; /* host byte order */

	v  = hdr->f<<31 | hdr->p<<30 | hdr->sbit<<27 | hdr->ebit<<24;
	v |= hdr->src<<21 | hdr->i<<20 | hdr->u<<19 | hdr->s<<18 | hdr->a<<17;
	v |= hdr->r<<13 | hdr->dbq<<11 | hdr->trb<<8 | hdr->tr<<0;

	return mbuf_write_u32(mb, htonl(v));
}


int h263_hdr_decode(struct h263_hdr *hdr, struct mbuf *mb)
{
	uint32_t v;
	enum h263_mode mode;

	if (mbuf_get_left(mb) < H263_HDR_SIZE_MODEA)
		return ENOENT;

	v = ntohl(mbuf_read_u32(mb));

	/* Common */
	hdr->f    = v>>31 & 0x1;
	hdr->p    = v>>30 & 0x1;
	hdr->sbit = v>>27 & 0x7;
	hdr->ebit = v>>24 & 0x7;
	hdr->src  = v>>21 & 0x7;

	if (hdr->f == 0)
		mode = H263_MODE_A;
	else if (hdr->p == 0)
		mode = H263_MODE_B;
	else
		mode = H263_MODE_C;

	switch (mode) {

	case H263_MODE_A:
		hdr->i    = v>>20 & 0x1;
		hdr->u    = v>>19 & 0x1;
		hdr->s    = v>>18 & 0x1;
		hdr->a    = v>>17 & 0x1;
		hdr->r    = v>>13 & 0xf;
		hdr->dbq  = v>>11 & 0x3;
		hdr->trb  = v>>8  & 0x7;
		hdr->tr   = v>>0  & 0xff;
		break;

	case H263_MODE_B:
		/* todo: decode Mode B header */
		v = ntohl(mbuf_read_u32(mb));
		break;

	case H263_MODE_C:
		/* todo: decode Mode C header */
		v = ntohl(mbuf_read_u32(mb));
		v = ntohl(mbuf_read_u32(mb));
		break;
	}

	return 0;
}


/** Find PSC (Picture Start Code) in bit-stream */
const uint8_t *h263_strm_find_psc(const uint8_t *p, uint32_t size)
{
	const uint8_t *end = p + size - 1;

	for (; p < end; p++) {
		if (p[0] == 0x00 && p[1] == 0x00)
			return p;
	}

	return NULL;
}


int h263_strm_decode(struct h263_strm *s, struct mbuf *mb)
{
	const uint8_t *p;

	if (mbuf_get_left(mb) < 6)
		return EINVAL;

	p = mbuf_buf(mb);

	s->psc[0] = p[0];
	s->psc[1] = p[1];

	s->temp_ref = (p[2]<<6 & 0xc0) | (p[3]>>2 & 0x3f);

	s->split_scr        = p[4]>>7 & 0x1;
	s->doc_camera       = p[4]>>6 & 0x1;
	s->pic_frz_rel      = p[4]>>5 & 0x1;
	s->src_fmt          = p[4]>>2 & 0x7;
	s->pic_type         = p[4]>>1 & 0x1;
	s->umv              = p[4]>>0 & 0x1;

	s->sac              = p[5]>>7 & 0x1;
	s->apm              = p[5]>>6 & 0x1;
	s->pb               = p[5]>>5 & 0x1;
	s->pquant           = p[5]>>0 & 0x1f;

	s->cpm              = p[6]>>7 & 0x1;
	s->pei              = p[6]>>6 & 0x1;

	return 0;
}


/** Copy H.263 bit-stream to H.263 RTP payload header */
void h263_hdr_copy_strm(struct h263_hdr *hdr, const struct h263_strm *s)
{
	hdr->f    = 0;  /* Mode A */
	hdr->p    = 0;
	hdr->sbit = 0;
	hdr->ebit = 0;
	hdr->src  = s->src_fmt;
	hdr->i    = s->pic_type;
	hdr->u    = s->umv;
	hdr->s    = s->sac;
	hdr->a    = s->apm;
	hdr->r    = 0;
	hdr->dbq  = 0;   /* No PB-frames */
	hdr->trb  = 0;   /* No PB-frames */
	hdr->tr   = s->temp_ref;
}


static enum h263_fmt h263_fmt(const struct pl *name)
{
	if (0 == pl_strcasecmp(name, "sqcif")) return H263_FMT_SQCIF;
	if (0 == pl_strcasecmp(name, "qcif"))  return H263_FMT_QCIF;
	if (0 == pl_strcasecmp(name, "cif"))   return H263_FMT_CIF;
	if (0 == pl_strcasecmp(name, "cif4"))  return H263_FMT_4CIF;
	if (0 == pl_strcasecmp(name, "cif16")) return H263_FMT_16CIF;
	return H263_FMT_OTHER;
}


int decode_sdpparam_h263(struct vidcodec_st *st, const struct pl *name,
			 const struct pl *val)
{
	enum h263_fmt fmt = h263_fmt(name);
	const int mpi = pl_u32(val);

	if (fmt == H263_FMT_OTHER) {
		DEBUG_NOTICE("h263: unknown param '%r'\n", name);
		return 0;
	}
	if (mpi < 1 || mpi > 32) {
		DEBUG_NOTICE("h263: %r: MPI out of range %d\n", name, mpi);
		return 0;
	}

	if (st->u.h263.picszn >= ARRAY_SIZE(st->u.h263.picszv)) {
		DEBUG_NOTICE("h263: picszv overflow: %r\n", name);
		return 0;
	}

	st->u.h263.picszv[st->u.h263.picszn].fmt = fmt;
	st->u.h263.picszv[st->u.h263.picszn].mpi = mpi;

	++st->u.h263.picszn;

	return 0;
}


int h263_packetize(struct vidcodec_st *st, struct mbuf *mb)
{
	struct h263_strm h263_strm;
	struct h263_hdr h263_hdr;
	size_t pos;
	int err;

	/* Decode bit-stream header, used by packetizer */
	err = h263_strm_decode(&h263_strm, mb);
	if (err)
		return err;

	h263_hdr_copy_strm(&h263_hdr, &h263_strm);

	/* Make space for RTP header */
	st->mb_frag->pos = st->mb_frag->end = RTP_PRESZ;
	err = h263_hdr_encode(&h263_hdr, st->mb_frag);
	pos = st->mb_frag->pos;

	/* Assemble frame into smaller packets */
	while (!err) {
		size_t sz, left = mbuf_get_left(mb);
		bool last = (left < MAX_RTP_SIZE);
		if (!left)
			break;

		sz = last ? left : MAX_RTP_SIZE;

		st->mb_frag->pos = st->mb_frag->end = pos;
		err = mbuf_write_mem(st->mb_frag, mbuf_buf(mb), sz);
		if (err)
			break;

		st->mb_frag->pos = RTP_PRESZ;
		err = st->sendh(last, st->mb_frag, st->arg);

		mbuf_advance(mb, sz);
	}

	return err;
}
