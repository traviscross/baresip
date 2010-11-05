/**
 * @file options.c  SIP OPTIONS
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


struct sip_options {
	struct sip_loopstate ls;
	struct ua *ua;
	struct sip_dialog *dlg;
	struct sip_auth *auth;
	struct sip_request *req;
	options_resp_h *resph;
	void *arg;
};


static int request(struct sip_options *so);


static void resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct sip_options *so = arg;

	if (err || sip_request_loops(&so->ls, msg->scode))
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
			err = sip_auth_authenticate(so->auth, msg);
			if (err) {
				err = (err == EAUTH) ? 0 : err;
				break;
			}

			err = request(so);
			if (err)
				break;

			return;
		}
	}

 out:
	so->resph(err, msg, so->arg);

	/* destroy now */
	mem_deref(so);
}


static int auth_handler(char **username, char **password,
			const char *realm, void *arg)
{
	struct sip_options *so = arg;

	return ua_auth(so->ua, username, password, realm);
}


static void destructor(void *data)
{
	struct sip_options *so = data;

	mem_deref(so->req);
	mem_deref(so->auth);
	mem_deref(so->dlg);
}


static int request(struct sip_options *so)
{
	return sip_drequestf(&so->req, uag_sip(), true,
			     "OPTIONS", so->dlg, 0,
			     so->auth, NULL, resp_handler, so,
			     "Accept: application/sdp\r\n"
			     "Content-Length: 0\r\n"
			     "\r\n");
}


int sip_options_send(struct ua *ua, const char *uri,
		     options_resp_h *resph, void *arg)
{
	struct sip_options *so;
	int err;

	if (!ua || !uri || !resph)
		return EINVAL;

	so = mem_zalloc(sizeof(*so), destructor);
	if (!so)
		return ENOMEM;

	so->ua    = ua;
	so->resph = resph;
	so->arg   = arg;

	err = sip_dialog_alloc(&so->dlg, uri, uri, NULL, ua_aor(ua), NULL, 0);
	if (err)
		goto out;

	err = sip_auth_alloc(&so->auth, auth_handler, so, false);
	if (err)
		goto out;

	err = request(so);

 out:
	if (err)
		mem_deref(so);

	return err;
}
