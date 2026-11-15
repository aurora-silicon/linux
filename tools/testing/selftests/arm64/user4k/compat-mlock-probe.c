// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding ELF32 memlock probe, only for the disposable PID1 fixture. */
typedef unsigned int u32;
struct result {
	u32 pagesize, passed, failed, error;
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
static int error(long n)
{
	return (u32)n >= 0xfffff001U;
}
static unsigned number(const char *p)
{
	unsigned n = 0;
	while (*p >= '0' && *p <= '9')
		n = n * 10 + *p++ - '0';
	return n;
}
static long locked_kb(void)
{
	char buf[8192];
	long fd = call(5, (long)"/proc/self/status", 0, 0, 0, 0, 0), n;
	const char name[] = "VmLck:";

	if (error(fd))
		return -1;
	n = call(3, fd, (long)buf, sizeof(buf) - 1, 0, 0, 0);
	call(6, fd, 0, 0, 0, 0, 0);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	for (long i = 0; i < n; i++) {
		unsigned j = 0;
		while (name[j] && i + j < (unsigned)n && buf[i + j] == name[j])
			j++;
		if (!name[j]) {
			const char *p = buf + i + j;
			while (*p == ' ' || *p == '\t')
				p++;
			return number(p);
		}
	}
	return -1;
}
__attribute__((noreturn)) void probe(u32 *sp)
{
	unsigned argc = *sp++, ps = 0, kb, stage = 1;
	char **argv = (void *)sp;
	struct result out = { 0 };
	long p, q, pid, ret = -1;
	int status = -1;

	sp += argc + 1;
	while (*sp)
		sp++;
	sp++;
	while (sp[0]) {
		if (sp[0] == 6)
			ps = sp[1];
		sp += 2;
	}
	out.pagesize = ps;
	if (argc != 2 || call(64, 0, 0, 0, 0, 0, 0) != 1 ||
	    ps != number(argv[1]))
		goto done;
	kb = ps / 1024;
	p = call(192, 0, 8 * ps, 3, 0x22, -1, 0);
	if (error(p) || ((u32)p & (ps - 1)) || locked_kb() != 0)
		goto done;
	ret = call(150, p + ps + 17, ps - 32, 0, 0, 0, 0);
	if (ret || locked_kb() != (long)kb)
		goto done;
	out.passed++;
	stage++;
	ret = call(150, p + ps, ps, 0, 0, 0, 0);
	if (ret || locked_kb() != (long)kb)
		goto done;
	ret = call(150, p + 2 * ps, ps, 0, 0, 0, 0);
	if (ret || locked_kb() != (long)(2 * kb))
		goto done;
	ret = call(151, p + ps, ps, 0, 0, 0, 0);
	if (ret || locked_kb() != (long)kb)
		goto done;
	out.passed++;
	stage++;
	*(volatile unsigned char *)(p + 2 * ps) = 0x63;
	q = call(163, p + 2 * ps, ps, 2 * ps, 1, 0, 0);
	ret = q;
	if (error(q) || locked_kb() != (long)(2 * kb) ||
	    *(volatile unsigned char *)q != 0x63)
		goto done;
	for (unsigned i = ps; i < 2 * ps; i++)
		if (((volatile unsigned char *)q)[i])
			goto done;
	out.passed++;
	stage++;
	pid = call(2, 0, 0, 0, 0, 0, 0);
	if (!pid) {
		call(1, locked_kb() != 0, 0, 0, 0, 0, 0);
		for (;;) {
		}
	}
	ret = call(114, pid, (long)&status, 0, 0, 0, 0);
	if (error(pid) || ret != pid || status || locked_kb() != (long)(2 * kb))
		goto done;
	out.passed++;
	stage++;
	ret = call(91, q, 2 * ps, 0, 0, 0, 0);
	if (ret || locked_kb() != 0)
		goto done;
	out.passed++;
	stage++;
	ret = call(390, p + 4 * ps, 2 * ps, 1, 0, 0, 0);
	if (ret || locked_kb() != (long)(2 * kb))
		goto done;
	*(volatile unsigned char *)(p + 4 * ps) = 0x75;
	ret = call(151, p + 4 * ps, 2 * ps, 0, 0, 0, 0);
	if (ret || locked_kb() != 0)
		goto done;
	out.passed++;
	stage = 0;
done:
	out.failed = stage;
	out.error = stage ? (u32)ret : 0;
	call(4, 1, (long)&out, sizeof(out), 0, 0, 0);
	call(1, stage != 0, 0, 0, 0, 0, 0);
	for (;;) {
	}
}
__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
