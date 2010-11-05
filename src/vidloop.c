/**
 * @file vidloop.c  Video loop
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _BSD_SOURCE 1
#include <string.h>
#include <time.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "vidloop"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


enum mode {
	MODE_NORMAL = 0,
	MODE_CODEC,

	MODE_MAX
};

/** Video loop */
struct video_loop {
	enum mode mode;
	struct tmr tmr;
	struct tmr tmr_bw;
	struct mbuf *mb_loop;
	struct vidframe frame;
	struct lock *lock;
	uint32_t frames;
	time_t start;

	struct vidsrc_prm vsrc_param;
	struct vidsrc_st *vsrc;

	struct vidcodec_prm enc_prm;
	struct vidcodec_st *enc;
	struct mbuf *mb_rtp;
	struct vidcodec_st *dec;

	struct vidisp_st *vidisp;

	size_t bytes;
	uint64_t tsamp;
	uint32_t bitrate;
};


/* Video flow:
 *
 *   v4l:                320 x 240  RGB24
 *
 *   scaler for encoder
 *
 *   H.263 encoder:      352 x 288  YUV420P
 *
 *   H.263 decoder:      352 x 288  YUV420P
 *
 *   scaler for decoder
 *
 *   Sink output:        640 x 480  YUV420P (example)
 *
 * XXX: direct display, no async buffering
 */


static int vsrc_reopen(struct video_loop *vl, const struct vidsz *sz);


/** Calculate the size of an YUV420P frame */
static inline size_t vidframe_size(const struct vidframe *f)
{
	return f->linesize[0] * f->size.h
		+ f->linesize[1] * f->size.h/2
		+ f->linesize[2] * f->size.h/2;
}


static void display(struct video_loop *vl, const struct vidframe *frame)
{
	vl->bytes += vidframe_size(frame);
	(void)vidisp_display(vl->vidisp, "Video Loop", frame);
}


static void codec_path(struct video_loop *vl, const struct vidframe *frame)
{
	int err;

	/* encode */
	vl->mb_rtp->pos = vl->mb_rtp->end = 0;
	err = vidcodec_get(vl->enc)->ench(vl->enc, false, frame);
	if (err) {
		DEBUG_WARNING("codec_encode: %s\n", strerror(err));
		return;
	}

	/* loop RTP */
	vl->mb_rtp->pos = 0;
}


static void vidsrc_frame_handler(const struct vidframe *frame, void *arg)
{
	struct video_loop *vl = arg;
	const uint8_t *p = frame->data[0]; /* note: assumes YUV420P */

	if (!frame->valid) {
		(void)re_fprintf(stderr, "dropping invalid frame\n");
		return;
	}

	++vl->frames;

	lock_write_get(vl->lock);

	if (vl->mb_loop->end > 0) {
		(void)re_printf("busy - skip frame\n");
	}
	else {
		/* copy picture size */
		vl->frame = *frame;

		mbuf_rewind(vl->mb_loop);
		(void)mbuf_write_mem(vl->mb_loop, p, vidframe_size(frame));
	}

	lock_rel(vl->lock);
}


static void disable_codec(struct video_loop *vl)
{
	vl->enc    = mem_deref(vl->enc);
	vl->dec    = mem_deref(vl->dec);
	vl->mb_rtp = mem_deref(vl->mb_rtp);
}


static void vidloop_destructor(void *data)
{
	struct video_loop *vl = data;

	tmr_cancel(&vl->tmr);
	tmr_cancel(&vl->tmr_bw);

	disable_codec(vl);

	mem_deref(vl->vsrc);
	mem_deref(vl->vidisp);
	mem_deref(vl->mb_loop);
	mem_deref(vl->lock);
}


static void scale(struct video_loop *vl, int diff)
{
	struct vidsz sz = vl->vsrc_param.size;

	sz.w += diff;
	sz.h += diff;

	(void)vsrc_reopen(vl, &sz);
}


static void vidisp_input_handler(char key, void *arg)
{
	struct video_loop *vl = arg;

	switch (key) {

	case '+':
		scale(vl, +16);
		break;

	case '-':
		scale(vl, -16);
		break;

	default:
		ui_input(key, NULL);
		break;
	}
}


static void vidisp_resize_handler(const struct vidsz *sz, void *arg)
{
	struct video_loop *vl = arg;
	int err;

	lock_write_get(vl->lock);

	DEBUG_NOTICE("new size: %u x %u\n", sz->w, sz->h);

	/* Flush buffer */
	mbuf_rewind(vl->mb_loop);

	/* signal to vsrc/encoder */
	err = vsrc_reopen(vl, sz);
	if (err) {
		DEBUG_WARNING("vsrc_reopen (%ux%u): %s\n",
			      sz->w, sz->h, strerror(err));
	}

	lock_rel(vl->lock);
}


static int vidcodec_send_handler(bool marker, struct mbuf *mb, void *arg)
{
	struct video_loop *vl = arg;
	struct vidframe frame;
	int err;

	vl->bytes += mbuf_get_left(mb);

	/* decode */
	frame.valid = false;
	err = vidcodec_get(vl->dec)->dech(vl->dec, &frame, marker, mb);
	if (err) {
		DEBUG_WARNING("codec_decode: %s\n", strerror(err));
		return err;
	}

	/* display - if valid picture frame */
	if (frame.valid)
		(void)vidisp_display(vl->vidisp, "Video Loop", &frame);

	return 0;
}


static int enable_codec(struct video_loop *vl)
{
	int err;

	vl->enc_prm.size    = vl->vsrc_param.size;
	vl->enc_prm.bitrate = config.video.bitrate;
	vl->enc_prm.fps     = config.video.fps;

	/* Use the first video codec */
	err = vidcodec_alloc(&vl->enc, vidcodec_name(vidcodec_find(NULL)),
			     &vl->enc_prm, &vl->enc_prm,
			     NULL, vidcodec_send_handler, vl);
	if (err) {
		DEBUG_WARNING("alloc encoder: %s\n", strerror(err));
		return err;
	}

	vl->dec = mem_ref(vl->enc);

	vl->mb_rtp = mbuf_alloc(2*32768);
	if (!vl->mb_rtp)
		return ENOMEM;

	(void)re_printf("enabled codec: %s\n",
			vidcodec_name(vidcodec_get(vl->enc)));

	/* OK */
	return 0;
}


static int update_mode(struct video_loop *vl)
{
	int err = 0;

	switch (vl->mode) {

	case MODE_NORMAL:
		disable_codec(vl);
		break;

	case MODE_CODEC:
		err = enable_codec(vl);
		break;

	default:
		DEBUG_WARNING("invalid mode %d\n", vl->mode);
		break;
	}

	return err;
}


static void print_status(struct video_loop *vl)
{
	uint32_t fps, dur;

	dur = (uint32_t)(time(NULL) - vl->start);
	fps = dur ? vl->frames / dur : 0;

	(void)re_fprintf(stderr, "\rstatus: fps=%u      %u kbit/s       \r",
			 fps, vl->bitrate);
}


/* use re-timer to workaround SDL/Darwin multi-threading issues .. */
static void timeout(void *arg)
{
	struct video_loop *vl = arg;
	int h;

	tmr_start(&vl->tmr, 10, timeout, vl);
	print_status(vl);

	lock_write_get(vl->lock);

	h = vl->frame.size.h;

	vl->frame.data[0] = vl->mb_loop->buf;
	vl->frame.data[1] = vl->frame.data[0] + vl->frame.linesize[0] * h;
	vl->frame.data[2] = vl->frame.data[1] + vl->frame.linesize[1] * h /2;

	/* the buffer has only 1 slot */
	if (vl->mb_loop->end > 0) {

		switch (vl->mode) {

		case MODE_NORMAL:
			display(vl, &vl->frame);
			break;

		case MODE_CODEC:
			codec_path(vl, &vl->frame);
			break;

		default:
			break;
		}

		mbuf_rewind(vl->mb_loop);
	}

	lock_rel(vl->lock);
}


static void calc_bitrate(struct video_loop *vl)
{
	const uint64_t now = tmr_jiffies();

	if (now > vl->tsamp)
		vl->bitrate = (uint32_t) (8 * vl->bytes / (now - vl->tsamp));

	vl->bytes = 0;
	vl->tsamp = now;
}


static void timeout_bw(void *arg)
{
	struct video_loop *vl = arg;

	tmr_start(&vl->tmr_bw, 5000, timeout_bw, vl);
	calc_bitrate(vl);
}


static int vsrc_reopen(struct video_loop *vl, const struct vidsz *sz)
{
	int err;

	(void)re_printf("open video source: %ux%u\n", sz->w, sz->h);

	vl->vsrc_param.size = *sz;
	vl->vsrc_param.fps  = config.video.fps;

	vl->vsrc = mem_deref(vl->vsrc);
	err = vidsrc_alloc(&vl->vsrc, NULL, &vl->vsrc_param, NULL,
			   config.video.device, vidsrc_frame_handler,
			   NULL, vl);
	return err;
}


static int video_loop_alloc(struct video_loop **vlp, const struct vidsz *size)
{
	struct video_loop *vl;
	int err;

	vl = mem_zalloc(sizeof(*vl), vidloop_destructor);
	if (!vl)
		return ENOMEM;

	err = lock_alloc(&vl->lock);
	if (err)
		goto out;

	tmr_init(&vl->tmr);
	tmr_init(&vl->tmr_bw);
	vl->start = time(NULL);

	vl->mb_loop = mbuf_alloc(115200);
	if (!vl->mb_loop) {
		err = ENOMEM;
		goto out;
	}

	err = vsrc_reopen(vl, size);
	if (err)
		goto out;

	(void)re_printf("%s: %ux%u\n", config.video.device,
			vl->vsrc_param.size.w, vl->vsrc_param.size.h);

	err = vidisp_alloc(&vl->vidisp, NULL, NULL, NULL,
			   vidisp_input_handler, vidisp_resize_handler, vl);
	if (err) {
		DEBUG_WARNING("video display failed: %s\n", strerror(err));
		goto out;
	}

	tmr_start(&vl->tmr, 20, timeout, vl);
	tmr_start(&vl->tmr_bw, 1000, timeout_bw, vl);

 out:
	if (err)
		mem_deref(vl);
	else
		*vlp = vl;

	return err;
}


void video_loop_test(bool stop)
{
	static struct video_loop *vl = NULL;
	int err;

	if (stop) {
		vl = mem_deref(vl);
		return;
	}

	if (vl) {
		/* toggle mode */
		vl->mode++;

		if (vl->mode >= MODE_MAX) {
			(void)re_printf("Disable video-loop\n");
			vl = mem_deref(vl);
			return;
		}

		err = update_mode(vl);
		if (err) {
			DEBUG_WARNING("update_mode failed: %s\n",
				      strerror(err));
			vl = mem_deref(vl);
		}
	}
	else {
		(void)re_printf("Enable video-loop on %s: %u x %u\n",
				config.video.device,
				config.video.size.w, config.video.size.h);
		err = video_loop_alloc(&vl, &config.video.size);
		if (err) {
			DEBUG_WARNING("vidloop alloc: %s\n", strerror(err));
		}
	}
}
