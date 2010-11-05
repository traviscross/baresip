/**
 * @file vpx.c  VP8 video codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#define VPX_CODEC_DISABLE_COMPAT 1
#define VPX_DISABLE_CTRL_TYPECHECKS 1
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>


/*
 * Experimental support for WebM VP8 video codec:
 *
 *     http://www.webmproject.org/
 *
 * TODO: define RTP payload format
 */


enum {
	MAX_RTP_SIZE = 1024,
	RTP_PRESZ    = 4 + RTP_HEADER_SIZE
};

struct vidcodec_st {
	struct vidcodec *vc;  /* base class */
	struct mbuf *mb;
	int pts;
	vpx_codec_ctx_t enc;
	vpx_codec_ctx_t dec;
	vidcodec_send_h *sendh;
	void *arg;
};

static struct vidcodec *vp8;


static void destructor(void *data)
{
	struct vidcodec_st *st = data;

	vpx_codec_destroy(&st->enc);
	vpx_codec_destroy(&st->dec);
	mem_deref(st->mb);

	mem_deref(st->vc);
}


static int init_encoder(struct vidcodec_st *st, struct vidcodec_prm *prm)
{
	vpx_codec_enc_cfg_t cfg;
	vpx_codec_err_t res;

	/* Encoder */
	res = vpx_codec_enc_config_default(&vpx_codec_vp8_cx_algo, &cfg, 0);
	if (res)
		return EPROTO;

	cfg.rc_target_bitrate = prm->size.w * prm->size.h
		* cfg.rc_target_bitrate
		/ cfg.g_w / cfg.g_h;
	cfg.g_w = prm->size.w;
	cfg.g_h = prm->size.h;
	cfg.g_error_resilient = 1;

	re_printf("VPX encoder bitrate: %d\n", cfg.rc_target_bitrate);

	res = vpx_codec_enc_init(&st->enc, &vpx_codec_vp8_cx_algo, &cfg, 0);
	if (res) {
		re_fprintf(stderr, "vpx: Failed to initialize encoder: %s\n",
			   vpx_codec_err_to_string(res));
		return EPROTO;
	}

	return 0;
}


static int init_decoder(struct vidcodec_st *st)
{
	vpx_codec_err_t res;

	/* Decoder */
	res = vpx_codec_dec_init(&st->dec, &vpx_codec_vp8_dx_algo, NULL, 0);
	if (res) {
		re_fprintf(stderr, "vpx: Failed to initialize decoder: %s\n",
			   vpx_codec_err_to_string(res));
		return EPROTO;
	}

	st->mb = mbuf_alloc(512);
	if (!st->mb)
		return ENOMEM;

	return 0;
}


static int alloc(struct vidcodec_st **stp, struct vidcodec *vc,
		 const char *name,
		 struct vidcodec_prm *encp, struct vidcodec_prm *decp,
		 const struct pl *sdp_fmtp,
		 vidcodec_send_h *sendh, void *arg)
{
	struct vidcodec_st *st;
	int err = 0;

	(void)sdp_fmtp;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->vc = mem_ref(vc);
	st->sendh = sendh;
	st->arg = arg;

	if (encp)
		err |= init_encoder(st, encp);
	if (decp)
		err |= init_decoder(st);
	if (err)
		goto out;

	re_printf("video codec %s: encoder=%ux%u decoder=%ux%u\n", name,
		  encp->size.w, encp->size.h, decp->size.w, decp->size.h);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int vpx_packetize(struct vidcodec_st *st, const uint8_t *buf, size_t sz)
{
	struct mbuf *mb = mbuf_alloc(512);
	const uint8_t *pmax = buf + sz;
	int err = 0;

	if (!mb)
		return ENOMEM;

	while (buf < pmax) {
		size_t chunk = min(sz, MAX_RTP_SIZE);
		bool last = (sz < MAX_RTP_SIZE);

		mb->pos = mb->end = RTP_PRESZ;
		err = mbuf_write_mem(mb, buf, chunk);
		if (err)
			break;
		mb->pos = RTP_PRESZ;

		st->sendh(last, mb, st->arg);

		buf += chunk;
		sz  -= chunk;
	};

	mem_deref(mb);

	return err;
}


static int enc(struct vidcodec_st *st, bool update,
	       const struct vidframe *frame)
{
	vpx_image_t img;
	vpx_codec_iter_t iter = NULL;
	const vpx_codec_cx_pkt_t *pkt;
	vpx_codec_err_t res;
	vpx_enc_frame_flags_t flags = 0;
	int err, i;

	if (update)
		flags |= VPX_EFLAG_FORCE_KF;

	memset(&img, 0, sizeof(img));

	img.fmt = VPX_IMG_FMT_YV12;
	img.w = img.d_w = frame->size.w;
	img.h = img.d_h = frame->size.h;
	for (i=0; i<4; i++) {
		img.planes[i] = frame->data[i];
		img.stride[i] = frame->linesize[i];
	}

	res = vpx_codec_encode(&st->enc, &img, st->pts++, 1,
			       flags, VPX_DL_REALTIME);
	if (res) {
		re_fprintf(stderr, "Failed to encode frame: %s\n",
			   vpx_codec_err_to_string(res));
		return EBADMSG;
	}

	while ((pkt = vpx_codec_get_cx_data(&st->enc, &iter)) ) {

		switch (pkt->kind) {

		case VPX_CODEC_CX_FRAME_PKT:
			err = vpx_packetize(st, pkt->data.frame.buf,
					    pkt->data.frame.sz);
			if (err)
				return err;
			break;

		default:
			break;
		}

		if (pkt->kind == VPX_CODEC_CX_FRAME_PKT
		    && (pkt->data.frame.flags & VPX_FRAME_IS_KEY)) {
			re_printf("{Send VPX Key-frame}\n");
		}
	}

	return 0;
}


static int dec(struct vidcodec_st *st, struct vidframe *frame,
	       bool eof, struct mbuf *src)
{
	vpx_codec_iter_t  iter = NULL;
	vpx_image_t      *img;
	vpx_codec_err_t res;
	int err;

	err = mbuf_write_mem(st->mb, mbuf_buf(src), mbuf_get_left(src));
	if (err)
		return err;

	if (!eof)
		return 0;

	res = vpx_codec_decode(&st->dec, st->mb->buf,
			       (unsigned int)st->mb->end, NULL, 0);
	if (res) {
		re_fprintf(stderr, "Failed to decode frame of %zu bytes: %s\n",
			   mbuf_get_left(src), vpx_codec_err_to_string(res));
		err = EBADMSG;
		goto out;
	}

	if ((img = vpx_codec_get_frame(&st->dec, &iter))) {
		int i;

		for (i=0; i<4; i++) {
			frame->data[i] = img->planes[i];
			frame->linesize[i] = img->stride[i];
		}
		frame->size.w = img->d_w;
		frame->size.h = img->d_h;
		frame->valid = true;
	}

 out:
	mbuf_rewind(st->mb);

	return err;
}


static int module_init(void)
{
	return vidcodec_register(&vp8, 0, "VP8", "", alloc, enc, dec);
}


static int module_close(void)
{
	vp8 = mem_deref(vp8);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(vpx) = {
	"vpx",
	"codec",
	module_init,
	module_close
};
