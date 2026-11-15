// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding AArch32 -> AArch64 exec with an inherited granule preference. */
#include "../../../../../include/uapi/linux/prctl.h"
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
static void fail(int code)
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
	char **env = (void *)sp;
	while (*sp)
		sp++;
	sp++;
	while (sp[0]) {
		if (sp[0] == 6)
			pages = sp[1];
		sp += 2;
	}
	if (argc != 4 || pages != number(argv[3]))
		fail(10);
	if (call(172, PR_AURORA_GET_DEFAULT_PAGE_SIZE, 0, 0, 0, 0, 0) !=
	    number(argv[2]))
		fail(11);
	if (call(172, PR_AURORA_GET_EXEC_PAGE_SIZE, 0, 0, 0, 0, 0))
		fail(12);
	long mem = call(192, 0, 4 * pages, 3, 0x22, -1, 0);
	if ((unsigned long)mem >= 0xfffff001UL)
		fail(13);
	long ret = call(125, mem + 4096, 4096, 1, 0, 0, 0);
	if (ret != (pages == 4096 ? 0 : -22))
		fail(14);
	if (call(91, mem, 4 * pages, 0, 0, 0, 0))
		fail(15);
	char *args[] = { "/init", "--compat-return", argv[1], argv[2], 0 };
	call(11, (long)args[0], (long)args, (long)env, 0, 0, 0);
	fail(16);
	for (;;)
		;
}
__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
