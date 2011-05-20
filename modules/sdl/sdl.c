/**
 * @file sdl.c  Simple DirectMedia Layer module for SDL v1.3
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define SDL_NO_COMPAT 1
#include <SDL/SDL.h>
#include <re.h>
#include <baresip.h>
#include "sdl.h"


struct vidisp_st {
	struct vidisp *vd;              /**< Inheritance (1st)     */
	struct le le;                   /**< Linked-list element   */
	SDL_Window *window;             /**< SDL Window            */
	SDL_Renderer *renderer;         /**< SDL Renderer          */
	SDL_Texture *texture;           /**< Texture for pixels    */
	struct vidsz size;              /**< Current size          */
	vidisp_input_h *inputh;         /**< Input handler         */
	vidisp_resize_h *resizeh;       /**< Screen resize handler */
	void *arg;                      /**< Handler argument      */
	bool fullscreen;                /**< Fullscreen flag       */
};


/* Global data */
static struct vidisp *vid;       /**< SDL Video-display      */
static struct list stl;          /**< List of video displays */
static struct tmr tmr_ev;        /**< Event timer            */


static void event_handler(void *arg);


static void refresh_timer(void)
{
	if (stl.head)
		tmr_start(&tmr_ev, 1000, event_handler, NULL);
	else
		tmr_cancel(&tmr_ev);
}


/** Map from WindowID to Vidisp state */
static struct vidisp_st *find_state(Uint32 windowID)
{
	struct le *le;

	for (le = stl.head; le; le = le->next) {
		struct vidisp_st *st = le->data;

		if (st->window == SDL_GetWindowFromID(windowID))
			return st;
	}

	return NULL;
}


#if 0
static void dump_rendererinfo(void)
{
	SDL_RendererInfo info;
	int i, n;

	n = SDL_GetNumRenderDrivers();

	re_printf("SDL Rendering info: n=%d\n", n);

	for (i=0; i<n; i++) {
		uint32_t j;

		SDL_GetRenderDriverInfo(i, &info);
		re_printf("    %d: %s flags=%08x num_formats=%u",
			  i, info.name, info.flags,
			  info.num_texture_formats);

		for (j=0; j<info.num_texture_formats; j++) {

			uint32_t fmt = info.texture_formats[j];

			re_printf(" %s", SDL_GetPixelFormatName(fmt));
		}

		re_printf("\n");
	}
}
#endif


#if 0
/**
 * Find a renderer that supports YUV420P Pixel format
 *
 * @return index, or -1 if not found
 */
static int find_yuv420p_renderer(void)
{
	SDL_RendererInfo info;
	int i, n;

	n = SDL_GetNumRenderDrivers();

	for (i=0; i<n; i++) {
		uint32_t j;

		SDL_GetRenderDriverInfo(i, &info);

		for (j=0; j<info.num_texture_formats; j++) {

			if (info.texture_formats[j] == SDL_PIXELFORMAT_YV12) {
				re_printf("using renderer: %s\n", info.name);
				return i;
			}
		}
	}

	return -1;
}
#endif


static void sdl_reset(struct vidisp_st *st)
{
	if (st->texture) {
		SDL_DestroyTexture(st->texture);
		st->texture = NULL;
	}

	if (st->renderer) {
		SDL_DestroyRenderer(st->renderer);
		st->renderer = NULL;
	}

	if (st->window) {
		SDL_DestroyWindow(st->window);
		st->window = NULL;
	}
}


static void event_handler(void *arg)
{
	struct vidisp_st *st;
	struct le *le;
	SDL_Event event;
	char ch;

	(void)arg;

	tmr_start(&tmr_ev, 100, event_handler, NULL);

	while (SDL_PollEvent(&event)) {

		switch (event.type) {

		case SDL_KEYDOWN:
			st = find_state(event.key.windowID);
			if (!st)
				return;

			ch = event.key.keysym.unicode & 0x7f;

			switch (event.key.keysym.sym) {

			case SDLK_ESCAPE:
				if (!st->fullscreen)
					break;

				st->fullscreen = false;
				sdl_reset(st);
				break;

			case SDLK_f:
				if (st->fullscreen)
					break;

				st->fullscreen = true;
				sdl_reset(st);
				break;

			default:

				/* Relay key-press to UI subsystem */
				if (isprint(ch) && st->inputh) {
					st->inputh(ch, st->arg);
				}
				break;
			}

			break;

		case SDL_KEYUP:
			st = find_state(event.key.windowID);
			if (!st)
				return;
			if (st->inputh)
				st->inputh(0x00, st->arg);
			break;

		case SDL_QUIT:
			for (le = stl.head; le; le = le->next) {
				struct vidisp_st *st = le->data;

				if (st->inputh)
					st->inputh('q', st->arg);
			}
			break;

		default:
			break;
		}
	}
}


static void destructor(void *arg)
{
	struct vidisp_st *st = arg;

	sdl_reset(st);
	list_unlink(&st->le);

	mem_deref(st->vd);

	refresh_timer();
}


static int alloc(struct vidisp_st **stp, struct vidisp_st *parent,
		 struct vidisp *vd, struct vidisp_prm *prm, const char *dev,
		 vidisp_input_h *inputh, vidisp_resize_h *resizeh, void *arg)
{
	struct vidisp_st *st;
	int err = 0;

	/* Not used by SDL */
	(void)parent;
	(void)prm;
	(void)dev;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	list_append(&stl, &st->le, st);

	st->vd      = mem_ref(vd);
	st->inputh  = inputh;
	st->resizeh = resizeh;
	st->arg     = arg;

	refresh_timer();

	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int display(struct vidisp_st *st, const char *title,
		   const struct vidframe *frame)
{
	Uint8 *pixelv[3];
	Uint16 pitches[3];
	void *pixels;
	int pitch;
	int ret;

	if (!vidsz_cmp(&st->size, &frame->size)) {
		if (st->size.w && st->size.h) {
			re_printf("SDL reset: %ux%u ---> %ux%u\n",
				  st->size.w, st->size.h,
				  frame->size.w, frame->size.h);
		}
		sdl_reset(st);
	}

	if (!st->window) {
		Uint32 flags = SDL_WINDOW_SHOWN;
		char capt[256];

		if (st->fullscreen)
			flags |= SDL_WINDOW_FULLSCREEN;
		else if (st->resizeh)
			flags |= SDL_WINDOW_RESIZABLE;

		if (title) {
			re_snprintf(capt, sizeof(capt), "%s - %u x %u",
				    title, frame->size.w, frame->size.h);
		}
		else {
			re_snprintf(capt, sizeof(capt), "%u x %u",
				    frame->size.w, frame->size.h);
		}

		st->window = SDL_CreateWindow(capt,
					      SDL_WINDOWPOS_CENTERED,
					      SDL_WINDOWPOS_CENTERED,
					      frame->size.w, frame->size.h,
					      flags);
		if (!st->window) {
			re_fprintf(stderr, "unable to create sdl window: %s\n",
				   SDL_GetError());
			return ENODEV;
		}

		st->size = frame->size;

		SDL_RaiseWindow(st->window);
	}

	if (!st->renderer) {

		Uint32 flags = 0;

		flags |= SDL_RENDERER_ACCELERATED;
		flags |= SDL_RENDERER_PRESENTVSYNC;

		st->renderer = SDL_CreateRenderer(st->window, -1, flags);
		if (!st->renderer) {
			re_fprintf(stderr, "unable to create renderer: %s\n",
				   SDL_GetError());
			return ENOMEM;
		}
	}

	if (!st->texture) {

		st->texture = SDL_CreateTexture(st->renderer,
						SDL_PIXELFORMAT_YV12,
						SDL_TEXTUREACCESS_STREAMING,
						frame->size.w, frame->size.h);
		if (!st->texture) {
			re_fprintf(stderr, "unable to create texture: %s\n",
				   SDL_GetError());
			return ENODEV;
		}
	}

	ret = SDL_LockTexture(st->texture, NULL, &pixels, &pitch);
	if (ret != 0) {
		re_fprintf(stderr, "unable to lock texture (ret=%d)\n", ret);
		return ENODEV;
	}

	/* Convert */
	pitches[0] = pitch;
	pitches[1] = pitch / 2;
	pitches[2] = pitch / 2;
	pixelv[0] = (Uint8 *) pixels;
	pixelv[1] = pixelv[0] + pitches[0] * frame->size.h;
	pixelv[2] = pixelv[1] + pitches[1] * frame->size.h / 2;

	picture_copy(pixelv, pitches, frame);
	SDL_UnlockTexture(st->texture);

	/* Blit the sprite onto the screen */
	SDL_RenderCopy(st->renderer, st->texture, NULL, NULL);

	/* Update the screen! */
	SDL_RenderPresent(st->renderer);

	return 0;
}


static void hide(struct vidisp_st *st)
{
	if (!st || !st->window)
		return;

	SDL_HideWindow(st->window);
}


static int module_init(void)
{
	int err;

	if (SDL_VideoInit(NULL) < 0) {
		re_fprintf(stderr, "SDL: unable to init Video: %s\n",
			   SDL_GetError());
		return ENODEV;
	}

#if 0
	dump_rendererinfo();
#endif

#if 0
	rend_idx = find_yuv420p_renderer();
	if (rend_idx < 0) {
		re_fprintf(stderr, "could not find YUV420P renderer\n");
		return ENODEV;
	}
#endif

	err = vidisp_register(&vid, "sdl", alloc, NULL, display, hide);
	if (err)
		return err;

	tmr_init(&tmr_ev);

	return 0;
}


static int module_close(void)
{
	tmr_cancel(&tmr_ev);
	vid = mem_deref(vid);

	SDL_VideoQuit();

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(sdl) = {
	"sdl",
	"vidisp",
	module_init,
	module_close,
};
