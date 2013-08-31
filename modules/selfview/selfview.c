/**
 * @file selfview.c  Selfview Video-Filter
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>


struct selfview {
	struct vidfilt_st vf;    /**< Inheritance      */

	struct lock *lock;       /**< Protect frame    */
	struct vidframe *frame;

	struct vidisp_st *disp;  /**< Selfview display */
};


static struct vidsz selfview_size = {0, 0};


static void destructor(void *arg)
{
	struct selfview *st = arg;

	list_unlink(&st->vf.le);

	mem_deref(st->disp);

	lock_write_get(st->lock);
	mem_deref(st->frame);
	lock_rel(st->lock);
	mem_deref(st->lock);
}


static int update(struct vidfilt_st **stp, struct vidfilt *vf)
{
	struct selfview *st;
	int err;

	if (!stp || !vf)
		return EINVAL;

	if (*stp)
		return 0;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	err = lock_alloc(&st->lock);
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(st);
	else
		*stp = (struct vidfilt_st *)st;

	return err;
}


static int encode_win(struct vidfilt_st *st, struct vidframe *frame)
{
	struct selfview *pip = (struct selfview *)st;
	int err;

	if (!frame)
		return 0;

	if (!pip->disp) {

		err = vidisp_alloc(&pip->disp, NULL,
				   NULL, NULL, NULL, NULL, NULL);
		if (err)
			return err;
	}

	return vidisp_display(pip->disp, "Selfview", frame);
}


static int encode_pip(struct vidfilt_st *st, struct vidframe *frame)
{
	struct selfview *sv = (struct selfview *)st;
	int err = 0;

	if (!frame)
		return 0;

	lock_write_get(sv->lock);
	if (!sv->frame) {
		struct vidsz sz;

		/* Use size if configured, or else 20% of main window */
		if (selfview_size.w && selfview_size.h) {
			sz = selfview_size;
		}
		else {
			sz.w = frame->size.w / 5;
			sz.h = frame->size.h / 5;
		}

		err = vidframe_alloc(&sv->frame, VID_FMT_YUV420P, &sz);
	}
	if (!err)
		vidconv(sv->frame, frame, NULL);
	lock_rel(sv->lock);

	return err;
}


static int decode_pip(struct vidfilt_st *st, struct vidframe *frame)
{
	struct selfview *sv = (struct selfview *)st;

	if (!frame)
		return 0;

	lock_read_get(sv->lock);
	if (sv->frame) {
		struct vidrect rect;

		rect.w = sv->frame->size.w;
		rect.h = sv->frame->size.h;
		rect.x = frame->size.w - rect.w - 10;
		rect.y = frame->size.h - rect.h - 10;

		vidconv(frame, sv->frame, &rect);
	}
	lock_rel(sv->lock);

	return 0;
}


static struct vidfilt selfview_win = {
	LE_INIT, "window", update, encode_win, NULL
};
static struct vidfilt selfview_pip = {
	LE_INIT, "pip", update, encode_pip, decode_pip,
};


static int module_init(void)
{
	struct pl pl;

	if (conf_get(conf_cur(), "video_selfview", &pl))
		return 0;

	if (0 == pl_strcasecmp(&pl, "window"))
		vidfilt_register(&selfview_win);
	else if (0 == pl_strcasecmp(&pl, "pip"))
		vidfilt_register(&selfview_pip);

	(void)conf_get_vidsz(conf_cur(), "selfview_size", &selfview_size);

	return 0;
}


static int module_close(void)
{
	vidfilt_unregister(&selfview_win);
	vidfilt_unregister(&selfview_pip);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(selfview) = {
	"selfview",
	"vidfilt",
	module_init,
	module_close
};
