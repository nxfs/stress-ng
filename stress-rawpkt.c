/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-builtin.h"
#include "core-capabilities.h"
#include "core-net.h"

#if defined(HAVE_LINUX_IF_PACKET_H)
#include <linux/if_packet.h>
#endif

#if defined(HAVE_LINUX_IF_TUN_H)
#include <linux/if_tun.h>
#endif

#if defined(HAVE_LINUX_SOCKIOS_H)
#include <linux/sockios.h>
#endif

#if defined(HAVE_LINUX_UDP_H)
#include <linux/udp.h>
#endif

#if defined(HAVE_NET_IF_H)
#include <net/if.h>
#endif

#if defined(HAVE_NETINET_IP_H)
#include <netinet/ip.h>
#endif

#include <arpa/inet.h>

#define MIN_RAWPKT_PORT		(1024)
#define MAX_RAWPKT_PORT		(65535)
#define DEFAULT_RAWPKT_PORT	(14000)

#if !defined(SOL_UDP)
#define SOL_UDP 	(17)
#endif
#define PACKET_SIZE	(2048)

static const stress_help_t help[] = {
	{ NULL, "rawpkt N",		"start N workers exercising raw packets" },
	{ NULL,	"rawpkt-ops N",		"stop after N raw packet bogo operations" },
	{ NULL,	"rawpkt-port P",	"use raw packet ports P to P + number of workers - 1" },
	{ NULL, "rawpkt-rxring N",	"setup raw packets with RX ring with N number of blocks, this selects TPACKET_V3"},
	{ NULL,	NULL,			NULL }
};

/*
 *  stress_rawpkt_supported()
 *      check if we can run this as root
 */
static int stress_rawpkt_supported(const char *name)
{
	if (!stress_check_capability(SHIM_CAP_NET_RAW)) {
		pr_inf_skip("%s stressor will be skipped, "
			"need to be running with CAP_NET_RAW "
			"rights for this stressor\n", name);
		return -1;
	}
	return 0;
}

/*
 *  stress_set_rawpkt_port()
 *	set port to use
 */
static int stress_set_port(const char *opt)
{
	int port;

	stress_set_net_port("rawpkt-port", opt,
		MIN_RAWPKT_PORT, MAX_RAWPKT_PORT - STRESS_PROCS_MAX,
		&port);
	return stress_set_setting("rawpkt-port", TYPE_ID_INT, &port);
}

/*
 *  stress_set_rawpkt_rxring()
 *  set RX ring to use
 */
static int stress_set_rxring(const char *opt)
{
	const uint64_t val = stress_get_uint64(opt);
	int ival;

	stress_check_power_of_2("rawpkt-rxring", val, (uint64_t)1, (uint64_t)16);
	ival = (int)val;
	return stress_set_setting("rawpkt-rxring", TYPE_ID_INT, &ival);
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_rawpkt_port,	stress_set_port },
	{ OPT_rawpkt_rxring,	stress_set_rxring },
	{ 0,			NULL }
};

#if defined(HAVE_LINUX_UDP_H) &&	\
    defined(HAVE_LINUX_IF_PACKET_H)
/*
 *  stress_rawpkt_sockopts()
 *	fetch some SOL_PACKET specific stats, ignore failures
 *	just exercise the interface.
 */
static void stress_rawpkt_sockopts(const int fd)
{
#if defined(PACKET_STATISTICS)
	{
		struct tpacket_stats stats;
		socklen_t len = sizeof(stats);

		(void)getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &stats, &len);
	}
#endif
#if defined(PACKET_AUXDATA)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &val, len);
	}
#endif
#if defined(PACKET_ORIGDEV)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
#if defined(PACKET_VNET_HDR)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_VNET_HDR, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
#if defined(PACKET_VERSION)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_VERSION, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
#if defined(PACKET_HDRLEN)
	{
		static const int vals[] = { 0, 1, 2 };
		int val = (int)stress_mwc32modn(SIZEOF_ARRAY(vals));
		socklen_t len = sizeof(val);

		(void)getsockopt(fd, SOL_PACKET, PACKET_HDRLEN, &val, &len);
	}
#endif
#if defined(PACKET_RESERVE)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_RESERVE, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
#if defined(PACKET_LOSS)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_LOSS, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
#if defined(PACKET_TIMESTAMP)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_TIMESTAMP, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
#if defined(PACKET_FANOUT)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_FANOUT, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
#if defined(PACKET_IGNORE_OUTGOING)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
#if defined(PACKET_ROLLOVER_STATS)
	{
		struct tpacket_rollover_stats rstats;
		socklen_t len = sizeof(rstats);

		(void)getsockopt(fd, SOL_PACKET, PACKET_ROLLOVER_STATS, &rstats, &len);
	}
#endif
#if defined(PACKET_TX_HAS_OFF)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_TX_HAS_OFF, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
#if defined(PACKET_QDISC_BYPASS)
	{
		int ret, val;
		socklen_t len = sizeof(val);

		ret = getsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, &val, &len);
		if (ret == 0)
			(void)setsockopt(fd, SOL_PACKET, PACKET_ORIGDEV, &val, len);
	}
#endif
	{
		int val;
		socklen_t len = sizeof(val);

		(void)getsockopt(fd, SOL_PACKET, ~(int)0, &val, &len);
	}
}

/*
 *  stress_rawpkt_client()
 *	client sender
 */
static void NORETURN OPTIMIZE3 stress_rawpkt_client(
	const stress_args_t *args,
	struct ifreq *hwaddr,
	struct ifreq *ifaddr,
	const struct ifreq *idx,
	const pid_t ppid,
	const int port)
{
	int rc = EXIT_FAILURE;
	uint16_t id = 12345;
	uint32_t buf[PACKET_SIZE / sizeof(uint32_t)] ALIGN64;
	struct ethhdr *eth = (struct ethhdr *)buf;
	struct iphdr *ip = (struct iphdr *)((uintptr_t)buf + sizeof(struct ethhdr));
	struct udphdr *udp = (struct udphdr *)((uintptr_t)buf + sizeof(struct ethhdr) + sizeof(struct iphdr));
	struct sockaddr_ll sadr;
	int fd;

	stress_parent_died_alarm();
	(void)sched_settings_apply(true);

	(void)shim_memset(&sadr, 0, sizeof(sadr));
	(void)shim_memset(buf, 0, sizeof(buf));

	(void)shim_memcpy(eth->h_dest, hwaddr->ifr_addr.sa_data, sizeof(eth->h_dest));
	(void)shim_memcpy(eth->h_source, hwaddr->ifr_addr.sa_data, sizeof(eth->h_dest));
	eth->h_proto = htons(ETH_P_IP);

	ip->ihl = 5;		/* Header length in 32 bit words */
	ip->version = 4;	/* IPv4 */
	ip->tos = 0;
	ip->tot_len = sizeof(struct iphdr) + sizeof(struct udphdr);
	ip->ttl = 16;  		/* Not too many hops! */
	ip->protocol = SOL_UDP;	/* UDP protocol */
	ip->saddr = inet_addr(inet_ntoa((((struct sockaddr_in *)&(ifaddr->ifr_addr))->sin_addr)));
	ip->daddr = ip->saddr;

	udp->source = htons(port);
	udp->dest = htons(port);
	udp->len = htons(sizeof(struct udphdr));

	sadr.sll_ifindex = idx->ifr_ifindex;
	sadr.sll_halen = ETH_ALEN;
	(void)shim_memcpy(&sadr.sll_addr, eth->h_dest, sizeof(eth->h_dest));

	if ((fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) < 0) {
		pr_fail("%s: socket failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		goto err;
	}

	do {
		ssize_t n;

		ip->id = htons(id++);
		ip->check = stress_ipv4_checksum((uint16_t *)ip, sizeof(struct iphdr) + sizeof(struct udphdr));

		n = sendto(fd, buf, sizeof(struct ethhdr) + ip->tot_len, 0, (struct sockaddr *)&sadr, sizeof(sadr));
		if (UNLIKELY(n < 0)) {
			pr_fail("%s: raw socket sendto failed on port %d, errno=%d (%s)\n",
				args->name, port, errno, strerror(errno));
		}
#if defined(SIOCOUTQ)
		/* Occasionally exercise SIOCOUTQ */
		if (UNLIKELY((id & 0xff) == 0)) {
			int queued;

			VOID_RET(int, ioctl(fd, SIOCOUTQ, &queued));
		}
#endif
	} while (keep_stressing(args));

	stress_rawpkt_sockopts(fd);
	(void)close(fd);

	rc = EXIT_SUCCESS;

err:
	/* Inform parent we're all done */
	(void)kill(ppid, SIGALRM);
	_exit(rc);
}

/*
 *  stress_rawpkt_server()
 *	server reader
 */
static int OPTIMIZE3 stress_rawpkt_server(
	const stress_args_t *args,
	struct ifreq *ifaddr,
	const int port,
	const int blocknr)
{
	int fd;
	int rc = EXIT_SUCCESS;
	uint32_t buf[PACKET_SIZE / sizeof(uint32_t)];
	struct ethhdr *eth = (struct ethhdr *)buf;
	const struct iphdr *ip = (struct iphdr *)((uintptr_t)buf + sizeof(struct ethhdr));
	const struct udphdr *udp = (struct udphdr *)((uintptr_t)buf + sizeof(struct ethhdr) + sizeof(struct iphdr));
	struct sockaddr saddr;
	int saddr_len = sizeof(saddr);
	const uint32_t addr = inet_addr(inet_ntoa((((struct sockaddr_in *)&(ifaddr->ifr_addr))->sin_addr)));
	uint64_t all_pkts = 0;
	const ssize_t min_size = sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr);
	double t_start, duration, bytes = 0.0, rate;

	if (stress_sig_stop_stressing(args->name, SIGALRM) < 0) {
		rc = EXIT_FAILURE;
		goto die;
	}
	if ((fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) < 0) {
		rc = stress_exit_status(errno);
		pr_fail("%s: socket failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		goto die;
	}

#if defined(PACKET_RX_RING) &&	\
    defined(PACKET_VERSION) &&	\
    defined(HAVE_TPACKET_REQ3)
	if (blocknr) {
		struct tpacket_req3 tp;
		int val = TPACKET_V3;

		if (setsockopt(fd, SOL_PACKET, PACKET_VERSION, &val, sizeof(val)) < 0) {
			rc = stress_exit_status(errno);
			pr_fail("%s: setsockopt failed to set packet version, errno=%d (%s)\n", args->name, errno, strerror(errno));
			goto close_fd;
		}
		(void)shim_memset(&tp, 0, sizeof(tp));
		tp.tp_block_size = getpagesize();
		tp.tp_block_nr = blocknr;
		tp.tp_frame_size = getpagesize() / blocknr;
		tp.tp_frame_nr = tp.tp_block_size / tp.tp_frame_size * blocknr;

		if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, (void*) &tp, sizeof(tp)) < 0) {
			rc = stress_exit_status(errno);
			pr_fail("%s: setsockopt failed to set rx ring, errno=%d (%s)\n", args->name, errno, strerror(errno));
			goto close_fd;
		}
	}
#else
	(void)blocknr;
#endif

	t_start = stress_time_now();
	do {
		ssize_t n;

		n = recvfrom(fd, buf, sizeof(buf), 0, &saddr, (socklen_t *)&saddr_len);
		if (LIKELY(n >= min_size)) {
			all_pkts++;
			if ((eth->h_proto == htons(ETH_P_IP)) &&
			    (ip->saddr == addr) &&
			    (ip->protocol == SOL_UDP) &&
			    (ntohs(udp->source) == port)) {
				inc_counter(args);
				bytes += (double)n;
			}
		}
#if defined(SIOCINQ)
		/* Exercise SIOCINQ */
		if (UNLIKELY((all_pkts & 0xff) == 0)) {
			int queued;

			VOID_RET(int, ioctl(fd, SIOCINQ, &queued));
		}
#endif
	} while (keep_stressing(args));

	duration = stress_time_now() - t_start;
	rate = (duration > 0.0) ? bytes / duration : 0.0;
	stress_metrics_set(args, 0, "MB recv'd per sec", rate / (double)MB);

	stress_rawpkt_sockopts(fd);
#if defined(PACKET_RX_RING) &&	\
    defined(PACKET_VERSION) &&	\
    defined(HAVE_TPACKET_REQ3)
close_fd:
#endif
	(void)close(fd);
die:
	pr_dbg("%s: %" PRIu64 " packets sent, %" PRIu64 " packets received\n", args->name, get_counter(args), all_pkts);

	return rc;
}

static void stress_sock_sigpipe_handler(int signum)
{
	(void)signum;

	keep_stressing_set_flag(false);
}

/*
 *  stress_rawpkt
 *	stress raw socket I/O UDP packet send/receive
 */
static int stress_rawpkt(const stress_args_t *args)
{
	pid_t pid;
	int rawpkt_port = DEFAULT_RAWPKT_PORT;
	int fd, rc = EXIT_FAILURE;
	struct ifreq hwaddr, ifaddr, idx;
	int rawpkt_rxring = 0;

	(void)stress_get_setting("rawpkt-port", &rawpkt_port);
	(void)stress_get_setting("rawpkt-rxring", &rawpkt_rxring);

	rawpkt_port += args->instance;

	pr_dbg("%s: process [%d] using socket port %d\n",
		args->name, (int)args->pid, rawpkt_port);

	if (stress_sighandler(args->name, SIGPIPE, stress_sock_sigpipe_handler, NULL) < 0)
		return EXIT_NO_RESOURCE;

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		pr_fail("%s: socket failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_FAILURE;
	}
	(void)shim_memset(&hwaddr, 0, sizeof(hwaddr));
	(void)shim_strlcpy(hwaddr.ifr_name, "lo", sizeof(hwaddr.ifr_name));
	if (ioctl(fd, SIOCGIFHWADDR, &hwaddr) < 0) {
		pr_fail("%s: ioctl SIOCGIFHWADDR on lo failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		(void)close(fd);
		return EXIT_FAILURE;
	}

	(void)shim_memset(&ifaddr, 0, sizeof(ifaddr));
	(void)shim_strlcpy(ifaddr.ifr_name, "lo", sizeof(ifaddr.ifr_name));
	if (ioctl(fd, SIOCGIFADDR, &ifaddr) < 0) {
		pr_fail("%s: ioctl SIOCGIFADDR on lo failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		(void)close(fd);
		return EXIT_FAILURE;
	}

	(void)shim_memset(&idx, 0, sizeof(idx));
	(void)shim_strlcpy(idx.ifr_name, "lo", sizeof(idx.ifr_name));
	if (ioctl(fd, SIOCGIFINDEX, &idx) < 0) {
		pr_fail("%s: ioctl SIOCGIFINDEX on lo failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		(void)close(fd);
		return EXIT_FAILURE;
	}
	(void)close(fd);

	stress_set_proc_state(args->name, STRESS_STATE_RUN);
again:
	pid = fork();
	if (pid < 0) {
		if (stress_redo_fork(errno))
			goto again;
		if (!keep_stressing(args)) {
			rc = EXIT_SUCCESS;
			goto finish;
		}
		pr_fail("%s: fork failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return rc;
	} else if (pid == 0) {
		stress_rawpkt_client(args, &hwaddr, &ifaddr, &idx, args->pid, rawpkt_port);
	} else {
		int status;

		rc = stress_rawpkt_server(args, &ifaddr, rawpkt_port, rawpkt_rxring);
		(void)kill(pid, SIGKILL);
		(void)shim_waitpid(pid, &status, 0);
	}
finish:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	return rc;
}

stressor_info_t stress_rawpkt_info = {
	.stressor = stress_rawpkt,
	.class = CLASS_NETWORK | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.supported = stress_rawpkt_supported,
	.verify = VERIFY_ALWAYS,
	.help = help
};
#else
stressor_info_t stress_rawpkt_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_NETWORK | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.supported = stress_rawpkt_supported,
	.help = help,
	.unimplemented_reason = "built without linux/if_packet.h, linux/if_tun.h, linux/sockios.h or linux/udp.h"
};
#endif
