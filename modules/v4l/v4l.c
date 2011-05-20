/**
 * @file v4l.c Video4Linux video-source
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _BSD_SOURCE 1
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#undef __STRICT_ANSI__ /* needed for RHEL4 kernel 2.6.9 */
#include <linux/videodev.h>
#include <pthread.h>
#include <re.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <baresip.h>


#define DEBUG_MODULE "v4l"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/* extra const-correctness added in 0.9.0 */
#if LIBSWSCALE_VERSION_INT >= ((0<<16) + (9<<8) + (0))
#define SRCSLICE_CAST (const uint8_t **)
#else
#define SRCSLICE_CAST (uint8_t **)
#endif


struct vidsrc_st {
	struct vidsrc *vs;  /* inheritance */

	int fd;
	pthread_t thread;
	bool run;
	struct vidsz size;
	struct mbuf *mb;
	struct SwsContext *sws;
	vidsrc_frame_h *frameh;
	void *arg;
};


static struct vidsrc *vidsrc;


static void v4l_get_caps(struct vidsrc_st *st)
{
	struct video_capability caps;

	if (-1 == ioctl(st->fd, VIDIOCGCAP, &caps)) {
		DEBUG_WARNING("VIDIOCGCAP: %s\n", strerror(errno));
		return;
	}

	re_printf("video: \"%s\" (%ux%u) - (%ux%u)\n", caps.name,
		  caps.minwidth, caps.minheight,
		  caps.maxwidth, caps.maxheight);

	if (VID_TYPE_CAPTURE != caps.type) {
		DEBUG_WARNING("not a capture device (%d)\n", caps.type);
	}
}


static int v4l_check_palette(struct vidsrc_st *st)
{
	struct video_picture pic;

	if (-1 == ioctl(st->fd, VIDIOCGPICT, &pic)) {
		DEBUG_WARNING("VIDIOCGPICT: %s\n", strerror(errno));
		return errno;
	}

	if (VIDEO_PALETTE_RGB24 != pic.palette) {
		DEBUG_WARNING("unsupported palette %d (only RGB24 supp.)\n",
			      pic.palette);
		return ENODEV;
	}

	return 0;
}


static int v4l_get_win(int fd, int width, int height)
{
	struct video_window win;

	if (-1 == ioctl(fd, VIDIOCGWIN, &win)) {
		DEBUG_WARNING("VIDIOCGWIN: %s\n", strerror(errno));
		return errno;
	}

	re_printf("video window: x,y=%u,%u (%u x %u)\n",
		  win.x, win.y, win.width, win.height);

	win.width = width;
	win.height = height;

	if (-1 == ioctl(fd, VIDIOCSWIN, &win)) {
		DEBUG_WARNING("VIDIOCSWIN: %s\n", strerror(errno));
		return errno;
	}

	return 0;
}


static void call_frame_handler(struct vidsrc_st *st, const uint8_t *buf)
{
	AVPicture pict_src, pict_dst;
	struct vidframe frame;
	struct mbuf *mb;
	int ret;

	mb = mbuf_alloc(yuv420p_size(&st->size));
	if (!mb)
		return;

	avpicture_fill(&pict_src, (uint8_t *)buf, PIX_FMT_RGB24,
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

	vidframe_init(&frame, &st->size, pict_dst.data, pict_dst.linesize);

	st->frameh(&frame, st->arg);

 out:
	mem_deref(mb);
}


static void *read_thread(void *arg)
{
	struct vidsrc_st *st = arg;

	while (st->run) {
		ssize_t n;

		n = read(st->fd, st->mb->buf, st->mb->size);
		if ((ssize_t)st->mb->size != n) {
			DEBUG_WARNING("video read: %d -> %d bytes\n",
				      st->mb->size, n);
			continue;
		}

		call_frame_handler(st, st->mb->buf);
	}

	return NULL;
}


static int vd_open(struct vidsrc_st *v4l, const char *device)
{
	/* NOTE: with kernel 2.6.26 it takes ~2 seconds to open
	 *       the video device.
	 */
	v4l->fd = open(device, O_RDWR);
	if (v4l->fd < 0) {
		DEBUG_WARNING("open %s: %s\n", device, strerror(errno));
		return errno;
	}

	return 0;
}


static void destructor(void *arg)
{
	struct vidsrc_st *st = arg;

	if (st->run) {
		st->run = false;
		pthread_join(st->thread, NULL);
	}

	if (st->fd >= 0)
		close(st->fd);

	if (st->sws)
		sws_freeContext(st->sws);

	mem_deref(st->mb);
	mem_deref(st->vs);
}


static uint32_t rgb24_size(const struct vidsz *sz)
{
	return sz ? (sz->w * sz->h * 24/8) : 0;
}


static int alloc(struct vidsrc_st **stp, struct vidsrc *vs,
		 struct vidsrc_prm *prm, const char *fmt,
		 const char *dev, vidsrc_frame_h *frameh,
		 vidsrc_error_h *errorh, void *arg)
{
	struct vidsrc_st *st;
	int err;

	(void)fmt;
	(void)errorh;

	if (!str_len(dev))
		dev = "/dev/video0";

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->vs     = mem_ref(vs);
	st->fd     = -1;
	st->size   = prm->size;
	st->frameh = frameh;
	st->arg    = arg;

	st->sws = sws_getContext(st->size.w, st->size.h, PIX_FMT_RGB24,
				 st->size.w, st->size.h, PIX_FMT_YUV420P,
				 SWS_BICUBIC, NULL, NULL, NULL);
	if (!st->sws) {
		err = ENOMEM;
		goto out;
	}

	DEBUG_NOTICE("open: %s %ux%u\n", dev, prm->size.w, prm->size.h);

	err = vd_open(st, dev);
	if (err)
		goto out;

	v4l_get_caps(st);

	err = v4l_check_palette(st);
	if (err)
		goto out;

	err = v4l_get_win(st->fd, st->size.w, st->size.h);
	if (err)
		goto out;

	/* note: assumes RGB24 */
	st->mb = mbuf_alloc(rgb24_size(&st->size));
	if (!st->mb) {
		err = ENOMEM;
		goto out;
	}

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


static int v4l_init(void)
{
	return vidsrc_register(&vidsrc, "v4l", alloc, NULL);
}


static int v4l_close(void)
{
	vidsrc = mem_deref(vidsrc);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(v4l) = {
	"v4l",
	"vidsrc",
	v4l_init,
	v4l_close
};
