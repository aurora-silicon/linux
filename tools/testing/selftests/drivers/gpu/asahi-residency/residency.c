// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * DRM_IOCTL_ASAHI_GEM_RESIDENCY on the GPU's render node: what it accepts and
 * refuses, the accounting (user_kept in the GPU's gpu_memory sysfs file, a
 * device total: run it while no other client changes the residency of its
 * objects) and what reclaim does with kept objects. No GPU work is submitted.
 * Skipped where the render node is not this driver's or lacks the feature.
 *
 * The cases that make the kernel reclaim memory act on the whole system.
 * They need root and run only when asked for in the environment:
 *
 * ASAHI_RESIDENCY_RECLAIM=1 runs light_reclaim_spares_kept_objects,
 * kept_objects_stay_purgeable and requests_serialize_with_reclaim, which
 * write to the memory.reclaim file of the root memory cgroup.
 *
 * ASAHI_RESIDENCY_DROP_CACHES=1 runs kept_objects_go_under_urgent_pressure,
 * which writes 2 to /proc/sys/vm/drop_caches. That runs every shrinker of
 * the system at the last reclaim priority: it drops the dentry and inode
 * caches, with the page cache of the files they stood for, and the idle
 * memory of every GPU client.
 *
 * A request to the root memory cgroup names an amount, not a priority. The
 * kernel starts at its lightest reclaim priority and goes on to the more
 * urgent ones until it has reclaimed that much, anywhere in the system, and
 * runs the system's shrinkers at each priority it comes to. Where little
 * else can be reclaimed, a request of a few MiB ends at the last priorities,
 * at which the kept objects of idle VMs are released like everything else
 * the system can give. The cases therefore bring the memory to reclaim
 * themselves: a sparse temporary file in the working directory, read once,
 * which leaves COLD_BYTES of clean page cache that nothing else uses. As
 * long as plenty of it is left, a request usually finds its pages there
 * within the first few priorities.
 *
 * Usually is not always, and the system reclaims of its own accord as well,
 * so a case in which a kept object was released has to find out whether
 * reclaim had leave to release it. There are two ways, and the cases say
 * which one they went by:
 *
 * - The record of the shrinker passes (preferred). The driver releases kept
 *   objects from a shrinker of their own, one per VM, which the shrinker
 *   core calls only in a pass whose target reaches everything that shrinker
 *   counted. The program records the vmscan:mm_shrink_slab_start and
 *   mm_shrink_slab_end events in a trace instance of its own (tracefs at
 *   /sys/kernel/tracing, with kernel symbols). The events name a shrinker by
 *   a pointer, which says nothing of the VM, so a case knows its own VM's
 *   shrinker by what it counts: the pages of the VM's idle kept objects,
 *   which the case makes a number of its own (see own_pages()). A kept
 *   object that went is a failure unless a pass called that shrinker before
 *   the case first saw the object gone and had not ended when the case last
 *   saw it resident; with such a pass the case is skipped, since the
 *   release was allowed. Where more than one shrinker of
 *   the record counts what the test's VM counts and a pass called one of
 *   them, the record cannot tell whose pass it was: the case is skipped as
 *   inconclusive, which says nothing about the release.
 * - Signs of the environment (the fallback, where the passes cannot be
 *   recorded). After every request the cases note whether the kernel met it
 *   and whether the file still has COLD_FLOOR in the cache, and at the end
 *   whether kswapd or direct reclaim scanned pages meanwhile. A case in
 *   which a kept object was released after a request that was not met, with
 *   less of the file left, or while the system reclaimed as well, is
 *   skipped with that reason. These are heuristics: they make it likely, and
 *   do not show, that reclaim went to its last priorities, and reclaim that
 *   went there with every request met and the cache left would fail the
 *   case.
 *
 * These cases also need four times their objects' size of available memory
 * (objects of RECLAIM_PAGES pages: 8 GiB each with 16K pages), a working
 * directory on a file system with a page cache, and a memory cgroup without
 * memory.min or memory.low protection, which would keep reclaim away from
 * the cold cache.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include <linux/magic.h>
#include <linux/mman.h>
#include <drm/asahi_drm.h>

#include "../../../kselftest_harness.h"

#ifndef __NR_cachestat
#define __NR_cachestat 451
#endif

#define PAGE 0x4000UL

/*
 * The idle pages a VM must have for the lightest reclaim pass to release
 * some: the shrinker core scans a shrinker once the count it reported,
 * divided by 2^12 at that priority and doubled, reaches the shrinker's batch
 * (256 pages for this driver).
 */
#define RECLAIM_PAGES (256UL << 11)

/* The time a VM must have been left alone before reclaim touches it, and more. */
#define IDLE_SECONDS 3

/* What one reclaim request asks for. */
#define REQUEST "4M"

/* The clean page cache the reclaim cases bring, and how much of it must be left. */
#define COLD_BYTES (2ULL << 30)
#define COLD_FLOOR (COLD_BYTES / 4)

/* The most requests a case makes to see reclaim release an object. */
#define MAX_REQUESTS 128

/* The requests through which a kept object has to stay resident. */
#define SPARED_REQUESTS 16

static int ioc(int fd, unsigned long cmd, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, cmd, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	return ret ? -errno : 0;
}

/* A field of the GPU's gpu_memory sysfs file. */
static long long gpu_memory_field(const char *key)
{
	char line[256];
	long long value = -1;
	size_t n = strlen(key);
	FILE *f = fopen("/sys/class/drm/renderD128/device/gpu_memory", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, n) && line[n] == ' ')
			value = atoll(line + n + 1);
	fclose(f);
	return value;
}

/* drm-resident-system of the client behind @fd, in bytes: the memory it holds. */
static long long client_resident(int fd)
{
	char path[64], line[256], unit[8];
	long long value = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unit[0] = 0;
		if (sscanf(line, "drm-resident-system: %lld %7s", &value, unit) < 1)
			continue;
		if (!strcmp(unit, "KiB"))
			value <<= 10;
		else if (!strcmp(unit, "MiB"))
			value <<= 20;
		break;
	}
	fclose(f);
	return value;
}

/* MemAvailable in bytes. */
static long long mem_available(void)
{
	char line[256];
	long long kb = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "MemAvailable: %lld kB", &kb) == 1)
			break;
	fclose(f);
	return kb < 0 ? -1 : kb << 10;
}

static int write_string(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	int ret;

	if (fd < 0)
		return -errno;
	ret = write(fd, text, strlen(text)) < 0 ? -errno : 0;
	close(fd);
	return ret;
}

#define MEMORY_RECLAIM "/sys/fs/cgroup/memory.reclaim"

/* The bytes of the cold cache file @fd that are in the page cache, or -errno. */
static long long cold_cached(int fd)
{
	struct cachestat_range range = { .off = 0, .len = COLD_BYTES };
	struct cachestat cs;

	if (syscall(__NR_cachestat, fd, &range, &cs, 0))
		return -errno;
	return (long long)cs.nr_cache * sysconf(_SC_PAGESIZE);
}

/*
 * Creates the cold cache: an unnamed sparse file of COLD_BYTES in the working
 * directory, read once from end to end. Reading a hole leaves clean pages in
 * the page cache without writing to the disk, and pages read once are the
 * first ones reclaim takes. Returns the file, which holds the cache until it
 * is closed, or -1 and the reason in @why.
 */
static int cold_cache(const char **why)
{
	static char buf[1 << 20];
	struct statfs fs;
	int fd = open(".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);

	*why = "cannot create a temporary file in the working directory";
	if (fd < 0)
		return -1;
	if (fstatfs(fd, &fs) || fs.f_type == TMPFS_MAGIC ||
	    fs.f_type == RAMFS_MAGIC) {
		*why = "the working directory is in memory: reclaim cannot drop its files";
		goto fail;
	}
	if (ftruncate(fd, COLD_BYTES))
		goto fail;
	*why = "cannot read the temporary file into the page cache";
	for (uint64_t off = 0; off < COLD_BYTES; off += sizeof(buf))
		if (pread(fd, buf, sizeof(buf), off) != sizeof(buf))
			goto fail;
	if (cold_cached(fd) < (long long)(COLD_BYTES - COLD_BYTES / 8)) {
		*why = "the file system does not keep a file that was read in the page cache";
		goto fail;
	}
	return fd;
fail:
	close(fd);
	return -1;
}

/*
 * The pages kswapd and direct reclaim have scanned since boot: reclaim the
 * system does of its own accord, at priorities the test knows nothing about.
 */
static long long scanned_unasked(void)
{
	char line[256];
	long long value, sum = 0;
	FILE *f = fopen("/proc/vmstat", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "pgscan_kswapd %lld", &value) == 1 ||
		    sscanf(line, "pgscan_direct %lld", &value) == 1)
			sum += value;
	fclose(f);
	return sum;
}

/*
 * Whether the memory cgroup of this process, or one above it, is protected
 * from reclaim (memory.min, memory.low): reclaim then passes the cold cache
 * by until it has tried everything else.
 */
static int cgroup_protected(void)
{
	char path[512] = "/sys/fs/cgroup", line[400], file[600], value[32];
	FILE *f = fopen("/proc/self/cgroup", "r");
	size_t root = strlen(path);
	char *end;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "0::", 3))
			snprintf(path + root, sizeof(path) - root, "%s", line + 3);
	fclose(f);
	path[strcspn(path, "\n")] = 0;
	for (end = path + strlen(path); end > path + root; end = strrchr(path, '/')) {
		static const char * const names[] = { "memory.min", "memory.low" };

		*end = 0;
		for (int i = 0; i < 2; i++) {
			snprintf(file, sizeof(file), "%s/%s", path, names[i]);
			f = fopen(file, "r");
			if (!f)
				continue;
			if (fgets(value, sizeof(value), f) && strcmp(value, "0\n")) {
				fclose(f);
				return 1;
			}
			fclose(f);
		}
	}
	return 0;
}

static double seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * A number of pages that a case adds to the size of its kept object, drawn
 * anew by every case: 0 to 1023 GPU pages. It makes what the VM's shrinker
 * of kept objects counts a number that another VM's is unlikely to count,
 * which is how the case finds its own shrinker in the record.
 */
static uint64_t own_pages(void)
{
	uint16_t r = 0;

	if (getrandom(&r, sizeof(r), 0) != sizeof(r))
		r = getpid() ^ time(NULL);
	return r % 1024;
}

/*
 * The record of the shrinker core's passes over the shrinkers: a trace
 * instance with the events that the core emits before it decides whether to
 * call a shrinker and after it is done with it.
 *
 * The instance belongs to the program, not to a case. main() makes it before
 * the cases fork, under a name with its process ID, and removes it when the
 * program exits or is told to end, whatever became of the cases: a case
 * that fails, times out or is killed leaves nothing to clean up. An
 * instance of that name that exists already is somebody else's and is left
 * alone; the cases then go without a record. A case only empties the buffer
 * and switches the events on and off.
 */
#define TRACE_EVENTS "/events/vmscan/mm_shrink_slab_"

static char trace_dir[80];
/* The enable files of the two events, made with trace_dir. */
static char trace_enable[2][160];
static pid_t trace_owner;
/* The signals that end the program, blocked while the instance changes hands. */
static const int trace_signals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
static sigset_t trace_sigset;

/* Whether this case is recording. */
static int tracing;

static int trace_write(const char *file, const char *text)
{
	char path[160];

	snprintf(path, sizeof(path), "%s%s", trace_dir, file);
	return write_string(path, text);
}

static void trace_events(const char *on)
{
	write_string(trace_enable[0], on);
	write_string(trace_enable[1], on);
}

/*
 * Only async-signal-safe calls, on paths made beforehand, from here: it also
 * runs from a signal handler. The signals that end the program are blocked
 * meanwhile, so that it never runs twice at once.
 */
static void trace_remove(void)
{
	static const struct timespec pause = { .tv_nsec = 50000000 };
	sigset_t old;

	if (!trace_dir[0] || getpid() != trace_owner)
		return;
	sigprocmask(SIG_BLOCK, &trace_sigset, &old);
	if (trace_dir[0]) {
		trace_events("0");
		/* A case that was just killed may still hold a file of it. */
		for (int i = 0; i < 40 && rmdir(trace_dir) && errno == EBUSY; i++)
			nanosleep(&pause, NULL);
		trace_dir[0] = 0;
	}
	sigprocmask(SIG_SETMASK, &old, NULL);
}

/* The signal stays blocked until the handler returns, and then ends the program. */
static void trace_remove_on(int sig)
{
	trace_remove();
	signal(sig, SIG_DFL);
	raise(sig);
}

/*
 * Makes the instance, if the reclaim cases were asked for. Its timestamps
 * are on the clock of seconds().
 */
static void trace_create(void)
{
	const char *reclaim = getenv("ASAHI_RESIDENCY_RECLAIM");
	const char *drop = getenv("ASAHI_RESIDENCY_DROP_CACHES");
	struct sigaction sa = { .sa_handler = trace_remove_on };
	sigset_t old;

	if (geteuid() || !((reclaim && !strcmp(reclaim, "1")) ||
			   (drop && !strcmp(drop, "1"))))
		return;
	snprintf(trace_dir, sizeof(trace_dir),
		 "/sys/kernel/tracing/instances/asahi-residency-%d", getpid());
	snprintf(trace_enable[0], sizeof(trace_enable[0]),
		 "%s" TRACE_EVENTS "start/enable", trace_dir);
	snprintf(trace_enable[1], sizeof(trace_enable[1]),
		 "%s" TRACE_EVENTS "end/enable", trace_dir);
	sigemptyset(&trace_sigset);
	for (unsigned int i = 0; i < ARRAY_SIZE(trace_signals); i++)
		sigaddset(&trace_sigset, trace_signals[i]);
	sa.sa_mask = trace_sigset;

	/*
	 * A signal waits until the instance is set up: until the handlers are
	 * in place, and until no file of the instance is open, which would
	 * keep the handler from removing it.
	 */
	sigprocmask(SIG_BLOCK, &trace_sigset, &old);
	if (mkdir(trace_dir, 0700)) {
		trace_dir[0] = 0;
	} else {
		trace_owner = getpid();
		atexit(trace_remove);
		for (unsigned int i = 0; i < ARRAY_SIZE(trace_signals); i++)
			sigaction(trace_signals[i], &sa, NULL);
		if (trace_write("/trace_clock", "mono") ||
		    trace_write("/buffer_size_kb", "4096"))
			trace_remove();
	}
	sigprocmask(SIG_SETMASK, &old, NULL);
}

/* Starts the record of a case afresh. */
static void trace_start(void)
{
	char path[160];
	int fd;

	if (!trace_dir[0])
		return;
	snprintf(path, sizeof(path), "%s/trace", trace_dir);
	fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		return;
	close(fd);
	if (write_string(trace_enable[0], "1") ||
	    write_string(trace_enable[1], "1")) {
		trace_events("0");
		return;
	}
	tracing = 1;
}

static void trace_stop(void)
{
	if (!tracing)
		return;
	trace_events("0");
	tracing = 0;
}

/* One of the driver's shrinker events. */
struct shrink_event {
	/* mm_shrink_slab_end, not _start. */
	int end;
	/* Of a shrinker of kept objects, not of a VM's other shrinker. */
	int kept;
	double time;
	/* The shrinker, as the event prints its pointer. */
	char shrinker[24];
	/* The task that makes the pass. */
	int pid;
	/* Of a start event: what the shrinker counted, the target, the priority. */
	long items, target;
	int priority;
};

/*
 * Reads a line of the record. Returns 1 for an event of one of the driver's
 * shrinkers, which are told by the names of their scan functions (scan_kept
 * of VmShrinker for the kept objects of a VM, VmShrinker's other one for the
 * rest), 0 for any other line and -1 for an event that does not read as
 * expected.
 */
static int shrink_event(const char *line, struct shrink_event *e)
{
	const char *at = strstr(line, ": mm_shrink_slab_");
	const char *nid, *p, *items, *target, *priority;
	size_t n;

	if (line[0] == '#' || !at)
		return 0;
	for (p = at; p > line && p[-1] != ' '; p--)
		;
	e->time = strtod(p, NULL);
	at += strlen(": mm_shrink_slab_");
	e->end = !strncmp(at, "end: ", 5);
	if (!e->end && strncmp(at, "start: ", 7))
		return 0;
	at = strchr(at, ' ') + 1;
	/* "<scan function> <shrinker>: nid: ..." */
	nid = strstr(at, ": nid: ");
	if (!nid)
		return -1;
	for (p = nid; p > at && p[-1] != ' '; p--)
		;
	n = nid - p;
	if (p == at || !n || n >= sizeof(e->shrinker))
		return -1;
	if (!memmem(at, p - at, "VmShrinker", strlen("VmShrinker")))
		return 0;
	e->kept = !!memmem(at, p - at, "scan_kept", strlen("scan_kept"));
	memcpy(e->shrinker, p, n);
	e->shrinker[n] = 0;
	p = strstr(nid, " pid: ");
	if (!p)
		return -1;
	e->pid = atoi(p + strlen(" pid: "));
	if (e->end)
		return 1;
	items = strstr(nid, " cache items ");
	target = strstr(nid, " total_scan ");
	priority = strstr(nid, " priority ");
	if (!items || !target || !priority)
		return -1;
	e->items = atol(items + strlen(" cache items "));
	e->target = atol(target + strlen(" total_scan "));
	e->priority = atoi(priority + strlen(" priority "));
	return 1;
}

/* What a case knows of its own VM for reading the record. */
struct own_vm {
	/*
	 * What the VM's shrinker of kept objects can have counted since the
	 * record began: the pages of the idle kept objects, in the one or two
	 * combinations the case brings about.
	 */
	long counts[2];
	/*
	 * When the case last saw its kept objects resident (taken before it
	 * looked) and first saw one gone (taken after it looked); 0 and
	 * infinity while it did not.
	 */
	double held_at, gone_at;
};

/* What the record has about the shrinkers of kept objects. */
struct kept_record {
	/* Passes over the VMs' other shrinker: the record has the driver's events. */
	int others;
	/* Shrinkers that only ever counted what the test's VM counts. */
	int shrinkers;
	/* Passes over those, and the ones among them that called the shrinker. */
	int passes, called;
	/* The called ones that could have released the object that went. */
	int released;
	/* The most urgent priority among those, or among the called ones. */
	int priority;
};

#define MAX_SHRINKERS 64
#define MAX_CALLS 256

/*
 * Reads the record for the case whose VM is @vm. The shrinker core calls a
 * shrinker when the target of the pass (total_scan) reaches the shrinker's
 * batch or everything it counted (cache items); the batch of a shrinker of
 * kept objects is out of reach, which leaves the second.
 *
 * The record names shrinkers by pointer. The one of the test's VM is among
 * those that never counted anything but @vm->counts; there is one such
 * shrinker unless another VM's happens to count the same.
 *
 * A pass could have released an object that the case saw resident at
 * held_at and gone at gone_at if it called the shrinker before gone_at and
 * had not ended by held_at. A pass has ended with the next end event of the
 * same shrinker from the same task.
 *
 * Returns 0, or -1 where there is no record, events were lost or it has
 * more shrinkers or calls than this keeps track of.
 */
static int read_record(const struct own_vm *vm, struct kept_record *r)
{
	static struct {
		char shrinker[24];
		int foreign, passes;
	} seen[MAX_SHRINKERS];
	static struct {
		/* pid: the task, and once the record is read, whether it is in the interval. */
		int shrinker, pid, priority;
		double start, end;
	} calls[MAX_CALLS];
	int nseen = 0, ncalls = 0, whole = 0, k;
	long in_buffer, written;
	char path[160], line[1024];
	struct shrink_event e;
	FILE *f;

	memset(r, 0, sizeof(*r));
	r->priority = -1;
	if (!tracing)
		return -1;
	snprintf(path, sizeof(path), "%s/trace", trace_dir);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		int ret = shrink_event(line, &e);

		if (sscanf(line, "# entries-in-buffer/entries-written: %ld/%ld",
			   &in_buffer, &written) == 2)
			whole = in_buffer == written;
		if (ret < 0)
			whole = 0;
		if (ret <= 0)
			continue;
		if (!e.kept) {
			r->others += !e.end;
			continue;
		}
		for (k = 0; k < nseen && strcmp(seen[k].shrinker, e.shrinker); k++)
			;
		if (e.end) {
			for (int c = ncalls - 1; c >= 0; c--) {
				if (calls[c].shrinker != k || calls[c].pid != e.pid ||
				    !isinf(calls[c].end))
					continue;
				calls[c].end = e.time;
				break;
			}
			continue;
		}
		if (k == nseen) {
			if (nseen == MAX_SHRINKERS) {
				whole = 0;
				break;
			}
			memset(&seen[k], 0, sizeof(seen[k]));
			strcpy(seen[k].shrinker, e.shrinker);
			nseen++;
		}
		seen[k].passes++;
		if (e.items != vm->counts[0] && e.items != vm->counts[1])
			seen[k].foreign = 1;
		if (e.target < e.items)
			continue;
		if (ncalls == MAX_CALLS) {
			whole = 0;
			break;
		}
		calls[ncalls].shrinker = k;
		calls[ncalls].pid = e.pid;
		calls[ncalls].priority = e.priority;
		calls[ncalls].start = e.time;
		calls[ncalls].end = INFINITY;
		ncalls++;
	}
	fclose(f);
	if (!whole)
		return -1;
	for (k = 0; k < nseen; k++) {
		if (seen[k].foreign)
			continue;
		r->shrinkers++;
		r->passes += seen[k].passes;
	}
	/* The timestamps of the record are rounded to a microsecond. */
	for (int c = 0; c < ncalls; c++) {
		calls[c].pid = calls[c].start <= vm->gone_at + 2e-6 &&
			       calls[c].end >= vm->held_at - 2e-6;
		if (seen[calls[c].shrinker].foreign)
			continue;
		r->called++;
		r->released += calls[c].pid;
	}
	for (int c = 0; c < ncalls; c++) {
		if (seen[calls[c].shrinker].foreign ||
		    (r->released && !calls[c].pid))
			continue;
		if (r->priority < 0 || calls[c].priority < r->priority)
			r->priority = calls[c].priority;
	}
	return 0;
}

/* The reclaim requests of a case, and whether the kernel could meet them lightly. */
struct pressure {
	/* The cold cache file. */
	int cold;
	/* The requests made. */
	int requests;
	/* Those the kernel did not meet in full. */
	int unmet;
	/* The least of the cold cache that was left after a request. */
	long long cold_low;
	/* scanned_unasked() when the case began. */
	long long unasked;
	/* For the record of the shrinker passes. */
	struct own_vm vm;
};

/*
 * Asks the root memory cgroup to reclaim REQUEST. The kernel returns EAGAIN
 * when it could not reclaim that much at any priority, which is counted;
 * any other error is returned.
 */
static int reclaim_some(struct pressure *p)
{
	int ret = write_string(MEMORY_RECLAIM, REQUEST);
	long long cached = cold_cached(p->cold);

	p->requests++;
	if (ret == -EAGAIN) {
		p->unmet++;
		ret = 0;
	}
	if (cached < p->cold_low)
		p->cold_low = cached;
	return ret;
}

/*
 * A heuristic, for where the shrinker passes cannot be recorded: a sign that
 * reclaim may have gone to its last priorities since the case began, or
 * NULL. With every request met, COLD_FLOOR of the cold cache left after each
 * and no other reclaim in the system, none is likely to have gone beyond the
 * first few priorities; that is not shown.
 */
static const char *urgent_reclaim_signs(const struct pressure *p)
{
	if (p->unmet)
		return "the kernel could not meet a reclaim request";
	if (p->cold_low < (long long)COLD_FLOOR)
		return "the cold cache ran low";
	if (scanned_unasked() != p->unasked)
		return "kswapd or direct reclaim ran meanwhile";
	return NULL;
}

/*
 * What to make of a kept object that went between p->vm.held_at and
 * p->vm.gone_at. Returns 0 if reclaim had no leave to release it, so that
 * the case fails, and 1 if the case is to be skipped; either way @why says
 * on what grounds.
 *
 * By the record of the shrinker passes where there is one that shows the
 * driver's shrinkers at work (the cases ask once reclaim has released an
 * object that is not kept). Reclaim had leave if, in that interval, a pass
 * called the test VM's shrinker of kept objects: "as it may". If the record
 * has several shrinkers that count what the test's VM counts and a pass
 * called one of them, it does not tell whether that was the test's:
 * "inconclusive", which claims nothing about the release. With no such pass
 * for any of them, reclaim had no leave.
 *
 * By the heuristic otherwise, and the words say so.
 */
static int kept_could_go(const struct pressure *p, char *why, size_t size)
{
	struct kept_record r;
	const char *sign;

	if (!read_record(&p->vm, &r) && r.others) {
		if (!r.released)
			snprintf(why, size,
				 "no reclaim pass called the shrinker of this VM's kept objects before it went (traced: %d shrinkers of kept objects count what this VM's counts, with %d passes and %d calls at other times)",
				 r.shrinkers, r.passes, r.called);
		else if (r.shrinkers == 1)
			snprintf(why, size,
				 "as it may: reclaim passes that called the shrinker of this VM's kept objects before it went: %d, down to priority %d (traced)",
				 r.released, r.priority);
		else
			snprintf(why, size,
				 "inconclusive: %d shrinkers of kept objects count what this VM's counts, and %d passes called one of them before it went (traced)",
				 r.shrinkers, r.released);
		return r.released > 0;
	}
	sign = urgent_reclaim_signs(p);
	if (sign)
		snprintf(why, size,
			 "as it may: %s (a heuristic: the shrinker passes were not traced)",
			 sign);
	else
		snprintf(why, size,
			 "no sign of urgent reclaim (a heuristic: the shrinker passes were not traced)");
	return sign != NULL;
}

/* For the log of a case that passed: what the record of the shrinker passes has. */
static const char *trace_note(const struct pressure *p, char *note, size_t size)
{
	struct kept_record r;

	if (!read_record(&p->vm, &r) && r.others)
		snprintf(note, size,
			 "reclaim passes over the shrinker of this VM's kept objects: %d, of which %d called it (traced, %d such shrinker%s)",
			 r.passes, r.called, r.shrinkers, r.shrinkers == 1 ? "" : "s");
	else
		snprintf(note, size, "the shrinker passes were not traced");
	return note;
}

static int residency(int fd, uint32_t handle, uint32_t flags)
{
	struct drm_asahi_gem_residency args = { .handle = handle, .flags = flags };

	return ioc(fd, DRM_IOCTL_ASAHI_GEM_RESIDENCY, &args);
}

/* Gives advice for @handle; returns whether its contents are retained, or -errno. */
static int madvise_object(int fd, uint32_t handle, uint32_t madv)
{
	struct drm_asahi_gem_madvise args = { .handle = handle, .madv = madv };
	int ret = ioc(fd, DRM_IOCTL_ASAHI_GEM_MADVISE, &args);

	return ret ? ret : (int)args.retained;
}

static int bind(int fd, uint32_t vm, uint32_t handle, uint64_t addr,
		uint64_t size, uint32_t flags)
{
	struct drm_asahi_gem_bind_op op = {
		.flags = flags, .handle = handle, .range = size, .addr = addr,
	};
	struct drm_asahi_vm_bind args = {
		.vm_id = vm, .num_binds = 1, .stride = sizeof(op),
		.userptr = (uintptr_t)&op,
	};

	return ioc(fd, DRM_IOCTL_ASAHI_VM_BIND, &args);
}

static void close_handle(int fd, uint32_t handle)
{
	struct drm_gem_close args = { .handle = handle };

	ioc(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

/* Whether the driver behind @fd is this one. */
static int is_asahi(int fd)
{
	char name[16] = "";
	struct drm_version version = {
		.name = name, .name_len = sizeof(name) - 1,
	};

	return !ioc(fd, DRM_IOCTL_VERSION, &version) && !strcmp(name, "aurora");
}

FIXTURE(asahi)
{
	int fd;
	/* The cold cache file of a reclaim case, or -1. */
	int cold;
	struct drm_asahi_params_global params;
	uint32_t vm;
	uint64_t va, va_end;
};

FIXTURE_SETUP(asahi)
{
	struct drm_asahi_get_params get = {
		.pointer = (uintptr_t)&self->params,
		.size = sizeof(self->params),
	};

	self->cold = -1;
	self->fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (self->fd < 0 || !is_asahi(self->fd))
		SKIP(return, "renderD128 is not an asahi render node");
	memset(&self->params, 0, sizeof(self->params));
	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_GET_PARAMS, &get));
	if (!(self->params.features & DRM_ASAHI_FEATURE_GEM_RESIDENCY))
		SKIP(return, "no DRM_ASAHI_FEATURE_GEM_RESIDENCY");

	uint64_t kernel = self->params.vm_kernel_min_size > (32ULL << 30) ?
			  self->params.vm_kernel_min_size : 32ULL << 30;
	struct drm_asahi_vm_create vm = {
		.kernel_start = self->params.vm_end - kernel,
		.kernel_end = self->params.vm_end,
	};
	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_VM_CREATE, &vm));
	self->vm = vm.vm_id;
	self->va = self->params.vm_start;
	self->va_end = vm.kernel_start;
}

FIXTURE_TEARDOWN(asahi)
{
	trace_stop();
	close(self->fd);
	/* Closing the unnamed file gives its page cache back. */
	if (self->cold >= 0)
		close(self->cold);
}

/*
 * Creates a VM-private object of @size bytes in @vm and binds it, which
 * gives it its memory. The driver keeps ranges of every VM for its own
 * mappings, where a bind fails with EINVAL, and GET_PARAMS does not list
 * them: the object goes to the first address after *@va, in steps of 4 GiB
 * and below @va_end, at which it binds, and *@va moves past it. Returns its
 * handle, or 0.
 */
static uint32_t bound_object(int fd, uint32_t vm, uint64_t *va, uint64_t va_end,
			     uint64_t size)
{
	struct drm_asahi_gem_create bo = {
		.size = size, .flags = DRM_ASAHI_GEM_VM_PRIVATE, .vm_id = vm,
	};

	if (ioc(fd, DRM_IOCTL_ASAHI_GEM_CREATE, &bo))
		return 0;
	for (;;) {
		*va = (*va + (4ULL << 30)) & ~((4ULL << 30) - 1);
		if (*va + size > va_end)
			break;
		if (!bind(fd, vm, bo.handle, *va, size,
			  DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE)) {
			*va += size;
			return bo.handle;
		}
	}
	close_handle(fd, bo.handle);
	return 0;
}

/*
 * Prepares a case that asks the root memory cgroup for reclaim, with objects
 * of @bytes in all: checks that it was asked for and can run here, and
 * creates the cold cache, whose file goes to *@cold. Returns NULL, or why
 * the case is skipped.
 */
static const char *reclaim_setup(struct pressure *p, uint64_t bytes, int *cold)
{
	const char *run = getenv("ASAHI_RESIDENCY_RECLAIM");
	const char *why;

	if (!run || strcmp(run, "1"))
		return "set ASAHI_RESIDENCY_RECLAIM=1 to ask the root memory cgroup for reclaim";
	if (geteuid())
		return "reclaim requests need root";
	if (access(MEMORY_RECLAIM, W_OK))
		return "no " MEMORY_RECLAIM;
	if (mem_available() < (long long)(4 * bytes + 2 * COLD_BYTES))
		return "not enough available memory for the test objects";
	if (cgroup_protected())
		return "the test's memory cgroup has memory.min or memory.low protection";
	*cold = cold_cache(&why);
	if (*cold < 0)
		return why;
	memset(p, 0, sizeof(*p));
	p->cold = *cold;
	p->cold_low = cold_cached(*cold);
	p->unasked = scanned_unasked();
	p->vm.gone_at = INFINITY;
	trace_start();
	return NULL;
}

/*
 * Notes, for the record, an observation of the case's kept objects that
 * began at @before: whether they were all resident (see struct own_vm).
 */
static void note_kept(struct own_vm *vm, double before, int intact)
{
	if (intact) {
		vm->held_at = before;
		vm->gone_at = INFINITY;
	} else if (isinf(vm->gone_at)) {
		vm->gone_at = seconds();
	}
}

/*
 * What the client behind @fd holds. Its kept objects are all resident while
 * that is @resident or @kept, which is noted for the record.
 */
static long long watch_resident(struct pressure *p, int fd, long long resident,
				long long kept)
{
	double before = seconds();
	long long now = client_resident(fd);

	note_kept(&p->vm, before, now == resident || now == kept);
	return now;
}

/*
 * Makes reclaim requests, at most @most, for as long as the client behind
 * @fd holds @resident bytes. @kept is what it holds once everything but its
 * kept objects has gone. Returns what it holds then, or a negative value:
 * -errno of a request that failed, or -1 where what the client holds could
 * not be read. The callers assert that it is not negative before they take
 * the result for a sign of anything.
 */
static long long reclaim_while(struct pressure *p, int fd, long long resident,
			       long long kept, int most)
{
	long long now = watch_resident(p, fd, resident, kept);

	for (int i = 0; i < most && now == resident; i++) {
		int ret = reclaim_some(p);

		if (ret)
			return ret;
		now = watch_resident(p, fd, resident, kept);
	}
	return now;
}

/* The kept total follows a VM-private object: once per object, until freed. */
TEST_F(asahi, kept_size_is_counted_once)
{
	const uint64_t size = 16 * PAGE;
	struct drm_asahi_gem_create bo = {
		.size = size, .flags = DRM_ASAHI_GEM_VM_PRIVATE, .vm_id = self->vm,
	};
	long long kept0;

	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_GEM_CREATE, &bo));
	kept0 = gpu_memory_field("user_kept");
	ASSERT_GE(kept0, 0);
	ASSERT_EQ(0, residency(self->fd, bo.handle, DRM_ASAHI_GEM_RESIDENCY_KEEP));
	ASSERT_EQ(0, residency(self->fd, bo.handle, DRM_ASAHI_GEM_RESIDENCY_KEEP));
	EXPECT_EQ(kept0 + (long long)size, gpu_memory_field("user_kept"));
	ASSERT_EQ(0, residency(self->fd, bo.handle, 0));
	EXPECT_EQ(kept0, gpu_memory_field("user_kept"));
	ASSERT_EQ(0, residency(self->fd, bo.handle, 0));
	EXPECT_EQ(kept0, gpu_memory_field("user_kept"));
	/* Freeing a kept object stops counting it. */
	ASSERT_EQ(0, residency(self->fd, bo.handle, DRM_ASAHI_GEM_RESIDENCY_KEEP));
	EXPECT_EQ(kept0 + (long long)size, gpu_memory_field("user_kept"));
	close_handle(self->fd, bo.handle);
	EXPECT_EQ(kept0, gpu_memory_field("user_kept"));
}

TEST_F(asahi, bad_arguments)
{
	struct drm_asahi_gem_create bo = {
		.size = PAGE, .flags = DRM_ASAHI_GEM_VM_PRIVATE, .vm_id = self->vm,
	};
	long long kept0 = gpu_memory_field("user_kept");

	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_GEM_CREATE, &bo));
	EXPECT_EQ(-EINVAL, residency(self->fd, bo.handle, 1u << 1));
	EXPECT_EQ(-EINVAL, residency(self->fd, bo.handle,
				     DRM_ASAHI_GEM_RESIDENCY_KEEP | 1u << 31));
	EXPECT_EQ(-ENOENT, residency(self->fd, 0xffffff,
				     DRM_ASAHI_GEM_RESIDENCY_KEEP));
	EXPECT_EQ(kept0, gpu_memory_field("user_kept"));
	close_handle(self->fd, bo.handle);
}

/* An object that is never released takes the flag all the same. */
TEST_F(asahi, shared_objects_take_the_flag)
{
	const uint64_t size = 2 * PAGE;
	struct drm_asahi_gem_create bo = { .size = size };
	long long kept0 = gpu_memory_field("user_kept");

	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_GEM_CREATE, &bo));
	EXPECT_EQ(0, residency(self->fd, bo.handle, DRM_ASAHI_GEM_RESIDENCY_KEEP));
	EXPECT_EQ(kept0 + (long long)size, gpu_memory_field("user_kept"));
	EXPECT_EQ(0, residency(self->fd, bo.handle, 0));
	EXPECT_EQ(kept0, gpu_memory_field("user_kept"));
	close_handle(self->fd, bo.handle);
}

/*
 * Of two idle objects, reclaim releases the one that is not kept first, at
 * any priority. Reclaim that does not come to the kept objects (see the top
 * of this file for how the case knows) leaves the kept one resident; once
 * the request is withdrawn, it is released as well. The kept object is the
 * larger one, so that what the client holds tells which one stayed.
 */
TEST_F_TIMEOUT(asahi, light_reclaim_spares_kept_objects, 180)
{
	const uint64_t size = RECLAIM_PAGES * sysconf(_SC_PAGESIZE);
	const uint64_t kept_size = size + (1024 + own_pages()) * PAGE;
	const long long both = kept_size + size;
	struct pressure p;
	const char *why = reclaim_setup(&p, both, &self->cold);
	long long resident;
	char reason[320];
	uint32_t a, b;
	int first;

	if (why)
		SKIP(return, "%s", why);
	p.vm.counts[0] = p.vm.counts[1] = kept_size / sysconf(_SC_PAGESIZE);
	a = bound_object(self->fd, self->vm, &self->va, self->va_end, kept_size);
	b = bound_object(self->fd, self->vm, &self->va, self->va_end, size);
	ASSERT_NE(0, a);
	ASSERT_NE(0, b);
	ASSERT_EQ(both, client_resident(self->fd));
	ASSERT_EQ(0, residency(self->fd, a, DRM_ASAHI_GEM_RESIDENCY_KEEP));

	sleep(IDLE_SECONDS);
	resident = reclaim_while(&p, self->fd, both, kept_size, MAX_REQUESTS);
	first = p.requests;
	ASSERT_GE(resident, 0);
	/* The shrinkers ran: something went, and never the kept object alone. */
	ASSERT_NE(both, resident);
	ASSERT_NE((long long)size, resident);
	if (resident == (long long)kept_size) {
		resident = reclaim_while(&p, self->fd, kept_size, kept_size,
					 SPARED_REQUESTS);
		ASSERT_GE(resident, 0);
	}
	if (resident != (long long)kept_size) {
		if (kept_could_go(&p, reason, sizeof(reason)))
			SKIP(return, "the kept object went after %d requests, %s",
			     p.requests, reason);
		TH_LOG("the kept object went after %d requests, without leave: %s",
		       p.requests, reason);
	}
	ASSERT_EQ((long long)kept_size, resident);
	TH_LOG("the object that is not kept went with request %d", first);
	TH_LOG("the kept one stayed through %d more, %lld MiB of the cold cache left; %s",
	       p.requests - first, p.cold_low >> 20,
	       trace_note(&p, reason, sizeof(reason)));

	ASSERT_EQ(0, residency(self->fd, a, 0));
	first = p.requests;
	resident = reclaim_while(&p, self->fd, kept_size, kept_size,
				 MAX_REQUESTS);
	ASSERT_GE(resident, 0);
	EXPECT_EQ(0, resident);
	TH_LOG("the object no longer kept went after %d requests",
	       p.requests - first);
}

/* Contents given up with DONTNEED are purged by reclaim, kept or not. */
TEST_F_TIMEOUT(asahi, kept_objects_stay_purgeable, 180)
{
	const uint64_t size = RECLAIM_PAGES * sysconf(_SC_PAGESIZE);
	struct pressure p;
	const char *why = reclaim_setup(&p, size, &self->cold);
	long long resident;
	uint32_t a;

	if (why)
		SKIP(return, "%s", why);
	a = bound_object(self->fd, self->vm, &self->va, self->va_end, size);
	ASSERT_NE(0, a);
	ASSERT_EQ(0, residency(self->fd, a, DRM_ASAHI_GEM_RESIDENCY_KEEP));
	ASSERT_EQ(1, madvise_object(self->fd, a, DRM_ASAHI_GEM_MADV_DONTNEED));

	sleep(IDLE_SECONDS);
	resident = reclaim_while(&p, self->fd, size, size, MAX_REQUESTS);
	ASSERT_GE(resident, 0);
	EXPECT_EQ(0, resident);
	TH_LOG("purged after %d requests", p.requests);
	EXPECT_EQ(0, madvise_object(self->fd, a, DRM_ASAHI_GEM_MADV_WILLNEED));
}

struct toggler {
	int fd;
	uint32_t handle;
	int stop;
	int failed;
	long requests;
};

static void *toggle_residency(void *arg)
{
	struct toggler *t = arg;

	while (!__atomic_load_n(&t->stop, __ATOMIC_RELAXED)) {
		if (residency(t->fd, t->handle, DRM_ASAHI_GEM_RESIDENCY_KEEP) ||
		    residency(t->fd, t->handle, 0))
			t->failed = 1;
		t->requests += 2;
	}
	return NULL;
}

/* The objects of the race case, as bits of what the client holds. */
#define TOGGLED 1
#define CONTROL 2
#define KEPT 4

/*
 * Which objects a client that holds @resident bytes has resident, as a mask
 * of the bits above, or -1. @sizes: their sizes, in the order of the bits.
 */
static int resident_objects(long long resident, const long long sizes[3])
{
	for (int mask = 0; mask < 8; mask++) {
		long long sum = 0;

		for (int i = 0; i < 3; i++)
			if (mask & (1 << i))
				sum += sizes[i];
		if (sum == resident)
			return mask;
	}
	return -1;
}

/*
 * Residency requests racing with reclaim. One thread sets and clears the
 * request on one object as fast as it can while reclaim runs. An object
 * beside it that is not kept (the control) has to be released during the
 * race, which shows that reclaim got to this VM between the requests; a kept
 * object beside it has to stay, as long as reclaim does not come to the kept
 * objects (see the top of this file for how the case knows). Whatever a
 * request that has returned found (the object resident or already released),
 * reclaim leaves that way, and the kept total counts each object once. The
 * three objects differ in size, so that what the client holds tells which of
 * them are resident.
 */
TEST_F_TIMEOUT(asahi, requests_serialize_with_reclaim, 180)
{
	const uint64_t size = RECLAIM_PAGES * sysconf(_SC_PAGESIZE);
	const uint64_t small = 1024 * PAGE;
	const long long sizes[3] = {
		size + 2 * small, size, small + own_pages() * PAGE,
	};
	struct toggler t = { .fd = self->fd };
	long long kept0 = gpu_memory_field("user_kept");
	struct pressure p;
	const char *why = reclaim_setup(&p, sizes[0] + sizes[1] + sizes[2],
					&self->cold);
	int resident = TOGGLED | CONTROL | KEPT, found, error = 0;
	int control_gone = 0;
	char reason[320];
	uint32_t control, kept;
	pthread_t thread;
	double end, before;

	if (why)
		SKIP(return, "%s", why);
	/* The kept object alone, or with the toggled one while that is kept. */
	p.vm.counts[0] = sizes[2] / sysconf(_SC_PAGESIZE);
	p.vm.counts[1] = (sizes[0] + sizes[2]) / sysconf(_SC_PAGESIZE);
	t.handle = bound_object(self->fd, self->vm, &self->va, self->va_end, sizes[0]);
	control = bound_object(self->fd, self->vm, &self->va, self->va_end, sizes[1]);
	kept = bound_object(self->fd, self->vm, &self->va, self->va_end, sizes[2]);
	ASSERT_NE(0, t.handle);
	ASSERT_NE(0, control);
	ASSERT_NE(0, kept);
	ASSERT_EQ(0, residency(self->fd, kept, DRM_ASAHI_GEM_RESIDENCY_KEEP));
	kept0 += sizes[2];
	ASSERT_EQ(0, pthread_create(&thread, NULL, toggle_residency, &t));
	/*
	 * From before the VM counts as idle until a second after reclaim
	 * released the control object, for ten seconds at most.
	 */
	sleep(IDLE_SECONDS - 2);
	end = seconds() + 10;
	while (seconds() < end && !error && resident >= 0 && (resident & KEPT)) {
		error = reclaim_some(&p);
		before = seconds();
		resident = resident_objects(client_resident(self->fd), sizes);
		note_kept(&p.vm, before, resident >= 0 && (resident & KEPT));
		if (resident >= 0 && !(resident & CONTROL) && !control_gone) {
			control_gone = p.requests;
			end = seconds() + 1;
		}
		usleep(100000);
	}
	__atomic_store_n(&t.stop, 1, __ATOMIC_RELAXED);
	ASSERT_EQ(0, pthread_join(thread, NULL));
	ASSERT_EQ(0, error);
	ASSERT_GE(resident, 0);
	EXPECT_EQ(0, t.failed);
	EXPECT_GT(t.requests, 0);
	EXPECT_EQ(kept0, gpu_memory_field("user_kept"));

	/* The toggled object is kept from here on, resident or not. */
	ASSERT_EQ(0, residency(self->fd, t.handle, DRM_ASAHI_GEM_RESIDENCY_KEEP));
	before = seconds();
	found = resident_objects(client_resident(self->fd), sizes);
	note_kept(&p.vm, before, found >= 0 && (found & KEPT));
	resident = found;
	for (int i = 0; i < SPARED_REQUESTS && resident == found; i++) {
		ASSERT_EQ(0, reclaim_some(&p));
		before = seconds();
		resident = resident_objects(client_resident(self->fd), sizes);
		note_kept(&p.vm, before, (found & KEPT) &&
			  (resident | CONTROL) == (found | CONTROL));
	}
	/* What the client holds was read, and is a sum of the objects' sizes. */
	ASSERT_GE(found, 0);
	ASSERT_GE(resident, 0);
	/* A kept object that went, during the race or after it. */
	if (!(found & KEPT) || (resident | CONTROL) != (found | CONTROL)) {
		if (kept_could_go(&p, reason, sizeof(reason)))
			SKIP(return, "a kept object went after %d requests, %s",
			     p.requests, reason);
		TH_LOG("a kept object went after %d requests, without leave: %s",
		       p.requests, reason);
		ASSERT_TRUE(found & KEPT);
		ASSERT_EQ(found | CONTROL, resident | CONTROL);
	}
	/* Reclaim got to this VM during the race. */
	ASSERT_NE(0, control_gone);
	TH_LOG("%ld requests raced with reclaim; the control object went with request %d",
	       t.requests, control_gone);
	TH_LOG("the toggled object was %s, %lld MiB of the cold cache left; %s",
	       found & TOGGLED ? "left" : "released", p.cold_low >> 20,
	       trace_note(&p, reason, sizeof(reason)));
	EXPECT_EQ(kept0 + sizes[0], gpu_memory_field("user_kept"));
	close_handle(self->fd, t.handle);
	EXPECT_EQ(kept0, gpu_memory_field("user_kept"));
	close_handle(self->fd, kept);
	close_handle(self->fd, control);
}

/*
 * Urgent reclaim (here: every shrinker run at the last priority) releases
 * kept objects too. Where the shrinker passes can be recorded, the record
 * has to show a pass that called the shrinker of this VM's kept objects
 * between the last look at the object and the first one that found it gone.
 */
TEST_F(asahi, kept_objects_go_under_urgent_pressure)
{
	const uint64_t size = 1024 * PAGE;
	const uint64_t kept_size = size + own_pages() * PAGE;
	const char *run = getenv("ASAHI_RESIDENCY_DROP_CACHES");
	long long kept0 = gpu_memory_field("user_kept");
	struct own_vm vm = {
		.counts = { kept_size / sysconf(_SC_PAGESIZE),
			    kept_size / sysconf(_SC_PAGESIZE) },
	};
	struct kept_record r;
	long long resident;
	uint32_t a, b;

	if (!run || strcmp(run, "1"))
		SKIP(return, "set ASAHI_RESIDENCY_DROP_CACHES=1 to drop the system's caches");
	if (geteuid())
		SKIP(return, "dropping caches needs root");
	a = bound_object(self->fd, self->vm, &self->va, self->va_end, kept_size);
	b = bound_object(self->fd, self->vm, &self->va, self->va_end, size);
	ASSERT_NE(0, a);
	ASSERT_NE(0, b);
	ASSERT_EQ(0, residency(self->fd, a, DRM_ASAHI_GEM_RESIDENCY_KEEP));

	sleep(IDLE_SECONDS);
	trace_start();
	vm.held_at = seconds();
	ASSERT_EQ((long long)(kept_size + size), client_resident(self->fd));
	ASSERT_EQ(0, write_string("/proc/sys/vm/drop_caches", "2"));
	resident = client_resident(self->fd);
	vm.gone_at = seconds();
	EXPECT_EQ(0, resident);
	if (!resident && !read_record(&vm, &r) && r.others) {
		if (r.shrinkers > 1) {
			TH_LOG(
"inconclusive: %d shrinkers of kept objects count what this VM's counts, and %d passes called one of them before the object went (traced)",
			       r.shrinkers, r.released);
		} else {
			EXPECT_GT(r.released, 0);
			TH_LOG(
"reclaim passes that called the shrinker of this VM's kept objects before it went: %d, down to priority %d (traced)",
			       r.released, r.priority);
		}
	}
	/* Released, not purged: the contents come back with the next binding. */
	EXPECT_EQ(1, madvise_object(self->fd, a, DRM_ASAHI_GEM_MADV_WILLNEED));
	EXPECT_EQ(kept0 + (long long)kept_size, gpu_memory_field("user_kept"));
}

/*
 * The trace instance is the program's: made before the cases fork and
 * removed when the program exits, however its cases ended.
 */
int main(int argc, char **argv)
{
	trace_create();
	return test_harness_run(argc, argv);
}
