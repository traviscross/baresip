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


struct vstat {
	uint64_t tsamp;
	uint32_t frames;
	size_t bytes;
	uint32_t bitrate;
	double efps;
};


/** Video loop */
struct video_loop {
	struct tmr tmr;
	struct tmr tmr_bw;
	struct mbuf *mb_loop;
	struct vidframe frame;
	struct lock *lock;
	struct vidsz size;
	struct vidsrc_st *vsrc;
	struct vidcodec_st *codec;
	struct vidisp_st *vidisp;
	struct vstat stat;
};


/*
 * XXX: direct display, no async buffering
 */


/** Calculate the size of an YUV420P frame */
static inline size_t vidframe_size(const struct vidframe *f)
{
	return f->linesize[0] * f->size.h
		+ f->linesize[1] * f->size.h/2
		+ f->linesize[2] * f->size.h/2;
}


static void display(struct video_loop *vl, const struct vidframe *frame)
{
	vl->stat.bytes += vidframe_size(frame);
	(void)vidisp_display(vl->vidisp, "Video Loop", frame);
}


static void codec_path(struct video_loop *vl, const struct vidframe *frame)
{
	int err;

	/* encode */
	err = vidcodec_get(vl->codec)->ench(vl->codec, false, frame);
	if (err) {
		DEBUG_WARNING("codec_encode: %s\n", strerror(err));
		return;
	}
}


static void vidsrc_frame_handler(const struct vidframe *frame, void *arg)
{
	struct video_loop *vl = arg;
	const uint8_t *p = frame->data[0]; /* note: assumes YUV420P */

	++vl->stat.frames;

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


static void vidloop_destructor(void *arg)
{
	struct video_loop *vl = arg;

	tmr_cancel(&vl->tmr);
	tmr_cancel(&vl->tmr_bw);

	mem_deref(vl->codec);
	mem_deref(vl->vsrc);
	mem_deref(vl->vidisp);
	mem_deref(vl->mb_loop);
	mem_deref(vl->lock);
}


static void vidisp_input_handler(char key, void *arg)
{
	(void)arg;
	ui_input(key, NULL);
}


static int vidcodec_send_handler(bool marker, struct mbuf *mb, void *arg)
{
	struct video_loop *vl = arg;
	struct vidframe frame;
	int err;

	vl->stat.bytes += mbuf_get_left(mb);

	/* decode */
	frame.valid = false;
	err = vidcodec_get(vl->codec)->dech(vl->codec, &frame, marker, mb);
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
	struct vidcodec_prm prm;
	int err;

	prm.size    = vl->size;
	prm.fps     = config.video.fps;
	prm.bitrate = config.video.bitrate;

	/* Use the first video codec */
	err = vidcodec_alloc(&vl->codec, vidcodec_name(vidcodec_find(NULL)),
			     &prm, &prm, NULL, vidcodec_send_handler, vl);
	if (err) {
		DEBUG_WARNING("alloc encoder: %s\n", strerror(err));
		return err;
	}

	/* OK */
	return 0;
}


static void print_status(struct video_loop *vl)
{
	(void)re_fprintf(stderr, "\rstatus: EFPS=%.1f      %u kbit/s       \r",
			 vl->stat.efps, vl->stat.bitrate);
}


/* use re-timer to workaround SDL/Darwin multi-threading issues .. */
static void timeout(void *arg)
{
	struct video_loop *vl = arg;
	int h;

	tmr_start(&vl->tmr, 10, timeout, vl);

	lock_write_get(vl->lock);

	h = vl->frame.size.h;

	vl->frame.data[0] = vl->mb_loop->buf;
	vl->frame.data[1] = vl->frame.data[0] + vl->frame.linesize[0] * h;
	vl->frame.data[2] = vl->frame.data[1] + vl->frame.linesize[1] * h /2;

	/* the buffer has only 1 slot */
	if (vl->mb_loop->end > 0) {

		if (vl->codec)
			codec_path(vl, &vl->frame);
		else
			display(vl, &vl->frame);

		mbuf_rewind(vl->mb_loop);
	}

	lock_rel(vl->lock);
}


static void calc_bitrate(struct video_loop *vl)
{
	const uint64_t now = tmr_jiffies();

	if (now > vl->stat.tsamp) {

		const uint32_t dur = (uint32_t)(now - vl->stat.tsamp);

		vl->stat.efps = 1000.0f * vl->stat.frames / dur;

		vl->stat.bitrate = (uint32_t) (8 * vl->stat.bytes / dur);
	}

	vl->stat.frames = 0;
	vl->stat.bytes = 0;
	vl->stat.tsamp = now;
}


static void timeout_bw(void *arg)
{
	struct video_loop *vl = arg;

	tmr_start(&vl->tmr_bw, 5000, timeout_bw, vl);

	calc_bitrate(vl);
	print_status(vl);
}


static int vsrc_reopen(struct video_loop *vl, const struct vidsz *sz)
{
	struct vidsrc *vs = (struct vidsrc *)vidsrc_find(NULL);
	struct vidsrc_prm prm;
	int err;

	if (!vs)
		return ENOENT;

	(void)re_printf("%s: open video source: %u x %u\n",
			vs->name, sz->w, sz->h);

	vl->size = *sz;

	prm.size   = *sz;
	prm.orient = VIDORIENT_PORTRAIT;
	prm.fps    = config.video.fps;

	vl->vsrc = mem_deref(vl->vsrc);

	err = vs->alloch(&vl->vsrc, vs, &prm, NULL,
			  config.video.device, vidsrc_frame_handler,
			  NULL, vl);
	if (err) {
		DEBUG_WARNING("vidsrc %s failed: %s\n",
			      vs->name, strerror(err));
	}

	return err;
}


static int video_loop_alloc(struct video_loop **vlp, const struct vidsz *size)
{
	struct video_loop *vl;
	struct vidisp *vd;
	int err;

	vd = (struct vidisp *)vidisp_find(NULL);
	if (!vd)
		return ENOENT;

	vl = mem_zalloc(sizeof(*vl), vidloop_destructor);
	if (!vl)
		return ENOMEM;

	err = lock_alloc(&vl->lock);
	if (err)
		goto out;

	tmr_init(&vl->tmr);
	tmr_init(&vl->tmr_bw);

	vl->mb_loop = mbuf_alloc(115200);
	if (!vl->mb_loop) {
		err = ENOMEM;
		goto out;
	}

	err = vsrc_reopen(vl, size);
	if (err)
		goto out;

	err = vd->alloch(&vl->vidisp, NULL, vd, NULL, NULL,
			 vidisp_input_handler, NULL, vl);
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
		if (vl)
			(void)re_printf("Disable video-loop\n");
		vl = mem_deref(vl);
		return;
	}

	if (vl) {
		if (vl->codec)
			vl->codec = mem_deref(vl->codec);
		else
			(void)enable_codec(vl);

		(void)re_printf("%sabled codec: %s\n",
				vl->codec ? "En" : "Dis",
				vidcodec_name(vidcodec_get(vl->codec)));
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
