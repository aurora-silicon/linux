/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T8122 (Apple M3) system suspend-to-RAM support.
 *
 * Implements the T8122 sleep register map, display/APCIE/ATC/MCC shutdown and
 * wake sequences, HID4/EHID4-unblocked PoP/PoC cache clean before deep sleep,
 * and CYC_OVRD mode-3 EL1 deep WFI arming.
 */

#define T8122_PS_GPIO		0x120
#define T8122_PS_DISP_SYS	0x1d8
#define T8122_PS_GFX		0x1e0
#define T8122_PS_DISP_FE	0x208
#define T8122_PS_SIO_CPU	0x210
#define T8122_PS_APCIE		0x3f0
#define T8122_PS_ANS		0x3f8
#define T8122_PS_ATC0_COMMON	0x400
#define T8122_PS_ATC1_COMMON	0x408
#define T8122_PS_DISPEXT_SYS	0x410
#define T8122_PS_APCIE_GP	0x440
#define T8122_PS_APCIE_ST	0x448
#define T8122_PS_DISPEXT_FE	0x480
#define T8122_PS_DISPEXT_CPU	0x488
#define T8122_PS_APCIE_PHY_SW	0x4a0
#define T8122_PS_SEP		0xc00
#define T8122_PS_DISP_CPU	0x10000

#define T8122_PS_AUTO_IDLE	0x1400000f

#define T8122_AON_ATC0_USB_AON	0xb0
#define T8122_AON_ATC1_USB_AON	0xb8
#define T8122_AON_ATC0_USB	0xc0
#define T8122_AON_ATC1_USB	0xc8

#define T8122_APCIE_PHY_CTRL_RESET	BIT(7)
#define T8122_APCIE_PHY_CTRL_UP		BIT(9)

#define T8122_PWRGATE_E		0x80
#define T8122_PWRGATE_P		0x88
#define T8122_PWRGATE_DISP_A	0xcac
#define T8122_PWRGATE_DISP_B	0xcd0
#define T8122_PWRGATE_DISP_C	0xcf0

#define T8122_CLUSTER_E		0x0
#define T8122_CLUSTER_P		0x4
#define T8122_CLLT_CTL		0x0
#define T8122_CLLT_LAST		0x24

#define T8122_SYS_HID4			sys_reg(3, 0, 15, 4, 0)
#define T8122_SYS_EHID4			sys_reg(3, 0, 15, 4, 1)
#define T8122_HID4_DISABLE_DC_MVA	BIT_ULL(11)
#define T8122_HID4_DISABLE_DC_SW_L2	BIT_ULL(44)
#define T8122_HID4_DC_MASK		(T8122_HID4_DISABLE_DC_MVA | T8122_HID4_DISABLE_DC_SW_L2)
#define T8122_MPIDR_P_CORE		BIT_ULL(16)

#define T8122_SYS_CYC_OVRD		sys_reg(3, 5, 15, 5, 0)
#define T8122_CYC_WFI_MODE		GENMASK_ULL(25, 24)

/* Every PMGR power-state word in the T8122 device tree. */
static const u32 t8122_ps_all[] = {
	0x100, 0x108, 0x110, 0x118, 0x120, 0x128, 0x130, 0x138, 0x140, 0x148,
	0x150, 0x158, 0x160, 0x168, 0x170, 0x178, 0x180, 0x188, 0x190, 0x198,
	0x1a8, 0x1b0, 0x1b8, 0x1d8, 0x1e0, 0x1e8, 0x1f0, 0x200, 0x208, 0x210,
	0x218, 0x220, 0x228, 0x230, 0x238, 0x240, 0x248, 0x250, 0x258, 0x260,
	0x268, 0x270, 0x278, 0x280, 0x288, 0x290, 0x298, 0x2a0, 0x2a8, 0x2b0,
	0x2b8, 0x2c0, 0x2c8, 0x2d0, 0x2d8, 0x2e0, 0x2e8, 0x2f0, 0x2f8, 0x300,
	0x308, 0x310, 0x318, 0x330, 0x338, 0x340, 0x348, 0x358, 0x360, 0x368,
	0x370, 0x378, 0x380, 0x388, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8,
	0x3c0, 0x3f0, 0x3f8, 0x400, 0x408, 0x410, 0x418, 0x420, 0x428, 0x430,
	0x438, 0x440, 0x448, 0x450, 0x458, 0x460, 0x468, 0x470, 0x478, 0x480,
	0x488, 0x490, 0x498, 0x4a0, 0x4a8, 0x4b0, 0x4b8, 0x4c0, 0xc00, 0x4000,
	0x4008, 0x4010, 0x4018, 0x4020, 0x4028, 0x4030, 0x4038, 0x4040, 0x4048, 0x4050,
	0x4058, 0x4060, 0x8000, 0x8008, 0x8010, 0x8018, 0x8020, 0x10000,
};

/*
 * On at macOS's commit and off once Linux's drivers have suspended:
 * DWI, GPIO, SOC_RC, SPI_P, UART_P, AUDIO_P, UART_N, UART0. Parents first.
 */
static const u32 t8122_ps_macos_on[] = {
	0x118, 0x120, 0x168, 0x278, 0x280, 0x288, 0x2d0, 0x2d8,
};

/* Off at macOS's commit and left clock-gated by the bootloader. */
static const u32 t8122_ps_macos_off[] = {
	0x108, 0x138, 0x140, 0x148, 0x150, 0x158, 0x178, 0x228, 0x258, 0x260,
	0x268, 0x270, 0x2f8, 0x300, 0x308, 0x378, 0x380, 0x388, 0x390, 0x3b8,
};

static const u32 t8122_ps_aon_all[] = {
	0x0, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88,
	0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8, 0xc0, 0xc8,
};

static const u32 t8122_ps_wake_order[] = {
	0xc00, 0x100, 0x110, 0x118, 0x120, 0x128, 0x130, 0x3f0,
	0x3f8, 0x440, 0x448, 0x450, 0x458, 0x4a0, 0x400, 0x408,
};

static const u32 t8122_ps_aon_wake_order[] = { 0x90 };

static const struct apple_sleep_word t8122_stray[] = {
	{ "isp_sys", 0x1e8 }, { "i2c1", 0x238 }, { "i2c2", 0x240 }, { "i2c3", 0x248 },
	{ "spi2", 0x2a8 }, { "qspi", 0x2c8 }, { "sio_cpu", 0x210 }, { "apcie_phy_sw", 0x4a0 },
};

static struct {
	void __iomem *hpm_notify;
	void __iomem *mcc[4];
	void __iomem *apcie_axi;
	void __iomem *apcie_rc;
	void __iomem *apcie_clkgen;
	void __iomem *apcie_phy;
	void __iomem *apcie_phy_ip;
	void __iomem *apcie_port0;
	u32 notify_saved[2];
	bool notify_staged;
	bool matched;
} t8122;

static __always_inline bool t8122_is_pcore(void)
{
	return !!(read_sysreg(mpidr_el1) & T8122_MPIDR_P_CORE);
}

static __always_inline u64 t8122_read_hid4(bool pcore)
{
	return pcore ? read_sysreg_s(T8122_SYS_HID4)
		     : read_sysreg_s(T8122_SYS_EHID4);
}

static __always_inline void t8122_write_hid4(bool pcore, u64 val)
{
	if (pcore)
		write_sysreg_s(val, T8122_SYS_HID4);
	else
		write_sysreg_s(val, T8122_SYS_EHID4);
	isb();
}

/*
 * Clean [start, end) past T8122's L2 into SLC/DRAM via clean-only dc cvadp
 * (FEAT_DPB2) or dc cvap (FEAT_DPB). Must run with HID4/EHID4 DISABLE_DC_MVA
 * cleared so the maintenance instructions are not ignored as NOPs.
 */
static __always_inline void t8122_clean_range_slc(unsigned long start, unsigned long end)
{
	unsigned long line = 4UL << ((read_sysreg(ctr_el0) >> 16) & 0xf);
	unsigned int dpb = read_sysreg(id_aa64isar1_el1) & 0xf;
	unsigned long addr = start & ~(line - 1);

	if (dpb >= 2) {
		for (; addr < end; addr += line)
			asm volatile("sys #3, c7, c13, #1, %0" : : "r"(addr) : "memory");
	} else if (dpb >= 1) {
		for (; addr < end; addr += line)
			asm volatile("sys #3, c7, c12, #1, %0" : : "r"(addr) : "memory");
	} else {
		for (; addr < end; addr += line)
			asm volatile("dc civac, %0" : : "r"(addr) : "memory");
	}
	dsb(sy);
}

/*
 * Register-only dc csw across all implemented data/unified cache levels
 * (L1 + L2 on T8122). Must run with HID4/EHID4 DISABLE_DC_SW_L2 cleared and
 * zero stack writes so L1/L2 stay clean through dsb(sy).
 */
static __always_inline void t8122_dcache_clean_all_levels(void)
{
	u64 r_clidr, r_ccidx, r_lvl, r_ctype, r_ccsidr, r_lshift;
	u64 r_way, r_mset, r_wshift, r_set, r_tmp;

	asm volatile(
		"dsb sy\n\t"
		"mrs %[clidr], clidr_el1\n\t"
		"mrs %[ccidx], id_aa64mmfr2_el1\n\t"
		"ubfx %[ccidx], %[ccidx], #20, #4\n\t"
		"mov %[lvl], #0\n\t"
		"1:\n\t"
		"cmp %[lvl], #7\n\t"
		"b.hs 5f\n\t"
		"add %[tmp], %[lvl], %[lvl], lsl #1\n\t"
		"lsr %[ctype], %[clidr], %[tmp]\n\t"
		"and %[ctype], %[ctype], #7\n\t"
		"cbz %[ctype], 5f\n\t"
		"cmp %[ctype], #2\n\t"
		"b.lo 4f\n\t"
		"cmp %[ctype], #4\n\t"
		"b.hi 4f\n\t"
		"lsl %[tmp], %[lvl], #1\n\t"
		"msr csselr_el1, %[tmp]\n\t"
		"isb\n\t"
		"mrs %[ccsidr], ccsidr_el1\n\t"
		"and %[lshift], %[ccsidr], #7\n\t"
		"add %[lshift], %[lshift], #4\n\t"
		"cbnz %[ccidx], 20f\n\t"
		"ubfx %[way], %[ccsidr], #3, #10\n\t"
		"ubfx %[mset], %[ccsidr], #13, #15\n\t"
		"b 21f\n\t"
		"20:\n\t"
		"ubfx %[way], %[ccsidr], #3, #21\n\t"
		"ubfx %[mset], %[ccsidr], #32, #24\n\t"
		"21:\n\t"
		"clz %w[wshift], %w[way]\n\t"
		"2:\n\t"
		"mov %[set], %[mset]\n\t"
		"lsl %[ctype], %[way], %[wshift]\n\t"
		"orr %[ctype], %[ctype], %[lvl], lsl #1\n\t"
		"3:\n\t"
		"lsl %[tmp], %[set], %[lshift]\n\t"
		"orr %[tmp], %[ctype], %[tmp]\n\t"
		"dc csw, %[tmp]\n\t"
		"subs %[set], %[set], #1\n\t"
		"b.ge 3b\n\t"
		"subs %[way], %[way], #1\n\t"
		"b.ge 2b\n\t"
		"dsb sy\n\t"
		"4:\n\t"
		"add %[lvl], %[lvl], #1\n\t"
		"b 1b\n\t"
		"5:\n\t"
		"msr csselr_el1, xzr\n\t"
		"dsb sy\n\t"
		"isb\n\t"
		: [clidr] "=&r"(r_clidr),
		  [ccidx] "=&r"(r_ccidx),
		  [lvl] "=&r"(r_lvl),
		  [ctype] "=&r"(r_ctype),
		  [ccsidr] "=&r"(r_ccsidr),
		  [lshift] "=&r"(r_lshift),
		  [way] "=&r"(r_way),
		  [mset] "=&r"(r_mset),
		  [wshift] "=&r"(r_wshift),
		  [set] "=&r"(r_set),
		  [tmp] "=&r"(r_tmp)
		:
		: "cc", "memory");
}

static bool t8122_linear_va_valid(unsigned long addr)
{
	pgd_t *pgdp = pgd_offset_k(addr);
	pgd_t pgd = READ_ONCE(*pgdp);
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep;

	if (pgd_none(pgd) || pgd_bad(pgd))
		return false;
	p4dp = p4d_offset(pgdp, addr);
	p4d = READ_ONCE(*p4dp);
	if (p4d_none(p4d) || p4d_bad(p4d))
		return false;
	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud))
		return false;
	if (pud_leaf(pud))
		return pud_valid(pud);
	if (pud_bad(pud))
		return false;
	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (pmd_none(pmd))
		return false;
	if (pmd_leaf(pmd))
		return pmd_valid(pmd);
	if (pmd_bad(pmd))
		return false;
	ptep = pte_offset_kernel(pmdp, addr);
	return ptep && pte_valid(READ_ONCE(*ptep));
}

static void t8122_clean_used_ram(void)
{
	struct memblock_region *reg;

	for_each_mem_region(reg) {
		unsigned long pfn, end_pfn;
		unsigned long run_start = 0, run_end = 0;

		if (memblock_is_nomap(reg))
			continue;
		pfn = PFN_UP(reg->base);
		end_pfn = PFN_DOWN(reg->base + reg->size);
		while (pfn < end_pfn) {
			struct page *page;
			unsigned long va;

			if (!pfn_valid(pfn)) {
				if (run_start) {
					t8122_clean_range_slc(run_start, run_end);
					run_start = 0;
				}
				pfn++;
				continue;
			}
			page = pfn_to_page(pfn);
			if (PageBuddy(page)) {
				unsigned long order = page_private(page);
				unsigned long nr = 1;

				if (order <= MAX_PAGE_ORDER &&
				    !(pfn & ((1UL << order) - 1)))
					nr = 1UL << order;
				if (nr > end_pfn - pfn)
					nr = end_pfn - pfn;
				if (run_start) {
					t8122_clean_range_slc(run_start, run_end);
					run_start = 0;
				}
				pfn += nr;
				continue;
			}
			if (PageOffline(page)) {
				if (run_start) {
					t8122_clean_range_slc(run_start, run_end);
					run_start = 0;
				}
				pfn++;
				continue;
			}
			va = (unsigned long)page_address(page);
			if (va && t8122_linear_va_valid(va)) {
				if (run_start && va == run_end) {
					run_end += PAGE_SIZE;
				} else {
					if (run_start)
						t8122_clean_range_slc(run_start, run_end);
					run_start = va;
					run_end = va + PAGE_SIZE;
				}
			} else if (run_start) {
				t8122_clean_range_slc(run_start, run_end);
				run_start = 0;
			}
			pfn++;
		}
		if (run_start)
			t8122_clean_range_slc(run_start, run_end);
	}
	dsb(sy);
}

/*
 * Clean the mapped kernel image (_text.._end). Under CTR_EL0.IDC=1 and
 * rodata=full, patched text/rodata aliases are unmapped in the linear map and
 * must be cleaned via kernel VAs so DRAM matches patched code on wake.
 */
static void t8122_clean_kernel_image(void)
{
	unsigned long start = (unsigned long)_text & PAGE_MASK;
	unsigned long end = PAGE_ALIGN((unsigned long)_end);
	unsigned long va, run_start = 0, run_end = 0;

	for (va = start; va < end; va += PAGE_SIZE) {
		if (t8122_linear_va_valid(va)) {
			if (run_start && va == run_end) {
				run_end += PAGE_SIZE;
			} else {
				if (run_start)
					t8122_clean_range_slc(run_start, run_end);
				run_start = va;
				run_end = va + PAGE_SIZE;
			}
		} else if (run_start) {
			t8122_clean_range_slc(run_start, run_end);
			run_start = 0;
		}
	}
	if (run_start)
		t8122_clean_range_slc(run_start, run_end);
	dsb(sy);
}

static void t8122_clean_percpu_slc(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		unsigned long area = (unsigned long)__per_cpu_start + per_cpu_offset(cpu);

		t8122_clean_range_slc(area, area + (__per_cpu_end - __per_cpu_start));
	}
}

static void t8122_clean_all_kptables(void)
{
	unsigned int i, j, k, l;
	pgd_t *pgd_base = swapper_pg_dir;

	t8122_clean_range_slc((unsigned long)pgd_base,
			      (unsigned long)pgd_base + PAGE_SIZE);
	for (i = 0; i < PTRS_PER_PGD; i++) {
		pgd_t pgd = READ_ONCE(pgd_base[i]);
		p4d_t *p4d_base;

		if (pgd_none(pgd) || pgd_bad(pgd) || pgd_leaf(pgd))
			continue;
		p4d_base = p4d_offset(&pgd_base[i], 0);
		if ((unsigned long)p4d_base != (unsigned long)&pgd_base[i])
			t8122_clean_range_slc((unsigned long)p4d_base,
					      (unsigned long)p4d_base + PAGE_SIZE);
		for (j = 0; j < PTRS_PER_P4D; j++) {
			p4d_t p4d = READ_ONCE(p4d_base[j]);
			pud_t *pud_base;

			if (p4d_none(p4d) || p4d_bad(p4d) || p4d_leaf(p4d))
				continue;
			pud_base = pud_offset(&p4d_base[j], 0);
			if ((unsigned long)pud_base != (unsigned long)&p4d_base[j])
				t8122_clean_range_slc((unsigned long)pud_base,
						      (unsigned long)pud_base + PAGE_SIZE);
			for (k = 0; k < PTRS_PER_PUD; k++) {
				pud_t pud = READ_ONCE(pud_base[k]);
				pmd_t *pmd_base;

				if (pud_none(pud) || pud_bad(pud) || pud_leaf(pud))
					continue;
				pmd_base = pmd_offset(&pud_base[k], 0);
				if ((unsigned long)pmd_base != (unsigned long)&pud_base[k])
					t8122_clean_range_slc((unsigned long)pmd_base,
							      (unsigned long)pmd_base + PAGE_SIZE);
				for (l = 0; l < PTRS_PER_PMD; l++) {
					pmd_t pmd = READ_ONCE(pmd_base[l]);
					pte_t *pte_base;

					if (pmd_none(pmd) || pmd_bad(pmd) || pmd_leaf(pmd))
						continue;
					pte_base = pte_offset_kernel(&pmd_base[l], 0);
					if (pte_base)
						t8122_clean_range_slc((unsigned long)pte_base,
								      (unsigned long)pte_base + PAGE_SIZE);
				}
			}
		}
	}
	dsb(sy);
}

static void t8122_clean_va_ptables(unsigned long start, unsigned long len)
{
	unsigned long addr = start & PAGE_MASK;
	unsigned long end = PAGE_ALIGN(start + len);

	for (; addr < end; addr += PAGE_SIZE) {
		pgd_t *pgdp = pgd_offset_k(addr);
		p4d_t *p4dp;
		pud_t *pudp;
		pmd_t *pmdp;
		pte_t *ptep;

		t8122_clean_range_slc((unsigned long)pgdp & PAGE_MASK,
				      ((unsigned long)pgdp & PAGE_MASK) + PAGE_SIZE);
		if (pgd_none(*pgdp) || pgd_bad(*pgdp))
			continue;
		p4dp = p4d_offset(pgdp, addr);
		if ((unsigned long)p4dp != (unsigned long)pgdp)
			t8122_clean_range_slc((unsigned long)p4dp & PAGE_MASK,
					      ((unsigned long)p4dp & PAGE_MASK) + PAGE_SIZE);
		if (p4d_none(*p4dp) || p4d_bad(*p4dp))
			continue;
		pudp = pud_offset(p4dp, addr);
		t8122_clean_range_slc((unsigned long)pudp & PAGE_MASK,
				      ((unsigned long)pudp & PAGE_MASK) + PAGE_SIZE);
		if (pud_none(*pudp) || pud_bad(*pudp) || pud_leaf(*pudp))
			continue;
		pmdp = pmd_offset(pudp, addr);
		t8122_clean_range_slc((unsigned long)pmdp & PAGE_MASK,
				      ((unsigned long)pmdp & PAGE_MASK) + PAGE_SIZE);
		if (pmd_none(*pmdp) || pmd_bad(*pmdp) || pmd_leaf(*pmdp))
			continue;
		ptep = pte_offset_kernel(pmdp, addr);
		if (ptep)
			t8122_clean_range_slc((unsigned long)ptep & PAGE_MASK,
					      ((unsigned long)ptep & PAGE_MASK) + PAGE_SIZE);
	}
}

static void t8122_clean_state_poc(struct apple_sleep *s)
{
	unsigned int i, stash_n = mpidr_hash_size();

	t8122_clean_range_slc((unsigned long)swapper_pg_dir,
			      (unsigned long)swapper_pg_dir + PAGE_SIZE);
	t8122_clean_range_slc((unsigned long)idmap_pg_dir,
			      (unsigned long)idmap_pg_dir + 3 * PAGE_SIZE);
	t8122_clean_va_ptables((unsigned long)current, sizeof(*current));
	t8122_clean_va_ptables((unsigned long)task_stack_page(current), THREAD_SIZE);
	t8122_clean_va_ptables((unsigned long)s, sizeof(*s));
	t8122_clean_va_ptables((unsigned long)&t8122, sizeof(t8122));
	if (sleep_save_stash) {
		unsigned long base = (unsigned long)sleep_save_stash;

		t8122_clean_va_ptables(base, stash_n * sizeof(*sleep_save_stash));
		t8122_clean_range_slc(base, base + stash_n * sizeof(*sleep_save_stash));
		for (i = 0; i < stash_n; i++) {
			unsigned long entry = READ_ONCE(sleep_save_stash[i]);

			if (entry) {
				t8122_clean_va_ptables(entry, sizeof(struct sleep_stack_data));
				t8122_clean_range_slc(entry, entry + sizeof(struct sleep_stack_data));
			}
		}
	}
	t8122_clean_range_slc((unsigned long)&mpidr_hash,
			      (unsigned long)(&mpidr_hash + 1));
	t8122_clean_range_slc((unsigned long)&sleep_save_stash,
			      (unsigned long)(&sleep_save_stash + 1));
	t8122_clean_range_slc((unsigned long)s, (unsigned long)(s + 1));
	t8122_clean_range_slc((unsigned long)&t8122, (unsigned long)(&t8122 + 1));
	if (s->mbox)
		t8122_clean_range_slc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
	if (s->ps_commit && s->soc->ps_all.count)
		t8122_clean_range_slc((unsigned long)s->ps_commit,
				      (unsigned long)(s->ps_commit + s->soc->ps_all.count));
	if (s->ps_aon_commit && s->soc->ps_aon_all.count)
		t8122_clean_range_slc((unsigned long)s->ps_aon_commit,
				      (unsigned long)(s->ps_aon_commit + s->soc->ps_aon_all.count));
	if (s->ps_on_saved && s->soc->ps_macos_on.count)
		t8122_clean_range_slc((unsigned long)s->ps_on_saved,
				      (unsigned long)(s->ps_on_saved + s->soc->ps_macos_on.count));
	if (s->ps_off_saved && s->soc->ps_macos_off.count)
		t8122_clean_range_slc((unsigned long)s->ps_off_saved,
				      (unsigned long)(s->ps_off_saved + s->soc->ps_macos_off.count));
	if (s->pins_saved && s->npins)
		t8122_clean_range_slc((unsigned long)s->pins_saved,
				      (unsigned long)(s->pins_saved + s->npins));
	t8122_clean_range_slc((unsigned long)current,
			      (unsigned long)current + sizeof(*current));
	t8122_clean_range_slc((unsigned long)task_stack_page(current),
			      (unsigned long)task_stack_page(current) + THREAD_SIZE);
	dsb(sy);
}

static void __iomem *t8122_map(struct apple_sleep *s, const char *name, struct resource *out)
{
	struct device_node *np = s->dev->of_node;
	struct resource res;
	void __iomem *base;
	int index;

	index = of_property_match_string(np, "reg-names", name);
	if (index < 0)
		return IOMEM_ERR_PTR(index);
	if (of_address_to_resource(np, index, &res))
		return IOMEM_ERR_PTR(-EINVAL);
	/* Never unmapped: the driver has no unbind, as the common map. */
	base = ioremap_np(res.start, resource_size(&res));
	if (!base)
		return IOMEM_ERR_PTR(-ENOMEM);
	if (out)
		*out = res;

	return base;
}

static int t8122_sleep_map_regs(struct apple_sleep *s)
{
	static const struct {
		const char *name;
		size_t offset;
	} regs[] = {
		{ "pwrgate", offsetof(struct apple_sleep, pwrgate) },
		{ "cpu-start", offsetof(struct apple_sleep, cpu_start) },
		{ "cllt", offsetof(struct apple_sleep, cllt) },
		{ "cluster", offsetof(struct apple_sleep, cluster) },
		{ "commit", offsetof(struct apple_sleep, commit) },
		{ "wdt", offsetof(struct apple_sleep, wdt) },
		{ "spmi", offsetof(struct apple_sleep, spmi) },
		{ "wake", offsetof(struct apple_sleep, wake) },
	};
	static const char *const mcc_names[4] = { "mcc-a", "mcc-b", "mcc-c", "mcc-d" };
	unsigned int i;
	bool pcore;
	u64 hid4_orig;
	int ret;

	s->ps = t8122_map(s, "ps", &s->ps_res);
	if (IS_ERR(s->ps))
		return dev_err_probe(s->dev, PTR_ERR(s->ps), "no ps registers\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		void __iomem **field = (void __iomem **)((u8 *)s + regs[i].offset);

		*field = t8122_map(s, regs[i].name, NULL);
		if (IS_ERR(*field))
			return dev_err_probe(s->dev, PTR_ERR(*field), "no %s registers\n",
					     regs[i].name);
	}
	t8122.hpm_notify = t8122_map(s, "spmi-hpm", NULL);
	if (IS_ERR(t8122.hpm_notify))
		return dev_err_probe(s->dev, PTR_ERR(t8122.hpm_notify), "no spmi-hpm registers\n");

	s->ps_aon = t8122_map(s, "ps-aon", NULL);
	if (IS_ERR(s->ps_aon)) {
		dev_warn(s->dev, "no ps-aon registers: USB/debug domains will not be lowered\n");
		s->ps_aon = NULL;
	}

	ret = apple_sleep_read_wake_sources(s);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(mcc_names); i++) {
		t8122.mcc[i] = t8122_map(s, mcc_names[i], NULL);
		if (IS_ERR(t8122.mcc[i])) {
			dev_warn(s->dev, "no %s registers: MCC channel step-down unavailable\n",
				 mcc_names[i]);
			t8122.mcc[i] = NULL;
		}
	}

	{
		static const struct {
			const char *name;
			void __iomem **slot;
		} apcie_regs[] = {
			{ "apcie-axi", &t8122.apcie_axi },
			{ "apcie-rc", &t8122.apcie_rc },
			{ "apcie-clkgen", &t8122.apcie_clkgen },
			{ "apcie-phy", &t8122.apcie_phy },
			{ "apcie-phy-ip", &t8122.apcie_phy_ip },
			{ "apcie-port0", &t8122.apcie_port0 },
		};

		for (i = 0; i < ARRAY_SIZE(apcie_regs); i++) {
			*apcie_regs[i].slot = t8122_map(s, apcie_regs[i].name, NULL);
			if (IS_ERR(*apcie_regs[i].slot))
				return dev_err_probe(s->dev, PTR_ERR(*apcie_regs[i].slot),
						     "no %s registers\n", apcie_regs[i].name);
		}
	}

	/* Ensure the EL1 deep-WFI stub is clean in DRAM before secondary cores jump to it. */
	pcore = t8122_is_pcore();
	hid4_orig = t8122_read_hid4(pcore);
	t8122_write_hid4(pcore, hid4_orig & ~T8122_HID4_DISABLE_DC_MVA);
	t8122_clean_range_slc((unsigned long)t8142_el1_wfi_stub,
			      (unsigned long)t8142_el1_wfi_stub_end);
	t8122_write_hid4(pcore, hid4_orig);

	s->driver_down = devm_bitmap_zalloc(s->dev, resource_size(&s->ps_res) / 4, GFP_KERNEL);
	if (!s->driver_down)
		return -ENOMEM;
	if (apple_sleep_alloc_books(s))
		return -ENOMEM;

	return 0;
}

static void t8122_ps_lower_if_on(struct apple_sleep *s, u32 off, u32 first, u32 second)
{
	if (!FIELD_GET(PS_ACTUAL, readl(s->ps + off)))
		return;
	if (first)
		writel(first, s->ps + off);
	writel(second, s->ps + off);
}

static int t8122_sleep_display_off(struct apple_sleep *s)
{
	u64 start, hz = arch_timer_get_cntfrq();
	bool off = false;

	if (FIELD_GET(PS_ACTUAL, readl(s->ps + T8122_PS_DISPEXT_CPU)))
		t8122_ps_lower_if_on(s, T8122_PS_DISPEXT_CPU, 0x0f0000ff, 0x0f0003f0);
	t8122_ps_lower_if_on(s, T8122_PS_DISPEXT_FE, 0, 0x23f0);
	t8122_ps_lower_if_on(s, T8122_PS_DISPEXT_SYS, 0, 0x23f0);

	if (FIELD_GET(PS_ACTUAL, readl(s->ps + T8122_PS_DISP_CPU))) {
		writel(0x0f0000ff, s->ps + T8122_PS_DISP_CPU);
		writel(3, s->pwrgate + T8122_PWRGATE_DISP_A);
		writel(0, s->pwrgate + T8122_PWRGATE_DISP_B);
		writel(0, s->pwrgate + T8122_PWRGATE_DISP_C);
		writel(0x0f0003f0, s->ps + T8122_PS_DISP_CPU);
	}
	t8122_ps_lower_if_on(s, T8122_PS_DISP_FE, 0, 0x23f0);
	t8122_ps_lower_if_on(s, T8122_PS_DISP_SYS, 0, 0x23f0);

	start = read_sysreg(cntvct_el0);
	while (read_sysreg(cntvct_el0) - start < hz / 10) {
		if (!FIELD_GET(PS_ACTUAL, readl(s->ps + T8122_PS_DISP_CPU)) &&
		    !FIELD_GET(PS_ACTUAL, readl(s->ps + T8122_PS_DISPEXT_CPU)) &&
		    !FIELD_GET(PS_ACTUAL, readl(s->ps + T8122_PS_DISPEXT_SYS))) {
			off = true;
			break;
		}
		cpu_relax();
	}
	if (!off) {
		dev_err(s->dev, "refused: display stayed on: DISP_CPU=%#x DISPEXT_CPU=%#x DISPEXT_SYS=%#x\n",
			readl(s->ps + T8122_PS_DISP_CPU),
			readl(s->ps + T8122_PS_DISPEXT_CPU),
			readl(s->ps + T8122_PS_DISPEXT_SYS));
		return -EBUSY;
	}

	return 0;
}

static void t8122_sleep_match(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	int i;

	t8122.matched = true;
	for (i = 0; i < soc->ps_macos_on.count; i++) {
		u32 offset = soc->ps_macos_on.offset[i];

		s->ps_on_saved[i] = readl(s->ps + offset);
		if (FIELD_GET(PS_ACTUAL, s->ps_on_saved[i]) == 0xf)
			continue;
		sleep_ps_raise_start(s, offset);
	}
	for (i = 0; i < soc->ps_macos_off.count; i++) {
		u32 offset = soc->ps_macos_off.offset[i];

		s->ps_off_saved[i] = readl(s->ps + offset);
		if (!sleep_ps_on(s, offset))
			continue;
		sleep_ps_lower(s, offset);
	}
	for (i = 0; i < soc->ps_macos_off.count; i++)
		sleep_ps_wait_off(s, soc->ps_macos_off.offset[i]);
}

static void t8122_sleep_match_undo(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	int i;

	if (!t8122.matched)
		return;
	t8122.matched = false;
	if (s->returned)
		return;
	for (i = soc->ps_macos_off.count - 1; i >= 0; i--)
		if (FIELD_GET(PS_ACTUAL, s->ps_off_saved[i]))
			sleep_ps_raise(s, soc->ps_macos_off.offset[i]);
	for (i = soc->ps_macos_on.count - 1; i >= 0; i--) {
		u32 val = s->ps_on_saved[i];

		if (!FIELD_GET(PS_ACTUAL, val))
			sleep_ps_lower(s, soc->ps_macos_on.offset[i]);
		else if (FIELD_GET(PS_ACTUAL, val) != 0xf)
			writel(val & ~PS_FLAGS, s->ps + soc->ps_macos_on.offset[i]);
	}
}

static void t8122_apcie_down(struct apple_sleep *s)
{
	void __iomem *axi600, *clk, *ctrl, *run, *phy0, *port0;
	u32 val, b_phy0;
	int ret;

	if (!t8122.apcie_axi || !t8122.apcie_clkgen || !t8122.apcie_phy ||
	    !t8122.apcie_phy_ip || !t8122.apcie_port0)
		return;

	ret = sleep_ps_raise_start(s, T8122_PS_APCIE);
	ret |= sleep_ps_raise_start(s, T8122_PS_ANS);
	ret |= sleep_ps_raise_start(s, T8122_PS_APCIE_GP);
	ret |= sleep_ps_raise_start(s, T8122_PS_APCIE_ST);
	ret |= sleep_ps_raise_start(s, T8122_PS_APCIE_PHY_SW);
	if (ret) {
		dev_warn(s->dev, "APCIE power-down raise failed: %d\n", ret);
		return;
	}

	axi600 = t8122.apcie_axi + 0x600;
	clk = t8122.apcie_clkgen + APCIE_CLK_CTRL;
	ctrl = t8122.apcie_phy + APCIE_PHY_CTRL;
	run = t8122.apcie_phy + APCIE_PHY_RUN;
	phy0 = t8122.apcie_phy + 0xc000;
	port0 = t8122.apcie_port0;

	/* Port 0 PHY + PERST shutdown */
	b_phy0 = readl(phy0);
	if (b_phy0 & 0xc0000603U) {
		writel(b_phy0 & ~0xc0000000U, phy0);
		writel(readl(axi600) & ~BIT(16), axi600);
		writel(readl(port0 + 0x13c) | 0x100, port0 + 0x13c);
		writel(0, port0 + APCIE_PORT_PERST);
		writel(readl(port0 + 0x13c) & ~0x100, port0 + 0x13c);
		writel(readl(phy0) & ~BIT(10), phy0);
		writel(readl(phy0) & ~BIT(9), phy0);
		writel(readl(phy0) | BIT(4), phy0);
		writel(readl(phy0) & ~BIT(0), phy0);
		readl_poll_timeout_atomic(phy0, val, !(val & BIT(2)), 1, APCIE_POLL_US);
		writel(readl(phy0) & ~BIT(1), phy0);
		readl_poll_timeout_atomic(phy0, val, !(val & BIT(3)), 1, APCIE_POLL_US);
		writel(readl(port0 + APCIE_PORT_APPCLK) & ~BIT(0), port0 + APCIE_PORT_APPCLK);
		writel(readl(axi600) | BIT(16), axi600);
	}

	/* Common PHY + PHY IP + AXI + CIO3PLL shutdown */
	writel(readl(t8122.apcie_phy_ip + 0x6c) & ~0xcU, t8122.apcie_phy_ip + 0x6c);
	writel(readl(t8122.apcie_phy_ip + 0x7c) | 0x0f000000U, t8122.apcie_phy_ip + 0x7c);
	writel(readl(ctrl) | T8122_APCIE_PHY_CTRL_RESET, ctrl);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_CLK1REQ, ctrl);
	readl_poll_timeout_atomic(ctrl, val, !(val & APCIE_PHY_CTRL_CLK1ACK), 1, APCIE_POLL_US);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_CLK0REQ, ctrl);
	readl_poll_timeout_atomic(ctrl, val, !(val & APCIE_PHY_CTRL_CLK0ACK), 1, APCIE_POLL_US);
	writel(readl(run) & ~APCIE_PHY_RUN_EN2, run);
	writel(readl(run) & ~APCIE_PHY_RUN_EN, run);
	writel(readl(axi600) & ~BIT(16), axi600);
	writel(readl(clk) & ~APCIE_CLK_RUN, clk);
	writel(readl(clk) | APCIE_CLK_OFF, clk);
	writel(readl(clk) & ~APCIE_GEN4_CLK_EN, clk);
	readl_poll_timeout_atomic(clk, val, !(val & APCIE_CLK_BUSY), 1, APCIE_POLL_US);
	writel(readl(clk) | APCIE_CLK_OFF2, clk);

	writel(T8122_PS_AUTO_IDLE, s->ps + T8122_PS_APCIE_PHY_SW);
	readl_poll_timeout_atomic(s->ps + T8122_PS_APCIE_PHY_SW, val,
				  FIELD_GET(PS_ACTUAL, val) == 4, 1, 5000);
	sleep_ps_raise_finish(s, T8122_PS_APCIE);
	sleep_ps_raise_finish(s, T8122_PS_ANS);
	sleep_ps_raise_finish(s, T8122_PS_APCIE_GP);
	sleep_ps_raise_finish(s, T8122_PS_APCIE_ST);
}

static void t8122_sleep_prepare_regs(struct apple_sleep *s)
{
	t8122_sleep_match(s);
	t8122_apcie_down(s);
	apple_sleep_pmu_write(s->spmi, s->soc->pmu_usid,
			      s->soc->pmu_sleep_reg, s->soc->pmu_sleep_val);
	t8122.notify_saved[0] = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	t8122.notify_saved[1] = readl(t8122.hpm_notify + SPMI_NOTIFY_ENABLE);
	t8122.notify_staged = true;
	writel(s->soc->spmi_notify_sleep, s->spmi + SPMI_NOTIFY_ENABLE);
	writel(s->soc->spmi_notify_sleep, t8122.hpm_notify + SPMI_NOTIFY_ENABLE);
}

static void t8122_sleep_undo_regs(struct apple_sleep *s)
{
	t8122_sleep_match_undo(s);
	if (!t8122.notify_staged)
		return;
	writel(t8122.notify_saved[0], s->spmi + SPMI_NOTIFY_ENABLE);
	writel(t8122.notify_saved[1], t8122.hpm_notify + SPMI_NOTIFY_ENABLE);
	t8122.notify_staged = false;
}

static void t8122_aon_lower(struct apple_sleep *s, u32 off)
{
	u32 val = readl(s->ps_aon + off);

	if (val & PS_AUTO_ENABLE) {
		val &= ~(PS_AUTO_ENABLE | PS_FLAGS);
		writel(val, s->ps_aon + off);
	}
	writel((val & ~PS_TARGET) | PS_FLAGS, s->ps_aon + off);
}

static void t8122_sleep_atc_off(struct apple_sleep *s)
{
	static const struct {
		u32 usb, usb_aon, common;
	} port[] = {
		{ T8122_AON_ATC0_USB, T8122_AON_ATC0_USB_AON, T8122_PS_ATC0_COMMON },
		{ T8122_AON_ATC1_USB, T8122_AON_ATC1_USB_AON, T8122_PS_ATC1_COMMON },
	};
	unsigned int i;

	if (!s->ps_aon)
		return;
	for (i = 0; i < ARRAY_SIZE(port); i++) {
		t8122_aon_lower(s, port[i].usb);
		t8122_aon_lower(s, port[i].usb_aon);
		sleep_ps_lower(s, port[i].common);
		sleep_ps_wait_off(s, port[i].common);
	}
}

/*
 * Step down T8122 MCC channel enable/stage registers after SLC write-back
 * (handled by drivers/soc/apple/mcc.c in suspend_noirq) and CPU cache clean.
 */
static const struct { u8 page; u16 off; bool wide; } t8122_mcc_words[] = {
	{ 2, 0x000, true }, { 0, 0x000, true }, { 1, 0x000, true }, { 2, 0x800, true },
	{ 0, 0x800, true }, { 1, 0x800, true }, { 3, 0x584, false }, { 3, 0x588, false },
};
static const char t8122_mcc_order[] =
	"ABABABABABABAAAAAAAAABABABAABABABABABABABACACCCCCCCCCCCCCCCDDDEDEDFDFDDGGGGGGGGGHH";

static void t8122_mcc_channel_stop(void)
{
	unsigned int i;

	if (!t8122.mcc[0] || !t8122.mcc[1] || !t8122.mcc[2] || !t8122.mcc[3])
		return;
	for (i = 0; t8122_mcc_order[i]; i++) {
		unsigned int w = t8122_mcc_order[i] - 'A';
		void __iomem *reg = t8122.mcc[t8122_mcc_words[w].page] + t8122_mcc_words[w].off;

		if (t8122_mcc_words[w].wide) {
			u64 q = readq(reg);

			if (q)
				writeq(q & (q - 1), reg);
		} else {
			u32 l = readl(reg);

			if (l)
				writel(l & (l - 1), reg);
		}
	}
}

static void t8122_sleep_commit_regs(struct apple_sleep *s)
{
	bool pcore;
	u64 hid4_orig;

	sleep_ps_save(s);
	t8122_sleep_atc_off(s);
	writel(0, s->wdt + WDT_WD0_CTL);

	/*
	 * Mask and save AIC state before the full cache clean so the saved
	 * interrupt masks reach DRAM before the clusters power off.
	 */
	aic_sleep_mask_all();

	if (s->mbox) {
		s->resumes_before = READ_ONCE(s->mbox->resumes);
		WRITE_ONCE(s->mbox->resume_pa, __pa_symbol(s->soc->ops->resume));
	}

	pcore = t8122_is_pcore();
	hid4_orig = t8122_read_hid4(pcore);
	t8122_write_hid4(pcore, hid4_orig & ~T8122_HID4_DC_MASK);
	t8122_clean_used_ram();
	t8122_clean_kernel_image();
	t8122_clean_percpu_slc();
	t8122_clean_all_kptables();
	t8122_clean_state_poc(s);
	t8122_dcache_clean_all_levels();
	if (s->mbox)
		t8122_clean_range_slc((unsigned long)s->mbox,
				      (unsigned long)(s->mbox + 1));
	t8122_clean_range_slc((unsigned long)current,
			      (unsigned long)current + sizeof(*current));
	t8122_clean_range_slc((unsigned long)task_stack_page(current),
			      (unsigned long)task_stack_page(current) + THREAD_SIZE);
	dsb(sy);
	t8122_write_hid4(pcore, hid4_orig);

	t8122_mcc_channel_stop();

	writel(PWRGATE_ON, s->pwrgate + T8122_PWRGATE_E);
	writel(PWRGATE_ON, s->pwrgate + T8122_PWRGATE_P);
	writel(0, s->cluster + T8122_CLUSTER_E);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntv_ctl_el0);
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
	writel(0, s->cluster + T8122_CLUSTER_P);
	writel(0x2500, s->cllt + T8122_CLLT_CTL);
	writel(0, s->cllt + T8122_CLLT_CTL);
	writel(0x1c17, s->cllt + T8122_CLLT_LAST);
}

/*
 * Common apple_sleep_commit() writes s->mbox->resume_pa and calls
 * dcache_clean_poc() right before pre_commit, which is a NOP on T8122 while
 * HID4.DISABLE_DC_MVA is set. Clean s->mbox and s with DISABLE_DC_MVA cleared.
 */
static void t8122_sleep_pre_commit(struct apple_sleep *s)
{
	bool pcore = t8122_is_pcore();
	u64 hid4_orig = t8122_read_hid4(pcore);

	t8122_write_hid4(pcore, hid4_orig & ~T8122_HID4_DISABLE_DC_MVA);
	if (s->mbox)
		t8122_clean_range_slc((unsigned long)s->mbox,
				      (unsigned long)(s->mbox + 1));
	t8122_clean_range_slc((unsigned long)s, (unsigned long)(s + 1));
	dsb(sy);
	t8122_write_hid4(pcore, hid4_orig);
}

/* AIC was already masked and saved before the cache clean in commit_regs. */
static void t8122_aic_mask_noop(void)
{
}

static void t8122_deep_wfi_arm(void)
{
	sysreg_clear_set_s(T8122_SYS_CYC_OVRD, 0, T8122_CYC_WFI_MODE);
	isb();
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
	isb();
	if (is_kernel_in_hyp_mode()) {
		dsb(sy);
		t8142_el1_wfi();
	}
}

static int t8122_raise_apcie(struct apple_sleep *s)
{
	static const struct { u32 off, val; } axi[] = {
		{ 0x0034, 0x0000ffff }, { 0x0038, 0x0000ffff }, { 0x0000, 0x00000211 },
		{ 0x000c, 0x00016800 }, { 0x0010, 0x0000b400 }, { 0x0108, 0x00100031 },
		{ 0x010c, 0x00016800 }, { 0x0110, 0x0000b400 }, { 0x0600, 0xc0010001 },
		{ 0x0c00, 0x01ffffff }, { 0x0d48, 0x00200000 }, { 0x0e58, 0x00800010 },
		{ 0x1000, 0x00000101 },
	};
	static const struct { u32 off, val; } cio3pll[] = {
		{ 0x2200, 0x00081f55 }, { 0x2224, 0x00000818 }, { 0x2228, 0x00000b18 },
		{ 0x224c, 0x1f800096 }, { 0x22e4, 0x01024201 }, { 0x22fc, 0x030b40b4 },
	};
	static const struct { u32 off, val; } phy_ip[] = {
		{ 0x0028, 0x00800800 }, { 0x006c, 0x0000004c }, { 0x0080, 0x80010001 }, { 0x0094, 0x00000001 },
		{ 0x1204, 0x0000004c }, { 0x1000, 0x2a2383c8 }, { 0x102c, 0x04454447 }, { 0x5080, 0x294d8190 },
		{ 0x5088, 0x00560800 }, { 0x508c, 0x294d8190 }, { 0x5094, 0x00060800 }, { 0x50a0, 0x00554800 },
		{ 0x50ac, 0x00554800 }, { 0x50b8, 0x00554800 }, { 0x5200, 0x0008155e }, { 0x5224, 0x00000818 },
		{ 0x5240, 0x04010f69 }, { 0x52b0, 0x10300811 }, { 0x52fc, 0x03078078 }, { 0x531c, 0x00000002 },
		{ 0x608c, 0x65918280 }, { 0x6224, 0x00000818 }, { 0x6228, 0x00000b18 }, { 0x6230, 0x0001e80f },
		{ 0x624c, 0x1f80009d }, { 0xa000, 0x080001aa }, { 0xa008, 0x0000000a }, { 0xa00c, 0x0000000a },
		{ 0xa010, 0x0000000a }, { 0xa014, 0x0000000a }, { 0xa038, 0x00000271 }, { 0xa040, 0x000000c8 },
		{ 0xa04c, 0x00000bff }, { 0xa050, 0x00000001 }, { 0xa068, 0x00000c41 }, { 0xa070, 0x55b00820 },
		{ 0xa084, 0x3e8f4240 }, { 0xa088, 0x00000fff }, { 0xa10c, 0x000f4240 }, { 0xa11c, 0x000f30dc },
		{ 0xa178, 0x00300000 }, { 0xa0c0, 0x14e051f8 }, { 0xa1a4, 0x00000901 }, { 0xa1b0, 0x00000000 },
		{ 0xa1b8, 0x00000000 }, { 0xa1e8, 0x00100000 }, { 0x903c, 0x018c3509 }, { 0x90d8, 0x0102068d },
		{ 0x90dc, 0x12004408 }, { 0x91e0, 0x00002008 }, { 0x9288, 0x0801172a }, { 0x9280, 0xcccccccc },
		{ 0x9000, 0x1e000000 }, { 0x9280, 0xccc8ccc1 }, { 0x928c, 0x00000003 }, { 0xb02c, 0x01980062 },
		{ 0xb098, 0x05028000 }, { 0xc01c, 0x000006a1 }, { 0xc080, 0x540c6831 }, { 0xc08c, 0x10be0c1f },
		{ 0xc0a0, 0x021a1780 }, { 0xc0ec, 0x00000000 }, { 0xd008, 0x0000000c }, { 0xd014, 0x04840408 },
	};
	void __iomem *ctrl, *run, *ready, *common;
	unsigned int i;
	u32 val;
	int ret;

	if (!t8122.apcie_axi || !t8122.apcie_rc || !t8122.apcie_clkgen ||
	    !t8122.apcie_phy || !t8122.apcie_phy_ip)
		return -ENODEV;

	ret = sleep_ps_raise_start(s, T8122_PS_APCIE);
	ret |= sleep_ps_raise_start(s, T8122_PS_ANS);
	ret |= sleep_ps_raise_start(s, T8122_PS_APCIE_GP);
	ret |= sleep_ps_raise_start(s, T8122_PS_APCIE_ST);
	ret |= sleep_ps_raise_start(s, T8122_PS_APCIE_PHY_SW);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(axi); i++)
		writel(axi[i].val, t8122.apcie_axi + axi[i].off);

	writel(0, t8122.apcie_rc + APCIE_RC_ZERO);

	writel(0x239, t8122.apcie_clkgen + APCIE_CLK_CTRL);
	writel(0x80000030, t8122.apcie_clkgen + APCIE_CLK_START);
	writel(0x80000239, t8122.apcie_clkgen + APCIE_CLK_CTRL);
	ret = readl_poll_timeout_atomic(t8122.apcie_clkgen + APCIE_CLK_CTRL, val,
					!(val & APCIE_CLK_BUSY), 1, APCIE_POLL_US);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(cio3pll); i++)
		writel(cio3pll[i].val, t8122.apcie_clkgen + cio3pll[i].off);
	writel(0x239, t8122.apcie_clkgen + APCIE_CLK_CTRL);

	ctrl = t8122.apcie_phy + APCIE_PHY_CTRL;
	run = t8122.apcie_phy + APCIE_PHY_RUN;
	ready = t8122.apcie_phy + APCIE_PHY_READY;
	common = t8122.apcie_phy + APCIE_PHY_COMMON;

	writel(0x03010030, t8122.apcie_phy + 0x800c);
	writel(0x0400c030, t8122.apcie_phy + 0x8010);
	writel(readl(ctrl) | APCIE_PHY_CTRL_CLK0REQ, ctrl);
	ret = readl_poll_timeout_atomic(ctrl, val, val & APCIE_PHY_CTRL_CLK0ACK, 1, APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(ctrl) | APCIE_PHY_CTRL_CLK1REQ, ctrl);
	ret = readl_poll_timeout_atomic(ctrl, val, val & APCIE_PHY_CTRL_CLK1ACK, 1, APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(ctrl) & ~T8122_APCIE_PHY_CTRL_RESET, ctrl);
	writel(readl(run) | APCIE_PHY_RUN_EN, run);

	for (i = 0; i < ARRAY_SIZE(phy_ip); i++)
		writel(phy_ip[i].val, t8122.apcie_phy_ip + phy_ip[i].off);

	writel(readl(run) | APCIE_PHY_RUN_EN2, run);
	ret = readl_poll_timeout_atomic(ready, val, val & BIT(0), 1, APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(common) | BIT(0), common);
	writel(readl(ctrl) | T8122_APCIE_PHY_CTRL_UP, ctrl);

	writel(0x140, t8122.apcie_rc + APCIE_RC_READY_CFG);
	writel(1, t8122.apcie_rc + APCIE_RC_READY_START);
	return readl_poll_timeout_atomic(t8122.apcie_rc + APCIE_RC_READY_STAT, val,
					 val & BIT(0), 1, APCIE_POLL_US);
}

static void t8122_port0_wake(struct apple_sleep *s)
{
	static const struct { u32 off, val; } port0_init[] = {
		{ 0x088, 0x00000110 }, { 0x100, 0xffffffff }, { 0x148, 0xffffffff },
		{ 0x210, 0xffffffff }, { 0x080, 0x00000000 }, { 0x084, 0x00000000 },
		{ 0x104, 0xfffffff0 }, { 0x124, 0x00000100 }, { 0x16c, 0x00000000 },
		{ 0x13c, 0x00000010 }, { 0x800, 0x00100100 }, { 0x808, 0x001000ff },
		{ 0x82c, 0x00000000 }, { 0x130, 0x03000000 }, { 0x140, 0x00000010 },
		{ 0x144, 0x00253770 }, { 0x21c, 0x00000000 }, { 0x834, 0x00000000 },
		{ 0x83c, 0x00000000 }, { 0x140, 0x00000011 }, { 0x8a4, 0x00002020 },
	};
	void __iomem *port = t8122.apcie_port0;
	unsigned int i;
	u32 val;

	writel(readl(t8122.apcie_axi + 0x600) & ~BIT(16), t8122.apcie_axi + 0x600);

	for (i = 0; i < ARRAY_SIZE(port0_init); i++)
		writel(port0_init[i].val, port + port0_init[i].off);

	writel(0x00100101, port + APCIE_PORT_APPCLK);
	writel(1, port + APCIE_PORT_PERST);
	readl_poll_timeout_atomic(port + 0x804, val, val & BIT(0), 1, APCIE_POLL_US);
	writel(0x00100001, port + APCIE_PORT_APPCLK);
	writel(3, port + 0x4020);

	writel(readl(t8122.apcie_axi + 0x600) | BIT(16), t8122.apcie_axi + 0x600);

	sleep_apcie_port_clock(port, t8122.apcie_phy + 0xc000);
}

static void t8122_sleep_wake(struct apple_sleep *s)
{
	u32 val;

	writel(CLUSTER_ON, s->cluster + T8122_CLUSTER_E);
	s->cluster_e_ret = readl_poll_timeout_atomic(s->cluster + T8122_CLUSTER_E, val,
						     val & CLUSTER_E_READY, 1,
						     CLUSTER_TIMEOUT_US);
	writel(CLUSTER_ON, s->cluster + T8122_CLUSTER_P);

	writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_wake[0]);
	writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_wake[1]);

	if (s->ps_aon)
		sleep_ps_rewake(s);
	/* Ensure GPIO domain is on before apple_sleep_restore_pins(). */
	sleep_ps_raise(s, T8122_PS_GPIO);
	s->pins_lost = s->pins_saved_valid;

	if (t8122.apcie_axi) {
		if (!t8122_raise_apcie(s) && t8122.apcie_port0 && t8122.apcie_phy)
			t8122_port0_wake(s);
		sleep_ps_raise_finish(s, T8122_PS_APCIE);
		sleep_ps_raise_finish(s, T8122_PS_ANS);
		sleep_ps_raise_finish(s, T8122_PS_APCIE_GP);
		sleep_ps_raise_finish(s, T8122_PS_APCIE_ST);
		sleep_ps_raise_finish(s, T8122_PS_APCIE_PHY_SW);
	}

	val = readl(s->ps + T8122_PS_SEP);
	if (val & PS_DEV_DISABLE)
		writel(val & ~PS_DEV_DISABLE, s->ps + T8122_PS_SEP);

	writel(PWRGATE_ON, s->pwrgate + T8122_PWRGATE_E);
	writel(PWRGATE_ON, s->pwrgate + T8122_PWRGATE_P);
}

static const struct apple_sleep_gate_coproc t8122_gate_coprocs[] = {
	{ "/soc/dcp@28ec00000", true, T8122_PS_DISP_CPU },
	{ "/soc/pmp@2d0500000", true },
	{ "/soc/gpu@292400000", true, T8122_PS_GFX },
	{ "/soc/sep@29e400000" },
	{ "/soc/nvme@30dcc0000" },
	{ "/soc/smc@2ec400000" },
};

static const struct apple_sleep_wake_name t8122_wake_names[] = {
	{ 3, "AOP.CPUWakeupAP" }, { 12, "AOP.GPIO.IRQ6" }, { 51, "AOP.OutboxNotEmpty" },
	{ 74, "AOP.SPMI0.SW3" }, { 83, "AOP.SPMI1.SW3" }, { 87, "ATC0.CIOWakeup" },
	{ 88, "ATC0.USBWakeup" }, { 89, "ATC1.CIOWakeup" }, { 90, "ATC1.USBWakeup" },
	{ 100, "MTP.DOCK.CHANNELS.AP0.IRQ" }, { 107, "MTP.GPIO.IRQ4" }, { 109, "MTP.GPIO.IRQ6" },
	{ 128, "NUB.APWakeupTime" }, { 129, "NUB.APWatchdog" }, { 143, "NUB.GPIO.IRQ6" },
	{ 155, "NUB.SPMI0.SW3" }, { 165, "NUB.SPMI1.SW3" }, { 179, "NUB.WakeupAP" },
	{ 184, "SMC.CPUWakeupAP" }, { 208, "SMC.OutboxNotEmpty" },
};

static const u32 t8122_wake_required[] = { 155 };

static int t8122_sleep_cpu_prepare(u64 mpidr)
{
	return 0;
}

static void t8122_sleep_cpu_restore(u64 mpidr)
{
}

static const struct apple_sleep_ops t8122_sleep_ops = {
	.deep_wfi_arm = t8122_deep_wfi_arm,
	.resume = cpu_resume,
	.aic_mask_all = t8122_aic_mask_noop,
	.aic_ack = aic_sleep_ack,
	.aic_restore = aic_sleep_restore,
	.map_regs = t8122_sleep_map_regs,
	.display_off = t8122_sleep_display_off,
	.prepare_regs = t8122_sleep_prepare_regs,
	.undo_regs = t8122_sleep_undo_regs,
	.commit_regs = t8122_sleep_commit_regs,
	.pre_commit = t8122_sleep_pre_commit,
	.wake = t8122_sleep_wake,
	.cpu_prepare = t8122_sleep_cpu_prepare,
	.cpu_restore = t8122_sleep_cpu_restore,
};

static const struct apple_sleep_soc t8122_sleep_soc = {
	.ops = &t8122_sleep_ops,
	.pen_deep = true,
	.el1_wfi = true,
	.no_pmu_notes = true,
	.early_context = true,
	.stray = t8122_stray,
	.nstray = ARRAY_SIZE(t8122_stray),
	.wake_mask = 0x08,
	.wake_status = 0x24,
	.wake_words = 7,
	.wake_last_word = GENMASK(16, 0),
	.wake_names = t8122_wake_names,
	.nwake_names = ARRAY_SIZE(t8122_wake_names),
	.no_wd2 = true,
	.early_cpu_wfi = true,
	.debug_cut_after_stops = true,
	.boot_mpidr = 0x10100,
	.cpu_count = 8,
	.wake_required = t8122_wake_required,
	.nwake_required = ARRAY_SIZE(t8122_wake_required),
	.boot_wfi_delay_ms = 2,
	.ndisp = 1,
	.nmcc = 4,
	.cluster_cpu = { T8122_CLUSTER_E, T8122_CLUSTER_P },
	.ncluster_cpu = 2,
	.cpu_clusters = 2,
	.cpu_cores_per_cluster = 4,
	.cpu_start_stride = 4,
	.pwrgate_wake = { 0x000, 0x040 },
	.pwrgate_cpm = { T8122_PWRGATE_E, T8122_PWRGATE_P },
	.pmu_usid = 0xe,
	.pmu_sleep_reg = 0xf701,
	.pmu_sleep_val = 0x50,
	.spmi_notify_sleep = 0x2000001,
	.ps_aon_debug_switch = 0xa8,
	.ps = {
		.disp = { { T8122_PS_DISP_CPU, T8122_PS_DISP_FE, T8122_PS_DISP_SYS } },
		.sep = T8122_PS_SEP,
		.ans = T8122_PS_ANS,
		.apcie_sys_gp = T8122_PS_ANS,
	},
	.ps_all = SLEEP_WORDS(t8122_ps_all),
	.ps_aon_all = SLEEP_WORDS(t8122_ps_aon_all),
	.ps_wake_order = SLEEP_WORDS(t8122_ps_wake_order),
	.ps_aon_wake_order = SLEEP_WORDS(t8122_ps_aon_wake_order),
	.ps_macos_on = SLEEP_WORDS(t8122_ps_macos_on),
	.ps_macos_off = SLEEP_WORDS(t8122_ps_macos_off),
	.gate_coprocs = t8122_gate_coprocs,
	.ngate_coprocs = ARRAY_SIZE(t8122_gate_coprocs),
};
