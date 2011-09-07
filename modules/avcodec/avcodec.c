/**
 * @file avcodec.c  Video codecs using FFmpeg libavcodec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include <libavcodec/avcodec.h>
#ifdef USE_X264
#include <x264.h>
#endif
#include "h26x.h"
#include "avcodec.h"


#define DEBUG_MODULE "avcodec"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


enum {
	DEFAULT_GOP_SIZE =   10,
};

static struct vidcodec *h263, *h264, *mpg4;
static char h264_fmtp[256];
static const uint8_t h264_level_idc = 0x0c;


static void destructor(void *arg)
{
	struct vidcodec_st *st = arg;

	mem_deref(st->vc);

	mem_deref(st->dec.mb);
	mem_deref(st->enc.mb);
	mem_deref(st->mb_frag);

#ifdef USE_X264
	if (st->x264)
		x264_encoder_close(st->x264);
#endif

	if (st->enc.ctx) {
		if (st->enc.ctx->codec)
			avcodec_close(st->enc.ctx);
		av_free(st->enc.ctx);
	}

	if (st->dec.ctx) {
		if (st->dec.ctx->codec)
			avcodec_close(st->dec.ctx);
		av_free(st->dec.ctx);
	}

	if (st->enc.pict)
		av_free(st->enc.pict);
	if (st->dec.pict)
		av_free(st->dec.pict);
}


static void param_handler(const struct pl *name, const struct pl *val,
			  void *arg)
{
	struct vidcodec_st *st = arg;

	if (st->codec_id == CODEC_ID_H263)
		(void)decode_sdpparam_h263(st, name, val);
	else if (st->codec_id == CODEC_ID_H264)
		(void)decode_sdpparam_h264(st, name, val);
}


static int init_encoder(struct vidcodec_st *st, const struct vidcodec_prm *prm)
{
	st->enc.size = prm->size;

	st->enc.codec = avcodec_find_encoder(st->codec_id);
	if (!st->enc.codec)
		return ENOENT;

#if LIBAVCODEC_VERSION_INT >= ((52<<16)+(92<<8)+0)
	st->enc.ctx = avcodec_alloc_context3(st->enc.codec);
#else
	st->enc.ctx = avcodec_alloc_context();
#endif

	st->enc.pict = avcodec_alloc_frame();

	if (!st->enc.ctx || !st->enc.pict)
		return ENOMEM;

	st->enc.ctx->bit_rate  = prm->bitrate;
	st->enc.ctx->width     = prm->size.w;
	st->enc.ctx->height    = prm->size.h;
	st->enc.ctx->gop_size  = DEFAULT_GOP_SIZE;
	st->enc.ctx->pix_fmt   = PIX_FMT_YUV420P;
	st->enc.ctx->time_base.num = 1;
	st->enc.ctx->time_base.den = prm->fps;

	/* params to avoid ffmpeg/x264 default preset error */
	if (st->codec_id == CODEC_ID_H264) {
		st->enc.ctx->me_method = ME_UMH;
		st->enc.ctx->me_range = 16;
		st->enc.ctx->qmin = 10;
		st->enc.ctx->qmax = 51;
		st->enc.ctx->max_qdiff = 4;
	}

#if LIBAVCODEC_VERSION_INT >= ((53<<16)+(8<<8)+0)
	if (avcodec_open2(st->enc.ctx, st->enc.codec, NULL) < 0)
		return ENOENT;
#else
	if (avcodec_open(st->enc.ctx, st->enc.codec) < 0)
		return ENOENT;
#endif

	return 0;
}


#ifdef USE_X264
static int init_encoder_x264(struct vidcodec_st *st, struct vidcodec_prm *prm)
{
	x264_param_t xprm;

	st->enc.size = prm->size;

	x264_param_default(&xprm);

#if X264_BUILD >= 87
	x264_param_apply_profile(&xprm, "baseline");
#endif

	xprm.i_level_idc = h264_level_idc;
	xprm.i_width = prm->size.w;
	xprm.i_height = prm->size.h;
	xprm.i_csp = X264_CSP_I420;
	xprm.i_fps_num = 1;
	xprm.i_fps_den = prm->fps;
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
	xprm.rc.i_aq_mode = 0;
	xprm.analyse.b_mixed_references = 0;
	xprm.analyse.i_trellis = 0;
	xprm.i_bframe_adaptive = X264_B_ADAPT_NONE;
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

	st->x264 = x264_encoder_open(&xprm);
	if (!st->x264) {
		DEBUG_WARNING("x264_encoder_open() failed\n");
		return ENOENT;
	}

	return 0;
}
#endif


static int init_decoder(struct vidcodec_st *st, const struct vidcodec_prm *prm)
{
	st->dec.size = prm->size;

	st->dec.codec = avcodec_find_decoder(st->codec_id);
	if (!st->dec.codec)
		return ENOENT;

#if LIBAVCODEC_VERSION_INT >= ((52<<16)+(92<<8)+0)
	st->dec.ctx = avcodec_alloc_context3(st->dec.codec);
#else
	st->dec.ctx = avcodec_alloc_context();
#endif

	st->dec.pict = avcodec_alloc_frame();

	if (!st->dec.ctx || !st->dec.pict)
		return ENOMEM;

#if LIBAVCODEC_VERSION_INT >= ((53<<16)+(8<<8)+0)
	if (avcodec_open2(st->dec.ctx, st->dec.codec, NULL) < 0)
		return ENOENT;
#else
	if (avcodec_open(st->dec.ctx, st->dec.codec) < 0)
		return ENOENT;
#endif

	return 0;
}


static int alloc(struct vidcodec_st **stp, struct vidcodec *vc,
		 const char *name,
		 struct vidcodec_prm *encp, struct vidcodec_prm *decp,
		 const struct pl *sdp_fmtp,
		 vidcodec_send_h *sendh, void *arg)
{
	struct vidcodec_st *st;
	int err;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->vc = mem_ref(vc);

	if (0 == str_casecmp(name, "H263"))
		st->codec_id = CODEC_ID_H263;
	else if (0 == str_casecmp(name, "H264"))
		st->codec_id = CODEC_ID_H264;
	else if (0 == str_casecmp(name, "MP4V-ES"))
		st->codec_id = CODEC_ID_MPEG4;
	else {
		err = EINVAL;
		goto out;
	}

	st->enc.mb  = mbuf_alloc(FF_MIN_BUFFER_SIZE * 20);
	st->dec.mb  = mbuf_alloc(1024);
	st->mb_frag = mbuf_alloc(1024);
	if (!st->enc.mb || !st->dec.mb || !st->mb_frag) {
		err = ENOMEM;
		goto out;
	}

	st->enc.sz_max = st->enc.mb->size;
	st->dec.sz_max = st->dec.mb->size;

	if (st->codec_id == CODEC_ID_H264) {
#ifdef USE_X264
		err = init_encoder_x264(st, encp);
#else
		err = init_encoder(st, encp);
#endif
	}
	else
		err = init_encoder(st, encp);
	if (err) {
		DEBUG_WARNING("%s: could not init encoder\n", name);
		goto out;
	}

	err = init_decoder(st, decp);
	if (err) {
		DEBUG_WARNING("%s: could not init decoder\n", name);
		goto out;
	}

	if (pl_isset(sdp_fmtp)) {
		fmt_param_apply(sdp_fmtp, param_handler, st);
	}

	st->sendh = sendh;
	st->arg = arg;

	re_printf("video codec %s: encoder=%ux%u decoder=%ux%u\n", name,
		  encp->size.w, encp->size.h, decp->size.w, decp->size.h);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int general_packetize(struct vidcodec_st *st, struct mbuf *mb)
{
	int err = 0;

	/* Assemble frame into smaller packets */
	while (!err) {
		size_t sz, left = mbuf_get_left(mb);
		bool last = (left < MAX_RTP_SIZE);
		if (!left)
			break;

		sz = last ? left : MAX_RTP_SIZE;

		st->mb_frag->pos = st->mb_frag->end = RTP_PRESZ;
		err = mbuf_write_mem(st->mb_frag, mbuf_buf(mb), sz);
		if (err)
			break;

		st->mb_frag->pos = RTP_PRESZ;
		err = st->sendh(last, st->mb_frag, st->arg);

		mbuf_advance(mb, sz);
	}

	return err;
}


static int enc(struct vidcodec_st *st, bool update,
	       const struct vidframe *frame)
{
	int i, err, ret;

	/* todo: compare frame-size with enc-size */

	for (i=0; i<4; i++) {
		st->enc.pict->data[i]     = frame->data[i];
		st->enc.pict->linesize[i] = frame->linesize[i];
	}
	st->enc.pict->pts = AV_NOPTS_VALUE;
	if (update) {
		re_printf("avcodec encoder picture update\n");
		st->enc.pict->key_frame = 1;
		st->enc.pict->pict_type = FF_I_TYPE;  /* Infra Frame */
	}
	else {
		st->enc.pict->key_frame = 0;
		st->enc.pict->pict_type = 0;
	}

	mbuf_rewind(st->enc.mb);
	ret = avcodec_encode_video(st->enc.ctx, st->enc.mb->buf,
				   (int)st->enc.mb->size, st->enc.pict);
	if (ret < 0 )
		return EBADMSG;

	/* todo: figure out proper buffer size */
	if (ret > (int)st->enc.sz_max) {
		re_printf("note: grow encode buffer %u --> %d\n",
			  st->enc.sz_max, ret);
		st->enc.sz_max = ret;
	}

	mbuf_set_end(st->enc.mb, ret);

	switch (st->codec_id) {

	case CODEC_ID_H263:
		err = h263_packetize(st, st->enc.mb);
		break;

	case CODEC_ID_H264:
		err = h264_packetize(st, st->enc.mb);
		break;

	case CODEC_ID_MPEG4:
		err = general_packetize(st, st->enc.mb);
		break;

	default:
		err = EPROTO;
		break;
	}

	return err;
}


/*
 * TODO: check input/output size
 */
static int ffdecode(struct vidcodec_st *st, struct vidframe *frame,
		    bool eof, struct mbuf *src)
{
	int i, got_picture, ret, err;

	/* assemble packets in "mb_rx" */
	err = mbuf_write_mem(st->dec.mb, mbuf_buf(src), mbuf_get_left(src));
	if (err)
		return err;

	if (!eof)
		return 0;

	st->dec.mb->pos = 0;

	if (!st->got_keyframe) {
		err = EPROTO;
		goto out;
	}

#if LIBAVCODEC_VERSION_INT <= ((52<<16)+(23<<8)+0)
	ret = avcodec_decode_video(st->dec.ctx, st->dec.pict, &got_picture,
				   st->dec.mb->buf,
				   (int)mbuf_get_left(st->dec.mb));
#else
	do {
		AVPacket avpkt;

		av_init_packet(&avpkt);
		avpkt.data = st->dec.mb->buf;
		avpkt.size = (int)mbuf_get_left(st->dec.mb);

		ret = avcodec_decode_video2(st->dec.ctx, st->dec.pict,
					    &got_picture, &avpkt);
	} while (0);
#endif

	if (ret < 0) {
		err = EBADMSG;
		goto out;
	}
	else if (ret && ret != (int)mbuf_get_left(st->dec.mb)) {
		DEBUG_NOTICE("decoded only %d of %u bytes (got_pict=%d)\n",
			     ret, mbuf_get_left(st->dec.mb), got_picture);
	}

	mbuf_skip_to_end(src);

	if (got_picture) {
		for (i=0; i<4; i++) {
			frame->data[i]     = st->dec.pict->data[i];
			frame->linesize[i] = st->dec.pict->linesize[i];
		}
		frame->size.w = st->dec.ctx->width;
		frame->size.h = st->dec.ctx->height;
		frame->fmt    = VID_FMT_YUV420P;
	}

 out:
	if (eof)
		mbuf_rewind(st->dec.mb);

	return err;
}


static int dec_h263(struct vidcodec_st *st, struct vidframe *frame,
		    bool eof, struct mbuf *src)
{
	struct h263_hdr h263_hdr;
	int err;

	if (!src)
		return 0;

	err = h263_hdr_decode(&h263_hdr, src);
	if (err)
		return err;

	if (!st->got_keyframe && h263_hdr.i == I_FRAME)
		st->got_keyframe = true;

	return ffdecode(st, frame, eof, src);
}


static int dec_h264(struct vidcodec_st *st, struct vidframe *frame,
		    bool eof, struct mbuf *src)
{
	int err;

	if (!src)
		return 0;

	err = h264_decode(st, src);
	if (err)
		return err;

	return ffdecode(st, frame, eof, src);
}


static int dec_mpeg4(struct vidcodec_st *st, struct vidframe *frame,
		     bool eof, struct mbuf *src)
{
	if (!src)
		return 0;

	/* let the decoder handle this */
	st->got_keyframe = true;

	return ffdecode(st, frame, eof, src);
}


static int module_init(void)
{
	const uint8_t profile_idc = 0x42; /* baseline profile */
	const uint8_t profile_iop = 0x80;
	int err = 0;

	if (re_snprintf(h264_fmtp, sizeof(h264_fmtp),
			"packetization-mode=0;profile-level-id=%02x%02x%02x"
#if 0
			";max-mbps=35000"
			";max-fs=3600"
			";max-smbps=98875"  /* times 4 for full HD */
#endif
			"",
			profile_idc, profile_iop, h264_level_idc) < 0)
		return ENOMEM;

#ifdef USE_X264
	re_printf("x264 build %d\n", X264_BUILD);
#else
	re_printf("using FFmpeg H.264 encoder\n");
#endif

	avcodec_init();
	avcodec_register_all();

#if 0
	av_log_set_level(AV_LOG_WARNING);
#endif

	if (avcodec_find_decoder(CODEC_ID_H264)) {

		/* XXX: add two h264 codecs */
		err |= vidcodec_register(&h264, 0,    "H264",
					 h264_fmtp,
					 alloc,
#ifdef USE_X264
					 enc_x264,
#else
					 enc,
#endif
					 dec_h264);
	}

	if (avcodec_find_decoder(CODEC_ID_H263)) {

		err |= vidcodec_register(&h263, "34", "H263",
					 "F=1;CIF=1;CIF4=1",
					 alloc, enc, dec_h263);
	}

	if (avcodec_find_decoder(CODEC_ID_MPEG4)) {

		err |= vidcodec_register(&mpg4, 0, "MP4V-ES",
					 "profile-level-id=3",
					 alloc, enc, dec_mpeg4);
	}

	return err;
}


static int module_close(void)
{
	h263 = mem_deref(h263);
	h264 = mem_deref(h264);
	mpg4 = mem_deref(mpg4);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(avcodec) = {
	"avcodec",
	"codec",
	module_init,
	module_close
};
