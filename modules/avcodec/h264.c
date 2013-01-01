/**
 * @file h264.c  H.264 video codec (RFC 3984)
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <libavcodec/avcodec.h>
#ifdef USE_X264
#include <x264.h>
#endif
#include "h26x.h"
#include "avcodec.h"


#define DEBUG_MODULE "avcodec_h264"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


int h264_hdr_encode(const struct h264_hdr *hdr, struct mbuf *mb)
{
	uint8_t v;

	v = hdr->f<<7 | hdr->nri<<5 | hdr->type<<0;

	return mbuf_write_u8(mb, v);
}


int h264_hdr_decode(struct h264_hdr *hdr, struct mbuf *mb)
{
	uint8_t v;

	if (mbuf_get_left(mb) < 1)
		return ENOENT;

	v = mbuf_read_u8(mb);

	hdr->f    = v>>7 & 0x1;
	hdr->nri  = v>>5 & 0x3;
	hdr->type = v>>0 & 0x1f;

	return 0;
}


int fu_hdr_encode(const struct fu *fu, struct mbuf *mb)
{
	uint8_t v = fu->s<<7 | fu->s<<6 | fu->r<<5 | fu->type;
	return mbuf_write_u8(mb, v);
}


int fu_hdr_decode(struct fu *fu, struct mbuf *mb)
{
	uint8_t v;

	if (mbuf_get_left(mb) < 1)
		return ENOENT;

	v = mbuf_read_u8(mb);

	fu->s    = v>>7 & 0x1;
	fu->e    = v>>6 & 0x1;
	fu->r    = v>>5 & 0x1;
	fu->type = v>>0 & 0x1f;

	return 0;
}


/*
 * Find the NAL start sequence in a H.264 byte stream
 *
 * @note: copied from ffmpeg source
 */
const uint8_t *h264_find_startcode(const uint8_t *p, const uint8_t *end)
{
	const uint8_t *a = p + 4 - ((long)p & 3);

	for (end -= 3; p < a && p < end; p++ ) {
		if (p[0] == 0 && p[1] == 0 && p[2] == 1)
			return p;
	}

	for (end -= 3; p < end; p += 4) {
		uint32_t x = *(const uint32_t*)p;
		if ( (x - 0x01010101) & (~x) & 0x80808080 ) {
			if (p[1] == 0 ) {
				if ( p[0] == 0 && p[2] == 1 )
					return p;
				if ( p[2] == 0 && p[3] == 1 )
					return p+1;
			}
			if ( p[3] == 0 ) {
				if ( p[2] == 0 && p[4] == 1 )
					return p+2;
				if ( p[4] == 0 && p[5] == 1 )
					return p+3;
			}
		}
	}

	for (end += 3; p < end; p++) {
		if (p[0] == 0 && p[1] == 0 && p[2] == 1)
			return p;
	}

	return end + 3;
}


int h264_decode_sprop_params(AVCodecContext *codec, struct pl *pl)
{
	static const uint8_t start_seq[] = {0, 0, 1};
	int err = 0;

	codec->extradata_size = 0;
	codec->extradata      = NULL;

	while (pl->l > 0) {
		uint8_t buf[1024], *dest;
		size_t olen, len;
		struct pl val, comma;

		comma.l = 0;
		err = re_regex(pl->p, pl->l, "[^,]+[,]*", &val, &comma);
		if (err)
			break;

		pl_advance(pl, val.l + comma.l);

		olen = sizeof(buf);
		err = base64_decode(val.p, val.l, buf, &olen);
		if (err)
			continue;

		len = olen + sizeof(start_seq) + codec->extradata_size;
		dest = av_malloc((unsigned int)len);
		if (!dest) {
			err = ENOMEM;
			break;
		}

		if (codec->extradata_size) {
			memcpy(dest, codec->extradata, codec->extradata_size);
			av_free(codec->extradata);
		}

		memcpy(dest + codec->extradata_size, start_seq,
		       sizeof(start_seq));
		memcpy(dest + codec->extradata_size + sizeof(start_seq),
		       buf, olen);

		codec->extradata       = dest;
		codec->extradata_size += (int)(sizeof(start_seq) + olen);
	}

	return err;
}


#if 0
static int add_sprop_params(struct mbuf *mb, const AVCodecContext *c)
{
	const uint8_t *r;
	bool first = true;
	int err;

	err = mbuf_printf(mb, ";sprop-parameter-sets=");

	r = h264_find_startcode(c->extradata,
				c->extradata + c->extradata_size);

	/* comma-separated list of parameters */
	while (r < c->extradata + c->extradata_size && !err) {
		char buf[1024];
		size_t olen;
		const uint8_t *r1;

		while (!*(r++))
			;

		r1 = h264_find_startcode(r, c->extradata + c->extradata_size);

		olen = sizeof(buf);
		err = base64_encode(r, r1 - r, buf, &olen);
		if (err)
			break;

		re_printf("add sprop nal %u (%s)\n", r[0] & 0x1f, buf);

		err = mbuf_printf(mb, "%s%b", first ? "" : ",", buf, olen);

		first = false;
		r = r1;
	}

	return err;
}
#endif


int decode_sdpparam_h264(struct vidcodec_st *st, const struct pl *name,
			 const struct pl *val)
{
	if (0 == pl_strcasecmp(name, "packetization-mode")) {
		st->u.h264.packetization_mode = pl_u32(val);

		if (st->u.h264.packetization_mode != 0) {
			DEBUG_WARNING("illegal packetization-mode %u\n",
				      st->u.h264.packetization_mode);
			return EPROTO;
		}
	}
	else if (0 == pl_strcasecmp(name, "profile-level-id")) {
		struct pl prof = *val;
		if (prof.l != 6) {
			DEBUG_WARNING("invalid profile-level-id (%r)\n", val);
			return EPROTO;
		}

		prof.l = 2;
		st->u.h264.profile_idc = pl_x32(&prof); prof.p += 2;
		st->u.h264.profile_iop = pl_x32(&prof); prof.p += 2;
		st->u.h264.level_idc   = pl_x32(&prof);
	}
#if 0
	else if (0 == pl_strcasecmp(name, "sprop-parameter-sets")) {
		decode_sprop_params(h, val);
	}
#endif
	else if (0 == pl_strcasecmp(name, "max-fs")) {
		st->u.h264.max_fs = pl_u32(val);
	}
	else if (0 == pl_strcasecmp(name, "max-smbps")) {
		st->u.h264.max_smbps = pl_u32(val);
	}

	return 0;
}


static int rtp_send_data(struct vidcodec_st *st,
			 const uint8_t *hdr, size_t hdr_sz,
			 const uint8_t *buf, size_t sz, bool eof)
{
	int err;

	/* Make space for RTP and TURN header */
	st->mb_frag->pos = st->mb_frag->end = RTP_PRESZ;

	err  = mbuf_write_mem(st->mb_frag, hdr, hdr_sz);
	err |= mbuf_write_mem(st->mb_frag, buf, sz);

	st->mb_frag->pos = RTP_PRESZ;
	err |= st->sendh(eof, st->mb_frag, st->arg);

	return err;
}


int h264_nal_send(struct vidcodec_st *st, bool first, bool last,
		  bool marker, uint32_t ihdr, const uint8_t *buf,
		  size_t size, size_t maxsz)
{
	uint8_t hdr = (uint8_t)ihdr;
	int err = 0;

	if (first && last && size <= maxsz) {
		err = rtp_send_data(st, &hdr, 1, buf, size, marker);
	}
	else {
		uint8_t fu_hdr[2];
		const uint8_t type = hdr & 0x1f;
		const uint8_t nri  = hdr & 0x60;
		const size_t sz = maxsz - 2;

		fu_hdr[0] = nri | H264_NAL_FU_A;
		fu_hdr[1] = first ? (1<<7 | type) : type;

		while (size > sz) {
			err |= rtp_send_data(st, fu_hdr, 2, buf, sz, false);
			buf += sz;
			size -= sz;
			fu_hdr[1] &= ~(1 << 7);
		}

		if (last)
			fu_hdr[1] |= 1<<6;  /* end bit */

		err |= rtp_send_data(st, fu_hdr, 2, buf, size, marker && last);
	}

	return err;
}


int h264_packetize(struct vidcodec_st *st, struct mbuf *mb)
{
	const uint8_t *start = mb->buf;
	const uint8_t *end   = start + mb->end;
	const uint8_t *r;
	int err = 0;

	r = h264_find_startcode(mb->buf, end);

	while (r < end) {
		const uint8_t *r1;

		/* skip zeros */
		while (!*(r++))
			;

		r1 = h264_find_startcode(r, end);

		if (st->enqh)
			err |= st->enqh((r1 >= end), r[0], r+1, r1-r-1,
					st->arg);
		else
			err |= h264_nal_send(st, true, true, (r1 >= end), r[0],
					     r+1, r1-r-1, MAX_RTP_SIZE);
		r = r1;
	}

	return err;
}


#ifdef USE_X264
static int open_encoder_x264(struct vidcodec_st *st, struct vidcodec_prm *prm,
			     const struct vidsz *size)
{
	x264_param_t xprm;

	x264_param_default(&xprm);

#if X264_BUILD >= 87
	x264_param_apply_profile(&xprm, "baseline");
#endif

	xprm.i_level_idc = h264_level_idc;
	xprm.i_width = size->w;
	xprm.i_height = size->h;
	xprm.i_csp = X264_CSP_I420;
	xprm.i_fps_num = prm->fps;
	xprm.i_fps_den = 1;
	xprm.rc.i_bitrate = prm->bitrate / 1024; /* kbit/s */
	xprm.rc.i_rc_method = X264_RC_CQP;
	xprm.i_log_level = X264_LOG_WARNING;

	/* ultrafast preset */
	xprm.i_frame_reference = 1;
	xprm.i_scenecut_threshold = 0;
	xprm.b_deblocking_filter = 0;
	xprm.b_cabac = 0;
	xprm.i_bframe = 0;
	xprm.analyse.intra = 0;
	xprm.analyse.inter = 0;
	xprm.analyse.b_transform_8x8 = 0;
	xprm.analyse.i_me_method = X264_ME_DIA;
	xprm.analyse.i_subpel_refine = 0;
#if X264_BUILD >= 59
	xprm.rc.i_aq_mode = 0;
#endif
	xprm.analyse.b_mixed_references = 0;
	xprm.analyse.i_trellis = 0;
#if X264_BUILD >= 63
	xprm.i_bframe_adaptive = X264_B_ADAPT_NONE;
#endif
#if X264_BUILD >= 70
	xprm.rc.b_mb_tree = 0;
#endif

	/* slice-based threading (--tune=zerolatency) */
#if X264_BUILD >= 80
	xprm.rc.i_lookahead = 0;
	xprm.i_sync_lookahead = 0;
	xprm.i_bframe = 0;
#endif

	/* put SPS/PPS before each keyframe */
	xprm.b_repeat_headers = 1;

#if X264_BUILD >= 82
	/* needed for x264_encoder_intra_refresh() */
	xprm.b_intra_refresh = 1;
#endif

	if (st->x264)
		x264_encoder_close(st->x264);

	st->x264 = x264_encoder_open(&xprm);
	if (!st->x264) {
		DEBUG_WARNING("x264_encoder_open() failed\n");
		return ENOENT;
	}

	st->encsize = *size;

	return 0;
}


int enc_x264(struct vidcodec_st *st, bool update,
	     const struct vidframe *frame)
{
	x264_picture_t pic_in, pic_out;
	x264_nal_t *nal;
	int i_nal;
	int i, err, ret;

	if (!st->x264 || !vidsz_cmp(&st->encsize, &frame->size)) {

		err = open_encoder_x264(st, &st->encprm, &frame->size);
		if (err)
			return err;
	}

	if (update) {
#if X264_BUILD >= 95
		x264_encoder_intra_refresh(st->x264);
#endif
		re_printf("x264 picture update\n");
	}

	memset(&pic_in, 0, sizeof(pic_in));

	pic_in.i_type = update ? X264_TYPE_IDR : X264_TYPE_AUTO;
	pic_in.i_qpplus1 = 0;
	pic_in.i_pts = ++st->pts;

	pic_in.img.i_csp = X264_CSP_I420;
	pic_in.img.i_plane = 3;
	for (i=0; i<3; i++) {
		pic_in.img.i_stride[i] = frame->linesize[i];
		pic_in.img.plane[i]    = frame->data[i];
	}

	ret = x264_encoder_encode(st->x264, &nal, &i_nal, &pic_in, &pic_out);
	if (ret < 0) {
		fprintf(stderr, "x264 [error]: x264_encoder_encode failed\n");
	}
	if (i_nal == 0)
		return 0;

	err = 0;
	for (i=0; i<i_nal && !err; i++) {
		const uint8_t hdr = nal[i].i_ref_idc<<5 | nal[i].i_type<<0;
		int offset = 0;

#if X264_BUILD >= 76
		const uint8_t *p = nal[i].p_payload;

		/* Find the NAL Escape code [00 00 01] */
		if (nal[i].i_payload > 4 && p[0] == 0x00 && p[1] == 0x00) {
			if (p[2] == 0x00 && p[3] == 0x01)
				offset = 4 + 1;
			else if (p[2] == 0x01)
				offset = 3 + 1;
		}
#endif

		/* skip Supplemental Enhancement Information (SEI) */
		if (nal[i].i_type == H264_NAL_SEI)
			continue;

		if (st->enqh)
			err = st->enqh((i+1) == i_nal, hdr,
				       nal[i].p_payload + offset,
				       nal[i].i_payload - offset,
				       st->arg);
		else
			err = h264_nal_send(st, true, true, (i+1)==i_nal, hdr,
					    nal[i].p_payload + offset,
					    nal[i].i_payload - offset,
					    MAX_RTP_SIZE);
	}

	return err;
}
#endif


int h264_decode(struct vidcodec_st *st, struct mbuf *src)
{
	struct h264_hdr h264_hdr;
	const uint8_t nal_seq[3] = {0, 0, 1};
	int err;

	err = h264_hdr_decode(&h264_hdr, src);
	if (err)
		return err;

	if (h264_hdr.f) {
		DEBUG_WARNING("H264 forbidden bit set!\n");
		return EBADMSG;
	}

	/* handle NAL types */
	if (1 <= h264_hdr.type && h264_hdr.type <= 23) {

		if (!st->got_keyframe) {
			switch (h264_hdr.type) {

			case H264_NAL_PPS:
			case H264_NAL_SPS:
				st->got_keyframe = true;
				break;
			}
		}

		/* prepend H.264 NAL start sequence */
		mbuf_write_mem(st->dec.mb, nal_seq, 3);

		/* encode NAL header back to buffer */
		err = h264_hdr_encode(&h264_hdr, st->dec.mb);
	}
	else if (H264_NAL_FU_A == h264_hdr.type) {
		struct fu fu;

		err = fu_hdr_decode(&fu, src);
		if (err)
			return err;
		h264_hdr.type = fu.type;

		if (fu.s) {
			/* prepend H.264 NAL start sequence */
			mbuf_write_mem(st->dec.mb, nal_seq, 3);

			/* encode NAL header back to buffer */
			err = h264_hdr_encode(&h264_hdr, st->dec.mb);
		}
	}
	else {
		DEBUG_WARNING("unknown NAL type %u\n", h264_hdr.type);
		return EBADMSG;
	}

	return err;
}
