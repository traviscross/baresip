/**
 * @file mwi.c Message Waiting Indication (RFC 3842)
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>


static struct sipsub *sub;
static struct tmr tmr;


static int auth_handler(char **username, char **password,
			const char *realm, void *arg)
{
	struct ua_prm *prm = arg;
	return ua_auth(prm, username, password, realm);
}


static void notify_handler(struct sip *sip, const struct sip_msg *msg,
			   void *arg)
{
	struct ua *ua = arg;

	if (mbuf_get_left(msg->mb)) {
		re_printf("----- MWI for %s -----\n", ua_aor(ua));
		re_printf("%b\n", mbuf_buf(msg->mb), mbuf_get_left(msg->mb));
	}

	(void)sip_treply(NULL, sip, msg, 200, "OK");
}


static void close_handler(int err, const struct sip_msg *msg,
			  const struct sipevent_substate *substate,
			  void *arg)
{
	struct ua *ua = arg;
	(void)substate;

	re_printf("mwi: subscription for %s closed: %s (%u %r)\n",
		  ua_aor(ua),
		  err ? strerror(err) : "",
		  err ? 0 : msg->scode,
		  err ? 0 : &msg->reason);

	sub = mem_deref(sub);
}


static int subscribe(void)
{
	const char *routev[1];
	struct ua *ua;
	int err;

	/* NOTE: We only use the first account */
	ua = uag_find_aor(NULL);
	if (!ua) {
		re_fprintf(stderr, "mwi: UA not found\n");
		return ENOENT;
	}

	routev[0] = ua_outbound(ua);

	re_printf("mwi: subscribing to messages for %s\n", ua_aor(ua));

	err = sipevent_subscribe(&sub, uag_sipevent_sock(), ua_aor(ua),
				 NULL, ua_aor(ua), "message-summary", NULL,
	                         600, ua_cuser(ua),
				 routev, routev[0] ? 1 : 0,
	                         auth_handler, ua_prm(ua), true, NULL,
				 notify_handler, close_handler, ua,
				 "Accept:"
				 " application/simple-message-summary\r\n");
	if (err) {
	        re_fprintf(stderr, "mwi: subscribe ERROR: %m\n", err);
	        return err;
	}

	return 0;
}


static void tmr_handler(void *arg)
{
	(void)arg;
	subscribe();
}


static int module_init(void)
{
	tmr_start(&tmr, 10, tmr_handler, 0);
	return 0;
}


static int module_close(void)
{
	tmr_cancel(&tmr);
	sub = mem_deref(sub);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(mwi) = {
	"mwi",
	"application",
	module_init,
	module_close,
};
