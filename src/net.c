/**
 * @file net.c Networking code
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "net"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


static struct {
	struct sa laddr;
	char ifname[16];
#ifdef HAVE_INET6
	struct sa laddr6;
	char ifname6[16];
#endif
	struct tmr tmr;
	struct dnsc *dnsc;
	struct sa nsv[4];    /**< Configured name servers           */
	uint32_t nsn;        /**< Number of configured name servers */
	uint32_t interval;
	char domain[64];     /**< DNS domain from network           */
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
	bool change = false;

	(void)arg;

	tmr_start(&net.tmr, net.interval * 1000, ipchange_handler, NULL);
	change = net_check();
	if (change && net.ch) {
		net.ch(net.arg);
	}
}


/**
 * Check if local IP address(es) changed
 *
 * @return True if changed, otherwise false
 */
bool net_check(void)
{
	struct sa la;
	bool change = false;

	DEBUG_INFO("checking for IPv4 change, current: %s:%j\n",
		   net.ifname, &net.laddr);

	dns_refresh();

	/* Get default source addresses */
	if (0 == net_default_source_addr_get(AF_INET, &la)
	    && !sa_cmp(&net.laddr, &la, SA_ADDR)) {
		DEBUG_NOTICE("local IPv4 addr changed: %j -> %j\n",
			     &net.laddr, &la);
		sa_cpy(&net.laddr, &la);
		change = true;
	}

	/* Get default routes */
	(void)net_rt_default_get(AF_INET, net.ifname, sizeof(net.ifname));

#ifdef HAVE_INET6
	if (0 == net_default_source_addr_get(AF_INET6, &la)
	    && !sa_cmp(&net.laddr6, &la, SA_ADDR)) {

		DEBUG_NOTICE("local IPv6 addr changed: %j -> %j\n",
			     &net.laddr6, &la);

		sa_cpy(&net.laddr6, &la);
		change = true;
	}
	(void)net_rt_default_get(AF_INET6, net.ifname6, sizeof(net.ifname6));
#endif

	return change;
}


static int dns_init(void)
{
	struct sa nsv[8];
	uint32_t i, nsn;
	int err;

	nsn = ARRAY_SIZE(nsv);

	err = dns_srv_get(net.domain, sizeof(net.domain), nsv, &nsn);
	if (err) {
		nsn = 0;
	}

	/* Add any configured nameservers */
	for (i=0; i<net.nsn && nsn < ARRAY_SIZE(nsv); i++)
		sa_cpy(&nsv[nsn++], &net.nsv[i]);

	return dnsc_alloc(&net.dnsc, NULL, nsv, nsn);
}


/**
 * Initialise networking
 *
 * @return 0 if success, otherwise errorcode
 */
int net_init(void)
{
	int err;

	tmr_init(&net.tmr);

	/* Initialise DNS resolver */
	err = dns_init();
	if (err) {
		DEBUG_WARNING("dns_init: %m\n", err);
		return err;
	}

	sa_init(&net.laddr, AF_INET);
	(void)sa_set_str(&net.laddr, "127.0.0.1", 0);

	if (str_isset(config.net.ifname)) {

		(void)re_printf("Binding to interface '%s'\n",
				config.net.ifname);

		str_ncpy(net.ifname, config.net.ifname, sizeof(net.ifname));

		err = net_if_getaddr(config.net.ifname,
				     AF_INET, &net.laddr);
		if (err) {
			DEBUG_WARNING("%s: could not get IPv4 address (%m)\n",
				      config.net.ifname, err);
		}

#ifdef HAVE_INET6
		str_ncpy(net.ifname6, config.net.ifname,
			 sizeof(net.ifname6));

		err = net_if_getaddr(config.net.ifname,
				     AF_INET6, &net.laddr6);
		if (err) {
			DEBUG_WARNING("%s: could not get IPv6 address (%m)\n",
				      config.net.ifname, err);
		}
#endif
	}
	else {
		(void)net_default_source_addr_get(AF_INET, &net.laddr);
		(void)net_rt_default_get(AF_INET, net.ifname,
					 sizeof(net.ifname));

#ifdef HAVE_INET6
		sa_init(&net.laddr6, AF_INET6);

		(void)net_default_source_addr_get(AF_INET6, &net.laddr6);
		(void)net_rt_default_get(AF_INET6, net.ifname6,
					 sizeof(net.ifname6));
#endif
	}

	(void)re_fprintf(stderr, "Local network address:");

	if (sa_isset(&net.laddr, SA_ADDR)) {
		(void)re_fprintf(stderr, " IPv4=%s:%j",
				 net.ifname, &net.laddr);
	}
#ifdef HAVE_INET6
	if (sa_isset(&net.laddr6, SA_ADDR)) {
		(void)re_fprintf(stderr, " IPv6=%s:%j",
				 net.ifname6, &net.laddr6);
	}
#endif
	(void)re_fprintf(stderr, "\n");

	return err;
}


/**
 * Reset the DNS resolver
 *
 * @return 0 if success, otherwise errorcode
 */
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


/**
 * Check for networking changes with a regular interval
 *
 * @param interval  Interval in seconds
 * @param ch        Handler called when a change was detected
 * @param arg       Handler argument
 */
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


static int dns_debug(struct re_printf *pf, void *unused)
{
	struct sa nsv[4];
	uint32_t i, nsn;
	int err;

	(void)unused;

	nsn = ARRAY_SIZE(nsv);

	err = dns_srv_get(NULL, 0, nsv, &nsn);
	if (err)
		nsn = 0;

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
 *
 * @return 0 if success, otherwise errorcode
 */
int net_debug(struct re_printf *pf, void *unused)
{
	int err;

	(void)unused;

	err  = re_hprintf(pf, "--- Network debug ---\n");
	err |= re_hprintf(pf, " Local IPv4: %9s - %j\n",
			  net.ifname, &net.laddr);
#ifdef HAVE_INET6
	err |= re_hprintf(pf, " Local IPv6: %9s - %j\n",
			  net.ifname6, &net.laddr6);
#endif

	err |= net_if_debug(pf, NULL);

	err |= net_rt_debug(pf, NULL);

	err |= dns_debug(pf, NULL);

	return err;
}


/**
 * Get the local IP Address for a specific Address Family (AF)
 *
 * @param af Address Family
 *
 * @return Local IP Address
 */
const struct sa *net_laddr_af(int af)
{
	switch (af) {

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


/**
 * Get the network domain name
 *
 * @return Network domain
 */
const char *net_domain(void)
{
	return net.domain[0] ? net.domain : NULL;
}
