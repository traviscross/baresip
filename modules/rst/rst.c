/**
 * @file rst.c Streamer
 *
 * Copyright (C) 2011 Creytiv.com
 */

#define _BSD_SOURCE 1
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#include <mpg123.h>


struct ausrc_st {
	struct ausrc *as;
	pthread_t thread;
	mpg123_handle *mp3;
	struct aubuf *aubuf;
	char *host;
	char *path;
	struct dns_query *dnsq;
	struct tcp_conn *tc;
	ausrc_read_h *rh;
	ausrc_error_h *errh;
	void *arg;
	bool head_recv;
	bool run;
	uint32_t psize;
	uint32_t ptime;
	uint16_t port;
};


static struct ausrc *ausrc;


static void destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (st->run) {
		st->run = false;
		pthread_join(st->thread, NULL);
	}

	if (st->mp3) {
		mpg123_close(st->mp3);
		mpg123_delete(st->mp3);
	}

	mem_deref(st->aubuf);
	mem_deref(st->host);
	mem_deref(st->path);
	mem_deref(st->dnsq);
	mem_deref(st->tc);
	mem_deref(st->as);
}


static void *play_thread(void *arg)
{
	uint64_t now, ts = tmr_jiffies();
	struct ausrc_st *st = arg;
	uint8_t *buf;

	buf = mem_alloc(st->psize, NULL);
	if (!buf)
		return NULL;

	while (st->run) {

		(void)usleep(4000);

		now = tmr_jiffies();

		if (ts > now)
			continue;
#if 1
		if (now > ts + 100) {
			re_printf("rst: cpu lagging behind (%u ms)\n",
				  now - ts);
		}
#endif

		aubuf_read(st->aubuf, buf, st->psize);

		st->rh(buf, st->psize, st->arg);

		ts += st->ptime;
	}

	mem_deref(buf);

	return NULL;
}


static inline int decode(struct ausrc_st *st)
{
	int err, ch, encoding;
	struct mbuf *mb;
	long srate;

	mb = mbuf_alloc(4096);
	if (!mb)
		return ENOMEM;

	err = mpg123_read(st->mp3, mb->buf, mb->size, &mb->end);

	switch (err) {

	case MPG123_NEW_FORMAT:
		mpg123_getformat(st->mp3, &srate, &ch, &encoding);
		re_printf("rst: new format: %i hz, %i ch, encoding 0x%04x\n",
		      srate, ch, encoding);
		/*@fallthrough@*/

	case MPG123_OK:
	case MPG123_NEED_MORE:
		if (mb->end == 0)
			break;
		aubuf_append(st->aubuf, mb);
		break;

	default:
		re_printf("rst: mpg123_read error: %s\n",
			  mpg123_plain_strerror(err));
		break;
	}

	mem_deref(mb);

	return err;
}


static void recv_handler(struct mbuf *mb, void *arg)
{
	struct ausrc_st *st = arg;
	int err;

	if (!st->head_recv) {
		st->head_recv = true;
		return;
	}

	err = mpg123_feed(st->mp3, mbuf_buf(mb), mbuf_get_left(mb));
	if (err)
		return;

	while (MPG123_OK == decode(st))
		;
}


static void estab_handler(void *arg)
{
	struct ausrc_st *st = arg;
	struct mbuf *mb;
	int err;

	mb = mbuf_alloc(512);
	if (!mb) {
		err = ENOMEM;
		goto out;
	}

	err = mbuf_printf(mb, "GET %s HTTP/1.0\r\n\r\n", st->path);
	if (err)
		goto out;

	mb->pos = 0;

	err = tcp_send(st->tc, mb);
	if (err)
		goto out;

 out:
	if (err) {
		re_printf("rst: error sending HTTP request: %s\n",
			  strerror(err));
	}

	mem_deref(mb);
}


static void close_handler(int err, void *arg)
{
	struct ausrc_st *st = arg;

	re_printf("rst: tcp closed: %i\n", err);

	st->tc = mem_deref(st->tc);
}


static void dns_handler(int err, const struct dnshdr *hdr, struct list *ansl,
			struct list *authl, struct list *addl, void *arg)
{
	struct ausrc_st *st = arg;
	struct dnsrr *rr;
	struct sa srv;

	(void)err;
	(void)hdr;
	(void)authl;
	(void)addl;

	rr = dns_rrlist_find(ansl, st->host, DNS_TYPE_A, DNS_CLASS_IN, true);
	if (!rr) {
		re_printf("rst: unable to resolve: %s\n", st->host);
		return;
	}

	sa_set_in(&srv, rr->rdata.a.addr, st->port);

	err = tcp_connect(&st->tc, &srv, estab_handler, recv_handler,
			  close_handler, st);
	if (err) {
		re_printf("rst: tcp connect error: %s\n", strerror(err));
		return;
	}
}


static int rst_alloc(struct ausrc_st **stp, struct ausrc *as,
		     struct ausrc_prm *prm, const char *dev,
		     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct pl host, port, path;
	struct ausrc_st *st;
	struct sa srv;
	int err;

	if (!stp || !as || !prm || !dev || !rh)
		return EINVAL;

	if (re_regex(dev, strlen(dev), "http://[^:/]+[:]*[0-9]*[^]+",
		     &host, NULL, &port, &path)) {
		re_printf("rst: bad http url: %s\n", dev);
		return EBADMSG;
	}

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->as   = mem_ref(as);
	st->rh   = rh;
	st->errh = errh;
	st->arg  = arg;

	st->mp3 = mpg123_new(NULL, &err);
	if (!st->mp3) {
		err = ENODEV;
		goto out;
	}

	err = mpg123_open_feed(st->mp3);
	if (err != MPG123_OK) {
		re_printf("rst: mpg123_open_feed: %s\n",
			  mpg123_strerror(st->mp3));
		err = ENODEV;
		goto out;
	}

	/* Set wanted output format */
	mpg123_format_none(st->mp3);
	mpg123_format(st->mp3, prm->srate, prm->ch, MPG123_ENC_SIGNED_16);

	st->ptime = (1000 * prm->frame_size) / (prm->srate * prm->ch);
	st->psize = prm->frame_size * 2;

	prm->fmt = AUFMT_S16LE;

	re_printf("rst: ptime=%u psize=%u aubuf=[%u:%u]\n",
		  st->ptime, st->psize,
		  prm->srate * prm->ch * 2,
		  prm->srate * prm->ch * 40);

	/* 1 - 20 seconds of audio */
	err = aubuf_alloc(&st->aubuf,
			  prm->srate * prm->ch * 2,
			  prm->srate * prm->ch * 40);
	if (err)
		goto out;

	err = pl_strdup(&st->host, &host);
	if (err)
		goto out;

	err = pl_strdup(&st->path, &path);
	if (err)
		goto out;

	st->port = pl_u32(&port);
	st->port = st->port ? st->port : 80;

	if (!sa_set_str(&srv, st->host, st->port)) {

		err = tcp_connect(&st->tc, &srv, estab_handler, recv_handler,
				  close_handler, st);
		if (err) {
			re_printf("rst: tcp connect error: %s\n",
				  strerror(err));
			goto out;
		}
	}
	else {
		err = dnsc_query(&st->dnsq, net_dnsc(), st->host, DNS_TYPE_A,
				 DNS_CLASS_IN, true, dns_handler, st);
		if (err) {
			re_printf("rst: dns query error: %s\n", strerror(err));
			goto out;
		}
	}

	st->run = true;

	err = pthread_create(&st->thread, NULL, play_thread, st);
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


static int module_init(void)
{
	int err;

	err = mpg123_init();
	if (err != MPG123_OK) {
		re_printf("rst: mpg123_init: %s\n",
			  mpg123_plain_strerror(err));
		return ENODEV;
	}

	return ausrc_register(&ausrc, "rst", rst_alloc);
}


static int module_close(void)
{
	ausrc = mem_deref(ausrc);

	mpg123_exit();

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(rst) = {
	"rst",
	"sound",
	module_init,
	module_close
};
