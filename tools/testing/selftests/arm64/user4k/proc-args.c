// SPDX-License-Identifier: GPL-2.0
/*
 * Compare argv/envp with /proc/PID/cmdline and /proc/PID/environ.
 *
 * Run it once natively and once under the 4K user granule (for example
 * "userpages 4k ./proc-args"). The process reads its own files, and a forked
 * child reads the parent's files through /proc/<ppid>. "--hold SECONDS"
 * keeps the process alive so a native process can read it from outside.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int failures;

static ssize_t read_file(const char *path, char *buf, size_t size)
{
	ssize_t got, total = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -errno;
	while ((size_t)total < size) {
		got = read(fd, buf + total, size - total);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			total = -errno;
			break;
		}
		if (!got)
			break;
		total += got;
	}
	close(fd);
	return total;
}

/* Join the complete vector; reserve one extra byte in the read buffer. */
static char *join(char **vec, size_t *size)
{
	size_t len = 0, offset = 0;
	char **entry, *buf;

	for (entry = vec; *entry; entry++) {
		size_t n = strlen(*entry) + 1;

		if (n > SSIZE_MAX - 1 - len) {
			errno = EOVERFLOW;
			return NULL;
		}
		len += n;
	}
	buf = malloc(len ? len : 1);
	if (!buf)
		return NULL;
	for (entry = vec; *entry; entry++) {
		size_t n = strlen(*entry) + 1;

		memcpy(buf + offset, *entry, n);
		offset += n;
	}
	*size = len;
	return buf;
}

static void check(const char *who, const char *name, pid_t pid, char **vec)
{
	char path[64], *want, *got;
	size_t want_len;
	ssize_t got_len;

	want = join(vec, &want_len);
	got = want ? malloc(want_len + 1) : NULL;
	if (!got) {
		printf("not ok %s %s allocation: %s\n", who, name,
		       strerror(errno));
		free(want);
		failures++;
		return;
	}
	snprintf(path, sizeof(path), "/proc/%d/%s", pid, name);
	/* A trailing byte rejects excess data; exact contents must reach EOF. */
	got_len = read_file(path, got, want_len + 1);
	if (got_len == (ssize_t)want_len && !memcmp(want, got, want_len)) {
		printf("ok %s %s %zd bytes\n", who, path, got_len);
	} else {
		printf("not ok %s %s want %zu got %zd", who, path, want_len,
		       got_len);
		if (got_len < 0)
			printf(" error %s", strerror(-got_len));
		else if (got_len > 0)
			printf(" first [%.*s]",
			       (int)(got_len < 64 ? got_len : 64), got);
		printf("\n");
		failures++;
	}
	free(got);
	free(want);
}

static void show_layout(pid_t pid, char **argv)
{
	char buf[4096], *p, *field;
	ssize_t len;
	int i;

	printf("pid %d AT_PAGESZ %lu argv[0] %p environ[0] %p\n", pid,
	       getauxval(AT_PAGESZ), (void *)argv[0],
	       environ[0] ? (void *)environ[0] : NULL);
	len = read_file("/proc/self/stat", buf, sizeof(buf) - 1);
	if (len <= 0)
		return;
	buf[len] = 0;
	/* Fields after the parenthesised comm start at field 3. */
	p = strrchr(buf, ')');
	if (!p || p[1] != ' ')
		return;
	printf("stat arg_start arg_end env_start env_end:");
	for (i = 3, field = strtok(p + 2, " "); field;
	     field = strtok(NULL, " "), i++)
		if (i >= 48 && i <= 51)
			printf(" %#lx", strtoul(field, NULL, 10));
	printf("\n");
}

int main(int argc, char **argv)
{
	pid_t self = getpid(), child;
	int status;

	setbuf(stdout, NULL);
	show_layout(self, argv);
	check("self", "cmdline", self, argv);
	check("self", "environ", self, environ);

	child = fork();
	if (child < 0)
		return 1;
	if (!child) {
		failures = 0;
		check("child", "cmdline", self, argv);
		check("child", "environ", self, environ);
		_exit(failures ? 1 : 0);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		failures++;

	if (argc > 2 && !strcmp(argv[1], "--hold")) {
		fflush(stdout);
		sleep(atoi(argv[2]));
	}
	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
