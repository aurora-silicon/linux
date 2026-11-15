/* SPDX-License-Identifier: GPL-2.0-only */
/* Freestanding AArch32 process; run only by compat-fixture-init in a guest. */
typedef unsigned int u32;
typedef unsigned short u16;
static long call(long n, long a, long b, long c, long d, long e, long f)
{
	register long r0 __asm__("r0") = a, r1 __asm__("r1") = b,
			 r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d, r4 __asm__("r4") = e,
			 r5 __asm__("r5") = f, r7 __asm__("r7") = n;
	__asm__ volatile("svc 0"
			 : "+r"(r0)
			 : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
			 : "memory", "cc");
	return r0;
}
static unsigned length(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}
static int equal(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}
static unsigned number(const char *s)
{
	unsigned n = 0;
	while (*s >= '0' && *s <= '9')
		n = n * 10 + *s++ - '0';
	return n;
}
static void out(const char *s)
{
	call(4, 1, (long)s, length(s), 0, 0, 0);
}
static int failures, checks;
static void result(int pass, const char *s)
{
	checks++;
	if (!pass)
		failures++;
	out(pass ? "ok - compat " : "not ok - compat ");
	out(s);
	out("\n");
}
static int error(long v)
{
	return (unsigned long)v >= 0xfffff001UL;
}
static long map(unsigned size, int prot, int flags, int fd, unsigned off)
{
	return call(192, 0, size, prot, flags, fd, off);
}
static void unmap(long p, unsigned size)
{
	if (!error(p))
		call(91, p, size, 0, 0, 0, 0);
}
static volatile unsigned signals;
static void handler(int n)
{
	if (n == 10)
		signals++;
}
struct action {
	u32 handler, flags, restorer, mask[2];
};
struct ehdr {
	unsigned char ident[16];
	u16 type, machine;
	u32 version, entry, phoff, shoff, flags;
	u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr {
	u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
struct dyn {
	u32 tag, val;
};
struct sym {
	u32 name, value, size;
	unsigned char info, other;
	u16 shndx;
};
static void *vdso_symbol(unsigned base, const char *name)
{
	struct ehdr *h = (void *)base;
	struct phdr *p = (void *)(base + h->phoff);
	struct dyn *d = 0;
	char *strings = 0;
	struct sym *symbols = 0;
	u32 *hash = 0;
	for (unsigned i = 0; i < h->phnum; i++)
		if (p[i].type == 2)
			d = (void *)(base + p[i].vaddr);
	if (!d)
		return 0;
	for (; d->tag; d++) {
		if (d->tag == 5)
			strings = (void *)(base + d->val);
		if (d->tag == 6)
			symbols = (void *)(base + d->val);
		if (d->tag == 4)
			hash = (void *)(base + d->val);
	}
	if (!strings || !symbols || !hash)
		return 0;
	for (unsigned i = 0; i < hash[1]; i++)
		if (equal(strings + symbols[i].name, name))
			return (void *)(base + symbols[i].value);
	return 0;
}
__attribute__((noreturn)) void probe(u32 *sp)
{
	unsigned argc = *sp++, pagesz = 0, vdso = 0;
	char **argv = (void *)sp;
	sp += argc + 1;
	char **env = (void *)sp;
	while (*sp)
		sp++;
	sp++;
	while (sp[0]) {
		if (sp[0] == 6)
			pagesz = sp[1];
		if (sp[0] == 33)
			vdso = sp[1];
		sp += 2;
	}
	if (argc > 1 && equal(argv[1], "--exec32")) {
		char *a[] = { argv[0], argv[2], argv[3], argv[4], 0 };
		call(11, (long)a[0], (long)a, (long)env, 0, 0, 0);
		call(1, 97, 0, 0, 0, 0, 0);
		for (;;)
			;
	}
	if (argc > 1 &&
	    (equal(argv[1], "--exec64") || equal(argv[1], "--exec64-4k"))) {
		int alternative = equal(argv[1], "--exec64-4k");
		if (alternative && call(172, 0x41555001, 4096, 0, 0, 0, 0)) {
			call(1, 99, 0, 0, 0, 0, 0);
			for (;;)
				;
		}
		char *a[] = { "/init",
			      alternative ? "--returned4k" : "--returned", 0 };
		call(11, (long)a[0], (long)a, (long)env, 0, 0, 0);
		call(1, 98, 0, 0, 0, 0, 0);
		for (;;)
			;
	}
	/* Script interpreter contributes its pathname before the ordinary arguments. */
	unsigned start = argc > 1 && equal(argv[1], "/script32") ? 2 : 1;
	unsigned expected = argc > start ? number(argv[start]) : 0;
	int fd = argc > start + 1 ? (int)number(argv[start + 1]) : -1;
	result(pagesz == expected, "AT_PAGESZ reflects final ELF32 selection");
	int args = argc > start + 2 && length(argv[start + 2]) == 90000;
	if (args)
		for (unsigned i = 0; i < 90000; i++)
			if (argv[start + 2][i] != (char)('a' + i % 23)) {
				args = 0;
				break;
			}
	result(args, "90KB staged argument survives granule transfer");
	long mem = map(4 * pagesz, 3, 0x22, -1, 0);
	int ok = !error(mem);
	if (ok) {
		unsigned char *p = (void *)mem;
		for (unsigned i = 0; i < 4 * pagesz; i++)
			p[i] = 0x30 + i / pagesz;
		for (unsigned i = 0; i < 4 * pagesz; i++)
			if (p[i] != (unsigned char)(0x30 + i / pagesz))
				ok = 0;
	}
	result(ok, "anonymous user leaves have independent bytes");
	result(!error(mem) && !call(125, mem + pagesz, pagesz, 1, 0, 0, 0) &&
		       !call(125, mem + pagesz, pagesz, 3, 0, 0, 0),
	       "mprotect at selected page boundaries");
	result(!error(mem) && call(125, mem + 1, pagesz, 1, 0, 0, 0) == -22 &&
		       call(91, mem + 1, pagesz, 0, 0, 0, 0) == -22,
	       "unaligned memory operations retain restrictions");
	long q = map(pagesz, 1, 1, fd, 3 * (pagesz >> 12));
	ok = !error(q);
	if (ok)
		for (unsigned i = 0; i < pagesz; i++)
			if (((unsigned char *)q)[i] !=
			    (unsigned char)(0x40 + (3 * pagesz + i) / 4096))
				ok = 0;
	result(ok, "mmap2 native-aligned file offset");
	unmap(q, pagesz);
	q = map(4096, 1, 1, fd, 1);
	ok = pagesz == 4096 ? !error(q) && *(unsigned char *)q == 0x41 :
			      q == -22;
	result(ok, "mmap2 4KB offset accepted only by matching ABI");
	unmap(q, pagesz);
	q = map(pagesz, 1, 1, fd, 0x100000 + (pagesz >> 12));
	ok = !error(q);
	if (ok)
		for (unsigned i = 0; i < pagesz; i++)
			if (((unsigned char *)q)[i] !=
			    (unsigned char)(0x80 + (pagesz + i) / 4096))
				ok = 0;
	result(ok, "mmap2 offset above 4GB remains full width");
	unmap(q, pagesz);
	long child = call(2, 0, 0, 0, 0, 0, 0);
	int status = 0;
	if (!child) {
		if (!error(mem))
			*(unsigned char *)mem = 0xa7;
		call(1, 0, 0, 0, 0, 0, 0);
		for (;;)
			;
	}
	ok = child > 0 &&
	     call(114, child, (long)&status, 0, 0, 0, 0) == child && !status &&
	     !error(mem) && *(unsigned char *)mem == 0x30;
	result(ok, "fork isolates private COW data");
	unmap(mem, 4 * pagesz);
	struct action act = { (u32)handler, 0, 0, { 0, 0 } };
	ok = !call(174, 10, (long)&act, 0, 8, 0, 0);
	if (ok)
		ok = !call(37, call(20, 0, 0, 0, 0, 0, 0), 10, 0, 0, 0, 0) &&
		     signals == 1;
	result(ok, "AArch32 signal frame and kernel sigreturn trampoline");
	volatile unsigned word = 17;
	int (*cmpxchg)(unsigned, unsigned, volatile unsigned *) =
		(void *)0xffff0fc0;
	result(*(volatile unsigned *)0xffff0ffc >= 5 &&
		       !cmpxchg(17, 29, &word) && word == 29,
	       "kuser helper page and atomic compare exchange");
	int (*gettime)(int, void *) =
		vdso ? (void *)vdso_symbol(vdso, "__vdso_clock_gettime") : 0;
	long time[2] = { 0, 0 };
	result(gettime && !gettime(1, time) && time[0] >= 0 && time[1] >= 0 &&
		       time[1] < 1000000000,
	       "compat vDSO clock_gettime and vvar mapping");
	out(failures ? "COMPAT PROBE FAIL\n" : "COMPAT PROBE PASS\n");
	call(1, failures ? 1 : 0, 0, 0, 0, 0, 0);
	for (;;)
		;
}
__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
