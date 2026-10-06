// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding ELF32 half of compat-uffd-init; no armhf sysroot required. */
typedef unsigned int u32;
typedef unsigned long long u64;
struct api {
	u64 api, features, ioctls;
};
struct registration {
	u64 start, len, mode, ioctls;
};
struct movement {
	u64 dst, src, len, mode;
	long long moved;
};
struct wire {
	u32 address, pagesize;
};
struct iovec32 {
	void *base;
	u32 len;
};
struct message32 {
	void *name;
	u32 namelen;
	struct iovec32 *iov;
	u32 iovlen;
	void *control;
	u32 controllen, flags;
};
struct rights32 {
	u32 len, level, type;
	int fd;
};
static long call(long n, long a, long b, long c, long d, long e, long f)
{
	register long r0 __asm__("r0") = a, r1 __asm__("r1") = b,
			 r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d, r4 __asm__("r4") = e,
			 r5 __asm__("r5") = f;
	register long r7 __asm__("r7") = n;
	__asm__ volatile("svc 0"
			 : "+r"(r0)
			 : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
			 : "memory", "cc");
	return r0;
}
static u32 number(const char *s)
{
	u32 n = 0;
	while (*s >= '0' && *s <= '9')
		n = n * 10 + *s++ - '0';
	return n;
}
__attribute__((noreturn)) static void finish(int code)
{
	call(1, code, 0, 0, 0, 0, 0);
	for (;;)
		;
}
__attribute__((noreturn)) void probe(u32 *sp)
{
	u32 argc = *sp++, pages = 0;
	char **argv = (void *)sp;
	sp += argc + 1;
	while (*sp)
		sp++;
	sp++;
	while (sp[0]) {
		if (sp[0] == 6)
			pages = sp[1];
		sp += 2;
	}
	if (argc != 4 || pages != number(argv[1]))
		finish(10);
	int sock = number(argv[2]), shared = argv[3][0] != '-';
	int file = shared ? (int)number(argv[3]) : -1;
	long map = call(192, 0, 3 * pages, 3, shared ? 1 : 0x22, file, 0);
	if ((unsigned long)map >= 0xfffff001UL)
		finish(11);
	/* Fine operations must retain native alignment restrictions on fallback. */
	if (call(125, map + 4096, 4096, 1, 0, 0, 0) !=
	    (pages == 4096 ? 0 : -22))
		finish(12);
	if (call(125, map, 3 * pages, 3, 0, 0, 0))
		finish(13);
	long fd = call(388, 0x80801, 0, 0, 0, 0,
		       0); /* CLOEXEC|NONBLOCK|USER_MODE_ONLY */
	if (fd < 0)
		finish(14);
	/* Negotiate shared missing support under both fine and fallback ABIs. */
	struct api api = { 0xaa, (1ULL << 5) | (1ULL << 16), 0 };
	if (call(54, fd, 0xc018aa3fU, (long)&api, 0, 0, 0) ||
	    !(api.features & (1ULL << 5)))
		finish(15);
	struct registration reg = { (u32)map, 3 * pages, 1, 0 };
	if (call(54, fd, 0xc020aa00U, (long)&reg, 0, 0, 0))
		finish(16);
	if (!(reg.ioctls & (1ULL << 3)) || !(api.features & (1ULL << 16)))
		finish(17); /* COPY and MOVE */
	struct wire data = { (u32)map, pages };
	struct iovec32 iov = { &data, sizeof(data) };
	struct rights32 rights = { sizeof(rights), 1, 1, fd };
	struct message32 msg = { 0, 0, &iov, 1, &rights, sizeof(rights), 0 };
	if (call(296, sock, (long)&msg, 0, 0, 0, 0) != sizeof(data))
		finish(18);
	volatile unsigned char *p = (void *)map;
	/* Each first access waits for the native pager's COPY. */
	for (u32 leaf = 0; leaf < 3; leaf++)
		for (u32 i = 0; i < pages; i++)
			if (p[leaf * pages + i] != (unsigned char)(0x51 + leaf))
				finish(19);
	if (!shared) {
		long target = call(192, 0, 3 * pages, 3, 0x22, -1, 0);
		if ((unsigned long)target >= 0xfffff001UL)
			finish(21);
		struct registration dst = { (u32)target, 3 * pages, 1, 0 };
		if (call(54, fd, 0xc020aa00U, (long)&dst, 0, 0, 0) ||
		    !(dst.ioctls & (1ULL << 5)))
			finish(22);
		struct movement m = { 1ULL << 32, (u32)map + 2 * pages, pages,
				      0, 0 };
		if (call(54, fd, 0xc028aa05U, (long)&m, 0, 0, 0) != -22)
			finish(23);
		m.dst = (u32)target + pages;
		if (pages > 4096) {
			m.len = 4096;
			if (call(54, fd, 0xc028aa05U, (long)&m, 0, 0, 0) != -22)
				finish(24);
			m.len = pages;
		}
		if (call(54, fd, 0xc028aa05U, (long)&m, 0, 0, 0) ||
		    m.moved != pages)
			finish(25);
		volatile unsigned char *q = (void *)target;
		for (u32 i = 0; i < pages; i++)
			if (q[pages + i] != 0x53 || p[i] != 0x51 ||
			    p[pages + i] != 0x52)
				finish(26);
		if (call(91, target, 3 * pages, 0, 0, 0, 0))
			finish(27);
		static const char ok[] =
			"ok - AArch32 MOVE, neighbours and native-fallback alignment\n";
		call(4, 1, (long)ok, sizeof(ok) - 1, 0, 0, 0);
	}
	if (call(91, map, 3 * pages, 0, 0, 0, 0))
		finish(20);
	finish(0);
}
__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
