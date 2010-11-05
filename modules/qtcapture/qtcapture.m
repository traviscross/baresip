/**
 * @file qtcapture.m Video source using QTKit QTCapture
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <QTKit/QTKit.h>


#if LIBSWSCALE_VERSION_MINOR >= 9
#define SRCSLICE_CAST (const uint8_t **)
#else
#define SRCSLICE_CAST (uint8_t **)
#endif


static void frame_handler(struct vidsrc_st *st,
			  const CVImageBufferRef videoFrame);
static struct vidsrc *vidsrc;


@interface qtcap : NSObject
{
	QTCaptureSession                 *sess;
	QTCaptureDeviceInput             *input;
	QTCaptureDecompressedVideoOutput *output;
	struct vidsrc_st *vsrc;
}
@end


struct vidsrc_st {
	struct vidsrc *vs;  /* inheritance */

	qtcap *cap;
	struct lock *lock;
	struct vidsz app_sz;
	struct vidsz sz;
	struct mbuf *buf;
	struct SwsContext *sws;  /* TODO: use apple routines instead */
	vidsrc_frame_h *frameh;
	void *arg;
	bool started;
#ifdef QTCAPTURE_RUNLOOP
	struct tmr tmr;
#endif
};


@implementation qtcap


- (id)init:(struct vidsrc_st *)st
       dev:(const char *)devname
{
	NSAutoreleasePool *pool;
	QTCaptureDevice *dev;
	BOOL success = NO;
	NSError *error;

	pool = [[NSAutoreleasePool alloc] init];
	if (!pool)
		return nil;

	self = [super init];
	if (!self)
		goto out;

	vsrc = st;
	sess = [[QTCaptureSession alloc] init];
	if (!sess)
		goto out;

	if (str_len(devname)) {
		NSString *s = [NSString stringWithUTF8String:devname];
		dev = [QTCaptureDevice deviceWithUniqueID:s];
		re_printf("qtcapture: using device: %s\n", devname);
	}
	else {
		dev = [QTCaptureDevice
		         defaultInputDeviceWithMediaType:QTMediaTypeVideo];
	}

	success = [dev open:&error];
	if (!success)
		goto out;

	input = [[QTCaptureDeviceInput alloc] initWithDevice:dev];
	success = [sess addInput:input error:&error];
	if (!success)
		goto out;

	output = [[QTCaptureDecompressedVideoOutput alloc] init];
	[output setDelegate:self];
	[output setPixelBufferAttributes:
	 [NSDictionary dictionaryWithObjectsAndKeys:
          [NSNumber numberWithInt:st->app_sz.h], kCVPixelBufferHeightKey,
          [NSNumber numberWithInt:st->app_sz.w], kCVPixelBufferWidthKey,
#if 0
	/* This does not work reliably */
	  [NSNumber numberWithInt:kCVPixelFormatType_420YpCbCr8Planar],
	    (id)kCVPixelBufferPixelFormatTypeKey,
#endif
		       nil]];

	success = [sess addOutput:output error:&error];
	if (!success)
		goto out;

	/* Start */
	[sess startRunning];

 out:
	if (!success && self) {
		[self dealloc];
		self = nil;
	}

	[pool release];

	return self;
}


- (void)stop:(id)unused
{
	(void)unused;

	[sess stopRunning];

	if ([[input device] isOpen]) {
		[[input device] close];
		[sess removeInput:input];
		[input release];
	}

	if (output) {
		[output setDelegate:nil];
		[sess removeOutput:output];
		[output release];
	}
}


- (void)dealloc
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

	[self performSelectorOnMainThread:@selector(stop:)
	      withObject:nil
	      waitUntilDone:YES];

	[sess release];

	[super dealloc];

	[pool release];
}


- (void)captureOutput:(QTCaptureOutput *)captureOutput
  didOutputVideoFrame:(CVImageBufferRef)videoFrame
     withSampleBuffer:(QTSampleBuffer *)sampleBuffer
       fromConnection:(QTCaptureConnection *)connection
{
	(void)captureOutput;
	(void)sampleBuffer;
	(void)connection;

#if 0
	printf("got frame: %zu x %zu - fmt=0x%08x\n",
	       CVPixelBufferGetWidth(videoFrame),
	       CVPixelBufferGetHeight(videoFrame),
	       CVPixelBufferGetPixelFormatType(videoFrame));
#endif

	frame_handler(vsrc, videoFrame);
}


@end


/** Calculate the size of an YUV420P frame */
static inline size_t vidframe_size(const AVPicture *f, int h)
{
	return f->linesize[0] * h
		+ f->linesize[1] * h/2
		+ f->linesize[2] * h/2;
}


static enum PixelFormat get_pixfmt(OSType type)
{
	switch (type) {

	case kCVPixelFormatType_420YpCbCr8Planar: return PIX_FMT_YUV420P;
	case kCVPixelFormatType_422YpCbCr8:       return PIX_FMT_UYVY422;
	case 0x79757673: /* yuvs */               return PIX_FMT_YUYV422;
	case kCVPixelFormatType_32ARGB:           return PIX_FMT_ARGB;
	default:                                  return PIX_FMT_NONE;
	}
}


static const char *pixfmt_name(enum PixelFormat pixfmt)
{
	switch (pixfmt) {

	case PIX_FMT_YUV420P: return "YUV420P";
	case PIX_FMT_UYVY422: return "UYVY422";
	case PIX_FMT_YUYV422: return "YUYV422";
	case PIX_FMT_ARGB:    return "ARGB";
	default:              return "???";
	}
}


static inline void avpict_init_planar(AVPicture *p, const CVImageBufferRef f)
{
	int i;

	if (!p)
		return;

	for (i=0; i<3; i++) {
		p->data[i]     =      CVPixelBufferGetBaseAddressOfPlane(f, i);
		p->linesize[i] = (int)CVPixelBufferGetBytesPerRowOfPlane(f, i);
	}

	p->data[3]     = NULL;
	p->linesize[3] = 0;
}


static inline void avpict_init_chunky(AVPicture *p, const CVImageBufferRef f)
{
	p->data[0]     =      CVPixelBufferGetBaseAddress(f);
	p->linesize[0] = (int)CVPixelBufferGetBytesPerRow(f);

	p->data[1]     = p->data[2]     = p->data[3]     = NULL;
	p->linesize[1] = p->linesize[2] = p->linesize[3] = 0;
}


static void avpict_init_yuv420p(struct vidsrc_st *st, AVPicture *p,
				int h, int lsz)
{
	p->linesize[0] = lsz;
	p->linesize[1] = lsz / 2;
	p->linesize[2] = lsz / 2;
	p->linesize[3] = 0;

	if (!st->buf) {
		st->buf = mbuf_alloc(vidframe_size(p, h));
		if (!st->buf)
			return;
	}

	p->data[0] = st->buf->buf;
	p->data[1] = p->data[0] + p->linesize[0] * h;
	p->data[2] = p->data[1] + p->linesize[1] * h/2;
	p->data[3] = NULL;
}


static void frame_handler(struct vidsrc_st *st,
			  const CVImageBufferRef videoFrame)
{
	AVPicture pict_src, pict_dst;
	struct vidframe vidframe;
	vidsrc_frame_h *frameh;
	void *arg;
	enum PixelFormat pixfmt;
	bool scale;
	int i, ret;

	lock_write_get(st->lock);
	frameh = st->frameh;
	arg    = st->arg;
	lock_rel(st->lock);

	if (!frameh)
		return;

	pixfmt = get_pixfmt(CVPixelBufferGetPixelFormatType(videoFrame));
	if (pixfmt == PIX_FMT_NONE) {
		re_printf("unknown pixel format: 0x%08x\n",
			  CVPixelBufferGetPixelFormatType(videoFrame));
		return;
	}

	st->started = true;

	st->sz.w = (int)CVPixelBufferGetWidth(videoFrame);
	st->sz.h = (int)CVPixelBufferGetHeight(videoFrame);

	scale = !vidsz_cmp(&st->sz, &st->app_sz) || pixfmt != PIX_FMT_YUV420P;

	if (scale && !st->sws) {

		re_printf("qtcapture: scaling from %s:%ux%u to yuv420:%ux%u\n",
			  pixfmt_name(pixfmt), st->sz.w, st->sz.h,
			  st->app_sz.w, st->app_sz.h);

		/* note: SWS_FAST_BILINEAR crash with
		   ffmpeg @0.5.1_1+darwin_10 */
		st->sws = sws_getContext(st->sz.w, st->sz.h, pixfmt,
					 st->app_sz.w, st->app_sz.h,
					 PIX_FMT_YUV420P,
					 SWS_BICUBIC, NULL, NULL, NULL);
		if (!st->sws)
			return;
	}

	CVPixelBufferLockBaseAddress(videoFrame, 0);

	if (CVPixelBufferIsPlanar(videoFrame))
		avpict_init_planar(&pict_src, videoFrame);
	else
		avpict_init_chunky(&pict_src, videoFrame);

	if (st->sws) {
		avpict_init_yuv420p(st, &pict_dst, st->app_sz.h,
				    pict_src.linesize[0] / 2);

		ret = sws_scale(st->sws, SRCSLICE_CAST pict_src.data,
				pict_src.linesize, 0, st->sz.h,
				pict_dst.data, pict_dst.linesize);
	}
	else {
		pict_dst = pict_src;
	}

	CVPixelBufferUnlockBaseAddress(videoFrame, 0);

	if (ret <= 0) {
		re_fprintf(stderr, "qtcapture: sws_scale: returned %d\n", ret);
		return;
	}

	for (i=0; i<4; i++) {
		vidframe.data[i]     = pict_dst.data[i];
		vidframe.linesize[i] = pict_dst.linesize[i];
	}

	vidframe.size = st->app_sz;
	vidframe.valid = true;

	frameh(&vidframe, arg);
}


static void destructor(void *arg)
{
	struct vidsrc_st *st = arg;

#ifdef QTCAPTURE_RUNLOOP
	tmr_cancel(&st->tmr);
#endif

	lock_write_get(st->lock);
	st->frameh = NULL;
	lock_rel(st->lock);

	[st->cap dealloc];

	if (st->sws)
		sws_freeContext(st->sws);

	mem_deref(st->buf);
	mem_deref(st->lock);

	mem_deref(st->vs);
}


#ifdef QTCAPTURE_RUNLOOP
static void tmr_handler(void *arg)
{
	struct vidsrc_st *st = arg;

	/* Check if frame_handler was called */
	if (st->started)
		return;

	tmr_start(&st->tmr, 100, tmr_handler, st);

	/* Simulate the Run-Loop */
	(void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, YES);
}
#endif


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
	st->frameh = frameh;
	st->arg    = arg;

	if (prm)
		st->app_sz = prm->size;

	err = lock_alloc(&st->lock);
	if (err)
		goto out;

	st->cap = [[qtcap alloc] init:st dev:dev];
	if (!st->cap) {
		err = ENODEV;
		goto out;
	}

#ifdef QTCAPTURE_RUNLOOP
	tmr_start(&st->tmr, 10, tmr_handler, st);
#endif

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static void device_info(void)
{
	NSAutoreleasePool *pool;
	NSArray *devs;

	pool = [[NSAutoreleasePool alloc] init];
	if (!pool)
		return;

	devs = [QTCaptureDevice inputDevicesWithMediaType:QTMediaTypeVideo];

	if (devs && [devs count] > 1) {
		QTCaptureDevice *d;

		re_printf("qtcapture devices:\n");

		for (d in devs) {
			NSString *name = [d localizedDisplayName];

			re_printf("  %s: %s\n",
				  [[d uniqueID] UTF8String],
				  [name UTF8String]);
		}
	}

	[pool release];
}


static int module_init(void)
{
	device_info();
	return vidsrc_register(&vidsrc, "qtcapture", alloc);
}


static int module_close(void)
{
	vidsrc = mem_deref(vidsrc);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(qtcapture) = {
	"qtcapture",
	"vidsrc",
	module_init,
	module_close
};
