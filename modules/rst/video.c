/**
 * @file rst/video.c MP3/ICY HTTP Video Source
 *
 * Copyright (C) 2011 Creytiv.com
 */

#include <string.h>
#include <re.h>
#include <baresip.h>
#include <cairo/cairo.h>
#include "rst.h"


struct vidsrc_st {
	struct vidsrc *vs;
	struct tmr tmr;
	struct vidsrc_prm prm;
	struct rst *rst;
	cairo_surface_t *surface;
	cairo_t *cairo;
	struct vidframe *frame;
	vidsrc_frame_h *frameh;
	void *arg;
};


static struct vidsrc *vidsrc;


static void destructor(void *arg)
{
	struct vidsrc_st *st = arg;

	rst_set_video(st->rst, NULL);
	mem_deref(st->rst);

	tmr_cancel(&st->tmr);

	if (st->cairo)
		cairo_destroy(st->cairo);

	if (st->surface)
		cairo_surface_destroy(st->surface);

	mem_deref(st->frame);
	mem_deref(st->vs);
}


static void tmr_handler(void *arg)
{
	struct vidsrc_st *st = arg;

	tmr_start(&st->tmr, 1000/st->prm.fps, tmr_handler, st);

	st->frameh(st->frame, st->arg);
}


static void background(cairo_t *cr, int width, int height)
{
	cairo_pattern_t *pat;
	double r, g, b;

	pat = cairo_pattern_create_linear(0.0, 0.0,  0.0, height);
	if (!pat)
		return;

	r = 0.0;
	g = 0.0;
	b = 0.8;

	cairo_pattern_add_color_stop_rgba(pat, 1, r, g, b, 1);
	cairo_pattern_add_color_stop_rgba(pat, 0, 0, 0, 0.2, 1);
	cairo_rectangle(cr, 0, 0, width, height);
	cairo_set_source(cr, pat);
	cairo_fill(cr);

	cairo_pattern_destroy(pat);
}


static void icy_printf(cairo_t *cr, int x, int y, double size,
		       const char *fmt, ...)
{
	char buf[4096] = "";
	va_list ap;

	va_start(ap, fmt);
	(void)re_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	/* Draw text */
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
	cairo_move_to(cr, x, y);
	cairo_text_path(cr, buf);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill_preserve(cr);
}


static int utf8_encode(struct re_printf *pf, const char *str)
{
	int err = 0;

	if (!str)
		return 0;

	while (*str && !err) {

		uint8_t ch = *str++;

		if (ch > 0x7e) {

			uint8_t utf8[2];

			switch (ch) {

			case 0xd8: /* Ø */
				utf8[0] = 0xc3;
				utf8[1] = 0x98;
				break;

			/* todo: full utf8 encoding */

			default:
				continue;
			}

			err = pf->vph((char *)&utf8, 2, pf->arg);
		}
		else {
			err = pf->vph((char *)&ch, 1, pf->arg);
		}
	}

	return err;
}


static size_t linelen(const struct pl *pl)
{
	size_t len = 74, i;

	if (pl->l <= len)
		return pl->l;

	for (i=len; i>1; i--) {

		if (pl->p[i-1] == ' ') {
			len = i;
			break;
		}
	}

	return len;
}


void rst_video_update(struct vidsrc_st *st, const char *name, const char *meta)
{
	struct vidframe frame;

	if (!st)
		return;

	background(st->cairo, st->prm.size.w, st->prm.size.h);

	icy_printf(st->cairo, 50, 100, 40.0, "%H", utf8_encode, name);

	if (meta) {

		struct pl title;

		if (!re_regex(meta, strlen(meta),
			      "StreamTitle='[ \t]*[^;]+;", NULL, &title)) {

			unsigned i;

			title.l--;

			for (i=0; title.l; i++) {

				const size_t len = linelen(&title);

				icy_printf(st->cairo, 50, 150 + 25*i, 18.0,
					   "%b", title.p, len);

				title.p += len;
				title.l -= len;
			}
		}
	}

	vidframe_init_buf(&frame, VID_FMT_RGB32, &st->prm.size,
			  cairo_image_surface_get_data(st->surface));

	vidconv(st->frame, &frame, NULL);

	tmr_start(&st->tmr, 0, tmr_handler, st);
}


static int alloc_handler(struct vidsrc_st **stp, struct vidsrc *vs,
			 struct media_ctx **ctx,
			 struct vidsrc_prm *prm, const char *fmt,
			 const char *dev, vidsrc_frame_h *frameh,
			 vidsrc_error_h *errorh, void *arg)
{
	struct vidsrc_st *st;
	int err;

	(void)fmt;
	(void)errorh;

	if (!stp || !vs || !prm || !frameh)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->vs     = mem_ref(vs);
	st->prm    = *prm;
	st->frameh = frameh;
	st->arg    = arg;

	st->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
						 prm->size.w, prm->size.h);
	if (!st->surface) {
		err = ENOMEM;
		goto out;
	}

	st->cairo = cairo_create(st->surface);
	if (!st->cairo) {
		err = ENOMEM;
		goto out;
	}

	err = vidframe_alloc(&st->frame, VID_FMT_YUV420P, &prm->size);
	if (err)
		goto out;

	if (ctx && *ctx && (*ctx)->id && !strcmp((*ctx)->id, "rst")) {
		st->rst = mem_ref(*ctx);
	}
	else {
		err = rst_alloc(&st->rst, dev);
		if (err)
			goto out;

		if (ctx)
			*ctx = (struct media_ctx *)st->rst;
	}

	rst_set_video(st->rst, st);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


int rst_video_init(void)
{
       return vidsrc_register(&vidsrc, "rst", alloc_handler, NULL);
}


void rst_video_close(void)
{
	vidsrc = mem_deref(vidsrc);
}
