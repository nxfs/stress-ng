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

static const stress_help_t help[] = {
	{ "p N", "pipe N",		"start N workers exercising pipe I/O" },
	{ NULL,	"pipe-data-size N",	"set pipe size of each pipe write to N bytes" },
	{ NULL,	"pipe-ops N",		"stop after N pipe I/O bogo operations" },
#if defined(F_SETPIPE_SZ)
	{ NULL,	"pipe-size N",		"set pipe size to N bytes" },
#endif
	{ NULL,	NULL,			NULL }
};

#if defined(F_SETPIPE_SZ)
/*
 *  stress_set_pipe_size()
 *	set pipe size in bytes
 */
static int stress_set_pipe_size(const char *opt)
{
	size_t pipe_size;

	pipe_size = (size_t)stress_get_uint64_byte(opt);
	stress_check_range_bytes("pipe-size", pipe_size, 4096, 1024 * 1024);
	return stress_set_setting("pipe-size", TYPE_ID_SIZE_T, &pipe_size);
}
#endif

/*
 *  stress_set_pipe_size()
 *	set pipe data write size in bytes
 */
static int stress_set_pipe_data_size(const char *opt)
{
	size_t pipe_data_size;

	pipe_data_size = (size_t)stress_get_uint64_byte(opt);
	stress_check_range_bytes("pipe-data-size", pipe_data_size,
		8, stress_get_page_size());
	return stress_set_setting("pipe-data-size", TYPE_ID_SIZE_T, &pipe_data_size);
}

#if defined(F_SETPIPE_SZ)
/*
 *  pipe_change_size()
 *	see if we can change the pipe size
 */
static void pipe_change_size(
	const stress_args_t *args,
	const int fd,
	const size_t pipe_size)
{
#if defined(F_GETPIPE_SZ)
	ssize_t sz;
#endif
	if (!pipe_size)
		return;

#if !(defined(HAVE_PIPE2) &&	\
    defined(O_DIRECT))
	if (pipe_size < args->page_size)
		return;
#endif
	if (fcntl(fd, F_SETPIPE_SZ, pipe_size) < 0) {
		pr_err("%s: cannot set pipe size, keeping "
			"default pipe size, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
	}
#if defined(F_GETPIPE_SZ)
	/* Sanity check size */
	if ((sz = fcntl(fd, F_GETPIPE_SZ)) < 0) {
		pr_err("%s: cannot get pipe size, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
	} else {
		if ((size_t)sz != pipe_size) {
			pr_err("%s: cannot set desired pipe size, "
				"pipe size=%zd, errno=%d (%s)\n",
				args->name, sz, errno, strerror(errno));
		}
	}
#endif
}
#endif

/*
 *  stress_pipe
 *	stress by heavy pipe I/O
 */
static int stress_pipe(const stress_args_t *args)
{
	pid_t pid;
	int pipefds[2];
	size_t pipe_data_size = 512;
	char *buf;
	uint32_t *buf32, val = stress_mwc32();
	double duration = 0.0, bytes = 0.0, rate;

	if (stress_sig_stop_stressing(args->name, SIGPIPE) < 0)
		return EXIT_FAILURE;

	(void)stress_get_setting("pipe-data-size", &pipe_data_size);

	buf = (char *)mmap(NULL, pipe_data_size, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (buf == MAP_FAILED) {
		pr_inf_skip("%s: failed to mmap %zu byte buffer, skipping stressor\n",
			args->name, pipe_data_size);
		return EXIT_NO_RESOURCE;
	}
	buf32 = (uint32_t *)buf;
	stress_rndbuf(buf, pipe_data_size);

	(void)shim_memset(pipefds, 0, sizeof(pipefds));

#if defined(HAVE_PIPE2) &&	\
    defined(O_DIRECT)
	if (pipe2(pipefds, O_DIRECT) < 0) {
		/*
		 *  Failed, fall back to standard pipe
		 */
		if (pipe(pipefds) < 0) {
			pr_fail("%s: pipe failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			(void)munmap((void *)buf, pipe_data_size);
			return EXIT_FAILURE;
		}
	}
#else
	if (pipe(pipefds) < 0) {
		pr_fail("%s: pipe failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		(void)munmap((void *)buf, pipe_data_size);
		return EXIT_FAILURE;
	}
#endif

#if defined(F_SETPIPE_SZ)
	{
		size_t pipe_size = 0;

		(void)stress_get_setting("pipe-size", &pipe_size);
		pipe_change_size(args, pipefds[0], pipe_size);
		pipe_change_size(args, pipefds[1], pipe_size);
	}
#else
	UNEXPECTED
#endif

	stress_set_proc_state(args->name, STRESS_STATE_RUN);
again:
	pid = fork();
	if (pid < 0) {
		if (stress_redo_fork(errno))
			goto again;
		(void)close(pipefds[0]);
		(void)close(pipefds[1]);
		(void)munmap((void *)buf, pipe_data_size);
		if (!keep_stressing(args))
			goto finish;
		pr_fail("%s: fork failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_FAILURE;
	} else if (pid == 0) {
#if defined(FIONREAD)
		int i = 0;
#endif
		const pid_t my_pid = getpid();
		const bool verify = !!(g_opt_flags & OPT_FLAGS_VERIFY);
		register const int fd = pipefds[0];

		stress_parent_died_alarm();
		(void)sched_settings_apply(true);

		(void)stress_read_fdinfo(my_pid, pipefds[0]);

		(void)close(pipefds[1]);
		while (keep_stressing_flag()) {
			ssize_t n;

			n = read(fd, buf, pipe_data_size);
			if (UNLIKELY(n <= 0)) {
				if (n == 0)
					break;
				if ((errno == EAGAIN) || (errno == EINTR))
					continue;
				if (errno == EPIPE)
					break;
				if (errno) {
					pr_fail("%s: read failed, errno=%d (%s)\n",
						args->name, errno, strerror(errno));
					break;
				}
				pr_fail("%s: zero bytes read\n", args->name);
				break;
			}

#if defined(FIONREAD)
			/* Occasionally exercise FIONREAD on read end */
			if (UNLIKELY((i++ & 0x1ff) == 0)) {
				int readbytes;

				VOID_RET(int, ioctl(pipefds[0], FIONREAD, &readbytes));
			}
#endif
			if (UNLIKELY(verify)) {
				if (UNLIKELY(*buf32 != val)) {
					pr_fail("%s: pipe read error detected, "
						"failed to read expected data\n", args->name);
				}
				val++;
			}
		}
		(void)close(pipefds[0]);
		(void)munmap((void *)buf, pipe_data_size);
		_exit(EXIT_SUCCESS);
	} else {
		int status;
		double t;
		register const int fd = pipefds[1];

		/* Parent */
		(void)close(pipefds[0]);

		t = stress_time_now();
		do {
			register ssize_t ret;

			*buf32 = val++;
			ret = write(fd, buf, pipe_data_size);
			if (UNLIKELY(ret <= 0)) {
				if ((errno == EAGAIN) || (errno == EINTR))
					continue;
				if (errno == EPIPE)
					break;
				if (errno) {
					pr_fail("%s: write failed, errno=%d (%s)\n",
						args->name, errno, strerror(errno));
					break;
				}
				continue;
			} else {
				bytes += (double)ret;
			}
			inc_counter(args);
		} while (keep_stressing(args));
		duration = stress_time_now() - t;
		rate = (duration > 0.0) ? (bytes / duration) / (double)MB : 0.0;
		stress_metrics_set(args, 0, "MB per sec pipe write rate", rate);

		(void)close(pipefds[1]);
		(void)kill(pid, SIGPIPE);
		(void)shim_waitpid(pid, &status, 0);
		(void)munmap((void *)buf, pipe_data_size);
	}
finish:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	return EXIT_SUCCESS;
}

static const stress_opt_set_func_t opt_set_funcs[] = {
#if defined(F_SETPIPE_SZ)
	{ OPT_pipe_size,	stress_set_pipe_size },
#endif
	{ OPT_pipe_data_size,	stress_set_pipe_data_size },
	{ 0,			NULL }
};

stressor_info_t stress_pipe_info = {
	.stressor = stress_pipe,
	.class = CLASS_PIPE_IO | CLASS_MEMORY | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.verify = VERIFY_OPTIONAL,
	.help = help
};
