/**
 * @file message.c  SIP MESSAGE
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


struct sip_message {
	struct sip_loopstate ls;
	struct ua *ua;
	struct sip_dialog *dlg;
	struct sip_auth *auth;
	struct sip_request *req;
	struct mbuf *msg;
	message_resp_h *resph;
	void *arg;
};


static int request(struct sip_message *sm);


static void resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct sip_message *sm = arg;

	if (err || sip_request_loops(&sm->ls, msg->scode))
		goto out;

	if (msg->scode < 200) {
		return;
	}
	else if (msg->scode < 300) {
		;
	}
	else {
		switch (msg->scode) {

		case 401:
		case 407:
			err = sip_auth_authenticate(sm->auth, msg);
			if (err) {
				err = (err == EAUTH) ? 0 : err;
				break;
			}

			err = request(sm);
			if (err)
				break;

			return;
		}
	}

 out:
	sm->resph(err, msg, sm->arg);

	/* destroy now */
	mem_deref(sm);
}


static int auth_handler(char **username, char **password,
			const char *realm, void *arg)
{
	struct sip_message *sm = arg;

	return ua_auth(sm->ua, username, password, realm);
}


static void destructor(void *data)
{
	struct sip_message *sm = data;

	mem_deref(sm->req);
	mem_deref(sm->auth);
	mem_deref(sm->dlg);
	mem_deref(sm->msg);
}


static int request(struct sip_message *sm)
{
	return sip_drequestf(&sm->req, uag_sip(), true,
			     "MESSAGE", sm->dlg, 0,
			     sm->auth, NULL, resp_handler, sm,
			     "Accept: text/plain\r\n"
			     "Content-Type: text/plain\r\n"
			     "Content-Length: %u\r\n"
			     "\r\n%b",
			     mbuf_get_left(sm->msg),
			     mbuf_buf(sm->msg), mbuf_get_left(sm->msg));
}


int sip_message_send(struct ua *ua, const char *uri, struct mbuf *msg,
		     message_resp_h *resph, void *arg)
{
	struct sip_message *sm;
	int err;

	if (!ua || !uri || !msg || !resph)
		return EINVAL;

	sm = mem_zalloc(sizeof(*sm), destructor);
	if (!sm)
		return ENOMEM;

	sm->ua    = ua;
	sm->resph = resph;
	sm->arg   = arg;

	sm->msg = mbuf_alloc(msg->end);
	if (!sm->msg) {
		err = ENOMEM;
		goto out;
	}

	(void)mbuf_write_mem(sm->msg, msg->buf, msg->end);
	sm->msg->pos = 0;

	err = sip_dialog_alloc(&sm->dlg, uri, uri, NULL, ua_aor(ua), NULL, 0);
	if (err)
		goto out;

	err = sip_auth_alloc(&sm->auth, auth_handler, sm, false);
	if (err)
		goto out;

	err = request(sm);

 out:
	if (err)
		mem_deref(sm);

	return err;
}
