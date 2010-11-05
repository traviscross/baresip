/**
 * @file cons.c  Socket-based command-line console
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "cons"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


enum {CONS_PORT = 5555};

struct ui_st {
	struct ui *ui; /* base class */
	struct udp_sock *us;
	struct mbuf mb;
	struct mbuf mbr;
	ui_input_h *h;
	void *arg;
};


static struct ui *cons;
static struct ui_st *cons_cur = NULL;  /* allow only one instance */


static int stderr_handler(const char *p, size_t size, void *arg)
{
	struct ui_st *st = arg;
	return mbuf_write_mem(&st->mbr, (uint8_t *)p, size);
}


static void udp_recv(const struct sa *src, struct mbuf *mb, void *arg)
{
	struct ui_st *st = arg;
	struct re_printf pf;

	pf.vph = stderr_handler;
	pf.arg = st;

	if (!mbuf_get_left(mb))
		return;

	if (!st->h)
		return;

	/* Save command */
	if (mb->end > 1) {
		mbuf_reset(&st->mb);
		(void)mbuf_write_mem(&st->mb, mb->buf, mb->end);
	}

	mbuf_rewind(&st->mbr);

	for (st->mb.pos = 0; mbuf_get_left(&st->mb); ) {
		const char key = mbuf_read_u8(&st->mb);

		st->h(key, &pf, st->arg);

		if ('\n' == key && mb->end > 1)
			break;
	}

	if (st->mbr.end > 0) {
		st->mbr.pos = 0;
		(void)udp_send(st->us, src, &st->mbr);
	}
}


static void cons_destructor(void *data)
{
	struct ui_st *st = data;

	mbuf_reset(&st->mb);
	mbuf_reset(&st->mbr);
	mem_deref(st->us);
	mem_deref(st->ui);

	cons_cur = NULL;
}


static int cons_alloc(struct ui_st **stp, struct ui_prm *prm,
		      ui_input_h *h, void *arg)
{
	struct sa local;
	struct ui_st *st;
	int err;

	if (!stp)
		return EINVAL;

	if (cons_cur) {
		*stp = mem_ref(cons_cur);
		return 0;
	}

	st = mem_zalloc(sizeof(*st), cons_destructor);
	if (!st)
		return ENOMEM;

	mbuf_init(&st->mb);
	mbuf_init(&st->mbr);

	st->ui = mem_ref(cons);
	st->h   = h;
	st->arg = arg;

	err = sa_set_str(&local, "0.0.0.0", prm->port ? prm->port : CONS_PORT);
	if (err)
		goto out;
	err = udp_listen(&st->us, &local, udp_recv, st);
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(st);
	else
		*stp = cons_cur = st;

	return err;
}


static int cons_init(void)
{
	return ui_register(&cons, "cons", cons_alloc, NULL);
}


static int cons_close(void)
{
	cons = mem_deref(cons);
	return 0;
}


const struct mod_export DECL_EXPORTS(cons) = {
	"cons",
	"ui",
	cons_init,
	cons_close
};
