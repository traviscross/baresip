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
	MAX_MUTED_FRAMES = 3,
};

enum selfview {
	SELFVIEW_NONE = 0,
	SELFVIEW_WINDOW,
	SELFVIEW_PIP
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
	bool shuttered;         /**< Video source is shuttered            */
	enum selfview selfview; /**< Selfview method                      */
};


/** Video stream - transmitter/encoder direction */
struct vtx {
	struct video *video;               /**< Parent                    */
	struct vidcodec_st *enc;           /**< Current video encoder     */
	struct vidsrc_prm vsrc_prm;        /**< Video source parameters   */
	struct vidsrc_st *vsrc;            /**< Video source              */
	struct vidisp_st *selfview;        /**< Selfview display          */
	struct lock *lock;                 /**< Lock selfview resources   */
	struct vidframe *mute_frame;       /**< Frame with muted video    */
	struct vidframe *frame;            /**< Source frame              */
	int muted_frames;                  /**< # of muted frames sent    */
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
	struct vidcodec_st *dec;           /**< Current video decoder     */
	struct vidisp_prm vidisp_prm;      /**< Video display parameters  */
	struct vidisp_st *vidisp;          /**< Video display             */
	enum vidorient orient;             /**< Display orientation       */
	bool fullscreen;                   /**< Fullscreen flag           */
	uint8_t pt_rx;                     /**< Incoming RTP payload type */
	int frames;                        /**< Number of frames received */
	int efps;                          /**< Estimated frame-rate      */
};


static void vtx_destructor(void *arg)
{
	struct vtx *vtx = arg;

	mem_deref(vtx->vsrc);  /* Note: Must be destroyed first */

	lock_write_get(vtx->lock);
	mem_deref(vtx->selfview);
	mem_deref(vtx->frame);
	lock_rel(vtx->lock);

	mem_deref(vtx->lock);
	mem_deref(vtx->mute_frame);
	mem_deref(vtx->enc);
}


static void vrx_destructor(void *arg)
{
	struct vrx *vrx = arg;

	mem_deref(vrx->dec);
	mem_deref(vrx->vidisp);
}


static void video_destructor(void *arg)
{
	struct video *v = arg;

	video_stop(v);
	tmr_cancel(&v->tmr);

	mem_deref(v->vtx);
	mem_deref(v->vrx);
	mem_deref(v->strm);
	mem_deref(v->peer);
}


static int get_fps(const struct video *v)
{
	const char *attr;

	/* RFC4566 */
	attr = sdp_media_rattr(stream_sdpmedia(v->strm), "framerate");
	if (attr) {
		/* NOTE: fractional values are ignored */
		const double fps = atof(attr);
		return (int)fps;
	}
	else
		return config.video.fps;
}


#if ENABLE_ENCODER
/**
 * Encode video and send via RTP stream
 *
 * @note This function has REAL-TIME properties
 */
static void encode_rtp_send(struct vtx *vtx, const struct vidframe *frame)
{
	int err;

	if (!vtx->enc)
		return;

	lock_write_get(vtx->lock);

	/* Convert image */
	if (frame->fmt != VID_FMT_YUV420P ||
	    !vidsz_cmp(&frame->size, &vtx->vsrc_prm.size) ||
	    vtx->video->selfview == SELFVIEW_PIP) {

		if (!vtx->frame) {

			err = vidframe_alloc(&vtx->frame, VID_FMT_YUV420P,
					     &vtx->vsrc_prm.size);
		}

		vidconv(vtx->frame, frame, 0);
		frame = vtx->frame;
	}

	/* External selfview Window */
	if (vtx->video->selfview == SELFVIEW_WINDOW) {

		if (!vtx->selfview) {
			struct vidisp *vd = (struct vidisp *)vidisp_find(NULL);

			if (vd) {
				vd->alloch(&vtx->selfview, NULL, vd,
					   NULL, NULL, NULL, NULL, NULL);
			}
		}

		(void)vidisp_display(vtx->selfview, "Selfview", vtx->frame);
	}

	lock_rel(vtx->lock);


	/* Encode the whole picture frame */
	err = vidcodec_get(vtx->enc)->ench(vtx->enc, vtx->picup, frame);
	if (err) {
		DEBUG_WARNING("encode rtp send: failed %s\n", strerror(err));
		return;
	}

	vtx->ts_tx += (SRATE/vtx->vsrc_prm.fps);
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
	if (vtx->muted)
		frame = vtx->mute_frame;

	if (vtx->muted && vtx->muted_frames >= MAX_MUTED_FRAMES)
		return;

	/* Encode and send */
	encode_rtp_send(vtx, frame);
	vtx->muted_frames++;
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
	int err;

	vtx = mem_zalloc(sizeof(*vtx), vtx_destructor);
	if (!vtx)
		return ENOMEM;

	err = lock_alloc(&vtx->lock);
	if (err)
		goto out;

	vtx->video = video;
	vtx->ts_tx = 160;
	vtx->pt_tx = PT_NONE;

 out:
	if (err)
		mem_deref(vtx);
	else
		*vtxp = vtx;

	return err;
}


static int vrx_alloc(struct vrx **vrxp, struct video *video)
{
	struct vrx *vrx;

	vrx = mem_zalloc(sizeof(*vrx), vrx_destructor);
	if (!vrx)
		return ENOMEM;

	vrx->video  = video;
	vrx->pt_rx  = PT_NONE;
	vrx->orient = VIDORIENT_PORTRAIT;

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
	struct vtx *vtx = v->vtx;
	struct vidframe frame;
	int err = 0;

	/* No decoder set */
	if (!vrx->dec) {
		DEBUG_WARNING("No video decoder!\n");
		return 0;
	}

	frame.data[0] = NULL;
	err = vidcodec_get(vrx->dec)->dech(vrx->dec, &frame, hdr->m, mb);
	if (err) {
		DEBUG_WARNING("decode error (%s)\n", strerror(err));

		/* send RTCP FIR to peer */
		stream_send_fir(v->strm);

		/* XXX: if RTCP is not enabled, send XML in SIP INFO ? */

		return err;
	}

	/* Got a full picture-frame? */
	if (!vidframe_isvalid(&frame))
		return 0;

	lock_write_get(vtx->lock);

	/* Selfview using PIP -- Picture-In-Picture */
	if (v->selfview == SELFVIEW_PIP && vtx->frame) {

		struct vidrect rect;

		rect.w = frame.size.w / 5;
		rect.h = frame.size.h / 5;
		rect.x = frame.size.w - rect.w - 10;
		rect.y = frame.size.h - rect.h - 10;

		/* todo: problems when writing into FFmpeg buffer */
		vidconv(&frame, vtx->frame, &rect);
	}

	lock_rel(vtx->lock);

	err = vidisp_display(vrx->vidisp, v->peer, &frame);

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

	if (!mb)
		goto out;

	/* Video payload-type changed? */
	if (hdr->pt == v->vrx->pt_rx)
		goto out;

	err = pt_handler(v, v->vrx->pt_rx, hdr->pt);
	if (err)
		return;

 out:
	(void)video_stream_decode(v->vrx, hdr, mb);
}


int video_alloc(struct video **vp, struct call *call,
		struct sdp_session *sdp_sess, int label,
		const struct mnat *mnat, struct mnat_sess *mnat_sess,
		const struct menc *menc, const char *content)
{
	struct video *v;
	struct le *le;
	int err = 0;

	if (!vp)
		return EINVAL;

	v = mem_zalloc(sizeof(*v), video_destructor);
	if (!v)
		return ENOMEM;

	MAGIC_INIT(v);

	tmr_init(&v->tmr);

	if (!str_casecmp(config.video.selfview, "window"))
		v->selfview = SELFVIEW_WINDOW;
	else if (!str_casecmp(config.video.selfview, "pip"))
		v->selfview = SELFVIEW_PIP;
	else
		v->selfview = SELFVIEW_NONE;

	err = stream_alloc(&v->strm, call, sdp_sess, "video", label,
			   mnat, mnat_sess, menc,
			   stream_recv_handler, v);
	if (err)
		goto out;

	err |= sdp_media_set_lattr(stream_sdpmedia(v->strm), true,
				   "framerate", "%d", config.video.fps);

	/* RFC 4585 */
	err |= sdp_media_set_lattr(stream_sdpmedia(v->strm), true,
				   "rtcp-fb", "* nack pli");

	/* RFC 4796 */
	if (content) {
		err |= sdp_media_set_lattr(stream_sdpmedia(v->strm), true,
					   "content", "%s", content);
	}

	if (err)
		goto out;

	err  = vtx_alloc(&v->vtx, v);
	err |= vrx_alloc(&v->vrx, v);
	if (err)
		goto out;

	v->max_rtp_size = 1024;

	/* Video codecs */
	for (le = list_head(ua_vidcodecl(call_get_ua(call)));
	     le;
	     le=le->next) {
		struct vidcodec *vc = le->data;
		err |= sdp_format_add(NULL, stream_sdpmedia(v->strm), false,
				      vc->pt, vc->name, 90000, 1,
				      NULL, vc, false, "%s", vc->fmtp);
	}

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

	/* XXX: update wanted picturesize and send re-invite to peer */
}


/* Set the video display - can be called multiple times */
static int set_vidisp(struct vrx *vrx)
{
	vrx->vidisp = mem_deref(vrx->vidisp);

	/* Video Display */
	vrx->vidisp_prm.view = NULL;

	if (!vrx->video->shuttered && !vrx->vidisp) {
		struct vidisp *vd = (struct vidisp *)vidisp_find(NULL);
		if (!vd)
			return ENOENT;

		return vd->alloch(&vrx->vidisp, NULL, vd, &vrx->vidisp_prm,
				  NULL, vidisp_input_handler,
				  vidisp_resize_handler, vrx);
	}

	return 0;
}
#endif


#if ENABLE_ENCODER
/* Set the encoder format - can be called multiple times */
static int set_encoder_format(struct vtx *vtx, const char *src,
			      const char *dev, struct vidsz *size)
{
	struct vidsrc *vs = (struct vidsrc *)vidsrc_find(src);
	int err;

	if (!vs)
		return ENOENT;

	vtx->vsrc_prm.size   = *size;
	vtx->vsrc_prm.fps    = get_fps(vtx->video);
	vtx->vsrc_prm.orient = VIDORIENT_PORTRAIT;

	vtx->vsrc = mem_deref(vtx->vsrc);

	if (!vtx->video->shuttered) {
		err = vs->alloch(&vtx->vsrc, vs, &vtx->vsrc_prm,
				 NULL, dev, vidsrc_frame_handler,
				 vidsrc_error_handler, vtx);
		if (err) {
			DEBUG_NOTICE("No video source: %s\n", strerror(err));
			return err;
		}
	}

	vtx->mute_frame = mem_deref(vtx->mute_frame);
	err = vidframe_alloc(&vtx->mute_frame, VID_FMT_YUV420P, size);
	if (err) {
		DEBUG_NOTICE("no mute frame: %s\n", strerror(err));
		return err;
	}

	vidframe_fill(vtx->mute_frame, 0xff, 0xff, 0xff);

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


int video_start(struct video *v, const char *src, const char *dev,
		const char *peer)
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
	err = set_encoder_format(v->vtx, src, dev, &config.video.size);
	if (err) {
		DEBUG_WARNING("could not set encoder format to"
			      " [%u x %u] %s\n",
			      config.video.size.w, config.video.size.h,
			      strerror(err));
	}
#else
	(void)src;
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
}


void video_mute(struct video *v, bool muted)
{
	if (!v)
		return;

	v->vtx->muted = muted;
	v->vtx->muted_frames = 0;
	video_update_picture(v);
}


static int vidisp_update(struct vrx *vrx)
{
	struct vidisp *vd = vidisp_get(vrx->vidisp);
	int err = 0;

	if (vd->updateh) {
		err = vd->updateh(vrx->vidisp, vrx->fullscreen,
				  vrx->orient, NULL);
	}

	return err;
}


int video_pip(struct video *v, const struct vidrect *rect)
{
	struct vtx *vtx;
	struct vidisp *vd;
	int err = 0;

	if (!v || !v->vrx->vidisp)
		return EINVAL;

	vtx = v->vtx;

	lock_write_get(vtx->lock);

	vtx->selfview = mem_deref(vtx->selfview);

	/* If rect is NULL we just want to remove the P-I-P,
	 * also if there is no video source, no sense in
	 * getting a PIP
	 */
	if (!rect || !vtx->vsrc)
		goto out;

	vd = vidisp_get(v->vrx->vidisp);
	err = vd->alloch(&vtx->selfview, v->vrx->vidisp, vd, NULL,
			 NULL, NULL, NULL, NULL);
	if (err)
		goto out;

	if (vd->updateh)
		err = vd->updateh(vtx->selfview, false, 0, rect);

 out:
	lock_rel(vtx->lock);

	return err;
}


int video_set_fullscreen(struct video *v, bool fs)
{
	if (!v)
		return EINVAL;

	v->vrx->fullscreen = fs;

	return vidisp_update(v->vrx);
}


static void vidsrc_update(struct vtx *vtx, const char *dev)
{
	struct vidsrc *vs = vidsrc_get(vtx->vsrc);

	if (vs && vs->updateh)
		vs->updateh(vtx->vsrc, &vtx->vsrc_prm, dev);
}


int video_set_orient(struct video *v, enum vidorient orient)
{
	int err = 0;

	if (!v)
		return EINVAL;

	if (v->vtx) {
		v->vtx->vsrc_prm.orient = orient;

		vidsrc_update(v->vtx, NULL);
	}

	if (v->vrx) {
		v->vrx->orient = orient;

		err |= vidisp_update(v->vrx);
	}

	return err;
}


static int vidcodec_send_handler(bool marker, struct mbuf *mb, void *arg)
{
	struct video *v = arg;

	return stream_send(v->strm, marker, v->vtx->pt_tx, v->vtx->ts_tx, mb);
}


static int vc_alloc(struct vidcodec_st **stp, struct vidcodec *vc,
		    struct video *v, const struct pl *fmtp)
{
	struct vidcodec_prm prm;

	prm.size    = config.video.size;
	prm.fps     = get_fps(v);
	prm.bitrate = config.video.bitrate;

	return vc->alloch(stp, vc, vc->name, &prm, &prm, fmtp,
			  vidcodec_send_handler, v);
}


#if ENABLE_ENCODER
int video_encoder_set(struct video *v, struct vidcodec *vc,
		      uint8_t pt_tx, const char *params)
{
	struct pl fmtp = pl_null;
	struct vtx *vtx;
	int err = 0;

	if (!v)
		return EINVAL;

	(void)re_fprintf(stderr, "Set video encoder: %s\n", vc->name);

	pl_set_str(&fmtp, params);

	vtx = v->vtx;
	vtx->pt_tx = pt_tx;
	vtx->enc = mem_deref(vtx->enc);

	if (!vidcodec_cmp(vc, vidcodec_get(v->vrx->dec))) {

		err = vc_alloc(&vtx->enc, vc, v, &fmtp);
		if (err) {
			DEBUG_WARNING("encoder alloc: %s\n", strerror(err));
		}
	}
#if ENABLE_DECODER
	else
		vtx->enc = mem_ref(v->vrx->dec);
#endif

	return err;
}
#else
int video_encoder_set(struct video *v, struct vidcodec *vc,
		      uint8_t pt_tx, const char *params)
{
	(void)v;
	(void)vc;
	(void)pt_tx;
	(void)params;

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

	if (!vidcodec_cmp(vc, vidcodec_get(v->vtx->enc))) {

		err = vc_alloc(&vrx->dec, vc, v, NULL);
		if (err) {
			DEBUG_WARNING("decoder alloc: %s\n", strerror(err));
		}
	}
#if ENABLE_ENCODER
	else {
		vrx->dec = mem_ref(v->vtx->enc);
	}
#endif

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


void video_vidsrc_set_device(struct video *v, const char *dev)
{
	if (v && v->vtx)
		vidsrc_update(v->vtx, dev);
}


void video_sdp_attr_decode(struct video *v)
{
	if (!v)
		return;

	stream_sdp_attr_decode(v->strm);
}


int video_debug(struct re_printf *pf, const struct video *v)
{
	int err;

	if (!v)
		return 0;

	err = re_hprintf(pf, "\n--- Video stream ---\n");

	if (v->vtx) {
		const struct vtx *vtx = v->vtx;

		err |= re_hprintf(pf, " tx: pt=%d, %d x %d, fps=%d\n",
				  vtx->pt_tx, vtx->vsrc_prm.size.w,
				  vtx->vsrc_prm.size.h, vtx->vsrc_prm.fps);
	}

	if (v->vrx) {
		const struct vrx *vrx = v->vrx;

		err |= re_hprintf(pf, " rx: pt=%d\n", vrx->pt_rx);
	}

	err |= stream_debug(pf, v->strm);

	return err;
}


int video_print(struct re_printf *pf, const struct video *v)
{
	if (!v)
		return 0;

	return re_hprintf(pf, " efps=%d/%d", v->vtx->efps, v->vrx->efps);
}


int video_set_shuttered(struct video *v, bool shuttered)
{
	if (!v)
		return EINVAL;

	v->shuttered = shuttered;

	return video_start(v, NULL, config.video.device, NULL);
}
