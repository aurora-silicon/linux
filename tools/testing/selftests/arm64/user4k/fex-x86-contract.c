// SPDX-License-Identifier: GPL-2.0-only
typedef unsigned long size_t;
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long r10 __asm__("r10") = d, r8 __asm__("r8") = e,
			  r9 __asm__("r9") = f;
	long r;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
			   "r"(r9)
			 : "rcx", "r11", "memory");
	return r;
}
static void say(const char *s, size_t n)
{
	sc(1, 1, (long)s, n, 0, 0, 0);
}
#define SAY(s) say(s, sizeof(s) - 1)
static void quit(int n)
{
	sc(231, n, 0, 0, 0, 0, 0);
	for (;;)
		;
}
static int check(unsigned char *p, size_t size)
{
	for (size_t i = 0; i < size; i++)
		if (p[i] != (unsigned char)(i ^ (i >> 12)))
			return 0;
	return 1;
}
long run(unsigned long *sp)
{
	unsigned long argc = *sp++, pagesz = 0;
	sp += argc + 1;
	unsigned long *env = sp;
	while (*sp)
		sp++;
	sp++;
	while (*sp) {
		if (sp[0] == 6)
			pagesz = sp[1];
		sp += 2;
	}
	if (pagesz != 4096)
		return 10;
	SAY("ok - FEX x86 guest 4K auxv\n");
	unsigned char *p = (void *)sc(9, 0, 12288, 3, 0x22, -1, 0);
	if ((long)p < 0)
		return 11;
	for (size_t i = 0; i < 12288; i++)
		p[i] = (unsigned char)(i ^ (i >> 12));
	if (sc(10, (long)p + 4096, 4096, 0, 0, 0, 0))
		return 12;
	long child = sc(57, 0, 0, 0, 0, 0, 0);
	int status = 0;
	if (!child) {
		*(volatile unsigned char *)(p + 4096) = 1;
		quit(13);
	}
	if (child < 0 || sc(61, child, (long)&status, 0, 0, 0, 0) != child ||
	    (status & 127) != 11)
		return 14;
	if (sc(10, (long)p + 4096, 4096, 3, 0, 0, 0) || !check(p, 12288))
		return 15;
	SAY("ok - FEX x86 independent 4K protection fault\n");
	child = sc(57, 0, 0, 0, 0, 0, 0);
	if (!child) {
		p[4096] = 0x71;
		quit(0);
	}
	if (child < 0 || sc(61, child, (long)&status, 0, 0, 0, 0) != child ||
	    status || !check(p, 12288))
		return 16;
	SAY("ok - FEX x86 fork COW\n");
	p = (void *)sc(25, (long)p, 12288, 16384, 1, 0, 0);
	if ((long)p < 0 || !check(p, 12288))
		return 17;
	for (size_t i = 12288; i < 16384; i++)
		if (p[i])
			return 18;
	SAY("ok - FEX x86 mremap preserve and zero extension\n");
	long fd = sc(319, (long)"fex-shared", 0, 0, 0, 0, 0);
	if (fd < 0 || sc(77, fd, 12288, 0, 0, 0, 0))
		return 19;
	unsigned char *a = (void *)sc(9, 0, 4096, 3, 1, fd, 4096),
		      *b = (void *)sc(9, 0, 4096, 3, 1, fd, 4096);
	if ((long)a < 0 || (long)b < 0)
		return 20;
	a[37] = 0x65;
	if (b[37] != 0x65)
		return 21;
	SAY("ok - FEX x86 memfd mapping at 4K file offset\n");
	unsigned char *code = (void *)sc(9, 0, 4096, 3, 0x22, -1, 0);
	if ((long)code < 0)
		return 22;
	code[0] = 0xb8;
	code[1] = 42;
	code[2] = 0;
	code[3] = 0;
	code[4] = 0;
	code[5] = 0xc3;
	if (sc(10, (long)code, 4096, 5, 0, 0, 0) ||
	    ((int (*)(void))code)() != 42)
		return 23;
	if (sc(10, (long)code, 4096, 3, 0, 0, 0))
		return 24;
	code[1] = 99;
	if (sc(10, (long)code, 4096, 5, 0, 0, 0) ||
	    ((int (*)(void))code)() != 99)
		return 25;
	SAY("ok - FEX x86 JIT permission transition and code replacement\n");
	unsigned char random[32];
	if (sc(318, (long)random, 32, 0, 0, 0, 0) != 32)
		return 26;
	if (argc == 1) {
		child = sc(57, 0, 0, 0, 0, 0, 0);
		if (!child) {
			char *args[] = { "/x86-contract", "--exec-child", 0 };
			sc(59, (long)args[0], (long)args, (long)env, 0, 0, 0);
			quit(28);
		}
		if (child < 0 ||
		    sc(61, child, (long)&status, 0, 0, 0, 0) != child || status)
			return 27;
		SAY("ok - FEX x86 fork and re-exec with 4K mappings\n");
	}
	SAY("FEX X86 4K CONTRACT PASS\n");
	return 0;
}
__asm__(".global _start\n_start:\nmov %rsp,%rdi\nand $-16,%rsp\ncall run\nmov %rax,%rdi\nmov $231,%rax\nsyscall\n");
