/**
 * @file net.c Networking code
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "net"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


static struct {
	struct sa laddr;
	char if_def[16];
#ifdef HAVE_INET6
	struct sa laddr6;
	char if6_def[16];
#endif
	int af;
	struct tmr tmr;
	struct dnsc *dnsc;
	struct sa nsv[4];    /**< Configured name servers           */
	uint32_t nsn;        /**< Number of configured name servers */
	uint32_t interval;
	net_change_h *ch;
	void *arg;
} net;


/**
 * Check for DNS Server updates
 */
static void dns_refresh(void)
{
	struct sa nsv[8];
	uint32_t i, nsn;
	int err;

	nsn = ARRAY_SIZE(nsv);

	err = dns_srv_get(NULL, 0, nsv, &nsn);
	if (err)
		return;

	for (i=0; i<net.nsn; i++)
		sa_cpy(&nsv[nsn++], &net.nsv[i]);

	(void)dnsc_srv_set(net.dnsc, nsv, nsn);
}


/**
 * Detect changes in IP address(es)
 */
static void ipchange_handler(void *arg)
{
	struct sa la;
	bool change = false;

	(void)arg;

	tmr_start(&net.tmr, net.interval * 1000, ipchange_handler, NULL);

	DEBUG_INFO("checking for IPv4 change, current: %s:%j\n",
		   net.if_def, &net.laddr);

	dns_refresh();

	/* Get default source addresses */
	if (0 == net_default_source_addr_get(AF_INET, &la)
	    && !sa_cmp(&net.laddr, &la, SA_ADDR)) {
		DEBUG_NOTICE("local IPv4 addr changed: %j -> %j\n",
			     &net.laddr, &la);
		sa_cpy(&net.laddr, &la);
		change = true;
	}

#ifdef HAVE_INET6
	if (0 == net_default_source_addr_get(AF_INET6, &la)
	    && !sa_cmp(&net.laddr6, &la, SA_ADDR)) {

		DEBUG_NOTICE("local IPv6 addr changed: %j -> %j\n",
			     &net.laddr6, &la);

		sa_cpy(&net.laddr6, &la);
		change = true;
	}
#endif

	/* Get default routes */
	(void)net_rt_default_get(AF_INET, net.if_def, sizeof(net.if_def));

#ifdef HAVE_INET6
	(void)net_rt_default_get(AF_INET6, net.if6_def, sizeof(net.if6_def));
#endif

	if (change && net.ch) {
		net.ch(net.arg);
	}
}


static int dns_init(void)
{
	struct sa nsv[8];
	uint32_t i, nsn;
	char domain[64] = "";
	int err;

	nsn = ARRAY_SIZE(nsv);

	err = dns_srv_get(domain, sizeof(domain), nsv, &nsn);
	if (err) {
		nsn = 0;
	}

	/* Add any configured nameservers */
	for (i=0; i<net.nsn && nsn < ARRAY_SIZE(nsv); i++)
		sa_cpy(&nsv[nsn++], &net.nsv[i]);

	if (domain[0])
		conf_set_domain(domain);

	return dnsc_alloc(&net.dnsc, NULL, nsv, nsn);
}


/**
 * Initialise networking
 *
 * @param prefer_ipv6 Prefer IPv6 flag
 *
 * @return 0 if success, otherwise errorcode
 */
int net_init(bool prefer_ipv6)
{
	int err;

	/* Initialise DNS resolver */
	err = dns_init();
	if (err) {
		DEBUG_WARNING("dns_init: %s\n", strerror(err));
		return err;
	}

	net.af = AF_INET;

	(void)sa_set_str(&net.laddr, "127.0.0.1", 0);

	/* Get default source addresses */
	err = net_default_source_addr_get(AF_INET, &net.laddr);
	if (err) {
		DEBUG_WARNING("net_default_source_addr_get: AF_INET (%s)\n",
			      strerror(err));
		return err;
	}

#ifdef HAVE_INET6
	(void)sa_set_str(&net.laddr6, "::1", 0);

	err = net_default_source_addr_get(AF_INET6, &net.laddr6);
	if (err) {
		if (prefer_ipv6)
			return err;
		else
			err = 0;
	}

	if (prefer_ipv6)
		net.af = AF_INET6;
#else
	if (prefer_ipv6) {
		DEBUG_WARNING("IPv6 support is disabled\n");
		return EAFNOSUPPORT;
	}
#endif

	/* Get default routes */
	(void)net_rt_default_get(AF_INET, net.if_def, sizeof(net.if_def));

#ifdef HAVE_INET6
	(void)net_rt_default_get(AF_INET6, net.if6_def, sizeof(net.if6_def));
#endif

	(void)re_fprintf(stderr, "Local IP address: IPv4=%s:%j",
			 net.if_def, &net.laddr);
#ifdef HAVE_INET6
	(void)re_fprintf(stderr, " IPv6=%s:%j", net.if6_def, &net.laddr6);
#endif
	(void)re_fprintf(stderr, "\n");

	tmr_init(&net.tmr);

	return err;
}


int net_reset(void)
{
	net.dnsc = mem_deref(net.dnsc);

	return dns_init();
}


/**
 * Close networking
 */
void net_close(void)
{
	net.dnsc = mem_deref(net.dnsc);
	tmr_cancel(&net.tmr);
}


/**
 * Add a DNS server
 *
 * @param sa DNS Server IP address and port
 *
 * @return 0 if success, otherwise errorcode
 */
int net_dnssrv_add(const struct sa *sa)
{
	if (net.nsn >= ARRAY_SIZE(net.nsv))
		return E2BIG;

	sa_cpy(&net.nsv[net.nsn++], sa);

	return 0;
}


void net_change(uint32_t interval, net_change_h *ch, void *arg)
{
	net.interval = interval;
	net.ch = ch;
	net.arg = arg;

	if (interval)
		tmr_start(&net.tmr, interval * 1000, ipchange_handler, NULL);
	else
		tmr_cancel(&net.tmr);
}


static int dns_debug(struct re_printf *pf)
{
	struct sa nsv[4];
	uint32_t i, nsn;
	int err;

	nsn = ARRAY_SIZE(nsv);

	err = dns_srv_get(NULL, 0, nsv, &nsn);
	if (err)
		return err;

	err = re_hprintf(pf, " DNS Servers: (%u)\n", nsn);
	for (i=0; i<nsn; i++)
		err |= re_hprintf(pf, "   %u: %J\n", i, &nsv[i]);
	for (i=0; i<net.nsn; i++)
		err |= re_hprintf(pf, "   %u: %J\n", nsn+i, &net.nsv[i]);

	return err;
}


/**
 * Print networking debug information
 *
 * @param pf     Print handler for debug output
 * @param unused Unused parameter
 */
int net_debug(struct re_printf *pf, void *unused)
{
	int err;

	(void)unused;

	err  = re_hprintf(pf, "--- Network debug ---\n");
	err |= re_hprintf(pf, " Local IPv4: %9s - %j\n",
			  net.if_def, &net.laddr);
#ifdef HAVE_INET6
	err |= re_hprintf(pf, " Local IPv6: %9s - %j\n",
			  net.if6_def, &net.laddr6);
#endif

	err |= net_if_debug(pf, NULL);

	err |= net_rt_debug(pf, NULL);

	err |= dns_debug(pf);

	return err;
}


/**
 * Get the local IP Address
 *
 * @return Local IP Address
 */
const struct sa *net_laddr(void)
{
	switch (net.af) {

	case AF_INET:  return &net.laddr;
#ifdef HAVE_INET6
	case AF_INET6: return &net.laddr6;
#endif
	default:       return NULL;
	}
}


/**
 * Get the DNS Client
 *
 * @return DNS Client
 */
struct dnsc *net_dnsc(void)
{
	return net.dnsc;
}
