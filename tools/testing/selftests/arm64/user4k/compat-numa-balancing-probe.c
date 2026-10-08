// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding AArch32 automatic NUMA migration and fallback checks. */
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
	char text[] = "A32 NUMAB FAIL stage=00\n";

	if (code) {
		text[sizeof(text) - 4] = '0' + code / 10;
		text[sizeof(text) - 3] = '0' + code % 10;
		call(4, 1, (long)text, sizeof(text) - 1, 0, 0, 0);
	} else {
		static const char pass[] = "A32 NUMAB PASS\n";

		call(4, 1, (long)pass, sizeof(pass) - 1, 0, 0, 0);
	}
	call(1, code, 0, 0, 0, 0, 0);
	for (;;)
		;
}

static void value(const char *label, unsigned int length, u32 n)
{
	char text[16];
	unsigned int pos = sizeof(text);

	text[--pos] = '\n';
	do {
		text[--pos] = '0' + n % 10;
		n /= 10;
	} while (n);
	call(4, 1, (long)label, length, 0, 0, 0);
	call(4, 1, (long)(text + pos), sizeof(text) - pos, 0, 0, 0);
}

static u32 seconds(void)
{
	struct {
		long long sec, nsec;
	} ts;

	/* Use time64 even when CONFIG_COMPAT_32BIT_TIME is disabled. */
	if (call(403, 1, (long)&ts, 0, 0, 0, 0))
		finish(11);
	return ts.sec;
}

static void cpu(unsigned int id)
{
	u32 mask = 1U << id;

	if (call(241, 0, sizeof(mask), (long)&mask, 0, 0, 0))
		finish(12);
}

static int node_at(unsigned char *address)
{
	int node = -1;

	if (call(320, (long)&node, 0, 0, (long)address, 3, 0))
		finish(13);
	return node;
}

static u32 sample(unsigned char *base, u32 length, u32 page)
{
	u32 moved = 0;

	for (u32 off = 0; off < length; off += page)
		if (off != 2 * page)
			moved += node_at(base + off + 16) == 1;
	return moved;
}

static void forbidden(unsigned char *address, int write_access)
{
	long child = call(2, 0, 0, 0, 0, 0, 0);
	int status = 0;

	if (child < 0)
		finish(40);
	if (!child) {
		/* Force the intended access; this is not thread synchronization. */
		if (write_access)
			*(volatile unsigned char *)address = 0x27;
		else
			(void)*(volatile unsigned char *)address;
		call(1, 2, 0, 0, 0, 0, 0);
		for (;;)
			;
	}
	if (call(114, child, (long)&status, 0, 0, 0, 0) != child ||
	    (status & 0x7f) != 11)
		finish(41);
}

static void mapping(u32 page)
{
	const u32 length = 8U << 20;
	u32 mask[2] = { 1, 0 }, got[2] = { 0, 0 };
	u32 loops = 0, moved = 0, started, total = length / page - 1;
	int mode = -1;
	long raw;
	unsigned char *base;

	if (call(320, (long)&mode, (long)got, 64, 0, 4, 0) || got[0] != 3 ||
	    got[1])
		finish(20);
	cpu(0);
	raw = call(192, 0, length, 3, 0x22, -1, 0);
	if ((u32)raw >= 0xfffff001U)
		finish(21);
	base = (void *)raw;
	/* A missing 4K capability must retain native alignment restrictions. */
	if (call(125, raw + 4096, 4096, 1, 0, 0, 0) != (page == 4096 ? 0 : -22))
		finish(22);
	if (call(125, raw, length, 3, 0, 0, 0))
		finish(23);
	if (call(220, raw, length, 15, 0, 0, 0)) /* MADV_NOHUGEPAGE */
		finish(24);
	if (call(319, raw, length, 2, (long)mask, 64, 0))
		finish(25);
	for (u32 off = 0; off < length; off += page) {
		u32 *p = (void *)(base + off);

		p[0] = 0x91720000 + off / page;
		p[1] = 0;
		p[page / 4 - 1] = 0x51320000 + off / page;
		if (node_at(base + off + 16) != 0)
			finish(26);
	}
	if (call(125, raw + page, page, 1, 0, 0, 0) ||
	    call(125, raw + 2 * page, page, 0, 0, 0, 0))
		finish(27);
	if (call(319, raw, length, 0, 0, 0,
		 0)) /* MPOL_DEFAULT, no MOVE flags */
		finish(28);
	cpu(1);
	started = seconds();
	do {
		for (u32 off = 0; off < length; off += page) {
			volatile u32 *p = (void *)(base + off);

			if (off == 2 * page)
				continue;
			if (p[0] != 0x91720000 + off / page ||
			    p[page / 4 - 1] != 0x51320000 + off / page)
				finish(29);
			if (off != page)
				p[1]++;
		}
		loops++;
		if (!(loops % 128)) {
			moved = sample(base, length, page);
			if (moved == total)
				break;
		}
	} while (seconds() - started < 30);
	moved = sample(base, length, page);
	value("A32 NUMAB page=", sizeof("A32 NUMAB page=") - 1, page);
	value("A32 NUMAB moved=", sizeof("A32 NUMAB moved=") - 1, moved);
	value("A32 NUMAB expected=", sizeof("A32 NUMAB expected=") - 1, total);
	value("A32 NUMAB loops=", sizeof("A32 NUMAB loops=") - 1, loops);
	if (moved != total)
		finish(30);
	for (u32 off = 0; off < length; off += page)
		if (off != page && off != 2 * page &&
		    *(u32 *)(base + off + 4) != loops)
			finish(31);
	forbidden(base + page, 1);
	forbidden(base + 2 * page, 0);
	if (call(125, raw + 2 * page, page, 1, 0, 0, 0) ||
	    *(u32 *)(base + 2 * page) != 0x91720002)
		finish(32);
	if (call(91, raw, length, 0, 0, 0, 0))
		finish(33);
}

_Noreturn void probe(u32 *sp)
{
	u32 argc = *sp++, page = 0;
	char **argv = (void *)sp;

	sp += argc + 1;
	while (*sp)
		sp++;
	sp++;
	while (sp[0]) {
		if (sp[0] == 6)
			page = sp[1];
		sp += 2;
	}
	if (argc != 2 || page != number(argv[1]))
		finish(10);
	mapping(page);
	finish(0);
}

__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
