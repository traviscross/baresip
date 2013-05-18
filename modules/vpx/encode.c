/**
 * @file vp8/encode.c VP8 Encode
 *
 * Copyright (C) 2010 Creytiv.com
 */

#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>
#include "vp8.h"


enum {
	HDR_SIZE = 4,
};


struct videnc_state {
	vpx_codec_ctx_t ctx;
	struct vidsz size;
	vpx_codec_pts_t pts;
	unsigned fps;
	unsigned bitrate;
	unsigned pktsize;
	bool ctxup;
	uint16_t picid;
};


static void destructor(void *arg)
{
	struct videnc_state *ves = arg;

	if (ves->ctxup)
		vpx_codec_destroy(&ves->ctx);
}


int vp8_encode_update(struct videnc_state **vesp, const struct vidcodec *vc,
		      struct videnc_param *prm, const char *fmtp)
{
	const struct vp8_vidcodec *vp8 = (struct vp8_vidcodec *)vc;
	struct videnc_state *ves;
	uint32_t max_fs;
	(void)vp8;

	if (!vesp || !vc || !prm || prm->pktsize < (HDR_SIZE + 1))
		return EINVAL;

	ves = *vesp;

	if (!ves) {

		ves = mem_zalloc(sizeof(*ves), destructor);
		if (!ves)
			return ENOMEM;

		ves->picid = rand_u16();

		*vesp = ves;
	}
	else {
		if (ves->ctxup && (ves->bitrate != prm->bitrate ||
				   ves->fps     != prm->fps)) {

			vpx_codec_destroy(&ves->ctx);
			ves->ctxup = false;
		}
	}

	ves->bitrate = prm->bitrate;
	ves->pktsize = prm->pktsize;
	ves->fps     = prm->fps;

	max_fs = vp8_max_fs(fmtp);
	if (max_fs > 0)
		prm->max_fs = max_fs * 256;

	return 0;
}


static int open_encoder(struct videnc_state *ves, const struct vidsz *size)
{
	vpx_codec_enc_cfg_t cfg;
	vpx_codec_err_t res;

	res = vpx_codec_enc_config_default(&vpx_codec_vp8_cx_algo, &cfg, 0);
	if (res)
		return EPROTO;

	cfg.g_w = size->w;
	cfg.g_h = size->h;
	cfg.rc_target_bitrate = ves->bitrate;
	cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;

	if (ves->ctxup) {
		re_printf("vp8: re-opening encoder\n");
		vpx_codec_destroy(&ves->ctx);
		ves->ctxup = false;
	}

	res = vpx_codec_enc_init(&ves->ctx, &vpx_codec_vp8_cx_algo, &cfg, 0);
	if (res) {
		re_fprintf(stderr, "vp8: enc init: %s\n",
			   vpx_codec_err_to_string(res));
		return EPROTO;
	}

	ves->ctxup = true;

	res = vpx_codec_control(&ves->ctx, VP8E_SET_CPUUSED, 16);
	if (res) {
		re_fprintf(stderr, "vp8: codec ctrl: %s\n",
			   vpx_codec_err_to_string(res));
	}

	return 0;
}


static inline void hdr_encode(uint8_t hdr[HDR_SIZE], bool noref, bool start,
			      uint16_t picid)
{
	hdr[0] = 1<<7 | noref<<5 | start<<4;
	hdr[1] = 1<<7;
	hdr[2] = 1<<7 | (picid>>8 & 0x7f);
	hdr[3] = picid & 0xff;
}


static inline int packetize(bool marker, const uint8_t *buf, size_t len,
			    size_t maxlen, bool noref, uint16_t picid,
			    videnc_packet_h *pkth, void *arg)
{
	uint8_t hdr[HDR_SIZE];
	bool start = true;
	int err = 0;

	maxlen -= sizeof(hdr);

	while (len > maxlen) {

		hdr_encode(hdr, noref, start, picid);

		err |= pkth(false, hdr, sizeof(hdr), buf, maxlen, arg);

		buf  += maxlen;
		len  -= maxlen;
		start = false;
	}

	hdr_encode(hdr, noref, start, picid);

	err |= pkth(marker, hdr, sizeof(hdr), buf, len, arg);

	return err;
}


static const vpx_codec_cx_pkt_t *get_cxdata(vpx_codec_ctx_t *ctx,
					    vpx_codec_iter_t *iter)
{
	for (;;) {
		const vpx_codec_cx_pkt_t *pkt;

		pkt = vpx_codec_get_cx_data(ctx, iter);
		if (!pkt)
			return NULL;

		if (pkt->kind == VPX_CODEC_CX_FRAME_PKT)
			return pkt;
	}

	return NULL;
}


int vp8_encode(struct videnc_state *ves, bool update,
		const struct vidframe *frame,
		videnc_packet_h *pkth, void *arg)
{
	const vpx_codec_cx_pkt_t *pkt, *next_pkt;
	vpx_enc_frame_flags_t flags = 0;
	vpx_codec_iter_t iter = NULL;
	vpx_codec_err_t res;
	vpx_image_t img;
	int err, i;

	if (!ves || !frame || !pkth || frame->fmt != VID_FMT_YUV420P)
		return EINVAL;

	if (!ves->ctxup || !vidsz_cmp(&ves->size, &frame->size)) {

		err = open_encoder(ves, &frame->size);
		if (err)
			return err;

		ves->size = frame->size;
	}

	if (update)
		flags |= VPX_EFLAG_FORCE_KF;

	memset(&img, 0, sizeof(img));

	img.fmt = VPX_IMG_FMT_I420;
	img.w = img.d_w = frame->size.w;
	img.h = img.d_h = frame->size.h;

	for (i=0; i<4; i++) {
		img.stride[i] = frame->linesize[i];
		img.planes[i] = frame->data[i];
	}

	res = vpx_codec_encode(&ves->ctx, &img, ves->pts++, 1,
			       flags, VPX_DL_REALTIME);
	if (res) {
		re_fprintf(stderr, "vp8: enc error: %s\n",
			   vpx_codec_err_to_string(res));
		return ENOMEM;
	}

	++ves->picid;

	next_pkt = get_cxdata(&ves->ctx, &iter);

	while (next_pkt) {

		bool keyframe = false;

		pkt      = next_pkt;
		next_pkt = get_cxdata(&ves->ctx, &iter);

		if (pkt->data.frame.flags & VPX_FRAME_IS_KEY)
			keyframe = true;

		err = packetize(next_pkt == NULL,
				pkt->data.frame.buf,
				pkt->data.frame.sz,
				ves->pktsize, !keyframe, ves->picid,
				pkth, arg);
		if (err)
			return err;
	}

	return 0;
}
