// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding AArch32 perf mmap layout, including native-page fallback. */
typedef unsigned int u32;
typedef unsigned long long u64;

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

static unsigned int number(const char *s)
{
	unsigned int n = 0;

	while (*s >= '0' && *s <= '9')
		n = n * 10 + *s++ - '0';
	return n;
}

static _Noreturn void finish(int code)
{
	char text[] = "A32 PERF FAIL stage=00\n";

	if (code) {
		text[20] = '0' + code / 10;
		text[21] = '0' + code % 10;
		call(4, 1, (long)text, sizeof(text) - 1, 0, 0, 0);
	} else {
		static const char pass[] = "A32 PERF PASS\n";

		call(4, 1, (long)pass, sizeof(pass) - 1, 0, 0, 0);
	}
	call(1, code, 0, 0, 0, 0, 0);
	for (;;)
		;
}

static void mapping(unsigned long pages, unsigned long native,
		    unsigned int count)
{
	static const struct {
		u32 type, size;
		u64 config, sample_period, sample_type, read_format, flags;
		u32 wakeup, bp_type;
		u64 config1;
	} attr = { .type = 1, .size = 64, .flags = 1 };
	long fd = call(364, (long)&attr, 0, -1, -1, 0, 0);
	unsigned long length = (1 + count) * pages;
	long raw;
	volatile u64 *metadata;

	if (fd < 0)
		finish(20);
	raw = call(192, 0, length, 3, 1, fd, 0);
	if ((unsigned long)raw >= 0xfffff001UL)
		finish(21);
	metadata = (void *)(raw + 1024);
	if (metadata[2] != pages || metadata[3] != count * pages)
		finish(22);
	metadata[1] = 0;
	metadata[6] = length;
	metadata[7] = native;
	/* mmap2 always takes 4K units, including the no4K compat fallback. */
	if (call(192, 0, native, 1, 1, fd, length / 4096) != -95)
		finish(23);
	if (call(91, raw, length, 0, 0, 0, 0))
		finish(24);
	if (call(6, fd, 0, 0, 0, 0, 0))
		finish(25);
}

_Noreturn void probe(u32 *sp)
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
	if (argc != 3 || pages != number(argv[2]))
		finish(10);
	unsigned long native = number(argv[1]);

	mapping(pages, native, 0);
	mapping(pages, native, 1);
	mapping(pages, native, 2);
	mapping(pages, native, 2 * native / pages);
	finish(0);
}

__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
