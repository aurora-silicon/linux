// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding AArch32 SysV attach/detach and fallback-alignment checks. */
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
	char text[] = "A32 SHM FAIL stage=00\n";

	if (code) {
		text[19] = '0' + code / 10;
		text[20] = '0' + code % 10;
		call(4, 1, (long)text, sizeof(text) - 1, 0, 0, 0);
	} else {
		static const char pass[] = "A32 SHM PASS\n";

		call(4, 1, (long)pass, sizeof(pass) - 1, 0, 0, 0);
	}
	call(1, code, 0, 0, 0, 0, 0);
	for (;;)
		;
}

static void mapping(unsigned long pages, int rounded)
{
	unsigned long align = 4 * pages, length = 4 * pages;
	long raw = call(192, 0, 12 * align, 3, 0x22, -1, 0);
	unsigned char *target;
	long id, result;

	if ((unsigned long)raw >= 0xfffff001UL)
		finish(20);
	result = call(125, raw + 4096, 4096, 1, 0, 0, 0);
	if (result != (pages == 4096 ? 0 : -22))
		finish(21);
	if (call(125, raw, 12 * align, 3, 0, 0, 0))
		finish(22);
	target = (void *)(((unsigned long)raw + align - 1) & ~(align - 1));
	target += align + (rounded ? 0 : pages);
	target[-1] = 0x51;
	target[length] = 0x72;
	if (call(91, (long)target, length, 0, 0, 0, 0))
		finish(23);
	id = call(307, 0, length - 1, 0600, 0, 0, 0);
	if (id < 0)
		finish(24);
	result = call(305, id, (long)(target + (rounded ? pages + 17 : 0)),
		      rounded ? 020000 : 0, 0, 0, 0);
	if (result != (long)target) {
		call(308, id, 0, 0, 0, 0, 0);
		finish(rounded ? 26 : 25);
	}
	if (call(308, id, 0, 0, 0, 0, 0))
		finish(27);
	target[pages + 13] = 0x67;
	if (call(125, (long)(target + pages), pages, 1, 0, 0, 0))
		finish(28);
	if (call(91, (long)target, pages, 0, 0, 0, 0))
		finish(29);
	if (call(306, (long)target, 0, 0, 0, 0, 0))
		finish(30);
	if (target[-1] != 0x51 || target[length] != 0x72)
		finish(31);
	if (call(91, raw, 12 * align, 0, 0, 0, 0))
		finish(32);
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
	mapping(pages, 0);
	mapping(pages, 1);
	finish(0);
}

__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
