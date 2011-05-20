/**
 * @file x11grab.c X11 grabbing video-source
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _BSD_SOURCE 1
#include <unistd.h>
#define _XOPEN_SOURCE 1
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <pthread.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "x11grab"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/*
 * TODO: add option to select a specific X window
 * TODO: how to select x,y offset ?
 */


/* extra const-correctness added in 0.9.0 */
#if LIBSWSCALE_VERSION_INT >= ((0<<16) + (9<<8) + (0))
#define SRCSLICE_CAST (const uint8_t **)
#else
#define SRCSLICE_CAST (uint8_t **)
#endif


struct vidsrc_st {
	struct vidsrc *vs;  /* inheritance */
	Display *disp;
	XImage *image;
	pthread_t thread;
	bool run;
	int fps;
	struct vidsz size;
	struct SwsContext *sws;
	enum PixelFormat pixfmt;
	vidsrc_frame_h *frameh;
	void *arg;
};


static struct vidsrc *vidsrc;


static int x11grab_open(struct vidsrc_st *st, const struct vidsz *sz)
{
	int screen_num, screen_width, screen_height;
	int x = 0, y = 0;

	st->disp = XOpenDisplay(NULL);
	if (!st->disp) {
		DEBUG_WARNING("error opening display\n");
		return ENODEV;
	}

	screen_num = DefaultScreen(st->disp);
	screen_width = DisplayWidth(st->disp, screen_num);
	screen_height = DisplayHeight(st->disp, screen_num);

	DEBUG_NOTICE("screen size: %d x %d\n", screen_width, screen_height);

	st->image = XGetImage(st->disp,
			      RootWindow(st->disp, DefaultScreen(st->disp)),
			      x, y, sz->w, sz->h, AllPlanes, ZPixmap);
	if (!st->image) {
		DEBUG_WARNING("error creating Ximage\n");
		return ENODEV;
	}

	switch (st->image->bits_per_pixel) {

	case 32:
		st->pixfmt = PIX_FMT_RGB32;
		break;

	case 16:
		st->pixfmt = (st->image->green_mask == 0x7e0)
			? PIX_FMT_RGB565
			: PIX_FMT_RGB555;
		break;

	default:
		DEBUG_WARNING("not supported: bpp=%d\n",
			      st->image->bits_per_pixel);
		return ENOSYS;
	}

	return 0;
}


static inline uint8_t *x11grab_read(struct vidsrc_st *st)
{
	const int x = 0, y = 0;
	XImage *im;

	im = XGetSubImage(st->disp,
			  RootWindow(st->disp, DefaultScreen(st->disp)),
			  x, y, st->size.w, st->size.h, AllPlanes, ZPixmap,
			  st->image, 0, 0);
	if (!im)
		return NULL;

	return (uint8_t *)st->image->data;
}


static void call_frame_handler(struct vidsrc_st *st, const uint8_t *buf)
{
	AVPicture pict_src, pict_dst;
	struct vidframe frame;
	struct mbuf *mb;
	int i, ret;

	if (!st->sws) {
		st->sws = sws_getContext(st->size.w, st->size.h,
					 st->pixfmt,
					 st->size.w, st->size.h,
					 PIX_FMT_YUV420P,
					 SWS_BICUBIC, NULL, NULL, NULL);
		if (!st->sws)
			return;
	}

	mb = mbuf_alloc(yuv420p_size(&st->size));
	if (!mb)
		return;

	avpicture_fill(&pict_src, (uint8_t *)buf, st->pixfmt,
		       st->size.w, st->size.h);

	avpicture_fill(&pict_dst, mb->buf, PIX_FMT_YUV420P,
		       st->size.w, st->size.h);

	ret = sws_scale(st->sws,
			SRCSLICE_CAST pict_src.data, pict_src.linesize,
			0, st->size.h,
			pict_dst.data, pict_dst.linesize);

	if (ret <= 0) {
		DEBUG_WARNING("scale: sws_scale: returned %d\n", ret);
		goto out;
	}

	for (i=0; i<4; i++) {
		frame.data[i]     = pict_dst.data[i];
		frame.linesize[i] = pict_dst.linesize[i];
	}

	frame.size = st->size;
	frame.valid = true;

	st->frameh(&frame, st->arg);

 out:
	mem_deref(mb);
}


static void *read_thread(void *arg)
{
	struct vidsrc_st *st = arg;
	uint64_t ts = tmr_jiffies();
	uint8_t *buf;

	while (st->run) {

		if (tmr_jiffies() < ts) {
			usleep(4000);
			continue;
		}

		buf = x11grab_read(st);
		if (!buf)
			continue;

		ts += (1000/st->fps);

		call_frame_handler(st, buf);
	}

	return NULL;
}


static void destructor(void *arg)
{
	struct vidsrc_st *st = arg;

	if (st->run) {
		st->run = false;
		pthread_join(st->thread, NULL);
	}

	if (st->image)
		XDestroyImage(st->image);

	if (st->sws)
		sws_freeContext(st->sws);

	if (st->disp)
		XCloseDisplay(st->disp);

	mem_deref(st->vs);
}


static int alloc(struct vidsrc_st **stp, struct vidsrc *vs,
		 struct vidsrc_prm *prm, const char *fmt,
		 const char *dev, vidsrc_frame_h *frameh,
		 vidsrc_error_h *errorh, void *arg)
{
	struct vidsrc_st *st;
	int err;

	(void)fmt;
	(void)dev;
	(void)errorh;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->vs     = mem_ref(vs);
	st->size   = prm->size;
	st->fps    = prm->fps;
	st->frameh = frameh;
	st->arg    = arg;

	err = x11grab_open(st, &prm->size);
	if (err)
		goto out;

	st->run = true;
	err = pthread_create(&st->thread, NULL, read_thread, st);
	if (err) {
		st->run = false;
		goto out;
	}

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int x11grab_init(void)
{
	return vidsrc_register(&vidsrc, "x11grab", alloc, NULL);
}


static int x11grab_close(void)
{
	vidsrc = mem_deref(vidsrc);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(x11grab) = {
	"x11grab",
	"vidsrc",
	x11grab_init,
	x11grab_close
};
