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

static const stress_help_t help[] = {
	{ NULL,	"mlock N",	"start N workers exercising mlock/munlock" },
	{ NULL,	"mlock-ops N",	"stop after N mlock bogo operations" },
	{ NULL,	NULL,		NULL }
};

#if defined(_POSIX_MEMLOCK_RANGE) &&	\
    defined(HAVE_MLOCK)

#define MLOCK_MAX	(256*1024)

#if defined(HAVE_MLOCK2)

#ifndef MLOCK_ONFAULT
#define MLOCK_ONFAULT 1
#endif

/*
 *  do_mlock()
 *	if mlock2 is available, randomly exercise this
 *	or mlock.  If not available, just fallback to
 *	mlock.  Also, pick random mlock2 flags
 */
static int do_mlock(
	const void *addr,
	size_t len,
	double *duration,
	double *count)
{
	static bool use_mlock2 = true;
	int ret;
	static int metrics_count = 0;

	if (LIKELY(metrics_count++ < 1000)) {
		/* faster non-metrics timed mlock operations */
		if (use_mlock2) {
			const uint32_t rnd = stress_mwc32() >> 5;

			/* Randomly use mlock2 or mlock */
			if (rnd & 1) {
				const int flags = (rnd & 2) ?
					0 : MLOCK_ONFAULT;

				ret = shim_mlock2(addr, len, flags);
				if (ret == 0)
					return 0;
				if (errno != ENOSYS)
					return ret;

				/* mlock2 not supported... */
				use_mlock2 = false;
			}
		}

		/* Just do mlock */
		ret = shim_mlock((const void *)addr, len);
	} else {
		double t;

		/* slower metrics timed mlock operations */
		if (use_mlock2) {
			const uint32_t rnd = stress_mwc32() >> 5;

			/* Randomly use mlock2 or mlock */
			if (rnd & 1) {
				const int flags = (rnd & 2) ?
					0 : MLOCK_ONFAULT;

				t = stress_time_now();
				ret = shim_mlock2(addr, len, flags);
				if (ret == 0) {
					(*duration) += stress_time_now() - t;
					(*count) += 1.0;
					return 0;
				}
				if (errno != ENOSYS)
					return ret;

				/* mlock2 not supported... */
				use_mlock2 = false;
			}
		}

		/* Just do mlock */
		t = stress_time_now();
		ret = shim_mlock((const void *)addr, len);
		if (ret == 0) {
			(*duration) += stress_time_now() - t;
			(*count) += 1.0;
		}
	}
	return ret;
}
#else
static inline int do_mlock(
	const void *addr,
	size_t len,
	double *duration,
	double *count)
{
	int ret;
	static int metrics_count = 0;

	if (LIKELY(metrics_count++ < 1000)) {
		/* faster non-metrics timed mlock operations */
		ret = shim_mlock((const void *)addr, len);
	} else {
		double t;

		/* slower metrics timed mlock operations */
		t = stress_time_now();
		ret = shim_mlock((const void *)addr, len);
		if (ret == 0) {
			(*duration) += stress_time_now() - t;
			(*count) += 1.0;
		}
	}
	return ret;
}
#endif

/*
 *  stress_mlock_max_lockable()
 *	find maximum mlockable region size
 */
static size_t stress_mlock_max_lockable(void)
{
	size_t sysconf_max = 0;
	size_t rlimit_max = 0;
	size_t max = MLOCK_MAX;

#if defined(_SC_MEMLOCK)
	{
		const long lockmax = sysconf(_SC_MEMLOCK);

		sysconf_max = (lockmax > 0) ? (size_t)lockmax : MLOCK_MAX;
	}
#endif
#if defined(RLIMIT_MEMLOCK)
	{
		struct rlimit rlim;

		if (getrlimit(RLIMIT_MEMLOCK, &rlim) == 0)
			rlimit_max = (size_t)rlim.rlim_max;
	}
#endif
	max = STRESS_MAXIMUM(max, sysconf_max);
	max = STRESS_MAXIMUM(max, rlimit_max);

	return max;
}

/*
 *  stress_mlock_misc()
 *	perform various invalid or unusual calls to
 *	exercise kernel a little more.
 */
static void stress_mlock_misc(const stress_args_t *args, const size_t page_size)
{
	(void)args;

	/*
	 *  mlock/munlock with invalid or unusual arguments
	 */
	VOID_RET(int, shim_mlock((void *)~0, page_size));
	VOID_RET(int, shim_munlock((void *)~0, page_size));

	VOID_RET(int, shim_mlock((void *)(~(uintptr_t)0 & ~(page_size - 1)), page_size << 1));
	VOID_RET(int, shim_munlock((void *)(~(uintptr_t)0 & ~(page_size - 1)), page_size << 1));

	VOID_RET(int, shim_mlock((void *)0, ~(size_t)0));
	VOID_RET(int, munlock((void *)0, ~(size_t)0));

	VOID_RET(int, shim_mlock((void *)0, 0));
	VOID_RET(int, munlock((void *)0, 0));

#if defined(HAVE_MLOCKALL)
	{
		int flag = 0;
#if defined(MCL_CURRENT)
		if (!keep_stressing(args))
			return;
		(void)shim_mlockall(MCL_CURRENT);
		flag |= MCL_CURRENT;
#endif
#if defined(MCL_FUTURE)
		if (!keep_stressing(args))
			return;
		(void)shim_mlockall(MCL_FUTURE);
		flag |= MCL_FUTURE;
#endif
#if defined(MCL_ONFAULT) &&	\
    defined(MCL_CURRENT)
		if (!keep_stressing(args))
			return;
		if (shim_mlockall(MCL_ONFAULT | MCL_CURRENT) == 0)
			flag |= (MCL_ONFAULT | MCL_CURRENT);
#endif
#if defined(MCL_ONFAULT) &&	\
    defined(MCL_FUTURE)
		if (!keep_stressing(args))
			return;
		if (shim_mlockall(MCL_ONFAULT | MCL_FUTURE) == 0)
			flag |= (MCL_ONFAULT | MCL_FUTURE);
#endif
#if defined(MCL_ONFAULT)
		if (!keep_stressing(args))
			return;
		/* Exercising Invalid mlockall syscall and ignoring failure */
		(void)shim_mlockall(MCL_ONFAULT);
#endif
		if (!keep_stressing(args))
			return;
		/* Exercise Invalid mlockall syscall with invalid flag */
		(void)shim_mlockall(~0);
		if (flag) { /* cppcheck-suppress knownConditionTrueFalse */
			if (!keep_stressing(args))
				return;
			(void)shim_mlockall(flag);
		}
	}
#endif
}

static int stress_mlock_child(const stress_args_t *args, void *context)
{
	size_t i, n;
	uint8_t **mappings;
	const size_t page_size = args->page_size;
	const size_t max = stress_mlock_max_lockable();
	const size_t mappings_len = max * sizeof(*mappings);
	size_t shmall, freemem, totalmem, freeswap, totalswap;
	double mlock_duration = 0.0, mlock_count = 0.0;
	double munlock_duration = 0.0, munlock_count = 0.0;
	double rate;

	stress_get_memlimits(&shmall, &freemem, &totalmem, &freeswap, &totalswap);

	(void)context;

	/*
	 *  In low-memory scenarios we should check if we should
	 *  keep stressing before attempting a calloc that can
	 *  for a OOM and a respawn if this function
	 */
	if (!keep_stressing(args))
		return EXIT_SUCCESS;

	mappings = (uint8_t **)mmap(NULL, mappings_len, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mappings == MAP_FAILED) {
		pr_fail("%s: cannot mmap mmapings table: %d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		int ret;

		for (n = 0; n < max; n++) {
			if (!keep_stressing(args))
				break;

			/* Low memory avoidance, re-start */
			if ((g_opt_flags & OPT_FLAGS_OOM_AVOID) && stress_low_memory(page_size * 3))
				break;

			mappings[n] = (uint8_t *)mmap(NULL, page_size * 3,
				PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_ANONYMOUS, -1, 0);
			if (mappings[n] == MAP_FAILED)
				break;

#if defined(HAVE_MLOCK2)
			if (!keep_stressing(args))
				break;
			/* Invalid mlock2 syscall with invalid flags and ignoring failure*/
			(void)shim_mlock2((void *)(mappings[n] + page_size), page_size, ~0);
#endif

			/*
			 *  Attempt a bogus mlock, ignore failure
			 */
			if (!keep_stressing(args))
				break;
			(void)do_mlock((void *)(mappings[n] + page_size), 0, &mlock_duration, &mlock_count);

			/*
			 *  Attempt a correct mlock
			 */
			if (!keep_stressing(args))
				break;
			ret = do_mlock((void *)(mappings[n] + page_size), page_size, &mlock_duration, &mlock_count);
			if (ret < 0) {
				if (errno == EAGAIN)
					continue;
				if ((errno == ENOMEM) || (errno == EPERM))
					break;
				pr_fail("%s: mlock failed, errno=%d (%s)\n",
					args->name, errno, strerror(errno));
				break;
			} else {
				/*
				 * Mappings are always page aligned so
				 * we can use the bottom bit to
				 * indicate if the page has been
				 * mlocked or not
				 */
				mappings[n] = (uint8_t *)
					((intptr_t)mappings[n] | 1);
				inc_counter(args);
			}

			if ((n & 1023) == 0)
				stress_mlock_misc(args, page_size);
		}

		for (i = 0; i < n; i++) {
			intptr_t addr = (intptr_t)mappings[i];
			intptr_t mlocked = addr & 1;

			if (keep_stressing(args)) {
				addr ^= mlocked;
				if (mlocked) {
					double t;

					t = stress_time_now();
					ret = shim_munlock((void *)((uint8_t *)addr + page_size), page_size);
					if (ret == 0) {
						munlock_duration += stress_time_now() - t;
						munlock_count += 1.0;
					}
				}
				/*
				 *  Attempt a bogus munlock, ignore failure
				 */
				(void)shim_munlock((void *)((uint8_t *)addr + page_size), 0);
			}
			(void)munmap((void *)addr, page_size * 3);
		}

		for (n = 0; n < max; n++) {
			if (!keep_stressing(args))
				break;

			mappings[n] = (uint8_t *)mmap(NULL, page_size,
				PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_ANONYMOUS, -1, 0);
			if (mappings[n] == MAP_FAILED)
				break;
		}
#if defined(HAVE_MUNLOCKALL)
		(void)shim_munlockall();
#endif
		for (i = 0; i < n; i++)
			(void)munmap((void *)mappings[i], page_size);
	} while (keep_stressing(args));

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	rate = (mlock_count > 0.0) ? mlock_duration / mlock_count : 0.0;
	stress_metrics_set(args, 0, "nanosecs per mlock call", rate * STRESS_DBL_NANOSECOND);
	rate = (munlock_count > 0.0) ? munlock_duration / munlock_count : 0.0;
	stress_metrics_set(args, 1, "nanosecs per munlock call", rate * STRESS_DBL_NANOSECOND);

	(void)munmap((void *)mappings, mappings_len);

	return EXIT_SUCCESS;
}

/*
 *  stress_mlock()
 *	stress mlock with pages being locked/unlocked
 */
static int stress_mlock(const stress_args_t *args)
{
	return stress_oomable_child(args, NULL, stress_mlock_child, STRESS_OOMABLE_NORMAL);
}

stressor_info_t stress_mlock_info = {
	.stressor = stress_mlock,
	.class = CLASS_VM | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.help = help
};
#else
stressor_info_t stress_mlock_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_VM | CLASS_OS,
	.help = help,
	.unimplemented_reason = "built without mlock() support or _POSIX_MEMLOCK_RANGE defined"
};
#endif
