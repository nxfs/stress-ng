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
	{ NULL,	"flock N",	"start N workers locking a single file" },
	{ NULL,	"flock-ops N",	"stop after N flock bogo operations" },
	{ NULL,	NULL,		NULL }
};

#if defined(HAVE_FLOCK) &&	\
    defined(LOCK_EX) &&		\
    defined(LOCK_UN)

#define MAX_FLOCK_STRESSORS	(3)

static void stress_flock_child(
	const stress_args_t *args,
	const int fd,
	const int bad_fd,
	const bool save_metrics)
{
	bool cont;
	int i;
	double lock_duration = 0.0, lock_count = 0.0;
	double unlock_duration = 0.0, unlock_count = 0.0;
	double rate;

	for (i = 0; ; i++) {
		double t;

#if defined(LOCK_EX)
		t = stress_time_now();
		if (flock(fd, LOCK_EX) == 0) {
			lock_duration += stress_time_now() - t;
			lock_count += 1.0;

			cont = keep_stressing(args);
			if (cont)
				inc_counter(args);

			t = stress_time_now();
			if (flock(fd, LOCK_UN) == 0) {
				unlock_duration += stress_time_now() - t;
				unlock_count += 1.0;
			}
			if (!cont)
				break;
		}
#else
		UNEXPECTED
#endif

		/*
		 *  Exercise flock with invalid fd
		 */
		(void)flock(bad_fd, LOCK_EX);
		(void)flock(bad_fd, LOCK_UN);

#if defined(LOCK_NB)
		t = stress_time_now();
		if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
			lock_duration += stress_time_now() - t;
			lock_count += 1.0;

			cont = keep_stressing(args);
			if (cont)
				inc_counter(args);

			t = stress_time_now();
			if (flock(fd, LOCK_UN) == 0) {
				unlock_duration += stress_time_now() - t;
				unlock_count += 1.0;
			}
			if (!cont)
				break;
		}

		/*
		 *  Exercise flock with invalid operation
		 */
		{
			int ret;

			ret = flock(fd, LOCK_NB);
			if (ret == 0) {
				pr_fail("%s: flock failed expected EINVAL, instead got "
					"errno=%d (%s)\n", args->name, errno, strerror(errno));
				(void)flock(fd, LOCK_UN);
			}
		}
#else
		UNEXPECTED
#endif

#if defined(LOCK_SH)
		if (!keep_stressing(args))
			break;

		t = stress_time_now();
		if (flock(fd, LOCK_SH) == 0) {
			lock_duration += stress_time_now() - t;
			lock_count += 1.0;

			cont = keep_stressing(args);
			if (cont)
				inc_counter(args);

			t = stress_time_now();
			if (flock(fd, LOCK_UN) == 0) {
				unlock_duration += stress_time_now() - t;
				unlock_count += 1.0;
			}
			if (!cont)
				break;
		}
#else
		UNEXPECTED
#endif

#if defined(LOCK_SH) &&		\
    defined(LOCK_NB)
		if (!keep_stressing(args))
			break;

		t = stress_time_now();
		if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
			lock_duration += stress_time_now() - t;
			lock_count += 1.0;

			cont = keep_stressing(args);
			if (cont)
				inc_counter(args);

			t = stress_time_now();
			if (flock(fd, LOCK_UN) == 0) {
				unlock_duration += stress_time_now() - t;
				unlock_count += 1.0;
			}
			if (!cont)
				break;
		}
#else
		UNEXPECTED
#endif

#if defined(LOCK_MAND) &&	\
    defined(LOCK_READ)
		if (!keep_stressing(args))
			break;

		t = stress_time_now();
		if (flock(fd, LOCK_MAND | LOCK_READ) == 0) {
			lock_duration += stress_time_now() - t;
			lock_count += 1.0;

			cont = keep_stressing(args);
			if (cont)
				inc_counter(args);

			t = stress_time_now();
			if (flock(fd, LOCK_UN) == 0) {
				unlock_duration += stress_time_now() - t;
				unlock_count += 1.0;
			}
			if (!cont)
				break;
		}
#else
		UNEXPECTED
#endif

#if defined(LOCK_MAND) &&	\
    defined(LOCK_WRITE)
		if (!keep_stressing(args))
			break;

		t = stress_time_now();
		if (flock(fd, LOCK_MAND | LOCK_WRITE) == 0) {
			lock_duration += stress_time_now() - t;
			lock_count += 1.0;

			cont = keep_stressing(args);
			if (cont)
				inc_counter(args);

			t = stress_time_now();
			if (flock(fd, LOCK_UN) == 0) {
				unlock_duration += stress_time_now() - t;
				unlock_count += 1.0;
			}
			if (!cont)
				break;
		}
#else
		UNEXPECTED
#endif

#if defined(LOCK_EX) &&		\
    defined(LOCK_SH)
		if (!keep_stressing(args))
			break;

		/* Exercise invalid lock combination */
		t = stress_time_now();
		if (flock(fd, LOCK_EX | LOCK_SH) == 0) {
			lock_duration += stress_time_now() - t;
			lock_count += 1.0;

			cont = keep_stressing(args);
			if (cont)
				inc_counter(args);

			t = stress_time_now();
			if (flock(fd, LOCK_UN) == 0) {
				unlock_duration += stress_time_now() - t;
				unlock_count += 1.0;
			}
			if (!cont)
				break;
		}
#else
		UNEXPECTED
#endif
#if defined(__linux__)
		if ((i & 0xff) == 0) {
			char buf[4096];

			VOID_RET(ssize_t, system_read("/proc/locks", buf, sizeof(buf)));
		}
#endif
	}
	if (save_metrics) {
		rate = (lock_count > 0.0) ? lock_duration / lock_count : 0.0;
		stress_metrics_set(args, 0, "nanosecs per flock lock call", rate * STRESS_DBL_NANOSECOND);
		rate = (unlock_count > 0.0) ? unlock_duration / unlock_count : 0.0;
		stress_metrics_set(args, 1, "nanosecs per flock unlock call", rate * STRESS_DBL_NANOSECOND);
	}
}

/*
 *  stress_flock
 *	stress file locking
 */
static int stress_flock(const stress_args_t *args)
{
	int fd, ret, rc = EXIT_FAILURE;
	const int bad_fd = stress_get_bad_fd();
	size_t i;
	pid_t pids[MAX_FLOCK_STRESSORS];
	char filename[PATH_MAX];

	ret = stress_temp_dir_mk_args(args);
	if (ret < 0)
		return stress_exit_status(-ret);

	(void)stress_temp_filename_args(args,
		filename, sizeof(filename), stress_mwc32());
	fd = open(filename, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		pr_err("%s: failed to create %s: errno=%d (%s)\n",
			args->name, filename, errno, strerror(errno));
		goto err;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	(void)shim_memset(pids, 0, sizeof(pids));
	for (i = 0; i < MAX_FLOCK_STRESSORS; i++) {
		pids[i] = fork();
		if (pids[i] < 0) {
			goto reap;
		} else if (pids[i] == 0) {
			stress_parent_died_alarm();
			(void)sched_settings_apply(true);

			stress_flock_child(args, fd, bad_fd, false);
			_exit(EXIT_SUCCESS);
		}
	}

	stress_flock_child(args, fd, bad_fd, true);
	rc = EXIT_SUCCESS;
reap:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	(void)close(fd);
	stress_kill_and_wait_many(args, pids, MAX_FLOCK_STRESSORS, SIGALRM, true);
	(void)shim_unlink(filename);
err:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	(void)stress_temp_dir_rm_args(args);

	return rc;
}

stressor_info_t stress_flock_info = {
	.stressor = stress_flock,
	.class = CLASS_FILESYSTEM | CLASS_OS,
	.help = help
};
#else
stressor_info_t stress_flock_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_FILESYSTEM | CLASS_OS,
	.help = help,
	.unimplemented_reason = "built without flock() or LOCK_EX/LOCK_UN support"
};
#endif
