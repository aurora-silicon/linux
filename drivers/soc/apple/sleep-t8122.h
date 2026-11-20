/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T8122 (J613, MacBook Air M3): sleep in qualification under the shared
 * untested/untested_commit gates and matched only with APPLE_SOC_SLEEP_T8122.
 *
 * Those gates are the only ones: deep sleep is offered only with apple_sleep.deep=1, and a
 * sleep is a dry run unless apple_sleep.untested_commit=1. The dry run runs every refusal and
 * the first witness and returns: no core held, no power word written, no SMC notice beyond
 * the ones its driver's own suspend sends and takes back, nothing stopped. The display
 * coprocessor is left running by it (display_no_restart). No PMU scratch is written for
 * step notes (no_pmu_notes): the scratch's place is unmeasured on this PMU.
 *
 * The register sequence is the one measured from a macOS sleep on J613 and tried alone by
 * apple-t8122-sleep.c: the PMU's sleep byte, both SPMI sleep-notify words, the display's
 * words, the two power gates, the cluster controls, CLLT and the commit word. That trial
 * reached the commit and the last WFI with the SoC still powered, with the GPU, the SEP and
 * the PMP never stopped: their refusals were forced through.
 *
 * Here the same sequence runs as this driver's SoC operations, so the refusals are real and
 * the coprocessors stop where the working SoCs stop theirs: SEP, then the GPU, then the PMP
 * and the rest. Whether that is what J613's sleep engine waits for is the hypothesis, so a
 * witness reads every named power word and each coprocessor's run control at four points:
 * after the last refusal, after the SEP and GPU stops, after the remaining stops (set against
 * macOS's commit, while the debug link still carries it), and after the link's cut.
 *
 * No wake is qualified. The wake sources are armed only with t8122_wake_mask=1, at mask
 * offsets taken from T8140's layout and unmeasured here; the display and the GPU have no
 * restart; and a real commit is recovered by reset.
 *
 * The SMC's order is not macOS's. Measured before its commit: MBSE=slpw 240 ms, pmVC 147 ms,
 * WKTP=2 129 ms, then the PMU's sleep byte, both notify words and the PMP's stop, and only
 * then MBSE=slpa (25.6 ms) and the AP idle notice (23.3 ms). Here macsmc's noirq suspend
 * sends slpa and the idle notice before this entry starts, so before the PMU byte and every
 * stop, and sends no pmVC. The shared driver has two ways to put the notice behind the peer
 * stops: J813's deferred notice, and the SMC policy of a SoC with early_context (T6050's:
 * slpw, pmVC after a measured spacing, then slpa and the idle notice from .commit_notice,
 * after the stops). The second is macOS's order here. It is not taken yet for two reasons:
 * pmVC's four bytes travel in the SMC's shared memory and were not captured, and its
 * saved-context mailbox transport has never run on this SMC, where a failed notice is fatal
 * after the stops. The order stays an open difference until both are settled.
 */

#include <linux/console.h>

#define T8122_PS_DISP_SYS	0x1d8
#define T8122_PS_GFX		0x1e0
#define T8122_PS_DISP_FE	0x208
#define T8122_PS_I2C3		0x248
#define T8122_PS_SPI2		0x2a8
#define T8122_PS_ANS		0x3f8
#define T8122_PS_DISPEXT_SYS	0x410
#define T8122_PS_PMP		0x450
#define T8122_PS_DISPEXT_FE	0x480
#define T8122_PS_DISPEXT_CPU	0x488
#define T8122_PS_SEP		0xc00
#define T8122_PS_DISP_CPU	0x10000

#define T8122_PWRGATE_E		0x80
#define T8122_PWRGATE_P		0x88
#define T8122_PWRGATE_DISP_A	0xcac
#define T8122_PWRGATE_DISP_B	0xcd0
#define T8122_PWRGATE_DISP_C	0xcf0
#define T8122_CLUSTER_E		0x0
#define T8122_CLUSTER_P		0x4
#define T8122_CLLT_CTL		0x0
#define T8122_CLLT_LAST		0x24

#define T8122_WDT_WD1_BITE	0x14
#define T8122_WDT_WD1_CTL	0x1c
#define T8122_WDT_RESET_EN	BIT(2)

/* The debug cable's DockChannel UART, channel 1 data page */
#define T8122_DC_TX8		0x04
#define T8122_DC_TX_FREE	0x14

static unsigned int t8122_wdt_secs;
module_param(t8122_wdt_secs, uint, 0600);
MODULE_PARM_DESC(t8122_wdt_secs,
		 "J613 trial: reset watchdog left armed this long across the commit (0: none)");

static unsigned int t8122_wdt_clear;
module_param(t8122_wdt_clear, uint, 0600);
MODULE_PARM_DESC(t8122_wdt_clear,
		 "J613 trial: 0 leave the watchdogs, 1 clear WD0's control, 2 the writes measured under macOS");

static unsigned int t8122_disp_off = 1;
module_param(t8122_disp_off, uint, 0600);
MODULE_PARM_DESC(t8122_disp_off,
		 "J613 trial: bit 0 lower the internal display as measured, bit 1 also the other words that window lowers");

static bool t8122_console;
module_param(t8122_console, bool, 0400);
MODULE_PARM_DESC(t8122_console, "J613 trial: witness and marks on the debug-cable DockChannel");

static bool t8122_keep_debug_link;
module_param(t8122_keep_debug_link, bool, 0400);
MODULE_PARM_DESC(t8122_keep_debug_link,
		 "J613 trial: leave a live debug link up across the commit (it may hold the SoC powered)");

static bool t8122_witness_full;
module_param(t8122_witness_full, bool, 0600);
MODULE_PARM_DESC(t8122_witness_full,
		 "J613 trial: the long witness at the last refusal, waiting on the cable (the SEP then misses its SLEEP)");

static bool t8122_witness_sep_asc;
module_param(t8122_witness_sep_asc, bool, 0600);
MODULE_PARM_DESC(t8122_witness_sep_asc,
		 "J613 trial: also read the SEP's ASC run control in the witness (unproven to be readable)");

static unsigned int t8122_match = 1;
module_param(t8122_match, uint, 0600);
MODULE_PARM_DESC(t8122_match,
		 "J613 trial: macOS's commit state: bit 0 the on and off lists, bit 1 ANS and its PCIe stream word on, bit 2 SIO_CPU and APCIE_PHY_SW as it has them");

static unsigned int t8122_atc_off = 3;
module_param(t8122_atc_off, uint, 0600);
MODULE_PARM_DESC(t8122_atc_off,
		 "J613 trial: lower the USB port's words as macOS has them at its commit: bit 0 ATC0's, bit 1 ATC1's, bit 2 IOA1 after them");

static unsigned int t8122_wake_mask;
module_param(t8122_wake_mask, uint, 0600);
MODULE_PARM_DESC(t8122_wake_mask,
		 "J613 trial: 1 masks every wake source but the tree's apple,wake-enable, through the driver's own arm");

static bool t8122_quiet;
module_param(t8122_quiet, bool, 0600);
MODULE_PARM_DESC(t8122_quiet,
		 "J613 trial: mask the EL1 timers and stop the core PMU before each core's last WFI, as T8142 does");

static bool t8122_mcc;
module_param(t8122_mcc, bool, 0600);
MODULE_PARM_DESC(t8122_mcc,
		 "J613 trial: step the memory controller down after the watchdog-control clear, as macOS");

static bool t8122_macos_commit;
static bool t8122_post_commit_watch;
module_param(t8122_macos_commit, bool, 0600);
MODULE_PARM_DESC(t8122_macos_commit,
		 "J613 trial: after the commit word, macOS's tail: PMU RTC alarm unmask and arm, then CLLT and the commit word twice more");

static bool t8122_macos_state;
module_param(t8122_macos_state, bool, 0600);
MODULE_PARM_DESC(t8122_macos_state,
		 "J613 trial: at the commit, macOS's wake-source masks and its off state for ISP, I2C1-3, SPI2, QSPI, nub SPI0 and nub OCLA (the commit-dump diff)");

static bool t8122_dump;
module_param(t8122_dump, bool, 0600);
MODULE_PARM_DESC(t8122_dump,
		 "J613 trial: print, before the commit word, the words macOS's intercepted-commit snapshot reads (capture runs 43-49), for a one-to-one diff");

static bool t8122_dry_regs;
module_param(t8122_dry_regs, bool, 0600);
MODULE_PARM_DESC(t8122_dry_regs,
		 "J613 trial: a dry run prints the boot core's timer, PMU and deep-sleep registers (reads only)");

/* Every power word J613's device tree names, as the trial's entry dump has them. */
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
 * The same words at macOS 25G83's commit on J613, as its hypervisor read them. Its console
 * was UART0, so UART_P, UART_N and UART0 stand on for that alone.
 */
#define T8122_M_NONE		0xffffffff
static const u32 t8122_ps_macos[ARRAY_SIZE(t8122_ps_all)] = {
	0x1f0002ff, 0x00000200, 0x000000ff, 0x000000ff, 0x000000ff, 0x1f0002ff,
	0x1f0002ff, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200,
	0x000000ff, 0x000000ff, 0x000000ff, 0x00000200, 0x04000200, 0x04000200,
	0x04000200, 0x04000200, 0x00000200, 0x1f0003ff, 0x00000300, 0x00002300,
	0x0f002300, 0x00002300, 0x0f002300, 0x0f000300, 0x00002300, 0x1400024f,
	0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200,
	0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200,
	0x000002ff, 0x000002ff, 0x000002ff, 0x00000300, 0x00000200, 0x00000200,
	0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x000002ff,
	0x000000ff, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200,
	0x00000200, 0x00000200, 0x00000200, 0x1f0000ff, 0x1f0000ff, 0x1f0000ff,
	0x1f0000ff, 0x1f0003ff, 0x1f0003ff, 0x1f0003ff, 0x1f0003ff, 0x00000200,
	0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200, 0x00000200,
	0x00000300, 0x00000200, 0x1f0000ff, 0x000000ff, 0x1f0023ff, 0x000023ff,
	0x000023ff, 0x00002300, 0x0f002300, 0x00002300, 0x0f002300, T8122_M_NONE,
	0x0f002300, 0x1f0023ff, 0x1f0003ff, 0x1f0f03ff, 0x1f0003ff, 0x00000300,
	0x00000300, 0x00000300, 0x00000300, 0x00002300, 0x0f040300, 0x00000300,
	0x00000300, 0x1400024f, 0x00000300, 0x00000300, 0x00000300, 0x00000300,
	0x1000270f, T8122_M_NONE, T8122_M_NONE, T8122_M_NONE, T8122_M_NONE, T8122_M_NONE,
	T8122_M_NONE, T8122_M_NONE, T8122_M_NONE, T8122_M_NONE, T8122_M_NONE, T8122_M_NONE,
	T8122_M_NONE, T8122_M_NONE, 0x00000300, 0x00000300, 0x00000300, 0x00000300,
	0x00000300, 0x0f000300,
};

/*
 * On at macOS's commit and off once Linux's drivers have suspended: DWI, GPIO, SOC_RC, SPI_P,
 * UART_P, AUDIO_P, UART_N, UART0. Parents first.
 */
static const u32 t8122_ps_macos_on[] = {
	0x118, 0x120, 0x168, 0x278, 0x280, 0x288, 0x2d0, 0x2d8,
};

/* Off at macOS's commit, and left clock-gated (state 4) by the bootloader: nothing binds them. */
static const u32 t8122_ps_macos_off[] = {
	0x108, 0x138, 0x140, 0x148, 0x150, 0x158, 0x178, 0x228, 0x258, 0x260,
	0x268, 0x270, 0x2f8, 0x300, 0x308, 0x378, 0x380, 0x388, 0x390, 0x3b8,
};

/*
 * Each USB port's words, in the main block and in the mini PMGR ("ps-aon"). macOS has all of
 * them off long before a sleep (0x23f0 to COMMON, 0x3f0 to the other two) and Linux's drivers
 * leave them on. Under the hypervisor macOS powered down only once the port carrying the
 * debug link had its three lowered as well; with them held on it committed and stayed up.
 */
/* The fourth word of the debug port's path: the hypervisor gated it with the other three. */
#define T8122_PS_IOA1		0x3c0
#define T8122_PS_ATC0_COMMON	0x400
#define T8122_PS_ATC1_COMMON	0x408
#define T8122_AON_ATC0_USB_AON	0xb0
#define T8122_AON_ATC1_USB_AON	0xb8
#define T8122_AON_ATC0_USB	0xc0
#define T8122_AON_ATC1_USB	0xc8

#define T8122_PS_SIO_CPU	0x210
#define T8122_PS_APCIE_ST	0x448
#define T8122_PS_APCIE_PHY_SW	0x4a0
/* Both as macOS has them at its commit: on, and idled to 4 by the hardware. */
#define T8122_PS_AUTO_IDLE	0x1400000f

/*
 * The coprocessors the witness reads: run control and status, only with the CPU's domain up,
 * since their registers don't answer without it.
 */
enum { T8122_W_GPU, T8122_W_PMP, T8122_W_ANS, T8122_W_DCP, T8122_W_SEP, T8122_W_COPROCS };

static const struct {
	const char *name;
	const char *path;
	const char *reg;
	u32 ps;
} t8122_witness_coprocs[T8122_W_COPROCS] = {
	[T8122_W_GPU] = { "gpu", "/soc/gpu@292400000", "asc", T8122_PS_GFX },
	[T8122_W_PMP] = { "pmp", "/soc/pmp@2d0500000", "asc", T8122_PS_PMP },
	[T8122_W_ANS] = { "ans", "/soc/nvme@30dcc0000", "ans", T8122_PS_ANS },
	[T8122_W_DCP] = { "dcp", "/soc/dcp@28ec00000", "coproc", T8122_PS_DISP_CPU },
	[T8122_W_SEP] = { "sep", "/soc/sep@29e400000", NULL, T8122_PS_SEP },
};

/* Not read: its domain was off, or (the SEP's) reading it wasn't asked for. */
#define T8122_W_UNREAD		0xdead0ff0

#define T8122_W_SNAPS		4
#define T8122_W_EXTRA		7
#define T8122_W_AON		5
/* The wake block whole, 0x00 to 0x44: SLP_DDR, the masks, and wherever the status sits. */
#define T8122_W_WAKE		18

struct t8122_witness {
	bool taken;
	u32 ps[ARRAY_SIZE(t8122_ps_all)];
	u32 control[T8122_W_COPROCS];
	u32 status[T8122_W_COPROCS];
	/* Power gates E and P, cluster controls E and P, CLLT's last word, both notify words */
	u32 extra[T8122_W_EXTRA];
	/* The mini PMGR's debug switch and the two ports' words, 0xa8 to 0xc8 */
	u32 aon[T8122_W_AON];
	u32 wake[T8122_W_WAKE];
};

static struct {
	void __iomem *asc[T8122_W_COPROCS];
	void __iomem *hpm_notify;
	void __iomem *dc_uart;
	struct t8122_witness snap[T8122_W_SNAPS];
	u32 notify_saved[2];
	bool notify_staged;
	bool matched;
	unsigned int match_done;
	u32 match_saved[4];
	bool wdt_armed;
	/* Nobody drained the DockChannel: no more is sent this sleep. */
	bool dc_stalled;
	/* The link is gone (it goes with the SEP's stop): what doesn't fit the FIFO is dropped. */
	bool dc_nowait;
	u32 dc_dropped;
	int disp_poll_us;
	unsigned int wfi_returns;
	void __iomem *mcc[5];
	bool mcc_ok;
	void __iomem *dump_aon;
} t8122;

/*
 * macOS's state at its commit where the Linux trial differed (commit-dump diff, d2 of
 * 2026-11-19 against capture runs 31 and 49): the wake-source masks at +0x08..+0x20 almost all
 * set where Linux left all zero, and these domains off where Linux had them on.
 */
static const u32 t8122_macos_wake_masks[] = {
	0xffffeff7, 0xfff7ffff, 0xf877fbff, 0xffffd7ef, 0xf7ff7ffc, 0xfef7ffdf, 0x0006ffff,
};
/* Second diff (d2 on n4): FPWM1 and MCA2 still partly on, and the other port's ATC1 domains. */
static const u16 t8122_macos_ps_off[] = { 0x1e8, 0x220, 0x238, 0x240, 0x248, 0x2a8, 0x2c8, 0x3a8 };
static const u16 t8122_macos_aon_off[] = { 0x80, 0x88, 0xb8, 0xc8 };

/*
 * J613 trial: wake source 208 (SMC.OutboxNotEmpty, status +0x3c bit 16) reads pending after
 * Linux's commit with the SMC's mailbox empty (n14, n15); at macOS's commit it reads 0 and
 * macOS leaves it unmasked. 1: mask it (a diagnostic: macOS does not); 2: write the bit back to
 * its status word before the commit, as a write-one-to-clear. Both print S208 before/after.
 */
static void t8122_dc_putc(unsigned char c);
static void t8122_dc_puts(const char *str);
static void t8122_dc_hex(u32 v);

static unsigned int t8122_wake208;
module_param(t8122_wake208, uint, 0600);
MODULE_PARM_DESC(t8122_wake208,
		 "J613 trial: wake source 208 pending at the commit: 1 mask it, 2 write-one-to-clear its status");

static void t8122_wake208_apply(struct apple_sleep *s)
{
	u32 before, after;

	if (!s->wake || !t8122_wake208)
		return;
	before = readl(s->wake + 0x3c);
	/* 3: report only (with the drain after the cut: how many it took, +1000 if the cut ran) */
	if (t8122_wake208 == 3) {
		t8122_dc_puts("\nS208 ");
		t8122_dc_hex(before);
		t8122_dc_puts(" drained ");
		t8122_dc_hex((u32)s->smc_drained);
		t8122_dc_putc('\n');
		return;
	}
	if (t8122_wake208 == 1)
		writel(readl(s->wake + 0x20) | BIT(16), s->wake + 0x20);
	else if (t8122_wake208 == 2 && (before & BIT(16)))
		writel(BIT(16), s->wake + 0x3c);
	after = readl(s->wake + 0x3c);
	t8122_dc_puts("\nS208 ");
	t8122_dc_hex(before);
	t8122_dc_putc('/');
	t8122_dc_hex(after);
	t8122_dc_puts(" m=");
	t8122_dc_hex(readl(s->wake + 0x20));
	t8122_dc_putc('\n');
}

/*
 * J613 trial, with t8122_keep_debug_link (no port-controller cold reset): macOS's own release of
 * the debug link at its commit. Under the capture hypervisor the ATC0 debug-link words at mini
 * PMGR +0xb0..+0xc8 drop to 0x300 (XNU's 0x3f0 requests) just before the commit, while
 * debug_gated (+0x00) and the debug switch (+0xa8) stay fully on (capture 63). macos_state
 * already lowers +0xb8 and +0xc8; this lowers +0xb0 and +0xc0 as well, last.
 */
static bool t8122_link_release;
module_param(t8122_link_release, bool, 0600);
MODULE_PARM_DESC(t8122_link_release, "J613 trial: with keep_debug_link, lower the ATC0 debug-link words (mini PMGR +0xb0, +0xc0) just before the commit, as macOS; debug_gated and the switch stay on");

static void t8122_link_release_apply(void)
{
	static const u16 off[] = { 0xb0, 0xc0 };
	unsigned int i;

	if (!t8122_link_release || !t8122.dump_aon)
		return;
	for (i = 0; i < ARRAY_SIZE(off); i++)
		writel(0x3f0, t8122.dump_aon + off[i]);
	t8122_dc_puts("\nL ");
	for (i = 0x00; i <= 0xc8; i += 8) {
		if (i != 0x00 && i != 0xa8 && i < 0xb0)
			continue;
		t8122_dc_hex(readl(t8122.dump_aon + i));
		t8122_dc_putc(' ');
	}
	t8122_dc_putc('\n');
}

static void t8122_macos_state_apply(struct apple_sleep *s)
{
	unsigned int i;

	if (s->wake)
		for (i = 0; i < ARRAY_SIZE(t8122_macos_wake_masks); i++)
			writel(t8122_macos_wake_masks[i], s->wake + 0x08 + i * 4);
	t8122_wake208_apply(s);
	for (i = 0; i < ARRAY_SIZE(t8122_macos_ps_off); i++) {
		u32 v = readl(s->ps + t8122_macos_ps_off[i]);

		if (v & 0xf)
			writel(v & ~0xfU, s->ps + t8122_macos_ps_off[i]);
	}
	if (t8122.dump_aon)
		for (i = 0; i < ARRAY_SIZE(t8122_macos_aon_off); i++) {
			u32 v = readl(t8122.dump_aon + t8122_macos_aon_off[i]);

			if (v & 0xf)
				writel(v & ~0xfU, t8122.dump_aon + t8122_macos_aon_off[i]);
		}
}

/*
 * J613's memory-controller stop, as macOS ran it on both sleeps of run 52: CPU4, after the
 * CPU disables and the WD0 control clear, before the power gates, over about 1.1 ms. Pages
 * (device-tree reg-names): "mcc-a" 0x220024000, "mcc-b" 0x220064000, "mcc-c" 0x220158000,
 * "mcc-d" 0x220400000, "mcc-e" 0x2201c1000. The first write clears mcc-e +0xc00; every later
 * step clears the lowest set bit of a fresh read of one word, in the measured interleave
 * below. Words A-F are 64-bit, G and H 32-bit, as macOS accessed them.
 */
static const struct {
	u8 page;
	u16 off;
	bool wide;
} t8122_mcc_words[] = {
	['A' - 'A'] = { 2, 0x000, true },	/* 0x220158000: 0x7ffffff down to 0 */
	['B' - 'A'] = { 0, 0x000, true },	/* 0x220024000: 0xffff down to 0 */
	['C' - 'A'] = { 1, 0x000, true },	/* 0x220064000: 0xffff down to 0 */
	['D' - 'A'] = { 2, 0x800, true },	/* 0x220158800: 0xff down to 0 */
	['E' - 'A'] = { 0, 0x800, true },	/* 0x220024800: 3, 2, 0 */
	['F' - 'A'] = { 1, 0x800, true },	/* 0x220064800: 3, 2, 0 */
	['G' - 'A'] = { 3, 0x584, false },	/* 0x220400584: 0x1ff down to 0 */
	['H' - 'A'] = { 3, 0x588, false },	/* 0x220400588: 0x11, 0x10, 0 */
};

static const char t8122_mcc_order[] =
	"ABABABABABABAAAAAAAAABABABAABABABABABABABACACCCCCCCCCCCCCCCDDDEDEDFDFDDGGGGGGGGGHH";

static void t8122_mcc_stop(struct apple_sleep *s)
{
	unsigned int i;

	if (!t8122.mcc_ok)
		return;
	writel(0, t8122.mcc[4] + 0xc00);
	for (i = 0; t8122_mcc_order[i]; i++) {
		unsigned int w = t8122_mcc_order[i] - 'A';
		void __iomem *reg = t8122.mcc[t8122_mcc_words[w].page] + t8122_mcc_words[w].off;

		if (t8122_mcc_words[w].wide) {
			u64 v = readq(reg);

			if (v)
				writeq(v & (v - 1), reg);
		} else {
			u32 v = readl(reg);

			if (v)
				writel(v & (v - 1), reg);
		}
	}
}

static void t8122_dc_putc(unsigned char c)
{
	/* The witness is long and the cable slow: a second for room. One timeout ends all output. */
	u64 start = read_sysreg(cntvct_el0), hz = arch_timer_get_cntfrq();
	bool room;

	if (!t8122.dc_uart || t8122.dc_stalled)
		return;
	if (t8122.dc_nowait) {
		if (readl(t8122.dc_uart + T8122_DC_TX_FREE))
			writel(c, t8122.dc_uart + T8122_DC_TX8);
		else
			t8122.dc_dropped++;
		return;
	}
	while (!(room = readl(t8122.dc_uart + T8122_DC_TX_FREE)) &&
	       read_sysreg(cntvct_el0) - start < hz)
		cpu_relax();
	if (room)
		writel(c, t8122.dc_uart + T8122_DC_TX8);
	else
		t8122.dc_stalled = true;
}

/*
 * The kernel's console on the same channel, for a trial booted from RAM with the debug cable
 * as its only output (apple_sleep.t8122_console=1): this tree gives T8122 no DockChannel UART
 * node, so no serial console binds. Each character waits a bounded time for room and is then
 * dropped, so a cable nobody reads never stalls printk.
 */
static void t8122_dc_console_write(struct console *con, const char *str, unsigned int n)
{
	while (n--) {
		unsigned char c = *str++;
		unsigned int pass = c == '\n' ? 2 : 1;

		while (pass--) {
			unsigned int tries = 4000;

			while (!readl(t8122.dc_uart + T8122_DC_TX_FREE) && --tries)
				cpu_relax();
			if (tries)
				writel(pass ? '\r' : c, t8122.dc_uart + T8122_DC_TX8);
		}
	}
}

static struct console t8122_dc_console = {
	.name = "t8122dc",
	.write = t8122_dc_console_write,
	.flags = CON_ENABLED | CON_ANYTIME | CON_PRINTBUFFER,
	.index = -1,
};

static void t8122_dc_puts(const char *str)
{
	while (*str) {
		if (*str == '\n')
			t8122_dc_putc('\r');
		t8122_dc_putc(*str++);
	}
}

static void t8122_dc_hex(u32 v)
{
	int i;

	for (i = 28; i >= 0; i -= 4)
		t8122_dc_putc("0123456789abcdef"[(v >> i) & 0xf]);
}

static void t8122_dc_pair(const char *name, u32 a, u32 b)
{
	t8122_dc_putc(' ');
	t8122_dc_puts(name);
	t8122_dc_putc('=');
	t8122_dc_hex(a);
	t8122_dc_putc('/');
	t8122_dc_hex(b);
}

static void t8122_witness_take(struct apple_sleep *s, unsigned int n)
{
	struct t8122_witness *w = &t8122.snap[n];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(t8122_ps_all); i++)
		w->ps[i] = readl(s->ps + t8122_ps_all[i]);
	for (i = 0; i < T8122_W_COPROCS; i++) {
		bool up = FIELD_GET(PS_ACTUAL, readl(s->ps + t8122_witness_coprocs[i].ps));

		w->control[i] = w->status[i] = T8122_W_UNREAD;
		/*
		 * With IOA1 lowered a coprocessor's registers abort (attempt N died here, in
		 * the snapshot after the lower): the last snapshot reads none.
		 */
		if (!t8122.asc[i] || !up || (i == T8122_W_SEP && !t8122_witness_sep_asc) ||
		    (n == T8122_W_SNAPS - 1 && (t8122_atc_off & 4)))
			continue;
		w->control[i] = readl(t8122.asc[i] + ASC_CPU_CONTROL);
		w->status[i] = readl(t8122.asc[i] + ASC_CPU_STATUS);
	}
	w->extra[0] = readl(s->pwrgate + T8122_PWRGATE_E);
	w->extra[1] = readl(s->pwrgate + T8122_PWRGATE_P);
	w->extra[2] = readl(s->cluster + T8122_CLUSTER_E);
	w->extra[3] = readl(s->cluster + T8122_CLUSTER_P);
	w->extra[4] = readl(s->cllt + T8122_CLLT_LAST);
	w->extra[5] = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	w->extra[6] = readl(t8122.hpm_notify + SPMI_NOTIFY_ENABLE);
	for (i = 0; i < T8122_W_AON; i++)
		w->aon[i] = s->ps_aon ? readl(s->ps_aon + 0xa8 + i * 8) : T8122_W_UNREAD;
	for (i = 0; i < T8122_W_WAKE; i++)
		w->wake[i] = s->wake ? readl(s->wake + i * 4) : T8122_W_UNREAD;
	w->taken = true;
}

/*
 * Straight to the DockChannel, with no printk this late. Snapshot 0 whole; a later one only
 * the power words that differ from the one before, and every coprocessor again.
 */
static void t8122_witness_dc(unsigned int n)
{
	const struct t8122_witness *w = &t8122.snap[n], *prev = n ? &t8122.snap[n - 1] : NULL;
	unsigned int i;

	if (!t8122.dc_uart || !w->taken)
		return;
	if (!t8122_witness_full) {
		/*
		 * Nothing may wait here: a second spent before the SEP's SLEEP and it never
		 * answers, and the link goes with its stop. So short lines that fit the FIFO
		 * (2047 bytes) for the next reader: PMP control and status, both notify words,
		 * the mini PMGR's five; after the last one, every word that differs from
		 * macOS's commit.
		 */
		t8122_dc_puts("\nW");
		t8122_dc_putc('0' + n);
		t8122_dc_pair("p", w->control[T8122_W_PMP], w->status[T8122_W_PMP]);
		t8122_dc_pair("n", w->extra[5], w->extra[6]);
		t8122_dc_puts(" a");
		for (i = 0; i < T8122_W_AON; i++) {
			t8122_dc_putc(i ? '/' : '=');
			t8122_dc_hex(w->aon[i]);
		}
		if (n != T8122_W_SNAPS - 1)
			return;
		for (i = 0; i < ARRAY_SIZE(t8122_ps_all); i++) {
			if (t8122_ps_macos[i] == T8122_M_NONE || t8122_ps_macos[i] == w->ps[i])
				continue;
			t8122_dc_putc(' ');
			t8122_dc_hex(t8122_ps_all[i]);
			t8122_dc_putc(':');
			t8122_dc_hex(w->ps[i]);
		}
		t8122_dc_puts(" k");
		for (i = 0; i < T8122_W_WAKE; i++) {
			t8122_dc_putc(i ? '/' : '=');
			t8122_dc_hex(w->wake[i]);
		}
		t8122_dc_puts(" d=");
		t8122_dc_hex(t8122.dc_dropped);
		t8122_dc_putc('\n');
		return;
	}
	t8122_dc_puts("\nW");
	t8122_dc_putc('0' + n);
	for (i = 0; i < T8122_W_COPROCS; i++)
		t8122_dc_pair(t8122_witness_coprocs[i].name, w->control[i], w->status[i]);
	t8122_dc_pair("gate", w->extra[0], w->extra[1]);
	t8122_dc_pair("clus", w->extra[2], w->extra[3]);
	t8122_dc_pair("cllt-notify", w->extra[4], w->extra[5]);
	t8122_dc_pair("hpm", w->extra[6], 0);
	t8122_dc_pair("aon-a8-b0", w->aon[0], w->aon[1]);
	t8122_dc_pair("aon-b8-c0", w->aon[2], w->aon[3]);
	t8122_dc_pair("aon-c8", w->aon[4], 0);
	for (i = 0; i < ARRAY_SIZE(t8122_ps_all); i++) {
		if (prev && prev->taken && prev->ps[i] == w->ps[i])
			continue;
		t8122_dc_puts("\nw");
		t8122_dc_putc('0' + n);
		t8122_dc_putc(' ');
		t8122_dc_hex(t8122_ps_all[i]);
		t8122_dc_putc(' ');
		t8122_dc_hex(w->ps[i]);
	}
	t8122_dc_puts("\nW");
	t8122_dc_putc('0' + n);
	t8122_dc_puts(" end\n");
}

/* Snapshot @n against macOS's commit: every word that differs, with both values. */
static void t8122_witness_macos_dc(unsigned int n)
{
	const struct t8122_witness *w = &t8122.snap[n];
	unsigned int i, differ = 0;

	if (!t8122.dc_uart || !w->taken)
		return;
	t8122_dc_puts("\nM");
	t8122_dc_putc('0' + n);
	t8122_dc_puts(" linux/macOS");
	for (i = 0; i < ARRAY_SIZE(t8122_ps_all); i++) {
		if (t8122_ps_macos[i] == T8122_M_NONE || t8122_ps_macos[i] == w->ps[i])
			continue;
		differ++;
		t8122_dc_puts("\nm");
		t8122_dc_putc('0' + n);
		t8122_dc_putc(' ');
		t8122_dc_hex(t8122_ps_all[i]);
		t8122_dc_putc(' ');
		t8122_dc_hex(w->ps[i]);
		t8122_dc_putc(' ');
		t8122_dc_hex(t8122_ps_macos[i]);
	}
	t8122_dc_puts("\nM");
	t8122_dc_putc('0' + n);
	t8122_dc_puts(" end differ=");
	t8122_dc_hex(differ);
	t8122_dc_putc('\n');
}

/* After a return, through printk: whatever was taken. */
static void t8122_witness_show(struct apple_sleep *s)
{
	unsigned int n, i;

	for (n = 0; n < T8122_W_SNAPS; n++) {
		const struct t8122_witness *w = &t8122.snap[n];

		if (!w->taken)
			continue;
		for (i = 0; i < T8122_W_COPROCS; i++)
			dev_info(s->dev, "J613 witness %u: %s ASC control %08x status %08x\n", n,
				 t8122_witness_coprocs[i].name, w->control[i], w->status[i]);
		dev_info(s->dev, "J613 witness %u: gates %08x %08x clusters %08x %08x CLLT %08x notify PMU %08x HPM %08x\n",
			 n, w->extra[0], w->extra[1], w->extra[2], w->extra[3], w->extra[4],
			 w->extra[5], w->extra[6]);
		for (i = 0; i < ARRAY_SIZE(t8122_ps_all); i++)
			dev_info(s->dev, "J613 witness %u: ps %05x %08x\n", n, t8122_ps_all[i],
				 w->ps[i]);
		for (i = 0; i < T8122_W_WAKE; i++)
			dev_info(s->dev, "J613 witness %u: wake %02x %08x\n", n, i * 4, w->wake[i]);
	}
	if (t8122.disp_poll_us != -1)
		dev_info(s->dev, "J613: DISP_CPU read off %d us after its lower\n",
			 t8122.disp_poll_us);
}

static void t8122_witness_reset(void)
{
	unsigned int n;

	for (n = 0; n < T8122_W_SNAPS; n++)
		t8122.snap[n].taken = false;
	t8122.disp_poll_us = -1;
}

/* This SoC's on-chip MMIO takes non-posted writes only: a posted one loses the machine. */
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
	};
	unsigned int i;

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
	if (t8122_mcc) {
		static const char *const names[] = { "mcc-a", "mcc-b", "mcc-c", "mcc-d", "mcc-e" };

		t8122.mcc_ok = true;
		for (i = 0; i < ARRAY_SIZE(names); i++) {
			t8122.mcc[i] = t8122_map(s, names[i], NULL);
			if (IS_ERR(t8122.mcc[i])) {
				dev_warn(s->dev, "no %s registers: no memory-controller stop\n", names[i]);
				t8122.mcc_ok = false;
			}
		}
		if (t8122.mcc_ok)
			dev_info(s->dev, "J613 trial: memory controller stepped down before the gates\n");
	}
	if (t8122_dump || t8122_macos_state) {
		t8122.dump_aon = t8122_map(s, "ps-aon", NULL);
		if (IS_ERR(t8122.dump_aon))
			t8122.dump_aon = NULL;
	}
	t8122.hpm_notify = t8122_map(s, "spmi-hpm", NULL);
	if (IS_ERR(t8122.hpm_notify))
		return dev_err_probe(s->dev, PTR_ERR(t8122.hpm_notify), "no spmi-hpm registers\n");
	/*
	 * The debug link's cut is the driver's own (apple_sleep_usb_down), gated as on the
	 * working SoCs: the trial's tree names the port controller, and the debug switch word
	 * says whether a link is live. A live link holds its domains on, so until the cut has
	 * run, a debug endpoint that still answers doesn't say the SoC failed to power down.
	 * Without "ps-aon" the gate stays shut and the link is left alone.
	 */
	if (t8122_keep_debug_link) {
		dev_info(s->dev, "J613 trial: a live debug link is kept across the commit\n");
	} else {
		s->ps_aon = t8122_map(s, "ps-aon", NULL);
		if (IS_ERR(s->ps_aon)) {
			dev_warn(s->dev, "no ps-aon registers: a live debug link is never cut\n");
			s->ps_aon = NULL;
		}
	}
	/*
	 * The wake block: pmgr reg[4] in the ADT, T8140's at this SoC's base. Its status offset
	 * is unmeasured here, so the witness reads the block whole.
	 */
	s->wake = t8122_map(s, "wake", NULL);
	if (IS_ERR(s->wake)) {
		dev_warn(s->dev, "no wake registers: wake sources are left as they are\n");
		s->wake = NULL;
	} else {
		int ret = apple_sleep_read_wake_sources(s);

		if (ret)
			return ret;
	}
	if (t8122_console) {
		t8122.dc_uart = t8122_map(s, "dockchannel", NULL);
		if (IS_ERR(t8122.dc_uart)) {
			dev_warn(s->dev, "no dockchannel registers: no late witness\n");
			t8122.dc_uart = NULL;
		} else {
			/* Kept across the suspend: the entry's own lines are the evidence. */
			console_suspend_enabled = false;
			register_console(&t8122_dc_console);
			dev_info(s->dev, "J613 trial: kernel console on the debug-cable DockChannel\n");
		}
	}
	s->driver_down = devm_bitmap_zalloc(s->dev, resource_size(&s->ps_res) / 4, GFP_KERNEL);
	if (!s->driver_down)
		return -ENOMEM;
	if (apple_sleep_alloc_books(s))
		return -ENOMEM;

	for (i = 0; i < T8122_W_COPROCS; i++) {
		struct device_node *np = of_find_node_by_path(t8122_witness_coprocs[i].path);
		int index = 0;

		if (!np)
			continue;
		if (t8122_witness_coprocs[i].reg)
			index = of_property_match_string(np, "reg-names",
							 t8122_witness_coprocs[i].reg);
		/* of_iomap() maps non-posted under this bus. */
		if (index >= 0)
			t8122.asc[i] = of_iomap(np, index);
		of_node_put(np);
	}
	t8122_witness_reset();

	return 0;
}

static void t8122_trial_wdt(struct apple_sleep *s, bool on)
{
	if (!on) {
		if (t8122.wdt_armed)
			writel(0, s->wdt + T8122_WDT_WD1_CTL);
		t8122.wdt_armed = false;
		return;
	}
	if (!t8122_wdt_secs || t8122_wdt_secs > 120)
		return;
	writel(WDT_TICKS_PER_S * t8122_wdt_secs, s->wdt + T8122_WDT_WD1_BITE);
	writel(0, s->wdt + WDT_WD1_COUNT);
	writel(T8122_WDT_RESET_EN, s->wdt + T8122_WDT_WD1_CTL);
	t8122.wdt_armed = true;
}

static bool t8122_asc_census;
module_param(t8122_asc_census, bool, 0600);
MODULE_PARM_DESC(t8122_asc_census,
		 "J613 trial: at the entry, the ASC control and status of the coprocessors no Linux driver stops (AOP, MTP, SIO, dcpext); macOS stops all but AOP over their mailboxes");

/* Read only: each ASC's CPU_CONTROL (+0x44) and CPU_STATUS (+0x48), where its domain is up. */
static void t8122_asc_census_log(struct apple_sleep *s)
{
	static const struct { const char *name; phys_addr_t asc; u32 ps; } c[] = {
		{ "aop", 0x2f4400000, 0 }, { "mtp", 0x2fa400000, 0 },
		{ "sio", 0x2a1c00000, T8122_PS_SIO_CPU }, { "dcpext", 0x31ac00000, T8122_PS_DISPEXT_CPU },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(c); i++) {
		u32 ps = c[i].ps ? readl(s->ps + c[i].ps) : 0;
		void __iomem *r;

		if (c[i].ps && !FIELD_GET(PS_ACTUAL, ps)) {
			dev_info(s->dev, "J613 ASC census: %s domain off (ps %#x)\n", c[i].name, ps);
			continue;
		}
		r = ioremap(c[i].asc, 0x4000);
		if (!r)
			continue;
		dev_info(s->dev, "J613 ASC census: %s control %#x status %#x (ps %#x)\n", c[i].name,
			 readl(r + 0x44), readl(r + 0x48), ps);
		iounmap(r);
	}
}

/*
 * J613 diagnostic: the commit word's page. n18 read the commit word back 0 straight after each
 * write (k0-k2); macOS's reads 1 after a real sleep. Logs 0x2e42d4000..+0xff at the entry.
 */
static bool t8122_commit_page;
static void __iomem *t8122_cp_page, *t8122_cp_mini;

/*
 * J613 trial (read only): each ASC's CPU_CONTROL (+0x44) and CPU_STATUS (+0x48) just before the
 * commit, where its power domain is up, to compare with macOS's at its commit (Aura's stop
 * snapshot, runs 48 and 49: PMP, ANS and SIO control 0 status 0x2e; DCP and GFX domains off; SMC
 * control 0x10 status 0x28). Mapped at the sleep's begin; one "A" line per ASC on the debug
 * console.
 */
static bool t8122_asc_commit;
module_param(t8122_asc_commit, bool, 0600);
MODULE_PARM_DESC(t8122_asc_commit, "J613 trial: every ASC's control and status just before the commit (reads only)");

static const struct { const char *name; phys_addr_t asc; u32 ps; } t8122_ascs[] = {
	{ "pmp", 0x2d0c00000, T8122_PS_PMP }, { "ans", 0x309400000, T8122_PS_ANS },
	{ "sio", 0x2a1c00000, T8122_PS_SIO_CPU }, { "dcp", 0x28ec00000, T8122_PS_DISP_CPU },
	{ "gfx", 0x292400000, T8122_PS_GFX }, { "smc", 0x2ec400000, 0 },
	{ "aop", 0x2f4400000, 0 }, { "mtp", 0x2fa400000, 0 },
	{ "dcpext", 0x31ac00000, T8122_PS_DISPEXT_CPU },
};
static void __iomem *t8122_asc_map[ARRAY_SIZE(t8122_ascs)];
module_param(t8122_commit_page, bool, 0600);
MODULE_PARM_DESC(t8122_commit_page, "J613 diagnostic: log the commit word's page (0x2e42d4000, 256 bytes) at the entry");

static void t8122_commit_page_log(struct apple_sleep *s)
{
	void __iomem *r = ioremap_np(0x2e42d4000, 0x100);
	char line[160];
	unsigned int i, n;

	if (!r)
		return;
	for (i = 0; i < 0x100; i += 0x20) {
		n = scnprintf(line, sizeof(line), "+%02x:", i);
		for (unsigned int j = 0; j < 0x20; j += 4)
			n += scnprintf(line + n, sizeof(line) - n, " %08x", readl(r + i + j));
		dev_info(s->dev, "J613 commit page %s\n", line);
	}
	iounmap(r);
}

static int t8122_sleep_begin(struct apple_sleep *s)
{
	if (t8122_commit_page) {
		t8122_commit_page_log(s);
		/* Process context here; the pre-commit dump runs with interrupts off. Kept mapped. */
		if (!t8122_cp_page)
			t8122_cp_page = ioremap_np(0x2e42d4000, 0x100);
		if (!t8122_cp_mini)
			t8122_cp_mini = ioremap_np(0x2e4280000, 0x108);
	}
	if (t8122_asc_census)
		t8122_asc_census_log(s);
	if (t8122_asc_commit) {
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(t8122_ascs); i++)
			if (!t8122_asc_map[i])
				t8122_asc_map[i] = ioremap_np(t8122_ascs[i].asc, 0x100);
	}
	t8122_witness_reset();
	t8122.wfi_returns = 0;
	t8122.dc_stalled = false;
	t8122.dc_nowait = !t8122_witness_full;
	t8122.dc_dropped = 0;
	/* What the cut's gate will find: the late witness goes dark with the link. */
	if (s->ps_aon)
		dev_info(s->dev, "J613: debug switch %#x, port controller %s: %s\n",
			 readl(s->ps_aon + s->soc->ps_aon_debug_switch),
			 s->hpm_spmi ? "named" : "not named",
			 sleep_debug_link_up(s) ? "live debug link, cut after the coprocessor stops" :
						  "no cut");
	if (apple_sleep_dry(s))
		dev_info(s->dev, "J613: dry run: the refusals and the first witness, then a return before any stop\n");
	else
		dev_warn(s->dev, "J613: this sleep commits: wake sources %s; no wake is qualified, the display and the GPU don't restart\n",
			 t8122_wake_mask && s->wake && s->wake_enable_count ?
			 "armed from the tree's apple,wake-enable" : "not armed, left as they are");

	return 0;
}

/*
 * First in .enter: from here a hang resets the machine, where the trial asked for that. Not in
 * a dry run, which returns and commits nothing: no reset is left armed over it.
 */
static int t8122_sleep_check(struct apple_sleep *s)
{
	if (!apple_sleep_dry(s))
		t8122_trial_wdt(s, true);

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

/*
 * The internal display, as the measured sleep leaves it 124 ms before the commit: DISP_CPU
 * 0x0f0000ff, the display gate triple 3/0/0, DISP_CPU 0x0f0003f0, DISP_FE and DISP_SYS
 * 0x23f0. Its driver has stopped the coprocessor, or the sleep was refused before this. The
 * engine of the working SoCs waits on DISP_CPU with no timeout, so one that stays on refuses.
 */
static int t8122_sleep_display_off(struct apple_sleep *s)
{
	u64 start, hz = arch_timer_get_cntfrq();

	if (!(t8122_disp_off & 1))
		return 0;
	if (FIELD_GET(PS_ACTUAL, readl(s->ps + T8122_PS_DISP_CPU))) {
		writel(0x0f0000ff, s->ps + T8122_PS_DISP_CPU);
		writel(3, s->pwrgate + T8122_PWRGATE_DISP_A);
		writel(0, s->pwrgate + T8122_PWRGATE_DISP_B);
		writel(0, s->pwrgate + T8122_PWRGATE_DISP_C);
		writel(0x0f0003f0, s->ps + T8122_PS_DISP_CPU);
	}
	t8122_ps_lower_if_on(s, T8122_PS_DISP_FE, 0, 0x23f0);
	t8122_ps_lower_if_on(s, T8122_PS_DISP_SYS, 0, 0x23f0);
	if (t8122_disp_off & 2) {
		t8122_ps_lower_if_on(s, T8122_PS_GFX, 0x0f0020ff, 0x0f0023f0);
		t8122_ps_lower_if_on(s, T8122_PS_DISPEXT_CPU, 0x0f0400ff, 0x0f0403f0);
		t8122_ps_lower_if_on(s, T8122_PS_DISPEXT_FE, 0, 0x23f0);
		t8122_ps_lower_if_on(s, T8122_PS_DISPEXT_SYS, 0, 0x23f0);
		t8122_ps_lower_if_on(s, T8122_PS_I2C3, 0, 0x3f0);
		t8122_ps_lower_if_on(s, T8122_PS_SPI2, 0, 0x3f0);
	}
	start = read_sysreg(cntvct_el0);
	t8122.disp_poll_us = -1;
	while (read_sysreg(cntvct_el0) - start < hz / 10) {
		if (!FIELD_GET(PS_ACTUAL, readl(s->ps + T8122_PS_DISP_CPU))) {
			t8122.disp_poll_us = div64_u64((read_sysreg(cntvct_el0) - start) * 1000000,
						       hz);
			break;
		}
		cpu_relax();
	}
	t8122_dc_putc(t8122.disp_poll_us >= 0 ? 'D' : 'd');
	if (t8122.disp_poll_us < 0) {
		dev_err(s->dev, "refused: DISP_CPU stayed on: %#x\n",
			readl(s->ps + T8122_PS_DISP_CPU));
		return -EBUSY;
	}

	return 0;
}

/* Final J613-specific witness; the shared untested gate controls commitment. */
static int t8122_sleep_final_check(struct apple_sleep *s)
{
	t8122_witness_take(s, 0);
	t8122_witness_dc(0);
	if (t8122_witness_full)
		t8122_witness_macos_dc(0);
	/*
	 * The boot core's own state where a sleep would find it, to set against what the
	 * hypervisor reads at macOS's last WFI. Reads only, of registers this kernel already uses.
	 */
	if (t8122_dry_regs && apple_sleep_dry(s))
		dev_info(s->dev, "J613 dry run: boot cpu deep-sleep %#llx cntp/cntv_el0 %#llx/%#llx cntp/cntv_el02 %#llx/%#llx VM_TMR_FIQ_ENA %#llx PMCR0 %#llx IPI_SR %#llx ISR %#llx\n",
			 read_sysreg_s(SYS_APL_DEEP_SLEEP), read_sysreg(cntp_ctl_el0),
			 read_sysreg(cntv_ctl_el0),
			 is_kernel_in_hyp_mode() ? read_sysreg_s(SYS_CNTP_CTL_EL02) : 0,
			 is_kernel_in_hyp_mode() ? read_sysreg_s(SYS_CNTV_CTL_EL02) : 0,
			 is_kernel_in_hyp_mode() ? read_sysreg_s(T8142_VM_TMR_FIQ_ENA) : 0,
			 read_sysreg_s(T8142_PMCR0), read_sysreg_s(SYS_APL_IPI_SR_EL1),
			 read_sysreg(isr_el1));

	return 0;
}

/* One byte to the PMU: the reply vector masked, then acked. */
static void t8122_pmu_write(struct apple_sleep *s, u16 reg, u8 val)
{
	unsigned int tries = 100000;
	u32 enable;

	enable = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	writel(enable & ~SPMI_NOTIFY_REPLY, s->spmi + SPMI_NOTIFY_ENABLE);
	writel(((u32)reg << 16) | SPMI_CMD_WRITE | FIELD_PREP(SPMI_CMD_USID, s->soc->pmu_usid),
	       s->spmi + SPMI_CMD);
	writel(val, s->spmi + SPMI_CMD);
	while ((readl(s->spmi + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY) && --tries)
		cpu_relax();
	if (tries)
		readl(s->spmi + SPMI_REPLY);
	writel(1, s->spmi + SPMI_NOTIFY_ACK);
	writel(enable, s->spmi + SPMI_NOTIFY_ENABLE);
	t8122_dc_putc(tries ? 'P' : 'p');
}

/*
 * The main block's words at macOS's commit: the ones it has on raised, the ones it has off
 * lowered. The common match does the same among steps this entry takes in its own order.
 */
static void t8122_match_ps(struct apple_sleep *s, int *raised, int *failed, int *lowered)
{
	const struct apple_sleep_soc *soc = s->soc;
	int i;

	for (i = 0; i < soc->ps_macos_on.count; i++) {
		u32 offset = soc->ps_macos_on.offset[i];

		s->ps_on_saved[i] = readl(s->ps + offset);
		/* State 4 reads as on, and macOS has these at f. */
		if (FIELD_GET(PS_ACTUAL, s->ps_on_saved[i]) == 0xf)
			continue;
		if (sleep_ps_raise_start(s, offset))
			(*failed)++;
		(*raised)++;
	}
	for (i = 0; i < soc->ps_macos_off.count; i++) {
		u32 offset = soc->ps_macos_off.offset[i];

		s->ps_off_saved[i] = readl(s->ps + offset);
		if (!sleep_ps_on(s, offset))
			continue;
		sleep_ps_lower(s, offset);
		(*lowered)++;
	}
}

static void t8122_match_ps_wait(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	int i;

	for (i = 0; i < soc->ps_macos_off.count; i++)
		sleep_ps_wait_off(s, soc->ps_macos_off.offset[i]);
}

static void t8122_match_ps_undo(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	int i;

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

/*
 * macOS's commit state where Linux's differs once its drivers have suspended: the SoC's two
 * lists, then the words with a stop of their own. ANS's driver has shut its firmware down
 * and released the domain; macOS commits with it up.
 */
static void t8122_sleep_match(struct apple_sleep *s)
{
	int raised = 0, failed = 0, lowered = 0;

	if (!t8122_match)
		return;
	t8122.match_saved[0] = readl(s->ps + T8122_PS_ANS);
	t8122.match_saved[1] = readl(s->ps + T8122_PS_APCIE_ST);
	t8122.match_saved[2] = readl(s->ps + T8122_PS_SIO_CPU);
	t8122.match_saved[3] = readl(s->ps + T8122_PS_APCIE_PHY_SW);
	t8122.matched = true;
	t8122.match_done = t8122_match;
	if (t8122_match & 1) {
		t8122_match_ps(s, &raised, &failed, &lowered);
		t8122_match_ps_wait(s);
	}
	if (t8122_match & 2) {
		if (sleep_ps_raise(s, T8122_PS_ANS))
			failed++;
		if (sleep_ps_raise(s, T8122_PS_APCIE_ST))
			failed++;
	}
	if (t8122_match & 4) {
		writel(T8122_PS_AUTO_IDLE, s->ps + T8122_PS_SIO_CPU);
		writel(T8122_PS_AUTO_IDLE, s->ps + T8122_PS_APCIE_PHY_SW);
	}
	t8122_dc_putc(failed ? 'x' : 'X');
}

static void t8122_sleep_match_undo(struct apple_sleep *s)
{
	if (!t8122.matched)
		return;
	t8122.matched = false;
	if (t8122.match_done & 4) {
		writel(t8122.match_saved[3] & ~PS_FLAGS, s->ps + T8122_PS_APCIE_PHY_SW);
		writel(t8122.match_saved[2] & ~PS_FLAGS, s->ps + T8122_PS_SIO_CPU);
	}
	if (t8122.match_done & 2) {
		if (!FIELD_GET(PS_ACTUAL, t8122.match_saved[1]))
			sleep_ps_lower(s, T8122_PS_APCIE_ST);
		if (!FIELD_GET(PS_ACTUAL, t8122.match_saved[0]))
			sleep_ps_lower(s, T8122_PS_ANS);
	}
	if (t8122.match_done & 1)
		t8122_match_ps_undo(s);
}

/*
 * Every coprocessor has stopped and the link is still up: the state the commit starts from,
 * against macOS's, with time for the cable to carry it before the cut.
 */
static void t8122_sleep_stopped(struct apple_sleep *s)
{
	t8122_witness_take(s, 2);
	t8122_witness_dc(2);
}

/*
 * Past the last refusal, with the SEP and the GPU stopped: the count clears, the PMU's sleep
 * byte and both notify words, in the measured order. The PMP's stop follows, as measured.
 */
static void t8122_sleep_prepare_regs(struct apple_sleep *s)
{
	/* The driver's own arm, where the working SoCs run it: after the first stops. */
	if (t8122_wake_mask && s->wake && s->wake_enable_count) {
		sleep_wake_arm(s);
		s->wake_masks_saved = true;
		t8122_dc_putc('K');
	}
	t8122_sleep_match(s);
	t8122_witness_take(s, 1);
	t8122_witness_dc(1);
	if (t8122_wdt_clear == 2) {
		writel(0, s->wdt + WDT_WD0_COUNT);
		writel(0, s->wdt + WDT_WD1_COUNT);
	}
	t8122_pmu_write(s, s->soc->pmu_sleep_reg, s->soc->pmu_sleep_val);
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

/*
 * J613 diagnostic: the boot core arms its own physical timer this many seconds ahead before its
 * last WFI. A core that lost its power cannot take it: no 'w' means the SoC went down. If the
 * WFI returns, the boot core writes 'w', the engine's words once (QT), and masks the timer.
 */
static unsigned int t8122_wfi_timer_s;
static unsigned int t8122_wfi_timer_ctrl;
module_param(t8122_wfi_timer_ctrl, uint, 0600);
MODULE_PARM_DESC(t8122_wfi_timer_ctrl, "J613 DIAGNOSTIC: with the hold, one timer-ended WFI first: 1 with the deep-sleep bits, 2 without");
module_param(t8122_wfi_timer_s, uint, 0600);
MODULE_PARM_DESC(t8122_wfi_timer_s,
		 "J613 DIAGNOSTIC: the boot core's own timer, this many s after its last WFI starts, shows whether it kept its power (0 = off)");
static bool t8122_wfi_timer_armed;

static void t8122_deep_wfi_arm(void)
{
	if (t8122_quiet)
		sleep_core_fiq_quiet();
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
	/* The boot core's, last: 'Z' going in. */
	if (apple_sleep && (read_cpuid_mpidr() & MPIDR_HWID_BITMASK) == apple_sleep->mbox->boot_mpidr) {
		unsigned int secs = READ_ONCE(t8122_wfi_timer_s);

		if (secs) {
			write_sysreg(read_sysreg(cntpct_el0) + (u64)secs * arch_timer_get_cntfrq(),
				     cntp_cval_el0);
			write_sysreg(ARCH_TIMER_CTRL_ENABLE, cntp_ctl_el0);
			isb();
			t8122_wfi_timer_armed = true;
		}
		t8122_dc_putc('Z');
	}
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

/*
 * The ports' words down, child first, as T8132's entry lowers ATC0's. The link's partner was
 * reset before this, and nothing returns from here without a wake: the ports' drivers bring
 * them back as after any power loss.
 */
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
		if (!(t8122_atc_off & BIT(i)))
			continue;
		t8122_aon_lower(s, port[i].usb);
		t8122_aon_lower(s, port[i].usb_aon);
		sleep_ps_lower(s, port[i].common);
		sleep_ps_wait_off(s, port[i].common);
	}
	if (t8122_atc_off & 4) {
		sleep_ps_lower(s, T8122_PS_IOA1);
		sleep_ps_wait_off(s, T8122_PS_IOA1);
	}
}

/* The other cores are stopped. The measured final 165 us, in its order. */
static void t8122_sleep_commit_regs(struct apple_sleep *s)
{
	t8122_sleep_atc_off(s);
	/* After the cut: what it changed, left in the DockChannel for whoever asks next. */
	t8122_witness_take(s, 3);
	t8122_witness_dc(3);
	if (t8122_wdt_clear)
		writel(0, s->wdt + WDT_WD0_CTL);
	/* The discriminator: a SoC that stays powered resets itself; one that slept may not. */
	t8122_trial_wdt(s, true);
	t8122_mcc_stop(s);
	if (t8122_macos_state)
		t8122_macos_state_apply(s);
	t8122_link_release_apply();

	writel(PWRGATE_ON, s->pwrgate + T8122_PWRGATE_E);
	writel(PWRGATE_ON, s->pwrgate + T8122_PWRGATE_P);
	writel(0, s->cluster + T8122_CLUSTER_E);
	/* macOS arms its boot core's deep WFI here. The driver arms it again at the WFI. */
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntv_ctl_el0);
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
	writel(0, s->cluster + T8122_CLUSTER_P);
	writel(0x2500, s->cllt + T8122_CLLT_CTL);
	writel(0, s->cllt + T8122_CLLT_CTL);
	writel(0x1c17, s->cllt + T8122_CLLT_LAST);
}

/*
 * The same words, in the same order, as macOS's commit snapshot (J613STOP, capture runs 43-49;
 * docs/j613-stock-sptm-sleep-20261117.md, B3-B5), so the two lists diff line for line. One
 * "K <base>" line per block, then one value per word. A coprocessor's run registers only with
 * its CPU domain up (they abort otherwise), as the witness reads them.
 */
static void t8122_dump_block(const char *tag, u64 base, void __iomem *reg, u32 first, u32 last,
			     u32 stride)
{
	u32 off;
	int n = 0;

	t8122_dc_puts("\nK ");
	t8122_dc_puts(tag);
	t8122_dc_putc(' ');
	t8122_dc_hex(upper_32_bits(base));
	t8122_dc_hex(lower_32_bits(base) + first);
	for (off = first; off <= last; off += stride) {
		t8122_dc_putc(n++ % 8 ? ' ' : '\n');
		t8122_dc_hex(reg ? readl(reg + off) : T8122_W_UNREAD);
	}
}

static void t8122_dump_macos(struct apple_sleep *s)
{
	static const u32 pwrgate[] = { 0x80, 0x88, 0xcac, 0xcd0, 0xcf0 };
	unsigned int i;

	t8122_dump_block("ps", 0x2d0700000, s->ps, 0x100, 0x4c0, 8);
	t8122_dump_block("ps4", 0x2d0700000, s->ps, 0x4000, 0x4060, 8);
	t8122_dump_block("ps8", 0x2d0700000, s->ps, 0x8000, 0x8020, 8);
	t8122_dump_block("ps10", 0x2d0700000, s->ps, 0x10000, 0x10000, 8);
	t8122_dump_block("sep", 0x2d0700000, s->ps, 0xc00, 0xc00, 8);
	t8122_dump_block("aon", 0x2e4280000, t8122.dump_aon, 0x0, 0x100, 8);
	t8122_dump_block("wdt", 0x2e42b0000, s->wdt, 0x0, 0x1c, 4);
	t8122_dump_block("wake", 0x2e4004000, s->wake, 0x0, 0x44, 4);
	t8122_dc_puts("\nK gate 2d0724000\n");
	for (i = 0; i < ARRAY_SIZE(pwrgate); i++) {
		t8122_dc_hex(readl(s->pwrgate + pwrgate[i]));
		t8122_dc_putc(' ');
	}
	t8122_dc_puts("\nK misc cluster/cllt/cpustart/notify\n");
	t8122_dc_hex(readl(s->cluster + T8122_CLUSTER_E));
	t8122_dc_putc(' ');
	t8122_dc_hex(readl(s->cluster + T8122_CLUSTER_P));
	t8122_dc_putc(' ');
	t8122_dc_hex(readl(s->cllt + T8122_CLLT_CTL));
	t8122_dc_putc(' ');
	t8122_dc_hex(readl(s->cllt + T8122_CLLT_LAST));
	t8122_dc_putc(' ');
	t8122_dc_hex(readl(s->cpu_start));
	t8122_dc_putc(' ');
	t8122_dc_hex(readl(s->spmi + SPMI_NOTIFY_ENABLE));
	t8122_dc_putc(' ');
	t8122_dc_hex(readl(t8122.hpm_notify + SPMI_NOTIFY_ENABLE));
	t8122_dc_puts("\nK asc control/status (UNREAD = CPU domain down)\n");
	for (i = 0; i < T8122_W_COPROCS; i++) {
		bool up = FIELD_GET(PS_ACTUAL, readl(s->ps + t8122_witness_coprocs[i].ps));

		t8122_dc_puts(t8122_witness_coprocs[i].name);
		t8122_dc_putc('=');
		if (t8122.asc[i] && up && i != T8122_W_SEP) {
			t8122_dc_hex(readl(t8122.asc[i] + ASC_CPU_CONTROL));
			t8122_dc_putc('/');
			t8122_dc_hex(readl(t8122.asc[i] + ASC_CPU_STATUS));
		} else {
			t8122_dc_puts(up ? "up-unread" : "down");
		}
		t8122_dc_putc(' ');
	}
	t8122_dc_puts("\nK end\n");
}

/* The mailbox is armed: 'c' before the commit word, 'C' once every line is masked after it. */
/* The same words as capture crumb bit 26's J613CPG, just before the first commit write. */
static void t8122_commit_page_dc(struct apple_sleep *s)
{
	void __iomem *page = t8122_cp_page, *mini = t8122_cp_mini;
	unsigned int i;

	t8122_dc_puts("\nP page");
	for (i = 0; page && i < 0x100; i += 4) {
		t8122_dc_putc(' ');
		t8122_dc_hex(readl(page + i));
	}
	t8122_dc_puts("\nP mini");
	for (i = 0; mini && i <= 0x100; i += 8) {
		t8122_dc_putc(' ');
		t8122_dc_hex(readl(mini + i));
	}
	t8122_dc_puts("\nP sio ");
	t8122_dc_hex(readl(s->ps + T8122_PS_SIO_CPU));
	t8122_dc_puts("\nP end\n");
}

/*
 * J613 trial (single variable): SIO_CPU at macOS's commit state, auto-idle (0x1400024f in capture
 * 63, just before the first commit write); Linux reads 0x1f0002ff there (n21: t8122_match bit 2's
 * write does not stick). Written just before the commit and polled 2 ms; 'S' before/after.
 */
/*
 * J613 trial (single variable): DISP_CPU (0x2d0710000) reads 0x0f040300 just before Linux's
 * commit; macOS's commit snapshot has 0x0f000300 (run 31). The only difference is bit 18.
 * Written back without it just before the commit (the W1C flags left alone); 'D' before/after.
 */
/*
 * J613 trial (single variable): APCIE_PHY_SW is one of two domains whose state, not just flags,
 * differs at the commit (n29's commit-dump diff): macOS 0x1400024f (auto, clock-gated) after it
 * steps the PCIe PHY down; Linux 0x1f0000ff (fully on; its PCIe port never probes in the trial,
 * and an auto-idle write does not stick). Lowered (off) just before the commit; 'P' before/after.
 */
static bool t8122_phy_off;
module_param(t8122_phy_off, bool, 0600);
MODULE_PARM_DESC(t8122_phy_off, "J613 trial: lower APCIE_PHY_SW just before the commit (macOS has it clock-gated there)");

static void t8122_phy_off_apply(struct apple_sleep *s)
{
	u32 before = readl(s->ps + T8122_PS_APCIE_PHY_SW);

	sleep_ps_lower(s, T8122_PS_APCIE_PHY_SW);
	t8122_dc_puts("\nP phy ");
	t8122_dc_hex(before);
	t8122_dc_putc('/');
	t8122_dc_hex(readl(s->ps + T8122_PS_APCIE_PHY_SW));
	t8122_dc_putc('\n');
}

/*
 * J613 trial (single variable): SIO_CPU, the other domain whose state differs at the commit
 * (n29's diff: macOS 0x1400024f, Linux 0x1f0002ff; an auto-idle write does not stick, n23).
 * Lowered (off) just before the commit; 'O' before/after.
 */
static bool t8122_sio_off;
module_param(t8122_sio_off, bool, 0600);
MODULE_PARM_DESC(t8122_sio_off, "J613 trial: lower SIO_CPU just before the commit (macOS has it clock-gated there)");

static void t8122_sio_off_apply(struct apple_sleep *s)
{
	u32 before = readl(s->ps + T8122_PS_SIO_CPU);

	sleep_ps_lower(s, T8122_PS_SIO_CPU);
	t8122_dc_puts("\nO sio ");
	t8122_dc_hex(before);
	t8122_dc_putc('/');
	t8122_dc_hex(readl(s->ps + T8122_PS_SIO_CPU));
	t8122_dc_putc('\n');
}

static bool t8122_disp_bit18;
module_param(t8122_disp_bit18, bool, 0600);
MODULE_PARM_DESC(t8122_disp_bit18, "J613 trial: clear DISP_CPU's bit 18 just before the commit, as macOS's 0x0f000300");

static void t8122_disp_bit18_apply(struct apple_sleep *s)
{
	u32 before = readl(s->ps + T8122_PS_DISP_CPU), after;

	if (before & BIT(18))
		writel(before & ~(BIT(18) | PS_FLAGS), s->ps + T8122_PS_DISP_CPU);
	after = readl(s->ps + T8122_PS_DISP_CPU);
	t8122_dc_puts("\nD disp ");
	t8122_dc_hex(before);
	t8122_dc_putc('/');
	t8122_dc_hex(after);
	t8122_dc_putc('\n');
}

static bool t8122_sio_auto;
module_param(t8122_sio_auto, bool, 0600);
MODULE_PARM_DESC(t8122_sio_auto, "J613 trial: SIO_CPU auto-idle just before the commit, as macOS (0x1400024f)");

static void t8122_sio_auto_apply(struct apple_sleep *s)
{
	u32 before = readl(s->ps + T8122_PS_SIO_CPU), after;
	u64 t0 = read_sysreg(cntpct_el0), hz = arch_timer_get_cntfrq();

	writel(T8122_PS_AUTO_IDLE, s->ps + T8122_PS_SIO_CPU);
	do {
		after = readl(s->ps + T8122_PS_SIO_CPU);
	} while (FIELD_GET(PS_ACTUAL, after) == 0xf && read_sysreg(cntpct_el0) - t0 < hz / 500);
	t8122_dc_puts("\nS sio ");
	t8122_dc_hex(before);
	t8122_dc_putc('/');
	t8122_dc_hex(after);
	t8122_dc_putc('\n');
}

static void t8122_asc_commit_dc(struct apple_sleep *s)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(t8122_ascs); i++) {
		u32 ps = t8122_ascs[i].ps ? readl(s->ps + t8122_ascs[i].ps) : 0;

		t8122_dc_puts("\nA ");
		t8122_dc_puts(t8122_ascs[i].name);
		t8122_dc_putc(' ');
		t8122_dc_hex(ps);
		if ((t8122_ascs[i].ps && !FIELD_GET(PS_ACTUAL, ps)) || !t8122_asc_map[i]) {
			t8122_dc_puts(" off");
			continue;
		}
		t8122_dc_putc(' ');
		t8122_dc_hex(readl(t8122_asc_map[i] + 0x44));
		t8122_dc_putc(' ');
		t8122_dc_hex(readl(t8122_asc_map[i] + 0x48));
	}
	t8122_dc_putc('\n');
}

static void t8122_sleep_pre_commit(struct apple_sleep *s)
{
	if (READ_ONCE(t8122_sio_off))
		t8122_sio_off_apply(s);
	if (READ_ONCE(t8122_phy_off))
		t8122_phy_off_apply(s);
	if (READ_ONCE(t8122_disp_bit18))
		t8122_disp_bit18_apply(s);
	if (READ_ONCE(t8122_sio_auto))
		t8122_sio_auto_apply(s);
	if (READ_ONCE(t8122_commit_page))
		t8122_commit_page_dc(s);
	if (READ_ONCE(t8122_dump))
		t8122_dump_macos(s);
	if (READ_ONCE(t8122_asc_commit))
		t8122_asc_commit_dc(s);
	t8122_dc_putc('c');
	/*
	 * Control for t8122_wfi_timer_s (with the hold only, so no commit): the boot core arms the
	 * same timer 2 s ahead, sets the deep-sleep bits (1) or not (2) and takes one WFI. 'W' and
	 * the elapsed ms if it returns: the timer can end such a WFI on a core with its power.
	 */
	if (READ_ONCE(sleep_untested_hold) && READ_ONCE(t8122_wfi_timer_ctrl)) {
		u64 t0 = read_sysreg(cntpct_el0), hz = arch_timer_get_cntfrq();
		u64 deep = read_sysreg_s(SYS_APL_DEEP_SLEEP);

		if (t8122_wfi_timer_ctrl == 1)
			write_sysreg_s(deep | DEEP_SLEEP_ENABLE, SYS_APL_DEEP_SLEEP);
		write_sysreg(t0 + 2 * hz, cntp_cval_el0);
		write_sysreg(ARCH_TIMER_CTRL_ENABLE, cntp_ctl_el0);
		isb();
		dsb(sy);
		wfi();
		write_sysreg(ARCH_TIMER_CTRL_ENABLE | ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
		write_sysreg_s(deep, SYS_APL_DEEP_SLEEP);
		isb();
		t8122_dc_puts(" W");
		t8122_dc_hex((u32)div64_u64((read_sysreg(cntpct_el0) - t0) * 1000, hz));
	}
	/* The hold: the commit word is not written, and the next thing is a reset from outside. */
	if (READ_ONCE(sleep_untested_hold))
		t8122_dc_puts(" HOLD before commit\n");
}

/*
 * macOS's commit tail (capture run 51, both cycles): after the first commit write it writes the
 * PMU RTC alarm itself (f80e mask <- 0, f800 control <- 0x41, around its compare write), then
 * CLLT 0, 0, 0x1c17 and the commit word, twice. The alarm's compare stays as the RTC driver set it.
 */
static void t8122_macos_commit_tail(struct apple_sleep *s)
{
	int n;

	t8122_pmu_write(s, 0xf80e, 0);
	t8122_pmu_write(s, 0xf800, 0x41);
	for (n = 0; n < 2; n++) {
		writel(0, s->cllt + T8122_CLLT_CTL);
		writel(0, s->cllt + T8122_CLLT_CTL);
		writel(0x1c17, s->cllt + T8122_CLLT_LAST);
		writel(1, s->commit);
		if (READ_ONCE(t8122_post_commit_watch)) {
			t8122_dc_puts(" k");
			t8122_dc_putc('1' + n);
			t8122_dc_putc('=');
			t8122_dc_hex(readl(s->commit));
		}
	}
}

static void t8122_sleep_post_timer(struct apple_sleep *s)
{
	/* The first commit write's readback, before anything else touches the engine */
	if (READ_ONCE(t8122_post_commit_watch)) {
		t8122_dc_puts("\nk0=");
		t8122_dc_hex(readl(s->commit));
	}
	if (READ_ONCE(t8122_macos_commit))
		t8122_macos_commit_tail(s);
	t8122_dc_putc('C');
}

/* 'w' each time the boot core's WFI returns: a core still awake, not a SoC that slept. */
static void t8122_q_line(struct apple_sleep *s, char tag);

static u32 t8122_aic_ack(void)
{
	if (t8122.wfi_returns++ < 32)
		t8122_dc_putc('w');
	if (t8122_wfi_timer_armed && apple_sleep) {
		t8122_wfi_timer_armed = false;
		write_sysreg(ARCH_TIMER_CTRL_ENABLE | ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
		isb();
		t8122_q_line(apple_sleep, 'T');
		t8122_dc_puts("\nQT end\n");
	}

	return aic_sleep_ack();
}

/* Unqualified: no wake has been seen. The engine normally raises the clusters itself. */
static void t8122_sleep_wake(struct apple_sleep *s)
{
	unsigned int i;

	writel(CLUSTER_ON, s->cluster + T8122_CLUSTER_E);
	writel(CLUSTER_ON, s->cluster + T8122_CLUSTER_P);
	/* own_wake keeps the common read away: the report still names what it finds here. */
	if (s->wake)
		for (i = 0; i < s->soc->wake_words; i++)
			s->wake_status[i] = readl(s->wake + s->soc->wake_status + i * 4);
}

/* .wake and .end, after a refusal or a return. */
static void t8122_sleep_finish(struct apple_sleep *s)
{
	bool taken = t8122.snap[0].taken || t8122.snap[1].taken || t8122.snap[2].taken ||
		     t8122.snap[3].taken;

	t8122_trial_wdt(s, false);
	if (taken)
		t8122_witness_show(s);
	t8122_witness_reset();
}

static int t8122_sleep_cpu_prepare(u64 mpidr)
{
	return 0;
}

static void t8122_sleep_cpu_restore(u64 mpidr)
{
}

/*
 * What must be bound for a sleep and stopped by its last step. The DCP's driver stops its
 * CPU, the GPU's leaves an idle GPU with GFX off, the PMP's and the SEP's stops are the
 * commit's. ANS goes off with its driver's shutdown.
 */
static const struct apple_sleep_gate_coproc t8122_gate_coprocs[] = {
	{ "/soc/dcp@28ec00000", true, T8122_PS_DISP_CPU },
	{ "/soc/pmp@2d0500000", true },
	{ "/soc/gpu@292400000", true, T8122_PS_GFX },
	{ "/soc/sep@29e400000" },
	{ "/soc/nvme@30dcc0000" },
	{ "/soc/smc@2ec400000" },
};

/* /arm-io/pmgr ap-wake-sources in J613's ADT. */
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

module_param(t8122_post_commit_watch, bool, 0600);
MODULE_PARM_DESC(t8122_post_commit_watch,
		 "J613 DIAGNOSTIC: after the commit and the other cores' release, this core spins 3 s and writes the sleep engine's words to the DockChannel at 5, 50, 500 and 3000 ms, then takes its WFI");

/*
 * After the commit: what the engine has done with the other cores while this one stays up.
 * Q<n>: commit word, E/P cluster words, E/P power gates, CPU-start's four words, CLLT control
 * and last, and the wake block's 0x24..0x44 status. macOS, read at its wake (run 56): gates
 * 0x80000000, clusters 0 / 0xf, CPU-start 0xef, CLLT last 0x31c17, status +0x34 = 0x08000000.
 */
static void t8122_q_line(struct apple_sleep *s, char tag)
{
	unsigned int i;

	t8122_dc_puts("\nQ");
	t8122_dc_putc(tag);
	t8122_dc_puts(" c=");
	t8122_dc_hex(readl(s->commit));
	t8122_dc_puts(" cl=");
	t8122_dc_hex(readl(s->cluster + T8122_CLUSTER_E));
	t8122_dc_putc('/');
	t8122_dc_hex(readl(s->cluster + T8122_CLUSTER_P));
	t8122_dc_puts(" g=");
	t8122_dc_hex(readl(s->pwrgate + T8122_PWRGATE_E));
	t8122_dc_putc('/');
	t8122_dc_hex(readl(s->pwrgate + T8122_PWRGATE_P));
	t8122_dc_puts(" s=");
	for (i = 0; i < 4; i++) {
		t8122_dc_hex(readl(s->cpu_start + 4 * i));
		t8122_dc_putc(i < 3 ? '/' : ' ');
	}
	t8122_dc_puts("l=");
	t8122_dc_hex(readl(s->cllt + T8122_CLLT_CTL));
	t8122_dc_putc('/');
	t8122_dc_hex(readl(s->cllt + T8122_CLLT_LAST));
	if (s->wake) {
		t8122_dc_puts(" w=");
		for (i = 0x24; i <= 0x44; i += 4) {
			t8122_dc_hex(readl(s->wake + i));
			t8122_dc_putc(i < 0x44 ? '/' : ' ');
		}
	}
}

static void t8122_sleep_post_release(struct apple_sleep *s)
{
	static const unsigned int at_ms[] = { 5, 50, 500, 3000 };
	unsigned int n, done = 0;

	if (!READ_ONCE(t8122_post_commit_watch))
		return;
	for (n = 0; n < ARRAY_SIZE(at_ms); n++) {
		mdelay(at_ms[n] - done);
		done = at_ms[n];
		t8122_q_line(s, '0' + n);
	}
	t8122_dc_puts("\nQ end\n");
}

static const struct apple_sleep_ops t8122_sleep_ops = {
	.post_release = t8122_sleep_post_release,
	.deep_wfi_arm = t8122_deep_wfi_arm,
	.resume = cpu_resume,
	.aic_mask_all = aic_sleep_mask_all,
	.aic_ack = t8122_aic_ack,
	.aic_restore = aic_sleep_restore,
	.map_regs = t8122_sleep_map_regs,
	.begin = t8122_sleep_begin,
	.check = t8122_sleep_check,
	.display_off = t8122_sleep_display_off,
	.final_check = t8122_sleep_final_check,
	.prepare_regs = t8122_sleep_prepare_regs,
	.stopped = t8122_sleep_stopped,
	.undo_regs = t8122_sleep_undo_regs,
	.commit_regs = t8122_sleep_commit_regs,
	.pre_commit = t8122_sleep_pre_commit,
	.post_timer = t8122_sleep_post_timer,
	.wake = t8122_sleep_wake,
	.finish = t8122_sleep_finish,
	.cpu_prepare = t8122_sleep_cpu_prepare,
	.cpu_restore = t8122_sleep_cpu_restore,
};

static const struct apple_sleep_soc t8122_sleep_soc = {
	.ops = &t8122_sleep_ops,
	.untested = true,
	/* J815's scratch offset isn't this PMU's: no step notes. */
	.no_pmu_notes = true,
	/* The DCP's driver stops it cold for a sleep that commits; a dry run leaves it running. */
	.display_no_restart = true,
	.early_context = true,
	/* The common arm stays off: the trial's switch runs it (t8122_wake_mask). */
	.own_wake = true,
	.wake_optional = true,
	/* T8140's layout at this base; ids to 208, so seven words. The status offset is unmeasured. */
	.wake_mask = 0x08,
	.wake_status = 0x28,
	.wake_words = 7,
	.wake_last_word = GENMASK(16, 0),
	.wake_names = t8122_wake_names,
	.nwake_names = ARRAY_SIZE(t8122_wake_names),
	/* WD2 is unmeasured on this SoC, and macOS's sleep writes none of it. */
	.no_wd2 = true,
	/* The SEP misses its SLEEP when the cut's seconds come first (candidates 19 to 21). */
	.debug_cut_after_stops = true,
	/* J613 alone: its boot core is the first P core, as under macOS, and it has eight. */
	.machine = "apple,j613",
	.boot_mpidr = 0x10100,
	.cpu_count = 8,
	/* NUB.SPMI0.SW3: the PMU, and so its RTC alarm, as on T8140 and T8132. */
	.wake_required = t8122_wake_required,
	.nwake_required = ARRAY_SIZE(t8122_wake_required),
	/* As macOS's boot core: the others reach their WFI in the 2 ms before this one's. */
	.boot_wfi_delay_ms = 2,
	.ndisp = 1,
	/* E 0.0.n, P 1.1.n: four each, a CPU-start nibble per cluster */
	.cpu_clusters = 2,
	.cpu_cores_per_cluster = 4,
	.cpu_start_stride = 4,
	/* The PMU's boot-stage register: the AP sleeps. Measured on J613. */
	.pmu_usid = 0xe,
	.pmu_sleep_reg = 0xf701,
	.pmu_sleep_val = 0x50,
	.spmi_notify_sleep = 0x2000001,
	/* debug_switch in the mini PMGR, as T8132's. Unmeasured here: the entry logs it. */
	.ps_aon_debug_switch = 0xa8,
	.ps = {
		.disp = { { T8122_PS_DISP_CPU, T8122_PS_DISP_FE, T8122_PS_DISP_SYS } },
		.sep = T8122_PS_SEP,
		.ans = T8122_PS_ANS,
		/* ANS has no PCIe block of its own here: the same word, for the shared check. */
		.apcie_sys_gp = T8122_PS_ANS,
	},
	.ps_macos_on = SLEEP_WORDS(t8122_ps_macos_on),
	.ps_macos_off = SLEEP_WORDS(t8122_ps_macos_off),
	.gate_coprocs = t8122_gate_coprocs,
	.ngate_coprocs = ARRAY_SIZE(t8122_gate_coprocs),
};
