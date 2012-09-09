/**
 * @file ui.c  User Interface
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "ui"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/** User Interface */
struct ui {
	struct le le;
	const char *name;
	struct ui_st *st;      /* only one instance */
	ui_output_h *outputh;
	struct cmd_ctx *ctx;
};

static struct list uil;  /**< List of UIs (struct ui) */


static void ui_handler(char key, struct re_printf *pf, void *arg)
{
	struct ui *ui = arg;

	(void)cmd_process(ui ? &ui->ctx : NULL, key, pf);
}


static void destructor(void *arg)
{
	struct ui *ui = arg;

	list_unlink(&ui->le);
	mem_deref(ui->st);
	mem_deref(ui->ctx);
}


static int stdout_handler(const char *p, size_t size, void *arg)
{
	(void)arg;

	if (1 != fwrite(p, size, 1, stdout))
		return ENOMEM;

	return 0;
}


/**
 * Register a new User-Interface (UI) module
 *
 * @param uip    Pointer to allocated UI module
 * @param name   Name of the UI module
 * @param alloch UI allocation handler
 * @param outh   UI output handler
 *
 * @return 0 if success, otherwise errorcode
 */
int ui_register(struct ui **uip, const char *name,
		ui_alloc_h *alloch, ui_output_h *outh)
{
	struct ui *ui;
	int err = 0;

	if (!uip)
		return EINVAL;

	ui = mem_zalloc(sizeof(*ui), destructor);
	if (!ui)
		return ENOMEM;

	list_append(&uil, &ui->le, ui);

	ui->name    = name;
	ui->outputh = outh;

	if (alloch) {
		struct ui_prm prm;

		prm.device = config.input.device;
		prm.port   = config.input.port;

		err = alloch(&ui->st, &prm, ui_handler, ui);
		if (err) {
			DEBUG_WARNING("register: module '%s' failed (%m)\n",
				      ui->name, err);
		}
	}

	if (err)
		mem_deref(ui);
	else
		*uip = ui;

	return err;
}


/**
 * Send input to the UI subsystem
 *
 * @param key Input character
 */
void ui_input(char key)
{
	struct re_printf pf;

	pf.vph = stdout_handler;
	pf.arg = NULL;

	ui_handler(key, &pf, list_ledata(uil.head));
}


/**
 * Send an input string to the UI subsystem
 *
 * @param str Input string
 */
void ui_input_str(const char *str)
{
	struct re_printf pf;
	struct ui *ui = list_ledata(uil.head);
	size_t n = str_len(str);

	if (!str)
		return;

	pf.vph = stdout_handler;
	pf.arg = NULL;

	while (*str)
		ui_handler(*str++, &pf, ui);

	if (n > 1 && *(str-1) != '\n')
		ui_handler('\n', &pf, ui);
}


/**
 * Send output to all modules registered in the UI subsystem
 *
 * @param str Output string
 */
void ui_output(const char *str)
{
	struct le *le;

	for (le = uil.head; le; le = le->next) {
		const struct ui *ui = le->data;

		if (ui->outputh)
			ui->outputh(ui->st, str);
	}
}
