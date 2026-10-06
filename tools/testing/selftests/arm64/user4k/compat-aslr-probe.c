/* SPDX-License-Identifier: GPL-2.0-only */
/* Freestanding ELF32 address sampler for the disposable ASLR fixture. */
typedef unsigned int u32;
typedef unsigned long long u64;
struct sample {
	u64 page_size, stack, mapping, brk;
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
static int error(long value)
{
	return (u32)value >= 0xfffff001U;
}
static unsigned number(const char *p)
{
	unsigned n = 0;
	while (*p >= '0' && *p <= '9')
		n = n * 10 + *p++ - '0';
	return n;
}
static char *find(char *p, char c)
{
	while (*p && *p != c)
		p++;
	return *p ? p : (char *)0;
}
static char *find_stack(char *p)
{
	const char name[] = "[stack]";
	while (*p) {
		unsigned i = 0;
		while (name[i] && p[i] == name[i])
			i++;
		if (!name[i])
			return p;
		p++;
	}
	return (char *)0;
}
static unsigned hex(char *p)
{
	unsigned n = 0, v;
	for (;;) {
		if (*p >= '0' && *p <= '9')
			v = *p - '0';
		else if (*p >= 'a' && *p <= 'f')
			v = *p - 'a' + 10;
		else
			return n;
		n = (n << 4) | v;
		p++;
	}
}
static int load(const char *path, char *buf, unsigned size)
{
	long fd = call(5, (long)path, 0, 0, 0, 0, 0), n;
	if (error(fd))
		return 1;
	n = call(3, fd, (long)buf, size - 1, 0, 0, 0);
	call(6, fd, 0, 0, 0, 0, 0);
	if (n <= 0)
		return 1;
	buf[n] = 0;
	return 0;
}
__attribute__((noreturn)) void probe(u32 *sp)
{
	unsigned argc = *sp++;
	char **argv = (void *)sp, buf[4096], *p, *q;
	struct sample sample = { 0 };
	long mapping;
	int bad = 1;
	sp += argc + 1;
	while (*sp)
		sp++;
	sp++;
	while (sp[0]) {
		if (sp[0] == 6)
			sample.page_size = sp[1];
		sp += 2;
	}
	if (argc != 3 || call(64, 0, 0, 0, 0, 0, 0) != 1)
		goto done;
	mapping = call(192, 0, sample.page_size, 3, 0x22, -1, 0);
	if (error(mapping))
		goto done;
	sample.mapping = (u32)mapping;
	if (load("/proc/self/maps", buf, sizeof(buf)))
		goto done;
	p = find_stack(buf);
	if (!p)
		goto done;
	while (p > buf && p[-1] != '\n')
		p--;
	p = find(p, '-');
	if (!p)
		goto done;
	sample.stack = hex(p + 1);
	if (load("/proc/self/stat", buf, sizeof(buf)))
		goto done;
	p = find(buf, ')');
	if (!p)
		goto done;
	while ((q = find(p + 1, ')')))
		p = q;
	p += 2;
	for (unsigned field = 3; field < 47; field++) {
		p = find(p, ' ');
		if (!p)
			goto done;
		p++;
	}
	sample.brk = number(p);
	if (!sample.page_size || !sample.stack || !sample.brk)
		goto done;
	bad = call(4, number(argv[2]), (long)&sample, sizeof(sample), 0, 0,
		   0) != sizeof(sample);
done:
	call(1, bad, 0, 0, 0, 0, 0);
	for (;;) {
	}
}
__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
