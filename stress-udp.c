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
#include "core-cpu.h"
#include "core-net.h"

#if defined(HAVE_LINUX_SOCKIOS_H)
#include <linux/sockios.h>
#else
UNEXPECTED
#endif

#if defined(HAVE_LINUX_UDP_H)
#include <linux/udp.h>
#else
UNEXPECTED
#endif

#if defined(HAVE_SYS_UN_H)
#include <sys/un.h>
#else
UNEXPECTED
#endif

#include <netinet/in.h>

#define MIN_UDP_PORT		(1024)
#define MAX_UDP_PORT		(65535)
#define DEFAULT_UDP_PORT	(7000)

#define UDP_BUF			(1024)	/* UDP I/O buffer size */

/* See bugs section of udplite(7) */
#if !defined(SOL_UDPLITE)
#define SOL_UDPLITE		(136)
#endif
#if !defined(UDPLITE_SEND_CSCOV)
#define UDPLITE_SEND_CSCOV	(10)
#endif
#if !defined(UDPLITE_RECV_CSCOV)
#define UDPLITE_RECV_CSCOV	(11)
#endif

static const stress_help_t help[] = {
	{ NULL,	"udp N",	"start N workers performing UDP send/receives " },
	{ NULL,	"udp-domain D",	"specify domain, default is ipv4" },
	{ NULL, "udp-gro",	"enable UDP-GRO" },
	{ NULL,	"udp-if I",	"use network interface I, e.g. lo, eth0, etc." },
	{ NULL,	"udp-lite",	"use the UDP-Lite (RFC 3828) protocol" },
	{ NULL,	"udp-ops N",	"stop after N udp bogo operations" },
	{ NULL,	"udp-port P",	"use ports P to P + number of workers - 1" },
	{ NULL,	NULL,		NULL }
};

static int stress_set_udp_port(const char *opt)
{
	int udp_port;

	stress_set_net_port("udp-port", opt,
		MIN_UDP_PORT, MAX_UDP_PORT, &udp_port);
	return stress_set_setting("udp-port", TYPE_ID_INT, &udp_port);
}

/*
 *  stress_set_udp_domain()
 *	set the udp domain option
 */
static int stress_set_udp_domain(const char *name)
{
	int ret, udp_domain;

	ret = stress_set_net_domain(DOMAIN_INET | DOMAIN_INET6, "udp-domain", name, &udp_domain);
	stress_set_setting("udp-domain", TYPE_ID_INT, &udp_domain);

	return ret;
}

static int stress_set_udp_lite(const char *opt)
{
	return stress_set_setting_true("udp-lite", opt);
}

static int stress_set_udp_gro(const char *opt)
{
	return stress_set_setting_true("udp-gro", opt);
}

static int stress_set_udp_if(const char *name)
{
	return stress_set_setting("udp-if", TYPE_ID_STR, name);
}

static int OPTIMIZE3 stress_udp_client(
	const stress_args_t *args,
	const pid_t mypid,
	const int udp_domain,
	const int udp_proto,
	const int udp_port,
	const bool udp_gro,
	const char *udp_if)
{
	struct sockaddr *addr = NULL;
	int rc = EXIT_FAILURE;
	int index = 0;

	stress_parent_died_alarm();
	(void)sched_settings_apply(true);

	do {
		socklen_t len;
		int fd;
		int j = 0;

		if ((fd = socket(udp_domain, SOCK_DGRAM, udp_proto)) < 0) {
			pr_fail("%s: socket failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			goto child_die;
		}

		if (stress_set_sockaddr_if(args->name, args->instance, mypid,
				udp_domain, udp_port, udp_if,
				&addr, &len, NET_ADDR_ANY) < 0) {
			(void)close(fd);
			goto child_die;
		}
#if defined(IPPROTO_UDPLITE) &&	\
    defined(UDPLITE_SEND_CSCOV)
		if (udp_proto == IPPROTO_UDPLITE) {
			int val = 8;	/* Just the 8 byte header */
			socklen_t slen;

			slen = sizeof(val);
			if (UNLIKELY(setsockopt(fd, SOL_UDPLITE, UDPLITE_SEND_CSCOV, &val, slen) < 0)) {
				pr_fail("%s: setsockopt failed, errno=%d (%s)\n",
					args->name, errno, strerror(errno));
				(void)close(fd);
				goto child_die;
			}
			slen = sizeof(val);
			(void)getsockopt(fd, SOL_UDPLITE, UDPLITE_SEND_CSCOV, &val, &slen);
		}
#endif
#if defined(IPPROTO_UDPLITE) &&	\
    defined(UDPLITE_RECV_CSCOV)
		if (udp_proto == IPPROTO_UDPLITE) {
			int val;
			socklen_t slen = sizeof(val);

			(void)getsockopt(fd, udp_proto, UDPLITE_RECV_CSCOV, &val, &slen);
		}
#endif

#if defined(UDP_GRO)
		if (udp_gro) {
			int val;
			socklen_t slen = sizeof(val);

			val = 1;
			VOID_RET(int, setsockopt(fd, udp_proto, UDP_GRO, &val, slen));
		}
#else
		(void)udp_gro;
#endif

#if defined(UDP_CORK)
		{
			int val, ret;
			socklen_t slen = sizeof(val);

			ret = getsockopt(fd, udp_proto, UDP_CORK, &val, &slen);
			if (LIKELY(ret == 0)) {
				slen = sizeof(val);
				VOID_RET(int, setsockopt(fd, udp_proto, UDP_CORK, &val, slen));
			}
		}
#else
		UNEXPECTED
#endif
#if defined(UDP_ENCAP)
		{
			int val, ret;
			socklen_t slen = sizeof(val);

			ret = getsockopt(fd, udp_proto, UDP_ENCAP, &val, &slen);
			if (LIKELY(ret == 0)) {
				slen = sizeof(val);
				VOID_RET(int, setsockopt(fd, udp_proto, UDP_ENCAP, &val, slen));
			}
		}
#else
		UNEXPECTED
#endif
#if defined(UDP_NO_CHECK6_TX)
		{
			int val, ret;
			socklen_t slen = sizeof(val);

			ret = getsockopt(fd, udp_proto, UDP_NO_CHECK6_TX, &val, &slen);
			if (LIKELY(ret == 0)) {
				slen = sizeof(val);
				VOID_RET(int, setsockopt(fd, udp_proto, UDP_NO_CHECK6_TX, &val, slen));
			}
		}
#else
		UNEXPECTED
#endif
#if defined(UDP_NO_CHECK6_RX)
		{
			int val, ret;
			socklen_t slen = sizeof(val);
			ret = getsockopt(fd, udp_proto, UDP_NO_CHECK6_RX, &val, &slen);
			if (LIKELY(ret == 0)) {
				slen = sizeof(val);
				VOID_RET(int, setsockopt(fd, udp_proto, UDP_NO_CHECK6_RX, &val, slen));
			}
		}
#else
		UNEXPECTED
#endif
#if defined(UDP_SEGMENT)
		{
			int val, ret;
			socklen_t slen = sizeof(val);

			ret = getsockopt(fd, udp_proto, UDP_SEGMENT, &val, &slen);
			if (LIKELY(ret == 0)) {
				slen = sizeof(val);
				VOID_RET(int, setsockopt(fd, udp_proto, UDP_SEGMENT, &val, slen));
			}
		}
#else
		UNEXPECTED
#endif
		do {
			char buf[UDP_BUF];
			register size_t i;

			for (i = 16; i < sizeof(buf); i += 16, j++) {
				static const char patterns[] ALIGN64 =
					"ABCDEFGHIJKLMNOPQRSTUVWXYZ_+@:#!";
				const int c = patterns[index++ & 0x1f];
				ssize_t ret;

				(void)shim_memset(buf, c, sizeof(buf));
				ret = sendto(fd, buf, i, 0, addr, len);
				if (UNLIKELY(ret < 0)) {
					if ((errno == EINTR) || (errno == ENETUNREACH))
						break;
					pr_fail("%s: sendto failed, errno=%d (%s)\n",
						args->name, errno, strerror(errno));
					break;
				}
			}
#if defined(SIOCOUTQ)
			{
				int pending;

				VOID_RET(int, ioctl(fd, SIOCOUTQ, &pending));
			}
#else
		UNEXPECTED
#endif
		} while (keep_stressing(args));
		(void)close(fd);
	} while (keep_stressing(args));

	rc = EXIT_SUCCESS;
child_die:

#if defined(AF_UNIX) &&		\
    defined(HAVE_SOCKADDR_UN)
	if ((udp_domain == AF_UNIX) && addr) {
		struct sockaddr_un *addr_un = (struct sockaddr_un *)addr;

		(void)shim_unlink(addr_un->sun_path);
	}
#endif
	return rc;
}

static int OPTIMIZE3 stress_udp_server(
	const stress_args_t *args,
	const pid_t mypid,
	const int udp_domain,
	const int udp_proto,
	const int udp_port,
	const bool udp_gro,
	const char *udp_if)
{
	char buf[UDP_BUF];
	int fd;
#if !defined(__minix__)
	int so_reuseaddr = 1;
#endif
	socklen_t addr_len = 0;
	struct sockaddr *addr = NULL;
	int rc = EXIT_FAILURE;

	if (stress_sig_stop_stressing(args->name, SIGALRM) < 0)
		goto die;
	if ((fd = socket(udp_domain, SOCK_DGRAM, udp_proto)) < 0) {
		pr_fail("%s: socket failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		goto die;
	}
	if (stress_set_sockaddr_if(args->name, args->instance, mypid,
			udp_domain, udp_port, udp_if,
			&addr, &addr_len, NET_ADDR_ANY) < 0) {
		goto die_close;
	}
#if defined(IPPROTO_UDPLITE)
	if (udp_proto == IPPROTO_UDPLITE) {
		int val = 8;	/* Just the 8 byte header */

		if (setsockopt(fd, SOL_UDPLITE, UDPLITE_RECV_CSCOV, &val, sizeof(val)) < 0) {
			pr_fail("%s: setsockopt failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			goto die_close;
		}
	}
#endif
	if (bind(fd, addr, addr_len) < 0) {
		pr_fail("%s: bind failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		goto die_close;
	}
#if !defined(__minix__)
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr)) < 0) {
		/*
		 *  Some systems don't support SO_REUSEADDR
		 */
		if (errno != EINVAL) {
			pr_fail("%s: setsockopt failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			goto die_close;
		}
	}
#endif

#if defined(UDP_GRO)
	if (udp_gro) {
		int val;
		socklen_t slen = sizeof(val);

		val = 1;
		VOID_RET(int, setsockopt(fd, udp_proto, UDP_GRO, &val, slen));
	}
#else
	(void)udp_gro;
#endif
	do {
		socklen_t len = addr_len;
		ssize_t n;
#if defined(SIOCOUTQ)
		{
			int pending;

			VOID_RET(int, ioctl(fd, SIOCINQ, &pending));
		}
#else
		UNEXPECTED
#endif
		n = recvfrom(fd, buf, sizeof(buf), 0, addr, &len);
		if (UNLIKELY(n <= 0)) {
			if (n == 0)
				break;
			if (errno != EINTR)
				pr_fail("%s: recvfrom failed, errno=%d (%s)\n",
					args->name, errno, strerror(errno));
			break;
		}
		inc_counter(args);
	} while (keep_stressing(args));

	rc = EXIT_SUCCESS;
die_close:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	(void)close(fd);
die:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
#if defined(AF_UNIX) &&		\
    defined(HAVE_SOCKADDR_UN)
	if ((udp_domain == AF_UNIX) && addr) {
		struct sockaddr_un *addr_un = (struct sockaddr_un *)addr;

		(void)shim_unlink(addr_un->sun_path);
	}
#endif
	return rc;
}

/*
 *  stress_udp
 *	stress by heavy udp ops
 */
static int stress_udp(const stress_args_t *args)
{
	int udp_port = DEFAULT_UDP_PORT;
	int udp_domain = AF_INET;
	pid_t pid, mypid = getpid();
	int rc = EXIT_SUCCESS, reserved_port;
	int udp_proto = 0;
#if defined(IPPROTO_UDPLITE)
	bool udp_lite = false;
#endif
	bool udp_gro = false;
	char *udp_if = NULL;

	(void)stress_get_setting("udp-if", &udp_if);
	(void)stress_get_setting("udp-port", &udp_port);
	(void)stress_get_setting("udp-domain", &udp_domain);
#if defined(IPPROTO_UDPLITE)
	(void)stress_get_setting("udp-lite", &udp_lite);

	udp_proto = udp_lite ? IPPROTO_UDPLITE : IPPROTO_UDP;

	if ((udp_proto == IPPROTO_UDPLITE) &&
	    (udp_domain == AF_UNIX)) {
		udp_proto = 0;
		if (args->instance == 0) {
			pr_inf("%s: disabling UDP-Lite as it is not "
				"available for UNIX domain UDP\n",
				args->name);
		}
	}
#endif
	udp_port += args->instance;
	reserved_port = stress_net_reserve_ports(udp_port, udp_port);
	if (reserved_port < 0) {
		pr_inf_skip("%s: cannot reserve port %d, skipping stressor\n",
			args->name, udp_port);
		return EXIT_NO_RESOURCE;
	}
        udp_port = reserved_port;
	pr_dbg("%s: process [%d] using udp port %d\n",
		args->name, (int)args->pid, udp_port);

#if defined(UDP_GRO)
	(void)stress_get_setting("udp-gro", &udp_gro);
#endif
	if (udp_if) {
		int ret;
		struct sockaddr if_addr;

		ret = stress_net_interface_exists(udp_if, udp_domain, &if_addr);
		if (ret < 0) {
			pr_inf("%s: interface '%s' is not enabled for domain '%s', defaulting to using loopback\n",
				args->name, udp_if, stress_net_domain(udp_domain));
			udp_if = NULL;
		}
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);
again:
	pid = fork();
	if (pid < 0) {
		if (keep_stressing_flag() && (errno == EAGAIN))
			goto again;
		pr_fail("%s: fork failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_FAILURE;
	} else if (pid == 0) {
		rc = stress_udp_client(args, mypid, udp_domain, udp_proto, udp_port, udp_gro, udp_if);

		/* Inform parent we're all done */
		(void)kill(getppid(), SIGALRM);
		_exit(rc);
	} else {
		int status;

		rc = stress_udp_server(args, mypid, udp_domain, udp_proto, udp_port, udp_gro, udp_if);
		(void)kill(pid, SIGKILL);
		(void)shim_waitpid(pid, &status, 0);
	}
	return rc;
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_udp_domain,	stress_set_udp_domain },
	{ OPT_udp_port,		stress_set_udp_port },
	{ OPT_udp_lite,		stress_set_udp_lite },
	{ OPT_udp_gro,		stress_set_udp_gro },
	{ OPT_udp_if,		stress_set_udp_if },
	{ 0,			NULL }
};

stressor_info_t stress_udp_info = {
	.stressor = stress_udp,
	.class = CLASS_NETWORK | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.help = help
};
