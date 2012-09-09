/**
 * @file vpx.c  VP8 video codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <rem.h>
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
 *     http://tools.ietf.org/html/draft-westin-payload-vp8-02
 */


enum {
	MAX_RTP_SIZE = 1024,
	RTP_PRESZ    = 4 + RTP_HEADER_SIZE
};

struct vidcodec_st {
	struct vidcodec *vc;  /* base class */
	struct vidcodec_prm encprm;
	struct vidsz encsz;
	struct mbuf *mb;
	uint64_t picid;
	int pts;
	vpx_codec_ctx_t enc;
	vpx_codec_ctx_t dec;
	vidcodec_send_h *sendh;
	void *arg;
	bool encup, decup;
};

/** Fragmentation information */
enum fi {
	FI_NONE   = 0x0,
	FI_FIRST  = 0x1,
	FI_MIDDLE = 0x2,
	FI_LAST   = 0x3
};

/** VP8 Payload Descriptor */
struct vp8desc {
	unsigned i:1;          /**< PictureID present         */
	unsigned n:1;          /**< Non-reference frame       */
	unsigned fi:2;         /**< Fragmentation information */
	unsigned b:1;          /**< Beginning VP8 frame       */
	uint64_t picid;        /**< PictureID                 */
};


static struct vidcodec *vp8;


static int picid_enc(struct mbuf *mb, uint64_t picid)
{
	uint8_t b, buf[8], *p = buf + 8;

	do {
		b = picid & 0x7f;

		picid >>= 7;

		if (p != (buf + 8))
			b |= 0x80;

		*--p = b;
	}
	while (picid);

	return mbuf_write_mem(mb, p, buf + 8 - p);
}


static int picid_dec(struct mbuf *mb, uint64_t *picid)
{
	uint64_t v = 0;
	uint8_t b;

	do {
		if (mbuf_get_left(mb) < 1)
			return EBADMSG;

		b = mbuf_read_u8(mb);

		v <<= 7;
		v  |= (b & 0x7f);
	}
	while (b & 0x80);

	*picid = v;

	return 0;
}


static int vp8desc_enc(struct mbuf *mb, bool i, bool n, enum fi fi, bool b,
		       uint64_t picid)
{
	uint8_t u8;
	int err;

	u8 = i<<4 | n<<3 | fi<<1 | b;

	err = mbuf_write_u8(mb, u8);

	if (i)
		err |= picid_enc(mb, picid);

	return err;
}


static int vp8desc_dec(struct vp8desc *desc, struct mbuf *mb)
{
	uint8_t u8;

	if (mbuf_get_left(mb) < 1)
		return EBADMSG;

	u8 = mbuf_read_u8(mb);

	desc->i  = (u8 >> 4) & 0x1;
	desc->n  = (u8 >> 3) & 0x1;
	desc->fi = (u8 >> 1) & 0x3;
	desc->b  = (u8 >> 0) & 0x1;

	if (desc->i)
		return picid_dec(mb, &desc->picid);

	return 0;
}


static void destructor(void *arg)
{
	struct vidcodec_st *st = arg;

	if (st->encup)
		vpx_codec_destroy(&st->enc);

	if (st->decup)
		vpx_codec_destroy(&st->dec);

	mem_deref(st->mb);

	mem_deref(st->vc);
}


static int open_encoder(struct vidcodec_st *st, struct vidcodec_prm *prm,
			const struct vidsz *size)
{
	vpx_codec_enc_cfg_t cfg;
	vpx_codec_err_t res;

	/* Encoder */
	res = vpx_codec_enc_config_default(&vpx_codec_vp8_cx_algo, &cfg, 0);
	if (res)
		return EPROTO;

	cfg.g_w = size->w;
	cfg.g_h = size->h;
	cfg.rc_target_bitrate = prm->bitrate / 1024;
	cfg.g_error_resilient = 1;

	re_printf("VPX encoder bitrate: %d\n", cfg.rc_target_bitrate);

	if (st->encup) {
		vpx_codec_destroy(&st->enc);
		st->encup = false;
	}

	res = vpx_codec_enc_init(&st->enc, &vpx_codec_vp8_cx_algo, &cfg, 0);
	if (res) {
		re_fprintf(stderr, "vpx: Failed to initialize encoder: %s\n",
			   vpx_codec_err_to_string(res));
		return EPROTO;
	}

	st->encup = true;
	st->encsz = *size;

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

	st->decup = true;

	st->mb = mbuf_alloc(512);
	if (!st->mb)
		return ENOMEM;

	return 0;
}


static int alloc(struct vidcodec_st **stp, struct vidcodec *vc,
		 const char *name, struct vidcodec_prm *encp,
		 const char *fmtp, vidcodec_enq_h *enqh,
		 vidcodec_send_h *sendh, void *arg)
{
	struct vidcodec_st *st;
	int err;

	(void)fmtp;
	(void)enqh;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->vc = mem_ref(vc);
	st->encprm = *encp;
	st->sendh = sendh;
	st->arg = arg;

	err = init_decoder(st);
	if (err)
		goto out;

	re_printf("video codec %s: %d fps, %d bit/s\n", name,
		  encp->fps, encp->bitrate);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int vpx_packetize(struct vidcodec_st *st, const uint8_t *buf, size_t sz,
			 bool keyframe)
{
	struct mbuf *mb = mbuf_alloc(512);
	const uint8_t *pmax = buf + sz;
	bool fragmented = sz > MAX_RTP_SIZE;
	bool begin = true;
	enum fi fi;
	int err = 0;

	if (!mb)
		return ENOMEM;

	while (buf < pmax) {
		size_t chunk = min(sz, MAX_RTP_SIZE);
		bool last = (sz < MAX_RTP_SIZE);

		mb->pos = mb->end = RTP_PRESZ;

		if (fragmented) {
			if (begin)
				fi = FI_FIRST;
			else if (last)
				fi = FI_LAST;
			else
				fi = FI_MIDDLE;
		}
		else {
			fi = FI_NONE;
		}

		err = vp8desc_enc(mb, true, !keyframe, fi, begin,
				  st->picid);

		begin = false;

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

	if (!st->encup || !vidsz_cmp(&st->encsz, &frame->size)) {

		err = open_encoder(st, &st->encprm, &frame->size);
		if (err)
			return err;
	}

	++st->picid;

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
		bool key;

		switch (pkt->kind) {

		case VPX_CODEC_CX_FRAME_PKT:

			key = pkt->data.frame.flags & VPX_FRAME_IS_KEY;

			err = vpx_packetize(st, pkt->data.frame.buf,
					    pkt->data.frame.sz, key);
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
	struct vp8desc desc;
	vpx_codec_iter_t iter = NULL;
	vpx_codec_err_t res;
	vpx_image_t *img;
	int err;

	if (!mbuf_get_left(src))
		return 0;

	err = vp8desc_dec(&desc, src);
	if (err) {
		re_printf("VP8: decode description failed: %m\n", err);
		return err;
	}

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
		frame->fmt    = VID_FMT_YUV420P;
	}

 out:
	mbuf_rewind(st->mb);

	return err;
}


static int module_init(void)
{
	return vidcodec_register(&vp8, 0, "VP8", "version=0",
				 alloc, enc, NULL, dec, NULL);
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
