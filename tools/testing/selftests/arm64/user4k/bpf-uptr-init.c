/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define SET_EXEC_PAGE_SIZE 0x41555001
#define CHECK(c)                                                            \
	do {                                                                \
		if (!(c)) {                                                 \
			printf("FAIL line %d: %s errno=%d\n", __LINE__, #c, \
			       errno);                                      \
			return 1;                                           \
		}                                                           \
	} while (0)
struct user_data {
	uint64_t a, b, result, marker;
};
struct value {
	struct user_data *p, *q;
	uint64_t cookie;
	uint8_t trailing;
} __attribute__((packed));
static char verifier_log[65536];
static int bpf(enum bpf_cmd cmd, union bpf_attr *a)
{
	size_t size;
#define END(field) (offsetof(union bpf_attr, field) + sizeof(a->field))
	switch (cmd) {
	case BPF_BTF_LOAD:
		size = END(btf_log_level);
		break;
	case BPF_MAP_CREATE:
		size = END(btf_value_type_id);
		break;
	case BPF_PROG_LOAD:
		size = END(log_buf);
		break;
	case BPF_RAW_TRACEPOINT_OPEN:
		size = END(raw_tracepoint.prog_fd);
		break;
	case BPF_MAP_UPDATE_ELEM:
		size = END(flags);
		break;
	case BPF_MAP_LOOKUP_ELEM:
		size = END(flags);
		break;
	case BPF_MAP_DELETE_ELEM:
		size = END(key);
		break;
	default:
		errno = EINVAL;
		return -1;
	}
#undef END
	return syscall(__NR_bpf, cmd, a, size);
}
struct btf_blob {
	unsigned char types[1024];
	char strings[512];
	unsigned nt, ns;
};
static uint32_t str(struct btf_blob *b, const char *s)
{
	unsigned at = b->ns;
	size_t n = strlen(s) + 1;
	memcpy(b->strings + b->ns, s, n);
	b->ns += n;
	return at;
}
static void push(struct btf_blob *b, const void *p, size_t n)
{
	memcpy(b->types + b->nt, p, n);
	b->nt += n;
}
static void type(struct btf_blob *b, const char *name, unsigned kind,
		 unsigned n, unsigned size_type)
{
	struct btf_type t = { .name_off = name ? str(b, name) : 0,
			      .info = kind << 24 | n,
			      .size = size_type };
	push(b, &t, sizeof(t));
}
static void member(struct btf_blob *b, const char *name, unsigned id,
		   unsigned offset)
{
	struct btf_member m = { .name_off = str(b, name),
				.type = id,
				.offset = offset * 8 };
	push(b, &m, sizeof(m));
}
static int create_map(void)
{
	struct btf_blob b = { .ns = 1 };
	uint32_t encoding;
	type(&b, "int", BTF_KIND_INT, 0, 4);
	encoding = BTF_INT_SIGNED << 24 | 32;
	push(&b, &encoding, 4);
	type(&b, "u64", BTF_KIND_INT, 0, 8);
	encoding = 64;
	push(&b, &encoding, 4);
	type(&b, "user_data", BTF_KIND_STRUCT, 4, sizeof(struct user_data));
	member(&b, "a", 2, 0);
	member(&b, "b", 2, 8);
	member(&b, "result", 2, 16);
	member(&b, "marker", 2, 24);
	type(&b, "uptr", BTF_KIND_TYPE_TAG, 0, 3);
	type(&b, NULL, BTF_KIND_PTR, 0, 4);
	type(&b, "value_type", BTF_KIND_STRUCT, 4, sizeof(struct value));
	member(&b, "p", 5, 0);
	member(&b, "q", 5, 8);
	member(&b, "cookie", 2, 16);
	member(&b, "trailing", 7, 24);
	type(&b, "u8", BTF_KIND_INT, 0, 1);
	encoding = 8;
	push(&b, &encoding, 4);
	unsigned char data[2048];
	struct btf_header h = { .magic = BTF_MAGIC,
				.version = BTF_VERSION,
				.hdr_len = sizeof(h),
				.type_len = b.nt,
				.str_off = b.nt,
				.str_len = b.ns };
	memcpy(data, &h, sizeof(h));
	memcpy(data + sizeof(h), b.types, b.nt);
	memcpy(data + sizeof(h) + b.nt, b.strings, b.ns);
	union bpf_attr a = { .btf = (uintptr_t)data,
			     .btf_size = sizeof(h) + b.nt + b.ns,
			     .btf_log_buf = (uintptr_t)verifier_log,
			     .btf_log_size = sizeof(verifier_log),
			     .btf_log_level = 1 };
	int btf = bpf(BPF_BTF_LOAD, &a);
	if (btf < 0) {
		printf("BTF load errno=%d: %s\n", errno, verifier_log);
		return -1;
	}
	a = (union bpf_attr){ .map_type = BPF_MAP_TYPE_TASK_STORAGE,
			      .key_size = sizeof(int),
			      .value_size = sizeof(struct value),
			      .map_flags = BPF_F_NO_PREALLOC,
			      .btf_fd = btf,
			      .btf_key_type_id = 1,
			      .btf_value_type_id = 6 };
	int fd = bpf(BPF_MAP_CREATE, &a);
	close(btf);
	return fd;
}
#define I(c, d, s, o, i)                    \
	((struct bpf_insn){ .code = (c),    \
			    .dst_reg = (d), \
			    .src_reg = (s), \
			    .off = (o),     \
			    .imm = (i) })
static int create_prog(int map, int *link)
{
	struct bpf_insn insns[40];
	unsigned n = 0, jumps[4];
#define EMIT(c, d, s, o, i) (insns[n++] = I(c, d, s, o, i))
	EMIT(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_get_current_task_btf);
	EMIT(BPF_ALU64 | BPF_MOV | BPF_X, 2, 0, 0, 0);
	EMIT(BPF_LD | BPF_DW | BPF_IMM, 1, BPF_PSEUDO_MAP_FD, 0, map);
	EMIT(0, 0, 0, 0, 0);
	EMIT(BPF_ALU64 | BPF_MOV | BPF_K, 3, 0, 0, 0);
	EMIT(BPF_ALU64 | BPF_MOV | BPF_K, 4, 0, 0, 0);
	EMIT(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_task_storage_get);
	jumps[0] = n;
	EMIT(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 0, 0);
	EMIT(BPF_ALU64 | BPF_MOV | BPF_X, 6, 0, 0, 0);
	EMIT(BPF_LDX | BPF_DW | BPF_MEM, 7, 6, 16, 0);
	jumps[1] = n;
	EMIT(BPF_JMP | BPF_JEQ | BPF_K, 7, 0, 0, 0);
	EMIT(BPF_LDX | BPF_DW | BPF_MEM, 1, 6, 0, 0);
	jumps[2] = n;
	EMIT(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 0, 0);
	EMIT(BPF_LDX | BPF_DW | BPF_MEM, 2, 1, 0, 0);
	EMIT(BPF_ALU64 | BPF_XOR | BPF_X, 2, 7, 0, 0);
	EMIT(BPF_STX | BPF_DW | BPF_MEM, 1, 2, 16, 0);
	EMIT(BPF_STX | BPF_DW | BPF_MEM, 1, 7, 24, 0);
	insns[jumps[2]].off = n - jumps[2] - 1;
	EMIT(BPF_LDX | BPF_DW | BPF_MEM, 1, 6, 8, 0);
	jumps[3] = n;
	EMIT(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 0, 0);
	EMIT(BPF_LDX | BPF_DW | BPF_MEM, 2, 1, 8, 0);
	EMIT(BPF_ALU64 | BPF_ADD | BPF_X, 2, 7, 0, 0);
	EMIT(BPF_STX | BPF_DW | BPF_MEM, 1, 2, 16, 0);
	EMIT(BPF_STX | BPF_DW | BPF_MEM, 1, 7, 24, 0);
	for (unsigned i = 0; i < 4; i++)
		if (i != 2)
			insns[jumps[i]].off = n - jumps[i] - 1;
	EMIT(BPF_ALU64 | BPF_MOV | BPF_K, 0, 0, 0, 0);
	EMIT(BPF_JMP | BPF_EXIT, 0, 0, 0, 0);
#undef EMIT
	union bpf_attr a = { .prog_type = BPF_PROG_TYPE_RAW_TRACEPOINT,
			     .insn_cnt = n,
			     .insns = (uintptr_t)insns,
			     .license = (uintptr_t)"GPL",
			     .log_buf = (uintptr_t)verifier_log,
			     .log_size = sizeof(verifier_log),
			     .log_level = 1 };
	verifier_log[0] = 0;
	int fd = bpf(BPF_PROG_LOAD, &a);
	if (fd < 0) {
		printf("prog load errno=%d: %s\n", errno, verifier_log);
		return -1;
	}
	a = (union bpf_attr){};
	a.raw_tracepoint.name = (uintptr_t)"sys_enter";
	a.raw_tracepoint.prog_fd = fd;
	*link = bpf(BPF_RAW_TRACEPOINT_OPEN, &a);
	if (*link < 0) {
		close(fd);
		return -1;
	}
	return fd;
}
static int update(int map, int key, struct value *v, unsigned flags)
{
	union bpf_attr a = { .map_fd = map,
			     .key = (uintptr_t)&key,
			     .value = (uintptr_t)v,
			     .flags = flags };
	return bpf(BPF_MAP_UPDATE_ELEM, &a);
}
static int delete(int map, int key)
{
	union bpf_attr a = { .map_fd = map, .key = (uintptr_t)&key };
	return bpf(BPF_MAP_DELETE_ELEM, &a);
}
struct pins {
	unsigned long long acquired, released;
};
static int pin_stats(struct pins *p)
{
	int fd = open("/proc/sys/vm/stat_refresh", O_RDONLY);
	char c;
	if (fd < 0)
		return -1;
	int ret = read(fd, &c, 1);
	close(fd);
	if (ret < 0)
		return -1;
	FILE *f = fopen("/proc/vmstat", "r");
	if (!f)
		return -1;
	char name[128];
	unsigned long long value;
	unsigned found = 0;
	while (fscanf(f, "%127s %llu", name, &value) == 2) {
		if (!strcmp(name, "nr_foll_pin_acquired")) {
			p->acquired = value;
			found |= 1;
		}
		if (!strcmp(name, "nr_foll_pin_released")) {
			p->released = value;
			found |= 2;
		}
	}
	fclose(f);
	return found == 3 ? 0 : -1;
}
static int wait_pins(struct pins base, unsigned active)
{
	struct pins p = {};
	for (unsigned i = 0; i < 500; i++) {
		if (pin_stats(&p))
			return 0;
		if (p.acquired - base.acquired >= active &&
		    p.acquired - base.acquired - (p.released - base.released) ==
			    active)
			return 1;
		usleep(10000);
	}
	printf("pin delta acquired=%llu released=%llu expected-active=%u\n",
	       p.acquired - base.acquired, p.released - base.released, active);
	return 0;
}
static int filled(unsigned char *p, size_t len, unsigned char value)
{
	for (size_t i = 0; i < len; i++)
		if (p[i] != value)
			return 0;
	return 1;
}
static int values(struct user_data *p, struct user_data *q, uint64_t cookie)
{
	return p->result == (p->a ^ cookie) && q->result == q->b + cookie &&
	       p->marker == cookie && q->marker == cookie;
}
static int exercise_case(size_t ps, unsigned shared)
{
	struct pins base;
	CHECK(!pin_stats(&base));
	int backing = -1;
	if (shared) {
		backing = memfd_create("bpf-uptr", 0);
		CHECK(backing >= 0 && !ftruncate(backing, 4 * ps));
	}
	unsigned char *src = mmap(
		(void *)0x20000000, 4 * ps, PROT_READ | PROT_WRITE,
		MAP_FIXED_NOREPLACE |
			(shared ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS),
		backing, 0);
	unsigned char *dst =
		mmap((void *)0x30000000, 4 * ps, PROT_READ | PROT_WRITE,
		     MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(src != MAP_FAILED && dst != MAP_FAILED);
	for (unsigned i = 0; i < 4; i++)
		memset(src + i * ps, 0x61 + i, ps);
	memset(dst, 0x91, 4 * ps);
	CHECK(mremap(src + 3 * ps, ps, ps, MREMAP_MAYMOVE | MREMAP_FIXED,
		     dst + ps) == dst + ps);
	unsigned char *leaf = dst + ps;
	struct user_data *p = (void *)(leaf + ps - 64), *q = p + 1;
	*p = (struct user_data){ .a = 0x12345678, .b = 3 };
	*q = (struct user_data){ .a = 4, .b = 0x87654321 };
	int map = create_map();
	CHECK(map >= 0);
	int link = -1, prog = create_prog(map, &link);
	CHECK(prog >= 0 && link >= 0);
	int key = syscall(SYS_pidfd_open, getpid(), 0);
	CHECK(key >= 0);
	struct value probe = {};
	int probe_ret = update(map, key, &probe, BPF_NOEXIST);
	CHECK(!probe_ret);
	CHECK(!delete(map, key));
	struct value v = {
		.p = p, .q = q, .cookie = 0xabc001, .trailing = 0x7d
	};
	CHECK(!update(map, key, &v, BPF_NOEXIST));
	CHECK(wait_pins(base, 2));
	syscall(SYS_getpid);
	CHECK(values(p, q, v.cookie));
	for (unsigned i = 0; i < 64; i++) {
		v.cookie++;
		CHECK(!update(map, key, &v, BPF_EXIST));
		syscall(SYS_getpid);
		CHECK(values(p, q, v.cookie));
	}
	CHECK(wait_pins(base, 2));
	struct {
		struct value v;
		unsigned char tail[16];
	} lookup;
	memset(&lookup, 0xa5, sizeof(lookup));
	union bpf_attr a = { .map_fd = map,
			     .key = (uintptr_t)&key,
			     .value = (uintptr_t)&lookup };
	CHECK(!bpf(BPF_MAP_LOOKUP_ELEM, &a));
	CHECK(!lookup.v.p && !lookup.v.q && lookup.v.cookie == v.cookie &&
	      lookup.v.trailing == v.trailing &&
	      filled(lookup.tail, sizeof(lookup.tail), 0xa5));
	errno = 0;
	CHECK(update(map, key, &v, BPF_NOEXIST) == -1 && errno == EEXIST);
	CHECK(wait_pins(base, 2));
	struct value half = v;
	half.p = NULL;
	CHECK(!update(map, key, &half, BPF_EXIST));
	CHECK(wait_pins(base, 1));
	p->result = p->marker = 0;
	syscall(SYS_getpid);
	CHECK(!p->result && !p->marker && q->result == q->b + v.cookie);
	CHECK(!update(map, key, &v, BPF_EXIST));
	CHECK(wait_pins(base, 2));
	struct value bad = v;
	bad.q = (void *)5;
	errno = 0;
	CHECK(update(map, key, &bad, BPF_EXIST) == -1 && errno == EFAULT);
	CHECK(wait_pins(base, 2));
	syscall(SYS_getpid);
	CHECK(values(p, q, v.cookie));
	bad.q = (void *)(leaf + ps - 16);
	errno = 0;
	CHECK(update(map, key, &bad, BPF_EXIST) == -1 && errno == EOPNOTSUPP);
	CHECK(wait_pins(base, 2));
	bad.q = (void *)(UINTPTR_MAX - 15);
	errno = 0;
	CHECK(update(map, key, &bad, BPF_EXIST) == -1 && errno == EFAULT);
	CHECK(wait_pins(base, 2));
	CHECK(!mprotect(leaf, ps, PROT_READ));
	errno = 0;
	CHECK(update(map, key, &v, BPF_EXIST) == -1 && errno == EFAULT);
	CHECK(!mprotect(leaf, ps, PROT_READ | PROT_WRITE));
	CHECK(wait_pins(base, 2));
	struct value empty = {};
	CHECK(!update(map, key, &empty, BPF_EXIST));
	CHECK(wait_pins(base, 0));
	p->result = q->result = p->marker = q->marker = 0;
	syscall(SYS_getpid);
	CHECK(!p->result && !q->result);
	CHECK(!update(map, key, &v, BPF_EXIST));
	CHECK(wait_pins(base, 2));
	/* The child owns storage pointing into the parent's pinned memory. */
	int barrier[2];
	CHECK(!pipe(barrier));
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(barrier[1]);
		char token;
		if (read(barrier[0], &token, 1) != 1)
			_exit(3);
		if (!shared) {
			p->a = 1;
			q->b = 2;
		}
		syscall(SYS_getpid);
		_exit(0);
	}
	close(barrier[0]);
	int childkey = syscall(SYS_pidfd_open, child, 0);
	CHECK(childkey >= 0);
	CHECK(!update(map, childkey, &v, BPF_NOEXIST));
	CHECK(!delete(map, key));
	CHECK(wait_pins(base, 2));
	p->result = q->result = p->marker = q->marker = 0;
	CHECK(write(barrier[1], "x", 1) == 1);
	close(barrier[1]);
	int status;
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	      !WEXITSTATUS(status));
	close(childkey);
	CHECK(values(p, q, v.cookie) && p->a == 0x12345678 &&
	      q->b == 0x87654321);
	CHECK(wait_pins(base, 0));
	/* The supplying mm exits while a different task retains its UPTRs. */
	int foreign_pipe[2];
	CHECK(!pipe(foreign_pipe));
	pid_t supplier = fork();
	CHECK(supplier >= 0);
	if (!supplier) {
		close(foreign_pipe[0]);
		*p = (struct user_data){ .a = 0x1111, .b = 3 };
		*q = (struct user_data){ .a = 4, .b = 0x2222 };
		struct value foreign = { .p = p, .q = q, .cookie = 0x765432 };
		if (update(map, key, &foreign, BPF_NOEXIST))
			_exit(4);
		struct iovec data = { .iov_base = p, .iov_len = 64 };
		if (vmsplice(foreign_pipe[1], &data, 1, 0) != 64)
			_exit(5);
		_exit(0);
	}
	close(foreign_pipe[1]);
	CHECK(waitpid(supplier, &status, 0) == supplier && WIFEXITED(status) &&
	      !WEXITSTATUS(status));
	CHECK(wait_pins(base, 2));
	syscall(SYS_getpid);
	struct user_data retired[2];
	size_t done = 0;
	while (done < sizeof(retired)) {
		ssize_t got = read(foreign_pipe[0], (char *)retired + done,
				   sizeof(retired) - done);
		CHECK(got > 0);
		done += got;
	}
	close(foreign_pipe[0]);
	CHECK(retired[0].a == 0x1111 && retired[1].b == 0x2222 &&
	      values(retired, retired + 1, 0x765432));
	CHECK(!delete(map, key) && wait_pins(base, 0));
	*p = (struct user_data){ .a = 0x12345678, .b = 3 };
	*q = (struct user_data){ .a = 4, .b = 0x87654321 };
	CHECK(!update(map, key, &v, BPF_NOEXIST));
	CHECK(wait_pins(base, 2));
	CHECK(filled(leaf, ps - 64, 0x64));
	int pipefd[2];
	CHECK(!pipe(pipefd));
	struct iovec iov = { .iov_base = leaf, .iov_len = ps };
	CHECK(vmsplice(pipefd[1], &iov, 1, 0) == (ssize_t)ps);
	CHECK(!munmap(leaf, ps));
	CHECK(mmap(leaf, ps, PROT_READ | PROT_WRITE,
		   MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1,
		   0) == leaf);
	memset(leaf, 0xe7, ps);
	unsigned char *old = malloc(ps);
	CHECK(old);
	size_t n = 0;
	while (n < ps) {
		ssize_t got = read(pipefd[0], old + n, ps - n);
		CHECK(got > 0);
		n += got;
	}
	close(pipefd[0]);
	close(pipefd[1]);
	CHECK(filled(old, ps - 64, 0x64) &&
	      values((void *)(old + ps - 64), (void *)(old + ps - 32),
		     v.cookie));
	CHECK(filled(leaf, ps, 0xe7));
	free(old);
	/* Closing the map alone is insufficient: the program retains its map ref. */
	close(map);
	close(link);
	close(prog);
	close(key);
	CHECK(wait_pins(base, 0));
	CHECK(filled(dst, ps, 0x91) && filled(dst + 2 * ps, 2 * ps, 0x91));
	for (unsigned i = 0; i < 3; i++)
		CHECK(filled(src + i * ps, ps, 0x61 + i));
	CHECK(!munmap(src, 3 * ps) && !munmap(dst, 4 * ps));
	if (backing >= 0)
		close(backing);
	printf("ok - %zuK BPF UPTR %s: relocated writes, updates, rollback, storage/supplier exit, unmap and pin balance\n",
	       ps / 1024, shared ? "shared" : "private");
	return 0;
}
static int exercise(size_t ps)
{
	CHECK(getauxval(AT_PAGESZ) == ps);
	alarm(90);
	for (unsigned shared = 0; shared < 2; shared++)
		CHECK(!exercise_case(ps, shared));
	return 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 3 && !strcmp(argv[1], "--exercise")) {
		if (getppid() != 1)
			return 2;
		return exercise(strtoul(argv[2], NULL, 10));
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Disposable VM PID1 only\n");
		return 2;
	}
	int fail = 0;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)) {
		perror("mount");
		fail = 1;
		goto done;
	}
	size_t sizes[] = { getauxval(AT_PAGESZ), 4096, 16384 };
	for (unsigned i = 0; i < (sizes[0] > 16384 ? 3U : 2U); i++) {
		pid_t c = fork();
		int status = 0;
		if (!c) {
			char size[24];
			snprintf(size, sizeof(size), "%zu", sizes[i]);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(4);
			execl("/init", "/init", "--exercise", size, NULL);
			_exit(5);
		}
		int ok = 0;
		for (unsigned ticks = 0; c > 0 && ticks < 1000; ticks++) {
			pid_t got = waitpid(c, &status, WNOHANG);
			if (got == c) {
				ok = WIFEXITED(status) && !WEXITSTATUS(status);
				break;
			}
			if (ticks == 999) {
				kill(c, SIGKILL);
				waitpid(c, &status, 0);
				break;
			}
			usleep(100000);
		}
		printf("%s - ABI %zuK BPF UPTR status=%#x\n",
		       ok ? "ok" : "not ok", sizes[i] / 1024, status);
		fail += !ok;
	}
done:
	printf("BPF UPTR %s\n", fail ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
