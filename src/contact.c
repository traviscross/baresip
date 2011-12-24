/**
 * @file contact.c  Contacts handling
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "contact"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/** Defines a contact */
struct contact {
	struct le le;     /**< Linked list element */
	char *addr;       /**< Contact address     */
};


static struct list contactl = LIST_INIT;  /**< Contacts (struct contact)  */
static bool options_verbose;              /**< Print verbose OPTIONS info */


/**
 * Initialise the contacts module
 */
void contact_init(void)
{
	list_init(&contactl);
}


/**
 * Delete all contacts
 */
void contact_close(void)
{
	list_flush(&contactl);
}


static void contact_destructor(void *arg)
{
	struct contact *c = arg;

	list_unlink(&c->le);
	mem_deref(c->addr);
}


/**
 * Add a new contact
 *
 * @param addr Name and SIP address of contact
 *
 * @return 0 if success, otherwise errorcode
 */
int contact_add(const struct pl *addr)
{
	struct sip_addr sip_addr;
	struct contact *c;
	int err;

	err = sip_addr_decode(&sip_addr, addr);
	if (err) {
		DEBUG_WARNING("could not decode SIP address `%r' (%s)\n",
			      addr, strerror(err));
		return err;
	}

	c = mem_zalloc(sizeof(*c), contact_destructor);
	if (!c)
		return ENOMEM;

	list_append(&contactl, &c->le, c);

	err = pl_strdup(&c->addr, addr);

	if (err)
		mem_deref(c);
	return err;
}


static bool casestr(const struct pl *pl, const struct pl *pl2)
{
	char expr[64];

	if (!pl || !pl2)
		return false;

	(void)pl_strcpy(pl2, expr, sizeof(expr));

	if (0 == re_regex(pl->p, pl->l, expr))
		return true;

	return false;
}


static bool find_handler(struct le *le, void *arg)
{
	const struct contact *c = le->data;
	const struct pl *str = arg;
	struct sip_addr addr;
	struct pl pl;

	pl_set_str(&pl, c->addr);

	if (0 != sip_addr_decode(&addr, &pl)) {
		DEBUG_WARNING("find: sip_addr_decode() failed\n");
		return false;
	}

	if (casestr(&addr.dname, str))
		return true;

	if (casestr(&addr.uri.user, str))
		return true;

	if (casestr(&addr.uri.host, str))
		return true;

	return false;
}


int contact_find(const struct pl *str, struct mbuf *uri)
{
	struct contact *c;

	c = list_ledata(list_apply(&contactl, true, find_handler,
				   (void *)str));
	if (!c)
		return ENOENT;

	return uri ? mbuf_write_str(uri, c->addr) : 0;
}


int contact_debug(struct re_printf *pf, void *unused)
{
	struct le *le;
	int err;

	(void)unused;

	err = re_hprintf(pf, "\n--- Contacts: (%u) ---\n",
			 list_count(&contactl));

	for (le = contactl.head; le && !err; le = le->next) {
		const struct contact *c = le->data;
		err = re_hprintf(pf, "%s\n", c->addr);
	}

	err |= re_hprintf(pf, "\n");

	return err;
}


static void options_resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	char *addr = arg;
	char resp_code[128];
	const struct sip_hdr *ua;

	if (msg && msg->scode < 200)
		return;

	if (err) {
		(void)re_snprintf(resp_code, sizeof(resp_code), "%s",
				  strerror(err));
	}
	else if (msg) {
		(void)re_snprintf(resp_code, sizeof(resp_code), "%u %r",
				  msg->scode, &msg->reason);
	}
	memcpy(&resp_code[22], "..\0", 3);

	(void)re_fprintf(stderr, "%-50s", addr);

	if (err) {
		(void)re_fprintf(stderr, " \x1b[31m%-24s\x1b[;m", resp_code);
	}
	else if (msg && msg->scode < 300) {
		(void)re_fprintf(stderr, " \x1b[32m%-24s\x1b[;m", resp_code);
	}
	else if (msg && 501 == msg->scode) {
		(void)re_fprintf(stderr, " \x1b[32m%-24s\x1b[;m", "Online");
	}
	else {
		(void)re_fprintf(stderr, " \x1b[31m%-24s\x1b[;m", resp_code);
	}

	ua = sip_msg_hdr(msg, SIP_HDR_USER_AGENT);
	if (!ua) {
		ua = sip_msg_hdr(msg, SIP_HDR_SERVER);
	}
	if (ua) {
		(void)re_fprintf(stderr, "  %r", &ua->val);
	}

	(void)re_fprintf(stderr, "\n");

	if (options_verbose) {
		const struct sip_hdr *hdr;

		hdr = sip_msg_hdr(msg, SIP_HDR_ALLOW);
		if (hdr) {
			(void)re_fprintf(stderr, "  Allow: %r\n", &hdr->val);
		}

		hdr = sip_msg_hdr(msg, SIP_HDR_SUPPORTED);
		if (hdr) {
			(void)re_fprintf(stderr, "  Supported: %r\n",
					 &hdr->val);
		}

		hdr = sip_msg_hdr(msg, SIP_HDR_CONTACT);
		if (hdr) {
			(void)re_fprintf(stderr, "  Contact: %r\n", &hdr->val);
		}

		hdr = sip_msg_hdr(msg, SIP_HDR_ACCEPT);
		if (hdr) {
			(void)re_fprintf(stderr, "  Accept: %r\n", &hdr->val);
		}
	}

	mem_deref(addr);
}


void contact_send_options(struct ua *ua, const char *uri, bool verbose)
{
	struct sip_addr addr;
	struct pl pl;
	char *str;
	int err;

	options_verbose = verbose;

	pl_set_str(&pl, uri);
	err = sip_addr_decode(&addr, &pl);
	if (err)
		return;

	err = pl_strdup(&str, &addr.auri);
	if (err)
		return;

	err = ua_options_send(ua, str, options_resp_handler, str);
	if (err) {
		DEBUG_WARNING("options: ua_send_options: (%s)\n",
			      strerror(err));
		mem_deref(str);
	}
}


void contact_send_options_to_all(struct ua *ua, bool verbose)
{
	struct le *le;

	(void)re_fprintf(stderr, "\n--- Contacts status: (%u) ---\n",
			 list_count(&contactl));

	for (le = contactl.head; le; le = le->next) {
		struct contact *c = le->data;
		contact_send_options(ua, c->addr, verbose);
	}
}
