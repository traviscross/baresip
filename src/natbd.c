/**
 * @file natbd.c  NAT Behavior Discovery
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "natbd"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


static struct {
	struct sa laddr;
	struct sa stun_srv;
	struct nat_hairpinning *nh_udp;
	struct nat_hairpinning *nh_tcp;
	struct nat_mapping *nm_udp;
	struct nat_mapping *nm_tcp;
	struct nat_filtering *nf;
	struct nat_lifetime *nl;
	struct nat_genalg *ga_udp;
	struct nat_genalg *ga_tcp;
	struct tmr tmr;

	int res_hp_udp;
	int res_hp_tcp;
	enum nat_type res_nm_udp;
	enum nat_type res_nm_tcp;
	enum nat_type res_nf;
	struct nat_lifetime_interval res_nl;
	uint32_t n_nl;
	int status_ga_udp;
	int status_ga_tcp;
	struct stun_dns *dns;
	bool terminated;
} natbd;


static const char *hairpinning_str(int res_hp)
{
	switch (res_hp) {

	case -1:  return "Unknown";
	case 0:   return "Not Supported";
	default:  return "Supported";
	}
}


static const char *genalg_str(int status)
{
	switch (status) {

	case -1: return "Not Detected";
	case 0:  return "Unknown";
	case 1:  return "Detected";
	default: return "???";
	}
}


static void natbd_start(void);


static void timeout(void *arg)
{
	(void)arg;
	natbd_start();
}


static void final_test(void)
{
	/* Start interval timer */
	tmr_start(&natbd.tmr, config.natbd.interval*1000, timeout, NULL);
}


static void nat_hairpinning_handler_udp(int err, bool supported, void *arg)
{
	const int res_hp = (0 == err) ? supported : -1;

	(void)arg;

	if (natbd.terminated)
		return;

	if (res_hp != natbd.res_hp_udp) {
		(void)re_fprintf(stderr, "NAT Hairpinning UDP changed from"
				 " (%s) to (%s)\n",
				 hairpinning_str(natbd.res_hp_udp),
				 hairpinning_str(res_hp));
	}

	natbd.res_hp_udp = res_hp;

	natbd.nh_udp = mem_deref(natbd.nh_udp);

	final_test();
}


static void nat_hairpinning_handler_tcp(int err, bool supported, void *arg)
{
	const int res_hp = (0 == err) ? supported : -1;

	(void)arg;

	if (natbd.terminated)
		return;

	if (res_hp != natbd.res_hp_tcp) {
		(void)re_fprintf(stderr, "NAT Hairpinning TCP changed from"
				 " (%s) to (%s)\n",
				 hairpinning_str(natbd.res_hp_tcp),
				 hairpinning_str(res_hp));
	}

	natbd.res_hp_tcp = res_hp;

	natbd.nh_tcp = mem_deref(natbd.nh_tcp);

	if (!natbd.nh_udp) {
		err = nat_hairpinning_alloc(&natbd.nh_udp,
					    &natbd.stun_srv, IPPROTO_UDP,
					    NULL, nat_hairpinning_handler_udp,
					    NULL);
		if (err) {
			DEBUG_WARNING("nat_hairpinning_alloc() failed (%s)\n",
				      strerror(err));
		}
		err = nat_hairpinning_start(natbd.nh_udp);
		if (err) {
			DEBUG_WARNING("nat_hairpinning_start() failed (%s)\n",
				      strerror(err));
		}
	}
}


static void nat_mapping_udp_handler(int err, enum nat_type type, void *arg)
{
	(void)arg;

	if (natbd.terminated)
		return;

	if (err) {
		DEBUG_WARNING("NAT mapping failed (%s)\n", strerror(err));
		goto out;
	}

	if (type != natbd.res_nm_udp) {
		(void)re_fprintf(stderr, "NAT Mapping UDP changed from (%s)"
				 " to (%s)\n",
				 nat_type_str(natbd.res_nm_udp),
				 nat_type_str(type));
	}
	natbd.res_nm_udp = type;

out:
	natbd.nm_udp = mem_deref(natbd.nm_udp);

	if (!natbd.nh_tcp) {
		err = nat_hairpinning_alloc(&natbd.nh_tcp,
					    &natbd.stun_srv, IPPROTO_TCP,
					    NULL, nat_hairpinning_handler_tcp,
					    NULL);
		if (err) {
			DEBUG_WARNING("nat_hairpinning_alloc() failed (%s)\n",
				      strerror(err));
		}
		err = nat_hairpinning_start(natbd.nh_tcp);
		if (err) {
			DEBUG_WARNING("nat_hairpinning_start() failed (%s)\n",
				      strerror(err));
		}
	}
}


static void nat_mapping_tcp_handler(int err, enum nat_type type, void *arg)
{
	(void)arg;

	if (natbd.terminated)
		return;

	if (err) {
		DEBUG_WARNING("NAT mapping failed (%s)\n", strerror(err));
		goto out;
	}

	if (type != natbd.res_nm_tcp) {
		(void)re_fprintf(stderr, "NAT Mapping TCP changed from (%s)"
				 " to (%s)\n",
				 nat_type_str(natbd.res_nm_tcp),
				 nat_type_str(type));
	}
	natbd.res_nm_tcp = type;

 out:
	natbd.nm_tcp = mem_deref(natbd.nm_tcp);

	if (!natbd.nm_udp) {
		err = nat_mapping_alloc(&natbd.nm_udp, &natbd.laddr,
					&natbd.stun_srv, IPPROTO_UDP,
					NULL, nat_mapping_udp_handler, NULL);
		if (err) {
			DEBUG_WARNING("nat_mapping_alloc() failed (%s)\n",
				      strerror(err));
		}
		err = nat_mapping_start(natbd.nm_udp);
		if (err) {
			DEBUG_WARNING("nat_mapping_start() failed (%s)\n",
				      strerror(err));
		}
	}
}


static void nat_filtering_handler(int err, enum nat_type type, void *arg)
{
	(void)arg;

	if (natbd.terminated)
		return;

	if (err) {
		DEBUG_WARNING("NAT filtering failed (%s)\n", strerror(err));
		goto out;
	}

	if (type != natbd.res_nf) {
		(void)re_fprintf(stderr, "NAT Filtering changed from (%s)"
				 " to (%s)\n",
				 nat_type_str(natbd.res_nf),
				 nat_type_str(type));
	}
	natbd.res_nf = type;

 out:
	natbd.nf = mem_deref(natbd.nf);

	if (!natbd.nm_tcp) {
		err = nat_mapping_alloc(&natbd.nm_tcp, &natbd.laddr,
					&natbd.stun_srv, IPPROTO_TCP,
					NULL, nat_mapping_tcp_handler, NULL);
		if (err) {
			DEBUG_WARNING("nat_mapping_alloc() failed (%s)\n",
				      strerror(err));
		}
		err = nat_mapping_start(natbd.nm_tcp);
		if (err) {
			DEBUG_WARNING("nat_mapping_start() failed (%s)\n",
				      strerror(err));
		}
	}
}


static void nat_lifetime_handler(int err,
				 const struct nat_lifetime_interval *interval,
				 void *arg)
{
	(void)arg;

	++natbd.n_nl;

	if (err) {
		DEBUG_WARNING("nat_lifetime_handler: (%s)\n", strerror(err));
		return;
	}

	natbd.res_nl = *interval;
}


static void nat_genalg_handler_udp(int err, uint16_t scode, const char *reason,
				   int status, const struct sa *map,
				   void *arg)
{
	(void)map;
	(void)arg;

	if (natbd.terminated)
		return;

	if (err) {
		DEBUG_WARNING("Generic ALG detection failed (%s)\n",
			      strerror(err));
		goto out;
	}

	if (scode) {
		DEBUG_WARNING("Generic ALG detection failed: %u %s\n",
			      scode, reason);
		goto out;
	}

	if (status != natbd.status_ga_udp) {
		(void)re_fprintf(stderr, "Generic ALG for UDP changed from"
				 " (%s) to (%s)\n",
				 genalg_str(natbd.status_ga_udp),
				 genalg_str(status));
	}

	natbd.status_ga_udp = status;

 out:
	natbd.ga_udp = mem_deref(natbd.ga_udp);

	if (!natbd.nf) {
		err = nat_filtering_alloc(&natbd.nf, &natbd.stun_srv,
					  NULL, nat_filtering_handler, NULL);
		if (err) {
			DEBUG_WARNING("nat_filtering_alloc() (%s)\n",
				      strerror(err));
		}
		err = nat_filtering_start(natbd.nf);
		if (err) {
			DEBUG_WARNING("nat_filtering_start() (%s)\n",
				      strerror(err));
		}
	}
}


static void nat_genalg_handler_tcp(int err, uint16_t scode, const char *reason,
				   int status, const struct sa *map,
				   void *arg)
{
	(void)map;
	(void)arg;

	if (natbd.terminated)
		return;

	if (err) {
		DEBUG_WARNING("Generic ALG detection failed (%s)\n",
			      strerror(err));
		goto out;
	}

	if (scode) {
		DEBUG_WARNING("Generic ALG detection failed: %u %s\n",
			      scode, reason);
		goto out;
	}

	if (status != natbd.status_ga_tcp) {
		(void)re_fprintf(stderr, "Generic ALG for TCP changed from"
				 " (%s) to (%s)\n",
				 genalg_str(natbd.status_ga_tcp),
				 genalg_str(status));
	}

	natbd.status_ga_tcp = status;

 out:
	natbd.ga_tcp = mem_deref(natbd.ga_tcp);

	/* Generic ALG Detection for UDP */
	if (!natbd.ga_udp) {
		err = nat_genalg_alloc(&natbd.ga_udp, &natbd.stun_srv,
				       IPPROTO_UDP,
				       NULL, nat_genalg_handler_udp, NULL);
		if (err) {
			DEBUG_WARNING("nat_genalg_alloc() UDP failed (%s)\n",
				      strerror(err));
		}
		err = nat_genalg_start(natbd.ga_udp);
		if (err) {
			DEBUG_WARNING("nat_genalg_start() UDP failed (%s)\n",
				      strerror(err));
		}
	}
}


static void natbd_start(void)
{
	int err;

	/* Generic ALG Detection for TCP */
	if (!natbd.ga_tcp) {
		err = nat_genalg_alloc(&natbd.ga_tcp, &natbd.stun_srv,
				       IPPROTO_TCP,
				       NULL, nat_genalg_handler_tcp, NULL);
		if (err) {
			DEBUG_WARNING("nat_genalg_alloc() TCP failed (%s)\n",
				      strerror(err));
		}
		err = nat_genalg_start(natbd.ga_tcp);
		if (err) {
			DEBUG_WARNING("nat_genalg_start() TCP failed (%s)\n",
				      strerror(err));
		}
	}
}


static void natbd_stop(void)
{
	natbd.nh_udp = mem_deref(natbd.nh_udp);
	natbd.nh_tcp = mem_deref(natbd.nh_tcp);
	natbd.nm_udp = mem_deref(natbd.nm_udp);
	natbd.nm_tcp = mem_deref(natbd.nm_tcp);
	natbd.nf = mem_deref(natbd.nf);
	natbd.nl = mem_deref(natbd.nl);
	natbd.ga_udp = mem_deref(natbd.ga_udp);
	natbd.ga_tcp = mem_deref(natbd.ga_tcp);
}


static void dns_handler(int err, const struct sa *addr, void *arg)
{
	(void)arg;

	if (err)
		goto out;

	sa_cpy(&natbd.stun_srv, addr);

	DEBUG_NOTICE("dns: starting NATBD on STUN server at %J\n",
		     &natbd.stun_srv);

	natbd_start();

	if (!natbd.nl) {
		err = nat_lifetime_alloc(&natbd.nl, &natbd.stun_srv, 3,
					 NULL, nat_lifetime_handler, NULL);
		if (err) {
			DEBUG_WARNING("nat_lifetime_alloc() failed (%s)\n",
				      strerror(err));
		}
		err = nat_lifetime_start(natbd.nl);
		if (err) {
			DEBUG_WARNING("nat_lifetime_start() failed (%s)\n",
				      strerror(err));
		}
	}

 out:
	natbd.dns = mem_deref(natbd.dns);
}


/**
 * Start the NAT Behavior Discovery
 *
 * @param laddr Local IP address
 * @param host  STUN Server hostname (domain)
 * @param port  STUN Server port number (0=SRV)
 * @param dnsc  DNS Client
 *
 * @return 0 if success, otherwise errorcode
 */
int natbd_init(const struct sa *laddr, const struct pl *host, uint16_t port,
	       struct dnsc *dnsc)
{
	char domain[256];
	int err;

	if (!laddr || !host)
		return EINVAL;

	if (natbd.dns)
		return 0;

	if (!config.natbd.interval)
		return 0;

	tmr_init(&natbd.tmr);
	sa_cpy(&natbd.laddr, laddr);

	(void)pl_strcpy(host, domain, sizeof(domain));
	err = stun_server_discover(&natbd.dns, dnsc, stun_usage_binding,
				   stun_proto_udp, sa_af(laddr),
				   domain, port, dns_handler, NULL);
	if (err) {
		DEBUG_WARNING("init: stun_server_discover: (%s)\n",
			      strerror(err));
	}

	return err;
}


/**
 * Stop the NAT Behavior Discovery
 */
void natbd_close(void)
{
	natbd.terminated = true;

	natbd_stop();
	tmr_cancel(&natbd.tmr);

	natbd.dns = mem_deref(natbd.dns);
}


/**
 * Print NAT Behavior Discovery status
 *
 * @param pf     Print handler for status output
 * @param unused Unused parameter
 */
int natbd_status(struct re_printf *pf, void *unused)
{
	int err;

	(void)unused;

	err  = re_hprintf(pf, "NAT Binding Discovery (using %J)"
			  " [next in %us]\n",
			  &natbd.stun_srv, tmr_get_expire(&natbd.tmr)/1000);
	err |= re_hprintf(pf, "  Hairpinning: %s (UDP), %s (TCP)\n",
			  hairpinning_str(natbd.res_hp_udp),
			  hairpinning_str(natbd.res_hp_tcp));
	err |= re_hprintf(pf, "  Mapping:     %s (UDP), %s (TCP)\n",
			  nat_type_str(natbd.res_nm_udp),
			  nat_type_str(natbd.res_nm_tcp));
	err |= re_hprintf(pf, "  Filtering:   %s\n",
			  nat_type_str(natbd.res_nf));
	err |= re_hprintf(pf, "  Lifetime:    min=%u cur=%u max=%u"
			  " (%u probes)\n",
			  natbd.res_nl.min, natbd.res_nl.cur, natbd.res_nl.max,
			  natbd.n_nl);
	err |= re_hprintf(pf, "  Generic ALG: %s (UDP), %s (TCP)\n",
			  genalg_str(natbd.status_ga_udp),
			  genalg_str(natbd.status_ga_tcp));

	return err;
}
