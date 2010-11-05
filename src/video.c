/**
 * @file video.c  Video stream
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <stdlib.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "video"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/** Magic number */
#define MAGIC 0x00070d10
#include "magic.h"


/* Useful macro switches for development/testing */
#define ENABLE_ENCODER 1
#define ENABLE_DECODER 1


enum {
	SRATE = 90000,
};


/**
 * Implements a generic video stream. The application can allocate multiple
 * instances of a video stream, mapping it to a particular SDP media line.
 * The video object has a Video Display and Source, and a video encoder
 * and decoder. A particular video object is mapped to a generic media
 * stream object.
 *
 *<pre>
 *            recv  send
 *              |    /|\
 *             \|/    |
 *            .---------.    .-------.
 *            |  video  |--->|encoder|
 *            |         |    |-------|
 *            | object  |--->|decoder|
 *            '---------'    '-------'
 *              |    /|\
 *              |     |
 *             \|/    |
 *        .-------.  .-------.
 *        |Video  |  |Video  |
 *        |Display|  |Source |
 *        '-------'  '-------'
 *</pre>
 */
struct video {
	MAGIC_DECL              /**< Magic number for debugging           */
	struct stream *strm;    /**< Generic media stream                 */
	struct vtx *vtx;        /**< Transmit/encoder direction           */
	struct vrx *vrx;        /**< Receive/decoder direction            */
	struct tmr tmr;         /**< Timer for frame-rate estimation      */
	size_t max_rtp_size;    /**< Maximum size of outgoing RTP packets */
	char *peer;             /**< Peer URI                             */
};


/** Video stream - transmitter/encoder direction */
struct vtx {
	struct video *video;               /**< Parent                    */
	struct vidcodec_prm enc_prm;       /**< Encoder parameters        */
	struct vidcodec_st *enc;           /**< Current video encoder     */
	struct vidsrc_prm vsrc_param;      /**< Video source parameters   */
	struct vidsrc_st *vsrc;            /**< Video source              */
	struct vidisp_st *selfview;        /**< Video selfview            */
	struct vidframe *mute_frame;       /**< Frame with muted video    */
	uint8_t pt_tx;                     /**< Outgoing RTP payload type */
	uint32_t ts_tx;                    /**< Outgoing RTP timestamp    */
	bool picup;                        /**< Send picture update       */
	bool muted;                        /**< Muted flag                */
	int frames;                        /**< Number of frames sent     */
	int efps;                          /**< Estimated frame-rate      */
};


/** Video stream - receiver/decoder direction */
struct vrx {
	struct video *video;               /**< Parent                    */
	struct vidcodec_prm dec_prm;       /**< Decoder parameters        */
	struct vidcodec_st *dec;           /**< Current video decoder     */
	struct vidisp_prm vidisp_prm;      /**< Video display parameters  */
	struct vidisp_st *vidisp;          /**< Video display             */
	uint8_t pt_rx;                     /**< Incoming RTP payload type */
	int frames;                        /**< Number of frames received */
	int efps;                          /**< Estimated frame-rate      */
};


static void vtx_destructor(void *data)
{
	struct vtx *vtx = data;

	mem_deref(vtx->vsrc);  /* Note: Must be destroyed first */
	mem_deref(vtx->selfview);
	mem_deref(vtx->mute_frame);
	mem_deref(vtx->enc);
}


static void vrx_destructor(void *data)
{
	struct vrx *vrx = data;

	mem_deref(vrx->dec);
	mem_deref(vrx->vidisp);
}


static void video_destructor(void *data)
{
	struct video *v = data;

	video_stop(v);
	tmr_cancel(&v->tmr);

	mem_deref(v->vtx);
	mem_deref(v->vrx);
	mem_deref(v->strm);
	mem_deref(v->peer);
}


#if ENABLE_ENCODER
/**
 * Encode video and send via RTP stream
 *
 * @note This function has REAL-TIME properties
 */
static void encode_rtp_send(struct vtx *vtx, const struct vidframe *frame)
{
	struct video *v = vtx->video;
	int err;

	if (!vtx->enc || !stream_is_active(v->strm))
		return;

	/* Encode the whole picture frame */
	err = vidcodec_get(vtx->enc)->ench(vtx->enc, vtx->picup, frame);
	if (err) {
		DEBUG_WARNING("encode rtp send: failed %s\n", strerror(err));
		return;
	}

	vtx->ts_tx += (SRATE/vtx->enc_prm.fps);
	vtx->picup = false;
}


/**
 * Read frames from video source
 *
 * @note This function has REAL-TIME properties
 */
static void vidsrc_frame_handler(const struct vidframe *frame, void *arg)
{
	struct vtx *vtx = arg;

	++vtx->frames;

	/* Is the video muted? If so insert video mute image */
	if (vtx->muted) {
		frame = vtx->mute_frame;
	}

	if (vtx->selfview)
		(void)vidisp_display(vtx->selfview, "", frame);

	/* Encode and send */
	encode_rtp_send(vtx, frame);
}


static void vidsrc_error_handler(int err, void *arg)
{
	struct vtx *vtx = arg;

	DEBUG_WARNING("Video-source error: %s\n", strerror(err));

	vtx->vsrc = mem_deref(vtx->vsrc);
}
#endif


static int vtx_alloc(struct vtx **vtxp, struct video *video)
{
	struct vtx *vtx;

	vtx = mem_zalloc(sizeof(*vtx), vtx_destructor);
	if (!vtx)
		return ENOMEM;

	vtx->video = video;
	vtx->ts_tx = 160;
	vtx->pt_tx = PT_NONE;
	vtx->enc_prm.size = config.video.size;
	vtx->enc_prm.fps = config.video.fps;

	*vtxp = vtx;

	return 0;
}


static int vrx_alloc(struct vrx **vrxp, struct video *video)
{
	struct vrx *vrx;

	vrx = mem_zalloc(sizeof(*vrx), vrx_destructor);
	if (!vrx)
		return ENOMEM;

	vrx->video = video;
	vrx->pt_rx = PT_NONE;
	vrx->dec_prm.size = config.video.size;
	vrx->dec_prm.fps = config.video.fps;

	*vrxp = vrx;

	return 0;
}


#if ENABLE_DECODER
/**
 * Decode incoming RTP packets using the Video decoder
 *
 * NOTE: mb=NULL if no packet received
 */
static int video_stream_decode(struct vrx *vrx, const struct rtp_header *hdr,
			       struct mbuf *mb)
{
	struct video *v = vrx->video;
	struct vidframe frame;
	int err = 0;

	/* No decoder set */
	if (!vrx->dec) {
		DEBUG_WARNING("No video decoder!\n");
		return 0;
	}

	frame.valid = false;
	err = vidcodec_get(vrx->dec)->dech(vrx->dec, &frame, hdr->m, mb);
	if (err) {
		DEBUG_WARNING("decode error (%s)\n", strerror(err));

		/* send RTCP FIR to peer */
		stream_send_fir(v->strm);

		/* XXX: if RTCP is not enabled, send XML in SIP INFO ? */

		return err;
	}

	/* Got a full picture-frame? */
	if (!frame.valid)
		return 0;

	err |= vidisp_display(vrx->vidisp, v->peer, &frame);
	++vrx->frames;

	return err;
}
#else
static int video_stream_decode(struct vrx *vrx, const struct rtp_header *hdr,
			       struct mbuf *mb)
{
	(void)vrx;
	(void)hdr;
	(void)mb;
	return 0;
}
#endif


static int pt_handler(struct video *v, uint8_t pt_old, uint8_t pt_new)
{
	const struct sdp_format *lc;

	lc = sdp_media_lformat(stream_sdpmedia(v->strm), pt_new);
	if (!lc)
		return ENOENT;

	(void)re_fprintf(stderr, "Video decoder changed payload %u -> %u\n",
			 pt_old, pt_new);

	return video_decoder_set(v, lc->data, lc->pt);
}


/* Handle incoming stream data from the network */
static void stream_recv_handler(const struct rtp_header *hdr,
				struct mbuf *mb, void *arg)
{
	struct video *v = arg;
	int err;

	MAGIC_CHECK(v);

	/* Video payload-type changed? */
	if (hdr->pt == v->vrx->pt_rx)
		goto out;

	err = pt_handler(v, v->vrx->pt_rx, hdr->pt);
	if (err)
		return;

 out:
	(void)video_stream_decode(v->vrx, hdr, mb);
}


int video_alloc(struct video **vp, struct call *call)
{
	struct video *v;
	int err = 0;

	if (!vp)
		return EINVAL;

	v = mem_zalloc(sizeof(*v), video_destructor);
	if (!v)
		return ENOMEM;

	MAGIC_INIT(v);

	tmr_init(&v->tmr);

	err = stream_alloc(&v->strm, call, "video", 2, stream_recv_handler, v);
	if (err)
		goto out;

	err  = vtx_alloc(&v->vtx, v);
	err |= vrx_alloc(&v->vrx, v);
	if (err)
		goto out;

	v->max_rtp_size = 1024;

 out:
	if (err)
		mem_deref(v);
	else
		*vp = v;

	return err;
}


#if ENABLE_DECODER
static void vidisp_input_handler(char key, void *arg)
{
	struct vrx *vrx = arg;

	(void)vrx;

	ui_input(key, NULL);
}


static void vidisp_resize_handler(const struct vidsz *sz, void *arg)
{
	struct vrx *vrx = arg;
	(void)vrx;

	(void)re_printf("resize: %u x %u\n", sz->w, sz->h);

	/* TODO: update wanted picturesize and send re-invite to peer */
}


/* Set the video display - can be called multiple times */
static int set_vidisp(struct vrx *vrx)
{
	vrx->vidisp_prm.view = NULL;
	if (!vrx->vidisp) {
		return vidisp_alloc(&vrx->vidisp, &vrx->vidisp_prm, NULL, NULL,
				    vidisp_input_handler,
				    vidisp_resize_handler, vrx);
	}

	return 0;
}
#endif


#if ENABLE_ENCODER
/* Set the encoder format - can be called multiple times */
static int set_encoder_format(struct vtx *vtx, const char *dev,
			      struct vidsz *size)
{
	int err;

	vtx->vsrc_param.size = *size;
	vtx->vsrc_param.fps  = config.video.fps;

	vtx->vsrc = mem_deref(vtx->vsrc);
	err = vidsrc_alloc(&vtx->vsrc, NULL, &vtx->vsrc_param,
			   NULL, dev, vidsrc_frame_handler,
			   vidsrc_error_handler, vtx);
	if (err) {
		DEBUG_NOTICE("No video source: %s\n", strerror(err));
		return err;
	}
	vtx->mute_frame = mem_deref(vtx->mute_frame);
	err = vidframe_alloc_filled(&vtx->mute_frame, size, 0x66, 0xff, 0xff);
	if (err) {
		DEBUG_NOTICE("no mute frame: %s\n", strerror(err));
		return err;
	}

	return err;
}
#endif

enum {TMR_INTERVAL = 5};
static void tmr_handler(void *arg)
{
	struct video *v = arg;

	tmr_start(&v->tmr, TMR_INTERVAL * 1000, tmr_handler, v);

	/* Estimate framerates */
	v->vtx->efps = v->vtx->frames / TMR_INTERVAL;
	v->vrx->efps = v->vrx->frames / TMR_INTERVAL;

	v->vtx->frames = 0;
	v->vrx->frames = 0;
}


int video_start(struct video *v, const char *dev, const char *peer)
{
	int err;

	if (!v)
		return EINVAL;

	if (peer) {
		mem_deref(v->peer);
		err = str_dup(&v->peer, peer);
		if (err)
			return err;
	}

	stream_set_srate(v->strm, SRATE, SRATE);

	err = stream_start(v->strm);
	if (err)
		return err;

#if ENABLE_DECODER
	err = set_vidisp(v->vrx);
	if (err) {
		DEBUG_WARNING("could not set vidisp: %s\n", strerror(err));
	}
#endif

#if ENABLE_ENCODER
	err = set_encoder_format(v->vtx, dev, &config.video.size);
	if (err) {
		DEBUG_WARNING("could not set encoder format to"
			      " [%ux%u -> %ux%u]: %s\n",
			      config.video.size.w, config.video.size.h,
			      v->vtx->enc_prm.size.w, v->vtx->enc_prm.size.h,
			      strerror(err));
	}
#else
	(void)dev;
#endif

	tmr_start(&v->tmr, TMR_INTERVAL * 1000, tmr_handler, v);

	return 0;
}


void video_stop(struct video *v)
{
	if (!v)
		return;

	MAGIC_CHECK(v);

	if (v->vtx)
		v->vtx->vsrc = mem_deref(v->vtx->vsrc);

	stream_stop(v->strm);
}


void video_mute(struct video *v, bool muted)
{
	if (!v)
		return;

	v->vtx->muted = muted;
	video_update_picture(v);
}


int video_selfview(struct video *v, void *view)
{
	struct vidisp_prm vprm;

	if (!v)
		return EINVAL;

	if (view) {
		/* Video Display */
		vprm.view = view;
		if (!v->vtx->selfview) {
			return vidisp_alloc(&v->vtx->selfview, &vprm,
					    NULL, NULL, NULL, NULL, v);
		}
	}
	else {
		v->vtx->selfview = mem_deref(v->vtx->selfview);
	}

	return 0;
}


int video_pip(struct video *v, const struct vidrect *rect)
{
	(void)v;
	(void)rect;
	return ENOSYS; /* todo */
}


int video_set_fullscreen(struct video *v, bool fs)
{
	if (!v)
		return EINVAL;

	(void)fs;

	return ENOSYS; /* todo */
}


static int vidcodec_send_handler(bool marker, struct mbuf *mb, void *arg)
{
	struct video *v = arg;

	return stream_send(v->strm, marker, v->vtx->pt_tx, v->vtx->ts_tx, mb);
}


#if ENABLE_ENCODER
int video_encoder_set(struct video *v, struct vidcodec *vc,
		      uint8_t pt_tx, const char *params)
{
	struct vtx *vtx;
	struct pl fmtp = pl_null;
	int err = 0;

	if (!v)
		return EINVAL;

	(void)re_fprintf(stderr, "Set video encoder: %s\n", vc->name);

	pl_set_str(&fmtp, params);

	vtx = v->vtx;
	vtx->pt_tx = pt_tx;

	vtx->enc = mem_deref(vtx->enc);

#if ENABLE_DECODER
	if (vidcodec_cmp(vc, vidcodec_get(v->vrx->dec))) {
		vtx->enc = mem_ref(v->vrx->dec);
		return 0;
	}
#endif

	vtx->enc_prm.bitrate = config.video.bitrate;

	err = vc->alloch(&vtx->enc, vc, vc->name,
			 &vtx->enc_prm, &v->vrx->dec_prm,
			 &fmtp, vidcodec_send_handler, v);

	if (err) {
		DEBUG_WARNING("video_encoder_set: codec_alloc() failed (%s)\n",
			      strerror(err));
	}

	return err;
}
#else
int video_encoder_set(struct video *v, struct vidcodec *vc,
		      uint8_t pt_tx, const char *params, uint32_t bitrate)
{
	(void)v;
	(void)vc;
	(void)pt_tx;
	(void)params;
	(void)bitrate;

	return 0;
}
#endif


int video_decoder_set(struct video *v, struct vidcodec *vc, uint8_t pt_rx)
{
	struct vrx *vrx;
	int err = 0;

	if (!v)
		return EINVAL;

	(void)re_fprintf(stderr, "Set video decoder: %s\n", vc->name);

	vrx = v->vrx;

#if ENABLE_DECODER
	vrx->pt_rx = pt_rx;
	vrx->dec = mem_deref(vrx->dec);

#if ENABLE_ENCODER
	if (v->vtx->enc && vidcodec_cmp(vc, vidcodec_get(v->vtx->enc))) {
		vrx->dec = mem_ref(v->vtx->enc);
		return 0;
	}
#endif

	err = vc->alloch(&vrx->dec, vc, vc->name,
			 &v->vtx->enc_prm, &vrx->dec_prm,
			 NULL, vidcodec_send_handler, v);
	if (err) {
		DEBUG_WARNING("set decoder: vidcodec_alloc() failed: %s\n",
			      strerror(err));
	}
#else
	(void)vc;
	(void)pt_rx;
#endif

	return err;
}


struct stream *video_strm(const struct video *v)
{
	return v ? v->strm : NULL;
}


void video_update_picture(struct video *v)
{
	if (!v || !v->vtx)
		return;
	v->vtx->picup = true;
}


void *video_view(const struct video *v)
{
	if (!v || !v->vrx)
		return NULL;

	return v->vrx->vidisp_prm.view;
}


int video_sdp_attr_encode(const struct video *v, struct sdp_media *m)
{
	int err = 0;

	if (!v)
		return EINVAL;

	err |= sdp_media_set_lattr(m, true, "framerate", "%d",
				   config.video.fps);

	/* RFC 4585 */
	err |= sdp_media_set_lattr(m, true, "rtcp-fb", "* nack pli");

	err |= stream_sdp_attr_encode(v->strm, m);

	return err;
}


void video_sdp_attr_decode(struct video *v, struct sdp_media *m)
{
	const char *attr;

	if (!v || !m)
		return;

	/* RFC4566 */
	attr = sdp_media_rattr(m, "framerate");
	if (attr) {
		/* NOTE: fractional values are ignored */
		const double fps = atof(attr);
		v->vtx->enc_prm.fps = (int)fps;
	}

	stream_sdp_attr_decode(v->strm);
}


int video_debug(struct re_printf *pf, const struct video *v)
{
	int err;

	if (!v)
		return 0;

	err  = re_hprintf(pf, "\n--- Video stream ---\n");
	err |= re_hprintf(pf, " pt_tx=%u pt_rx=%u\n",
			  v->vtx->pt_tx, v->vrx->pt_rx);
	err |= stream_debug(pf, v->strm);

	return err;
}


int video_print(struct re_printf *pf, const struct video *v)
{
	if (!v)
		return 0;

	return re_hprintf(pf, " efps=%d/%d", v->vtx->efps, v->vrx->efps);
}
