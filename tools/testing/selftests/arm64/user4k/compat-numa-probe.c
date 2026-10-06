// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding AArch32 NUMA policy and native-granule fallback checks. */
typedef unsigned int u32;
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
	char text[] = "A32 NUMA FAIL stage=00\n";

	if (code) {
		text[sizeof(text) - 4] = '0' + code / 10;
		text[sizeof(text) - 3] = '0' + code % 10;
		call(4, 1, (long)text, sizeof(text) - 1, 0, 0, 0);
	} else {
		static const char pass[] = "A32 NUMA PASS\n";

		call(4, 1, (long)pass, sizeof(pass) - 1, 0, 0, 0);
	}
	call(1, code, 0, 0, 0, 0, 0);
	for (;;)
		;
}

static void mapping(unsigned long native, unsigned long page)
{
	unsigned long length = 4 * native;
	long raw = call(192, 0, length, 3, 0x22, -1, 0);
	unsigned char *base;
	u32 mask[2] = { 1, 0 }, got[2] = { 0, 0 };
	int mode = -1, node;

	if ((unsigned long)raw >= 0xfffff001UL)
		finish(20);
	base = (void *)(((unsigned long)raw + native - 1) & ~(native - 1));
	if (call(320, (long)&mode, (long)got, 64, 0, 4, 0) || got[0] != 3 ||
	    got[1])
		finish(21);
	if (call(319, (long)base, 2 * native, 2, (long)mask, 64, 0))
		finish(22);
	mask[0] = 2;
	if (call(319, (long)(base + page), 1, 2, (long)mask, 64, 0))
		finish(23);
	if (call(450, (long)(base + page), 1, 1, 0, 0, 0))
		finish(24);
	if (call(320, (long)&mode, (long)got, 64, (long)(base + page), 2, 0) ||
	    mode != 2 || got[0] != 2 || got[1])
		finish(25);
	for (unsigned long off = 0; off < 2 * native; off += page) {
		base[off + 17] = 0x51 + off / page;
		node = -1;
		if (call(320, (long)&node, 0, 0, (long)(base + off + 17), 3,
			 0) ||
		    node != (off == page ? 1 : 0))
			finish(26);
	}
	if (call(319, (long)base, 2 * native, 2, (long)mask, 64, 1) != -5)
		finish(27);
	if (call(319, (long)(base + 4096), 4096, 2, (long)mask, 64, 0) !=
	    (page == 4096 ? 0 : -22))
		finish(28);
	if (call(319, (long)base, 2 * native, 2, (long)mask, 64, 5))
		finish(29);
	for (unsigned long off = 0; off < 2 * native; off += page) {
		node = -1;
		if (base[off + 17] != (unsigned char)(0x51 + off / page) ||
		    call(320, (long)&node, 0, 0, (long)(base + off + 17), 3,
			 0) ||
		    node != 1)
			finish(30);
	}
	if (call(91, raw, length, 0, 0, 0, 0))
		finish(31);
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
	mapping(number(argv[1]), pages);
	finish(0);
}

__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
