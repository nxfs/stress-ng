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

#if defined(__NR_userfaultfd)
#define HAVE_USERFAULTFD
#endif

#if defined(HAVE_LINUX_USERFAULTFD_H)
#include <linux/userfaultfd.h>
#endif

#if defined(HAVE_POLL_H)
#include <poll.h>
#else
UNEXPECTED
#endif

#define MIN_USERFAULT_BYTES	(4 * KB)
#define MAX_USERFAULT_BYTES	(MAX_MEM_LIMIT)
#define DEFAULT_USERFAULT_BYTES	(256 * MB)

static const stress_help_t help[] = {
	{ NULL,	"userfaultfd N",	"start N page faulting workers with userspace handling" },
	{ NULL,	"userfaultfd-ops N",	"stop after N page faults have been handled" },
	{ NULL,	NULL,			NULL }
};

#if defined(HAVE_USERFAULTFD) && 	 \
    defined(HAVE_LINUX_USERFAULTFD_H) && \
    defined(HAVE_POLL_H) &&		 \
    defined(HAVE_CLONE)

#define STACK_SIZE	(64 * 1024)
#define COUNT_MAX	(256)

/* Context for clone */
typedef struct {
	const stress_args_t *args;
	uint8_t *data;
	size_t page_size;
	size_t sz;
	pid_t parent;
} stress_context_t;

#endif

static int stress_set_userfaultfd_bytes(const char *opt)
{
	size_t userfaultfd_bytes;

	userfaultfd_bytes = (size_t)stress_get_uint64_byte_memory(opt, 1);
	stress_check_range_bytes("userfaultfd-bytes", userfaultfd_bytes,
		MIN_USERFAULT_BYTES, MAX_USERFAULT_BYTES);
	return stress_set_setting("userfaultfd-bytes", TYPE_ID_SIZE_T, &userfaultfd_bytes);
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_userfaultfd_bytes,	stress_set_userfaultfd_bytes },
	{ 0,				NULL }
};

#if defined(HAVE_USERFAULTFD) && 		\
    defined(HAVE_LINUX_USERFAULTFD_H) && 	\
    defined(HAVE_CLONE) &&			\
    defined(HAVE_POSIX_MEMALIGN)

#define STRESS_USERFAULT_REPORT_ALWAYS		(0x01)
#define STRESS_USERFAULT_SUPPORTED_CHECK	(0x02)
#define STRESS_USERFAULT_SUPPORTED_CHECK_ALWAYS	(STRESS_USERFAULT_REPORT_ALWAYS |	\
						 STRESS_USERFAULT_SUPPORTED_CHECK)

/*
 *  stress_userfaultfd_error()
 *	convert errno into stress-ng return error code and report
 *	a message if carp is true
 */
static int stress_userfaultfd_error(const char *name, const int err, const int mode)
{
	int rc;
	static const char skipped[] = "stressor will be skipped";

	switch (err) {
	case EPERM:
		if (mode & STRESS_USERFAULT_REPORT_ALWAYS)
			pr_inf_skip("%s: %s, insufficient privilege\n",
				name, skipped);
		rc = EXIT_NO_RESOURCE;
		break;
	case ENOSYS:
		if (mode & STRESS_USERFAULT_REPORT_ALWAYS)
			pr_inf_skip("%s: %s, userfaultfd() not supported\n",
				name, skipped);
		rc = EXIT_NOT_IMPLEMENTED;
		break;
	default:
		rc = stress_exit_status(errno);
		if (mode & STRESS_USERFAULT_REPORT_ALWAYS) {
			if (mode & STRESS_USERFAULT_SUPPORTED_CHECK) {
				pr_inf_skip("%s: %s, userfaultfd() failed, errno = %d (%s)\n",
					name, skipped, errno, strerror(errno));
				rc = EXIT_NO_RESOURCE;
			} else {
				pr_fail("%s: userfaultfd() failed, errno = %d (%s)\n",
					name, errno, strerror(errno));
			}
		}
		break;
	}
	return rc;
}


static int stress_userfaultfd_supported(const char *name)
{
	int fd;

	fd = shim_userfaultfd(0);
	if (fd >= 0) {
		(void)close(fd);
		return 0;
	}
	stress_userfaultfd_error(name, errno, STRESS_USERFAULT_SUPPORTED_CHECK_ALWAYS);
	return -1;
}

/*
 *  stress_child_alarm_handler()
 *	SIGALRM handler to terminate child immediately
 */
static void MLOCKED_TEXT NORETURN stress_child_alarm_handler(int signum)
{
	(void)signum;

	_exit(0);
}

/*
 *  stress_userfaultfd_clone()
 *	generate page faults for parent to handle
 */
static int stress_userfaultfd_clone(void *arg)
{
	stress_context_t *c = (stress_context_t *)arg;
	const stress_args_t *args = c->args;

	stress_parent_died_alarm();
	(void)sched_settings_apply(true);

	if (stress_sighandler(args->name, SIGALRM, stress_child_alarm_handler, NULL) < 0)
		return EXIT_NO_RESOURCE;

	do {
		uint8_t *ptr, *end = c->data + c->sz;

		/* hint we don't need these pages */
		if (shim_madvise(c->data, c->sz, MADV_DONTNEED) < 0) {
			pr_fail("%s: madvise failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			(void)kill(c->parent, SIGALRM);
			return -1;
		}
		/* and trigger some page faults */
		for (ptr = c->data; ptr < end; ptr += c->page_size)
			*ptr = 0xff;
	} while (keep_stressing(args));

	return 0;
}

/*
 *  handle_page_fault()
 *	handle a write page fault caused by child
 */
static inline int handle_page_fault(
	const stress_args_t *args,
	const int fd,
	uint8_t *addr,
	void *zero_page,
	const uint8_t *data_start,
	const uint8_t *data_end,
	const size_t page_size)
{
	if ((addr < data_start) || (addr >= data_end)) {
		pr_fail("%s: page fault address is out of range\n", args->name);
		return -1;
	}

	if (stress_mwc32() & 1) {
		struct uffdio_copy copy;

		copy.copy = 0;
		copy.mode = 0;
		copy.dst = (unsigned long)addr;
		copy.src = (unsigned long)zero_page;
		copy.len = page_size;

		if (ioctl(fd, UFFDIO_COPY, &copy) < 0) {
			pr_fail("%s: page fault ioctl UFFDIO_COPY failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			return -1;
		}
	} else {
		struct uffdio_zeropage zeropage;

		zeropage.range.start = (unsigned long)addr;
		zeropage.range.len = page_size;
		zeropage.mode = 0;
		if (ioctl(fd, UFFDIO_ZEROPAGE, &zeropage) < 0) {
			pr_fail("%s: page fault ioctl UFFDIO_ZEROPAGE failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			return -1;
		}
	}
	return 0;
}

/*
 *  stress_userfaultfd_oomable()
 *	stress userfaultfd system call, this
 *	is an OOM-able child process that the
 *	parent can restart
 */
static int stress_userfaultfd_child(const stress_args_t *args, void *context)
{
	const size_t page_size = args->page_size;
	size_t sz;
	uint8_t *data;
	void *zero_page = NULL;
	int fd = -1, rc = EXIT_SUCCESS, count = 0;
	const unsigned int uffdio_copy = 1 << _UFFDIO_COPY;
	const unsigned int uffdio_zeropage = 1 << _UFFDIO_ZEROPAGE;
	pid_t pid;
	const pid_t self = getpid();
	struct uffdio_api api;
	struct uffdio_register reg;
	stress_context_t c;
	bool do_poll = true;
	static uint8_t stack[STACK_SIZE]; /* Child clone stack */
	uint8_t *stack_top = (uint8_t *)stress_get_stack_top((void *)stack, STACK_SIZE);
	size_t userfaultfd_bytes = DEFAULT_USERFAULT_BYTES;
	double t, duration = 0.0, rate;

	(void)context;

	if (!stress_get_setting("userfaultfd-bytes", &userfaultfd_bytes)) {
		if (g_opt_flags & OPT_FLAGS_MAXIMIZE)
			userfaultfd_bytes = MAX_32;
		if (g_opt_flags & OPT_FLAGS_MINIMIZE)
			userfaultfd_bytes = MIN_USERFAULT_BYTES;
	}
	userfaultfd_bytes /= args->num_instances;
	if (userfaultfd_bytes < MIN_USERFAULT_BYTES)
		userfaultfd_bytes = MIN_USERFAULT_BYTES;
	if (userfaultfd_bytes < args->page_size)
		userfaultfd_bytes = args->page_size;

	sz = userfaultfd_bytes & ~(page_size - 1);

	if (posix_memalign(&zero_page, page_size, page_size)) {
		pr_err("%s: zero page allocation failed\n", args->name);
		return EXIT_NO_RESOURCE;
	}

	data = mmap(NULL, sz, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (data == MAP_FAILED) {
		rc = EXIT_NO_RESOURCE;
		pr_err("%s: mmap failed\n", args->name);
		goto free_zeropage;
	}

	/* Exercise invalid flags */
	fd = shim_userfaultfd(~0);
	if (fd >= 0)
		(void)close(fd);

	/* Get userfault fd */
	fd = shim_userfaultfd(0);
	if (fd < 0) {
		rc = stress_userfaultfd_error(args->name, errno,
			args->instance ? 0 : STRESS_USERFAULT_REPORT_ALWAYS);
		goto unmap_data;
	}

	if (stress_set_nonblock(fd) < 0)
		do_poll = false;

	/* API sanity check */
	(void)shim_memset(&api, 0, sizeof(api));
	api.api = UFFD_API;
	api.features = 0;
	if (ioctl(fd, UFFDIO_API, &api) < 0) {
		pr_fail("%s: ioctl UFFDIO_API failed, errno = %d (%s)\n",
			args->name, errno, strerror(errno));
		rc = EXIT_FAILURE;
		goto unmap_data;
	}
	if (api.api != UFFD_API) {
		pr_fail("%s: ioctl UFFDIO_API API check failed\n",
			args->name);
		rc = EXIT_FAILURE;
		goto unmap_data;
	}

	/* Register fault handling mode */
	(void)shim_memset(&reg, 0, sizeof(reg));
	reg.range.start = (unsigned long)data;
	reg.range.len = sz;
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(fd, UFFDIO_REGISTER, &reg) < 0) {
		pr_fail("%s: ioctl UFFDIO_REGISTER failed, errno = %d (%s)\n",
			args->name, errno, strerror(errno));
		rc = EXIT_FAILURE;
		goto unmap_data;
	}

	/* OK, so do we have copy supported? */
	if ((reg.ioctls & uffdio_copy) != uffdio_copy) {
		pr_fail("%s: ioctl UFFDIO_REGISTER did not support _UFFDIO_COPY\n",
			args->name);
		rc = EXIT_FAILURE;
		goto unmap_data;
	}
	/* OK, so do we have zeropage supported? */
	if ((reg.ioctls & uffdio_zeropage) != uffdio_zeropage) {
		pr_fail("%s: ioctl UFFDIO_REGISTER did not support _UFFDIO_ZEROPAGE\n",
			args->name);
		rc = EXIT_FAILURE;
		goto unmap_data;
	}

	/* Set up context for child */
	c.args = args;
	c.data = data;
	c.sz = sz;
	c.page_size = page_size;
	c.parent = self;

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	/*
	 *  We need to clone and share the same VM address space
	 *  as parent so we can perform the page fault handling
	 */
	pid = clone(stress_userfaultfd_clone, stress_align_stack(stack_top),
		SIGCHLD | CLONE_FILES | CLONE_FS | CLONE_SIGHAND | CLONE_VM, &c);
	if (pid < 0) {
		pr_err("%s: fork failed, errno = %d (%s)\n",
			args->name, errno, strerror(errno));
		goto unreg;
	}

	/* Parent */
	do {
		struct uffd_msg msg;
		struct uffdio_range wake;
		ssize_t ret;
		double counter;

		/* check we should break out before we block on the read */
		if (!keep_stressing_flag())
			break;

		t = stress_time_now();
		/*
		 * polled wait exercises userfaultfd_poll
		 * in the kernel, but only works if fd is NONBLOCKing
		 */
		if (do_poll) {
			struct pollfd fds[1];

			(void)shim_memset(fds, 0, sizeof fds);
			fds[0].fd = fd;
			fds[0].events = POLLIN;
			/* wait for 1 second max */

			ret = poll(fds, 1, 1000);
			if (ret == 0)
				continue;	/* timed out, redo the poll */
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				if (errno != ENOMEM) {
					pr_fail("%s: poll failed, errno=%d (%s)\n",
						args->name, errno, strerror(errno));
					if (!keep_stressing_flag())
						break;
				}
				/*
				 *  poll ran out of free space for internal
				 *  fd tables, so give up and block on the
				 *  read anyway
				 */
				goto do_read;
			}
			/* No data, re-poll */
			if (!(fds[0].revents & POLLIN))
				continue;

			if (UNLIKELY(count++ >= COUNT_MAX)) {
				(void)stress_read_fdinfo(self, fd);
				count = 0;
			}
		}

do_read:
		ret = read(fd, &msg, sizeof(msg));
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			pr_fail("%s: read failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			if (!keep_stressing_flag())
				break;
			continue;
		}
		/* We only expect a page fault event */
		if (msg.event != UFFD_EVENT_PAGEFAULT) {
			pr_fail("%s: msg event not a pagefault event\n", args->name);
			continue;
		}
		/* We only expect a write fault */
		if (!(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE)) {
			pr_fail("%s: msg event not write page fault event\n", args->name);
			continue;
		}
		/* Go handle the page fault */
		if (handle_page_fault(args, fd, (uint8_t *)(intptr_t)msg.arg.pagefault.address,
				zero_page, data, data + sz, page_size) < 0)
			break;
		duration += stress_time_now() - t;
		inc_counter(args);
		counter = (double)get_counter(args);

		rate = (counter > 0.0) ? duration / counter : 0.0;
		stress_metrics_set(args, 0, "nanosecs per page fault", rate * STRESS_DBL_NANOSECOND);

		(void)shim_memset(&wake, 0, sizeof(wake));
		wake.start = (uintptr_t)data;
		wake.len = page_size;
		VOID_RET(int, ioctl(fd, UFFDIO_WAKE, &wake));

	} while (keep_stressing(args));

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	stress_kill_and_wait(args, pid, SIGALRM, false);
unreg:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	if (ioctl(fd, UFFDIO_UNREGISTER, &reg) < 0) {
		pr_fail("%s: ioctl UFFDIO_UNREGISTER failed, errno = %d (%s)\n",
			args->name, errno, strerror(errno));
		rc = EXIT_FAILURE;
		goto unmap_data;
	}
unmap_data:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	(void)munmap(data, sz);
free_zeropage:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	free(zero_page);
	if (fd > -1)
		(void)close(fd);

	return rc;
}

/*
 *  stress_userfaultfd()
 *	stress userfaultfd
 */
static int stress_userfaultfd(const stress_args_t *args)
{
	return stress_oomable_child(args, NULL, stress_userfaultfd_child, STRESS_OOMABLE_NORMAL);
}

stressor_info_t stress_userfaultfd_info = {
	.stressor = stress_userfaultfd,
	.class = CLASS_VM | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.supported = stress_userfaultfd_supported,
	.verify = VERIFY_ALWAYS,
	.help = help
};
#else
stressor_info_t stress_userfaultfd_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_VM | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.help = help,
	.unimplemented_reason = "built without linux/userfaultfd.h, clone(), posix_memalign() or userfaultfd()"
};
#endif
