/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T6000 (M1 Pro): suspend-to-RAM through the PMGR's S2R timer, in the order macOS has on this
 * SoC. Included by sleep.c after the other profiles, so every shared helper is in scope.
 *
 * Based on the t6000 suspend-to-RAM reference by Andreas Fernandez.
 */

/* Per-core power-down controls, as on the other Firestorm/Icestorm SoCs */
#define T6000_CYC_OVRD			sys_reg(3, 5, 15, 5, 0)
#define T6000_CYC_DISABLE_WFI_RET	BIT_ULL(0)
#define T6000_CYC_FIQ_MODE		GENMASK_ULL(21, 20)
#define T6000_CYC_IRQ_MODE		GENMASK_ULL(23, 22)
#define T6000_CYC_WFI_MODE		GENMASK_ULL(25, 24)
#define T6000_ACC_OVRD			sys_reg(3, 5, 15, 6, 0)
#define T6000_ACC_PWR_DN_SRM		GENMASK_ULL(14, 13)
#define T6000_ACC_DIS_L2_FLUSH		GENMASK_ULL(16, 15)
#define T6000_ACC_TRAIN_DOWN_LINK	GENMASK_ULL(18, 17)
#define T6000_ACC_POWER_DOWN_CPM	GENMASK_ULL(26, 25)
#define T6000_ACC_DIS_PIO_WFI		BIT_ULL(32)
#define T6000_ACC_DEEP_SLEEP		BIT_ULL(34)

/*
 * The watchdog is WD1. Each step of the entry arms it again, with the time in seconds the
 * steps up to the next arm are given, and the wake arms it once more for the resume.
 */
#define T6000_WDT_WAKE_SECS		170

/* "wake": seven mask words (205 valid sources), then seven status words */
#define T6000_WAKE_MASK			0x08
#define T6000_WAKE_STATUS		0x24
#define T6000_WAKE_WORDS		7
#define T6000_WAKE_LAST_WORD		GENMASK(12, 0)

/* "sep-pg": bit 2 makes the S2R sequencer wait for SEP's power gate */
#define T6000_SEP_PG_WAIT		BIT(2)

/*
 * "cluster": one word per complex, at four times its index. The index is the first byte of
 * the complex's entry in the ADT's soc-clusters. Target in bits 3:0, busy in bit 4.
 */
#define T6000_CLUSTERS			9
#define T6000_CLUSTER_EACC		0
#define T6000_CLUSTER_PACC0		1
#define T6000_CLUSTER_PACC1		2
#define T6000_CLUSTER_ANE0		3
#define T6000_CLUSTER_AFR		5
#define T6000_CLUSTER_DISPLAY		6
#define T6000_CLUSTER_AVEMSR		8
#define T6000_CLUSTERS_CPU		(BIT(T6000_CLUSTER_EACC) | BIT(T6000_CLUSTER_PACC0) | \
					 BIT(T6000_CLUSTER_PACC1))
#define T6000_CLUSTERS_VALID		(T6000_CLUSTERS_CPU | BIT(T6000_CLUSTER_ANE0) | \
					 BIT(T6000_CLUSTER_AFR) | BIT(T6000_CLUSTER_DISPLAY) | \
					 BIT(T6000_CLUSTER_AVEMSR))
#define T6000_CLUSTER_PS_TARGET		GENMASK(3, 0)
#define T6000_CLUSTER_PS_BUSY		BIT(4)

/*
 * "acc0".."acc2": the CPU complexes' blocks (E, P0, P1). The two DVFS command values are the
 * ones recorded from macOS; APSC_OFF and BUSY are bits of the same word.
 */
#define T6000_ACCS			3
#define T6000_ACC_DVFS_CMD		0x00020
#define T6000_ACC_DVFS_APSC_OFF		BIT_ULL(22)
#define T6000_ACC_DVFS_BUSY		BIT_ULL(7)
#define T6000_ACC_DVFS_RELOCK		0x100101ULL
#define T6000_ACC_DVFS_APSC		0x40000500101ULL
#define T6000_ACC_PERF_CTL		0x00660
#define T6000_ACC_PERF_EN		0x15ULL
#define T6000_ACC_LLC_THRTL		0x20240
#define T6000_ACC_AMX_THRTL		0x20250
#define T6000_ACC_THRTL_EN		BIT_ULL(63)
#define T6000_ACC_ENERGY_CTL		0x28080
#define T6000_ACC_ENERGY_EN		(BIT_ULL(63) | BIT_ULL(61))
#define T6000_ACC_PPT_THRTL		0x28400

/*
 * "clkgen": muxes parked onto the reference clock before PLL0 goes off, plus four muxes moved
 * to source 7 and four muxes disabled at commit. Select in bits 27:24, enable in bit 31, busy
 * in bit 30.
 */
#define T6000_CLKGEN_MUX_ON		BIT(31)
#define T6000_CLKGEN_MUX_BUSY		BIT(30)
#define T6000_CLKGEN_MUX_SEL		GENMASK(27, 24)
#define T6000_CLKGEN_LATE_SEL		7

/* "pll0": off in three writes, each waited for: off mode 3, off allowed, then the enable */
#define T6000_PLL_ON			BIT(0)
#define T6000_PLL_OFF_OK		BIT(1)
#define T6000_PLL_OFF_MODE		GENMASK(5, 3)
#define T6000_PLL_BUSY			BIT(31)

/* "dcs": the memory clock's performance state, target in bits 3:0, busy in bit 31 */
#define T6000_DCS_PS_TARGET		GENMASK(3, 0)
#define T6000_DCS_PS_BUSY		BIT(31)
/* The states the commit steps through on the way down, and the first on the way back */
#define T6000_DCS_PS_STEP		7
#define T6000_DCS_PS_SLEEP		5

/*
 * "amcc0".."amcc3": the memory controllers, each with four DCS channel blocks 2 MiB apart from
 * 2 MiB above its base. Bits 12 and 8 enable dynamic low-power gating across the commit.
 */
#define T6000_AMCCS			4
#define T6000_AMCC_DRAIN_CTL		0x1c00
#define T6000_AMCC_DRAIN_COUNT		0x1c04
#define T6000_AMCC_LOCK			0x10000c
#define T6000_DCS_PER_AMCC		4
#define T6000_DCS_BLOCK(k)		(0x200000ULL * ((k) + 1))
#define T6000_DCS_BLOCK_LEN		0x20
#define T6000_DCS_CTL			0x10
#define T6000_DCS_CTL_LP		(BIT(12) | BIT(8))

/*
 * A fabric bridge's handshake before its domain powers off, as on T8103: 1 at +0x14, then 1,
 * 2 and 4 at +0x2bc. "apcie-q" is PCIe's; the others are 0x8000 into their fabric blocks.
 */
#define T6000_BRIDGE_8014		0x014
#define T6000_BRIDGE_82BC		0x2bc
#define T6000_BRIDGE_LEN		0x300

/*
 * "ptd", the PMP's telemetry: entry n is read at 0x10 * n and written at 0x10000 + 8 * n.
 * 0xf8 is the device power requests, one bit per pmp_report entry; 0x100 acknowledges them.
 */
#define T6000_PTD_PS_REQ_RD		0xf80
#define T6000_PTD_PS_ACK_RD		0x1000
#define T6000_PTD_PS_REQ_WR		0x107c0
#define T6000_PTD_DVFS_WR(n)		((n) == 3 ? 0x10860 : 0x10840 + 8 * (n))
#define T6000_PTD_DVFS_RD(n)		((n) == 3 ? 0x10c0 : 0x1080 + 0x10 * (n))
#define T6000_PTD_DVFS_IDLE		0x2000000000000000ULL
/* The requests of the GPU and of the display coprocessors */
#define T6000_PTD_REQ_GFX		BIT_ULL(0x9)
#define T6000_PTD_REQ_DISP0		BIT_ULL(0xc)
#define T6000_PTD_REQ_DISPEXT0		BIT_ULL(0xd)
#define T6000_PTD_REQ_DISPEXT1		BIT_ULL(0xe)
/* The PMP's sleep word: 4 from .begin, 1 at .prepare, back as it was at .finish */
#define T6000_PTD_SLEEP			0x18328
#define T6000_PTD_SLEEP_BEGIN		4
#define T6000_PTD_SLEEP_COMMIT		1

/*
 * "spmi" (nub, gen 1): vector enable words from +0x20, their acknowledgment 0x40 above. The
 * PMU RTC's alarm is vector 2.
 */
#define T6000_SPMI_VEC_ENABLE		0x20
#define T6000_SPMI_VEC_ACK		0x60
#define T6000_SPMI_VEC_WORDS		7
#define T6000_SPMI_VEC_RTC		BIT(2)

/* Power words in "ps" (pmgr), "ps-east" (pmgr_east), and "ps-aon" (pmgr_mini) */
#define T6000_PS_GPIO			0x120
#define T6000_PS_PMGR_SOC_OCLA		0x130
#define T6000_PS_PCIE0_REF		0x138
#define T6000_PS_PCIE1_REF		0x140
#define T6000_PS_APCIE_ST		0x148
#define T6000_PS_AFR			0x1e8
#define T6000_PS_AFNC1_LW2		0x220
#define T6000_PS_ATC0_COMMON		0x238
#define T6000_PS_DISPEXT0_FE		0x258
#define T6000_PS_DISPEXT1_FE		0x260
#define T6000_PS_DISPEXT0_CPU0		0x280
#define T6000_PS_DISPEXT1_CPU0		0x2a8
#define T6000_PS_PMP			0x2d8
#define T6000_PS_PMS_SRAM		0x2e0
#define T6000_PS_APCIE_ST_SYS		0x2e8
#define T6000_PS_APCIE_ST1_SYS		0x2f0
#define T6000_PS_ATC2_COMMON		0x2f8
#define T6000_PS_ATC3_COMMON		0x300
#define T6000_PS_APCIE_GP_SYS		0x320
#define T6000_PS_ATC0_PCIE		0x330
#define T6000_PS_ATC1_PCIE		0x340
#define T6000_PS_ATC2_PCIE		0x350
#define T6000_PS_ATC3_PCIE		0x360

#define T6000_PS_EAST_AFNC2_LW1		0x160
#define T6000_PS_EAST_AFNC3_LW0		0x178
#define T6000_PS_EAST_SIO		0x180
#define T6000_PS_EAST_SIO_CPU		0x188
#define T6000_PS_EAST_FPWM0		0x190
#define T6000_PS_EAST_SPI_P		0x1e8
#define T6000_PS_EAST_SIO_SPMI0		0x218
#define T6000_PS_EAST_SIO_SPMI1		0x220
#define T6000_PS_EAST_SIO_SPMI2		0x228
#define T6000_PS_EAST_UART_N		0x238
#define T6000_PS_EAST_UART1		0x248
#define T6000_PS_EAST_UART2		0x250
#define T6000_PS_EAST_UART3		0x258
#define T6000_PS_EAST_UART4		0x260
#define T6000_PS_EAST_UART6		0x268
#define T6000_PS_EAST_UART7		0x270
#define T6000_PS_EAST_AUDIO_P		0x278
#define T6000_PS_EAST_AES		0x2d0
#define T6000_PS_EAST_DISP0_FE		0x328
#define T6000_PS_EAST_DISP0_CPU0	0x350
#define T6000_PS_EAST_ANS2		0x3b8
#define T6000_PS_EAST_GFX		0x3c0
#define T6000_PS_EAST_SEP		0xc00

#define T6000_PS_AON_MSG		0x078
#define T6000_PS_AON_ATC0_USB_AON	0x088
#define T6000_PS_AON_ATC1_USB_AON	0x090
#define T6000_PS_AON_ATC2_USB_AON	0x098
#define T6000_PS_AON_ATC3_USB_AON	0x0a0
#define T6000_PS_AON_ATC0_USB		0x0d0
#define T6000_PS_AON_ATC1_USB		0x0d8
#define T6000_PS_AON_ATC2_USB		0x0e0

/* PS word fields the core doesn't name, as pmgr-pwrstate has them */
#define T6000_PS_RESET			BIT(31)
#define T6000_PS_AUTO			GENMASK(27, 24)
#define T6000_PS_MIN			GENMASK(19, 16)
#define T6000_PS_BUSY			BIT(11)
#define T6000_PS_WAS_CLKGATED		BIT(9)
/* Bits 12:8: the latches and status bits a restoring write leaves at 0 */
#define T6000_PS_LATCHES		(PS_UNLATCH | T6000_PS_BUSY | PS_DEV_DISABLE | PS_FLAGS)

#define T6000_PS_WORD(auto_en, auto_st, flags, target) \
	(((auto_en) ? PS_AUTO_ENABLE : 0) | \
	 FIELD_PREP_CONST(T6000_PS_AUTO, (auto_st)) | \
	 (flags) | \
	 FIELD_PREP_CONST(PS_TARGET, (target)))

#define T6000_PS_WORD_MIN(auto_en, auto_st, min_st, flags, target) \
	(T6000_PS_WORD((auto_en), (auto_st), (flags), (target)) | \
	 FIELD_PREP_CONST(T6000_PS_MIN, (min_st)))

/*
 * Blocks with no register entry in the sleep node. Each belongs to a device with its own
 * node or to none, and only the words named here are touched.
 */
/* The fabric bridges of dispext0, disp0, dispext1, and the GPU (+0x8000 into each fabric) */
static const u64 t6000_disp_bridge_pa[] = {
	0x28a008000ULL, 0x38c008000ULL, 0x28d008000ULL,
};
#define T6000_GFX_BRIDGE_PA		0x405008000ULL
/*
 * The enable word of the thermal/voltage monitor in front of the GFX domain, the T6000's
 * counterpart of the T8132 GFX TVM gate. As macOS: 1 before AFR and GFX are powered for the
 * bridge quiesce, 0 once both are lowered again. Zeroed with the other monitors too, in
 * t6000_quiesce_zero_pa.
 */
#define T6000_AFR_GATE			0x28e5e4000ULL
/* SIO's CPU control, mailbox I2A control, and its two DARTs' stream enables */
static const u64 t6000_sio_stop_pa[] = {
	0x39bc00044ULL,		/* sio ASC_CPU_CONTROL */
	0x39bc08114ULL,		/* sio mailbox I2A_CONTROL */
	0x39b0040fcULL,		/* sio_dart_0 stream enable */
	0x39b0080fcULL,		/* sio_dart_1 stream enable */
};
/*
 * The PMP's address filter (16 entries of six words, in use with bits 1:0 of the first set)
 * and reset vector. The sleep loses both and the PMP's restart needs them: saved at probe.
 */
#define T6000_PMP_DAPF			0x28e304000ULL
#define T6000_PMP_DAPF_ENTRIES		16
#define T6000_PMP_DAPF_STRIDE		0x40
#define T6000_PMP_DAPF_WORDS		6
#define T6000_PMP_DAPF_IN_USE		GENMASK(1, 0)
#define T6000_PMP_IORVBAR		0x28e850000ULL
#define T6000_PMP_IORVBAR_VALID		BIT_ULL(0)

/*
 * The NCO's channels, five words each as in clk-apple-nco. A channel is enabled across the
 * commit, and its words are written back at the wake in that driver's order.
 */
#define T6000_NCO_CHANNELS		5
#define T6000_NCO_STRIDE		0x4000
#define T6000_NCO_WORDS			5
#define T6000_NCO_CTRL			0
#define T6000_NCO_CTRL_ENABLE		BIT(31)

enum {
	T6000_ACCR_PERF,
	T6000_ACCR_ENERGY,
	T6000_ACCR_AMX,
	T6000_ACCR_LLC,
	T6000_ACCR_PPT,
	T6000_ACCR_DVFS,
	T6000_ACCR_N,
};

static const u32 t6000_acc_regs[T6000_ACCR_N] = {
	[T6000_ACCR_PERF]   = T6000_ACC_PERF_CTL,
	[T6000_ACCR_ENERGY] = T6000_ACC_ENERGY_CTL,
	[T6000_ACCR_AMX]    = T6000_ACC_AMX_THRTL,
	[T6000_ACCR_LLC]    = T6000_ACC_LLC_THRTL,
	[T6000_ACCR_PPT]    = T6000_ACC_PPT_THRTL,
	[T6000_ACCR_DVFS]   = T6000_ACC_DVFS_CMD,
};

/* What the quiesce clears in each of the first five */
static const u64 t6000_acc_enable[T6000_ACCR_DVFS] = {
	[T6000_ACCR_PERF]   = T6000_ACC_PERF_EN,
	[T6000_ACCR_ENERGY] = T6000_ACC_ENERGY_EN,
	[T6000_ACCR_AMX]    = T6000_ACC_THRTL_EN,
	[T6000_ACCR_LLC]    = T6000_ACC_THRTL_EN,
	[T6000_ACCR_PPT]    = T6000_ACC_THRTL_EN,
};

/* In "clkgen": muxes parked onto refclk before PLL0 off, late source-7 muxes, and off muxes */
static const u16 t6000_clkgen_mux[] = { 0x164, 0x16c, 0x168, 0x15c, 0x170 };
static const u16 t6000_clkgen_late_mux[] = { 0x180, 0x184, 0x188, 0x18c };
static const u16 t6000_clkgen_off_mux[] = { 0x98, 0x9c, 0xa0, 0xa4 };

/*
 * Words zeroed at the head of the quiesce and restored on wake:
 *  - 4 fabric error-handler enables (south/west/east/gfx fabrics)
 *  - 9 PMGR performance/telemetry controls
 *  - 6 thermal/voltage monitor enables (two east monitors, the second of them the GFX
 *    monitor t6000_gfx_down() switches as T6000_AFR_GATE, + four SoC TVMs)
 */
static const u64 t6000_quiesce_zero_pa[] = {
	/* Fabric error-handler enables */
	0x282000a00ULL, 0x304000a00ULL, 0x383000a00ULL, 0x402000a00ULL,
	/* PMGR performance/telemetry controls */
	0x28e0ad000ULL, 0x28e0f8000ULL, 0x28e0f9000ULL, 0x28e0fa000ULL,
	0x28e0fb000ULL, 0x28e0fc000ULL, 0x28e0fd000ULL, 0x28e070000ULL,
	0x28e5d8000ULL,
	/* Thermal/voltage monitors */
	0x28e5e0000ULL, T6000_AFR_GATE,
	0x28e0c8200ULL, 0x28e0c8400ULL, 0x28e0c8600ULL, 0x28e0c8000ULL,
};

/* ADT /arm-io/pmgr ap-wake-sources on T6000, for wake reporting and SMC-only detection */
static const struct apple_sleep_wake_name t6000_wake_names[] = {
	{ 12, "AOP.GPIO.IRQ4" },
	{ 14, "AOP.GPIO.IRQ6" },
	{ 26, "AOP.OutboxNotEmpty" },
	{ 27, "AOP.CPUWakeupAP" },
	{ 40, "NUB.WakeupAP" },
	{ 43, "NUB.GPIO.IRQ2" },
	{ 49, "NUB.APWatchdog" },
	{ 50, "NUB.APWakeupTime" },
	{ 51, "NUB.SPMISw0IRQ" },
	{ 52, "NUB.SPMISw1IRQ" },
	{ 54, "NUB.SPMISw3IRQ" },
	{ 85, "SMC.CPUWakeupAP" },
	{ 90, "SMC.OutboxNotEmpty" },
	{ 126, "ATC0.CIOWakeup" },
	{ 127, "ATC1.CIOWakeup" },
	{ 128, "ATC2.CIOWakeup" },
	{ 129, "ATC3.CIOWakeup" },
	{ 130, "ATC0.USBWakeup" },
	{ 131, "ATC1.USBWakeup" },
	{ 132, "ATC2.USBWakeup" },
	{ 133, "ATC3.USBWakeup" },
};

/* The register blocks a power word is in */
enum {
	T6000_PS,		/* "ps": pmgr */
	T6000_PS_EAST,		/* "ps-east": pmgr_east */
	T6000_PS_AON,		/* "ps-aon": pmgr_mini, the always-on (NUB) domains */
	T6000_BLOCKS,
};

struct t6000_ps_word {
	u8 block;
	u16 offset;
};

/* Each display coprocessor's CPU and front-end words: dispext0, disp0, dispext1 */
static const struct t6000_ps_word t6000_disp_ps[6] = {
	{ T6000_PS, T6000_PS_DISPEXT0_CPU0 },
	{ T6000_PS, T6000_PS_DISPEXT0_FE },
	{ T6000_PS_EAST, T6000_PS_EAST_DISP0_CPU0 },
	{ T6000_PS_EAST, T6000_PS_EAST_DISP0_FE },
	{ T6000_PS, T6000_PS_DISPEXT1_CPU0 },
	{ T6000_PS, T6000_PS_DISPEXT1_FE },
};

/* The order macOS lowers them in: disp0, dispext0, dispext1, each CPU before its front end */
static const u8 t6000_disp_order[6] = { 2, 3, 0, 1, 4, 5 };

/* The PMP's request of each, by the index of its pair in t6000_disp_ps[] */
static const u64 t6000_disp_request[3] = {
	T6000_PTD_REQ_DISPEXT0, T6000_PTD_REQ_DISP0, T6000_PTD_REQ_DISPEXT1,
};

/* The domains that must read off before a complex's target is taken off (offset 0 ends) */
static const struct t6000_ps_word t6000_gate_ane0[] = {
	{ T6000_PS, 0x268 },		/* ane_sys */
	{ T6000_PS, 0x2c8 },		/* ane_sys_cpu */
	{ }
};

static const struct t6000_ps_word t6000_gate_afr[] = {
	{ T6000_PS, T6000_PS_AFR },
	{ T6000_PS_EAST, T6000_PS_EAST_GFX },
	{ T6000_PS, 0x268 },		/* ane_sys */
	{ T6000_PS, 0x270 },		/* avd_sys */
	{ T6000_PS_EAST, 0x390 },	/* jpg */
	{ T6000_PS_EAST, 0x398 },	/* msr0 */
	{ T6000_PS_EAST, 0x3b0 },	/* venc_sys */
	{ }
};

static const struct t6000_ps_word t6000_gate_display[] = {
	{ T6000_PS_EAST, T6000_PS_EAST_DISP0_CPU0 },
	{ T6000_PS, T6000_PS_DISPEXT0_CPU0 },
	{ T6000_PS, T6000_PS_DISPEXT1_CPU0 },
	{ }
};

static const struct t6000_ps_word t6000_gate_avemsr[] = {
	{ T6000_PS, 0x270 },		/* avd_sys */
	{ T6000_PS_EAST, 0x398 },	/* msr0 */
	{ T6000_PS_EAST, 0x3b0 },	/* venc_sys */
	{ }
};

/*
 * Always-on ("ps-aon") domains lowered at commit: they keep their written state across the
 * sleep, so each is saved at .prepare and restored on wake.
 */
static const struct {
	u16 offset;
	u32 value;
} t6000_aon_commit_off[] = {
	{ T6000_PS_AON_ATC0_USB,     T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS_AON_ATC2_USB,     T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS_AON_ATC1_USB,     T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS_AON_ATC0_USB_AON, T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS_AON_ATC2_USB_AON, T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS_AON_ATC1_USB_AON, T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS_AON_ATC3_USB_AON, T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS_AON_MSG,          T6000_PS_WORD(false, 0, T6000_PS_WAS_CLKGATED, 0) },
};

/* Non-AON PMGR domains programmed into their sequencer commit state before the S2R timer */
static const struct {
	u8 block;
	u16 offset;
	u32 value;
} t6000_ps_commit_state[] = {
	/* Lower stopped USB-C PCIe/common and UART domains */
	{ T6000_PS,      T6000_PS_ATC0_PCIE,       T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS,      T6000_PS_ATC1_PCIE,       T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS,      T6000_PS_ATC2_PCIE,       T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS,      T6000_PS_ATC3_PCIE,       T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS,      T6000_PS_ATC0_COMMON,     T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS,      T6000_PS_ATC2_COMMON,     T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS,      T6000_PS_ATC3_COMMON,     T6000_PS_WORD(false, 0, PS_FLAGS, 0) },
	{ T6000_PS_EAST, T6000_PS_EAST_UART2,
	  T6000_PS_WORD(false, 0, T6000_PS_WAS_CLKGATED, 0) },
	{ T6000_PS_EAST, T6000_PS_EAST_UART3,
	  T6000_PS_WORD(false, 0, T6000_PS_WAS_CLKGATED, 0) },
	{ T6000_PS_EAST, T6000_PS_EAST_UART4,
	  T6000_PS_WORD(false, 0, T6000_PS_WAS_CLKGATED, 0) },
	{ T6000_PS_EAST, T6000_PS_EAST_UART6,
	  T6000_PS_WORD(false, 0, T6000_PS_WAS_CLKGATED, 0) },
	{ T6000_PS_EAST, T6000_PS_EAST_UART7,
	  T6000_PS_WORD(false, 0, T6000_PS_WAS_CLKGATED, 0) },
	/* Hand fabric link-wrapper and PCIe/storage/SIO domains to the S2R sequencer */
	{ T6000_PS,      T6000_PS_AFNC1_LW2,       T6000_PS_WORD(true,  0,   PS_FLAGS, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_AFNC2_LW1,  T6000_PS_WORD(true,  0,   PS_FLAGS, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_AFNC3_LW0,  T6000_PS_WORD(true,  0,   PS_FLAGS, 0xf) },
	{ T6000_PS,      T6000_PS_APCIE_GP_SYS,    T6000_PS_WORD(true,  0,   PS_FLAGS, 0xf) },
	{ T6000_PS,      T6000_PS_PCIE0_REF,
	  T6000_PS_WORD(true, 4, T6000_PS_WAS_CLKGATED, 0xf) },
	{ T6000_PS,      T6000_PS_PCIE1_REF,
	  T6000_PS_WORD(true, 4, T6000_PS_WAS_CLKGATED, 0xf) },
	{ T6000_PS,      T6000_PS_APCIE_ST_SYS,    T6000_PS_WORD(true,  0xf, PS_FLAGS, 0xf) },
	{ T6000_PS,      T6000_PS_APCIE_ST1_SYS,   T6000_PS_WORD(true,  0xf, PS_FLAGS, 0xf) },
	{ T6000_PS,      T6000_PS_APCIE_ST,        T6000_PS_WORD(false, 0,   0, 0xf) },
	{ T6000_PS,      T6000_PS_GPIO,            T6000_PS_WORD(false, 0,   0, 0xf) },
	{ T6000_PS,      T6000_PS_PMGR_SOC_OCLA,   T6000_PS_WORD(false, 0,   0, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_SIO_CPU,
	  T6000_PS_WORD(false, 0xf, T6000_PS_WAS_CLKGATED, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_FPWM0,
	  T6000_PS_WORD(false, 0, T6000_PS_WAS_CLKGATED, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_SPI_P,      T6000_PS_WORD(false, 0,   0, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_SIO_SPMI0,  T6000_PS_WORD(true,  0xf, 0, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_SIO_SPMI1,  T6000_PS_WORD(true,  0xf, 0, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_SIO_SPMI2,  T6000_PS_WORD(true,  0xf, 0, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_UART_N,     T6000_PS_WORD(false, 0,   0, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_UART1,      T6000_PS_WORD(false, 0,   0, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_AUDIO_P,
	  T6000_PS_WORD(false, 0, T6000_PS_WAS_CLKGATED, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_AES,        T6000_PS_WORD(false, 0,   PS_FLAGS, 0xf) },
	{ T6000_PS_EAST, T6000_PS_EAST_ANS2,       T6000_PS_WORD(true,  0xf, PS_FLAGS, 0xf) },
};

/* How long the late commit writes get to take effect before the timer is written */
#define T6000_LATE_SETTLE_MS	200

/*
 * Every PMGR power domain of the device tree: the sleep cuts their power while genpd counts
 * them on, so each is raised again at the wake, parents first. Not SEP's, afc or afi.
 */
#define T6000_DOMAINS_MAX	256
#define T6000_DOMAIN_BLOCKS	8
#define T6000_DOMAIN_PASSES	30

/*
 * Steps of the entry that were left out or did not finish, kept for the report after the
 * wake: the commit runs with one CPU and no console.
 */
#define T6000_INCOMPLETE_SEP		BIT(0)
#define T6000_INCOMPLETE_PMP		BIT(1)
#define T6000_INCOMPLETE_PMS_SRAM	BIT(2)
#define T6000_INCOMPLETE_DCS_KEPT	BIT(3)
#define T6000_INCOMPLETE_DCS_BUSY	BIT(4)
#define T6000_INCOMPLETE_PLL_KEPT	BIT(5)

static const char *const t6000_incomplete_names[] = {
	"SEP not power-gated",
	"PMP not off",
	"PMP SRAM not off",
	"memory clock state not lowered",
	"memory clock state not reached",
	"clock mux busy and PLL0 left on",
};

static struct t6000_domain {
	struct device_node *np;
	void __iomem *reg;
	const char *name;
	u32 saved;
	/* A display word: its driver lowers it in its suspend, so it is read at .begin */
	bool save_at_begin;
	/* In "ps-aon": it keeps its state, and isn't raised here */
	bool always_on;
	/* A dcs_* word: what it read at the commit of the first sleep since boot */
	u32 commit_ref;
} t6000_domains[T6000_DOMAINS_MAX];

static struct {
	resource_size_t start;
	void __iomem *base;
} t6000_domain_blocks[T6000_DOMAIN_BLOCKS];

static struct t6000_sleep {
	void __iomem *ps_east, *sep_pg, *clkgen, *pll0, *dcs, *apcie_q, *ptd;
	void __iomem *acc[T6000_ACCS];
	void __iomem *amcc[T6000_AMCCS];
	void __iomem *dcs_block[T6000_AMCCS][T6000_DCS_PER_AMCC];
	void __iomem *disp_bridge[ARRAY_SIZE(t6000_disp_bridge_pa)];
	void __iomem *gfx_bridge, *afr_gate, *pmp_dapf, *pmp_iorvbar, *nco, *fabric_ps;
	void __iomem *sio_stop[ARRAY_SIZE(t6000_sio_stop_pa)];
	void __iomem *zero[ARRAY_SIZE(t6000_quiesce_zero_pa)];
	/* Where "ps", "ps-east" and "ps-aon" start, by their enum */
	resource_size_t block_start[T6000_BLOCKS];

	unsigned int ndomains, ndomain_blocks;
	bool domains_saved;
	unsigned int domain_pass, domains_stuck;

	u32 aon_saved[ARRAY_SIZE(t6000_aon_commit_off)];
	bool aon_saved_valid;

	u32 sep_pg_saved;
	bool sep_pg_cleared;

	u32 spmi_saved[T6000_SPMI_VEC_WORDS];
	bool spmi_armed;

	u64 ptd_req_saved;
	u64 dvfs_saved[4];
	bool ptd_written;
	u32 ptd_sleep_saved;
	bool ptd_sleep_set;

	u32 afr_cluster_before;
	bool afr_cluster_valid;

	u32 dcs_ctl_saved[T6000_AMCCS][T6000_DCS_PER_AMCC];
	u32 zero_saved[ARRAY_SIZE(t6000_quiesce_zero_pa)];
	u32 apcie_saved[2];
	u64 acc_saved[T6000_ACCS][T6000_ACCR_N];
	u64 relock_saved[2];
	u32 dcs_saved;
	/* The dcs_* words: the first sleep's values taken, and what the commit did to them */
	bool dcs_ref_taken;
	unsigned int dcs_floors, dcs_changed;
	const struct t6000_domain *dcs_changed_first;
	u32 dcs_changed_was;
	u32 afr_saved;
	u32 cluster_saved[T6000_CLUSTERS];
	u32 cluster_written;
	/* T6000_INCOMPLETE_*, and the complexes whose target stayed: a domain of theirs was on */
	u32 incomplete, clusters_kept;
	u32 mux_saved[ARRAY_SIZE(t6000_clkgen_mux)];
	u32 mux_n;
	u32 pll_saved;
	bool pll_done, dcs_done, afr_done, mcc_done, zeros_done, apcie_done, acc_done, relock_done;

	u32 wdt_saved_ctl, wdt_saved_bite;
	bool wdt_saved;
	bool aic_masked;

	u32 nco_saved[T6000_NCO_CHANNELS][T6000_NCO_WORDS];
	bool nco_saved_valid;

	u32 pmp_dapf_saved[T6000_PMP_DAPF_ENTRIES][T6000_PMP_DAPF_WORDS];
	unsigned int pmp_dapf_n;
	u64 pmp_iorvbar_saved;
	bool pmp_iorvbar_valid;
} t6000_sleep;

static void __iomem *t6000_map(struct apple_sleep *s, const char *name, struct resource *res)
{
	struct device_node *np = s->dev->of_node;
	int index = of_property_match_string(np, "reg-names", name);

	if (index < 0 || of_address_to_resource(np, index, res))
		return IOMEM_ERR_PTR(-ENODEV);
	/* Shared with the drivers of these blocks, so not claimed. */
	return apple_sleep_map(s->dev, name);
}

/* Mapped for good, non-posted (a posted write to these blocks is an SError), or NULL */
static void __iomem *t6000_ioremap(struct apple_sleep *s, phys_addr_t pa, size_t len)
{
	void __iomem *base = ioremap_np(pa, len);

	if (base && devm_add_action_or_reset(s->dev, apple_sleep_iounmap, (void __force *)base))
		return NULL;
	return base;
}

static void __iomem *t6000_block(struct apple_sleep *s, unsigned int block)
{
	switch (block) {
	case T6000_PS:		return s->ps;
	case T6000_PS_EAST:	return t6000_sleep.ps_east;
	case T6000_PS_AON:	return s->ps_aon;
	default:		return NULL;
	}
}

static void __iomem *t6000_ps_reg(struct apple_sleep *s, struct t6000_ps_word word)
{
	return t6000_block(s, word.block) + word.offset;
}

/* A PMGR node's block, mapped once however many of its domains are tracked */
static void __iomem *t6000_domain_block(struct apple_sleep *s, struct device_node *parent,
					resource_size_t *start)
{
	struct t6000_sleep *t = &t6000_sleep;
	struct resource res;
	unsigned int b;

	if (of_address_to_resource(parent, 0, &res))
		return NULL;
	*start = res.start;
	for (b = 0; b < t->ndomain_blocks; b++)
		if (t6000_domain_blocks[b].start == res.start)
			return t6000_domain_blocks[b].base;
	if (t->ndomain_blocks == T6000_DOMAIN_BLOCKS)
		return NULL;
	t6000_domain_blocks[b].start = res.start;
	t6000_domain_blocks[b].base = t6000_ioremap(s, res.start, resource_size(&res));
	if (t6000_domain_blocks[b].base)
		t->ndomain_blocks++;
	return t6000_domain_blocks[b].base;
}

static void t6000_domains_init(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	struct device_node *np, *parent;
	resource_size_t start;
	void __iomem *base;
	const char *label;
	unsigned int i;
	u32 off;

	for_each_compatible_node(np, NULL, "apple,pmgr-pwrstate") {
		struct t6000_domain *d = &t6000_domains[t->ndomains];

		if (t->ndomains == T6000_DOMAINS_MAX) {
			of_node_put(np);
			break;
		}
		label = NULL;
		of_property_read_string(np, "label", &label);
		if (!label || !strcmp(label, "sep") || !strcmp(label, "afc") ||
		    !strcmp(label, "afi"))
			continue;
		if (of_property_read_u32(np, "reg", &off))
			continue;
		parent = of_get_parent(np);
		base = parent ? t6000_domain_block(s, parent, &start) : NULL;
		of_node_put(parent);
		if (!base)
			continue;
		d->np = of_node_get(np);
		d->reg = base + off;
		d->name = label;
		d->always_on = start == t->block_start[T6000_PS_AON];
		for (i = 0; i < ARRAY_SIZE(t6000_disp_ps); i++)
			if (t->block_start[t6000_disp_ps[i].block] + t6000_disp_ps[i].offset ==
			    start + off)
				d->save_at_begin = true;
		t->ndomains++;
	}
}

static void t6000_domains_save(bool at_begin)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int i;

	t->domains_saved = false;
	for (i = 0; i < t->ndomains; i++)
		if (t6000_domains[i].save_at_begin == at_begin)
			t6000_domains[i].saved = readl(t6000_domains[i].reg);
	t->domains_saved = !at_begin;
}

static int t6000_domain_find(struct device_node *np)
{
	unsigned int i;

	for (i = 0; i < t6000_sleep.ndomains; i++)
		if (t6000_domains[i].np == np)
			return i;
	return -1;
}

/*
 * One domain back to its saved state, parents first: minimum state, target with AUTO_ENABLE
 * off, then AUTO_ENABLE. A domain that doesn't follow is taken to 0 first on later passes.
 */
static void t6000_domain_on(unsigned int i, unsigned int depth, unsigned long *seen)
{
	struct t6000_sleep *t = &t6000_sleep;
	struct t6000_domain *d = &t6000_domains[i];
	struct of_phandle_iterator it;
	u32 saved = d->saved, cur;
	unsigned int n;
	int err, parent;

	if (test_and_set_bit(i, seen) || depth > 8)
		return;
	if (d->always_on)
		return;
	if (!FIELD_GET(PS_ACTUAL, saved))
		return;
	of_for_each_phandle(&it, err, d->np, "power-domains", "#power-domain-cells", 0) {
		parent = t6000_domain_find(it.node);
		if (parent >= 0)
			t6000_domain_on(parent, depth + 1, seen);
	}
	cur = readl(d->reg);
	if ((cur ^ saved) & T6000_PS_MIN) {
		writel((cur & ~(T6000_PS_LATCHES | T6000_PS_MIN)) | (saved & T6000_PS_MIN),
		       d->reg);
		cur = readl(d->reg);
	}
	if (FIELD_GET(PS_TARGET, cur) == FIELD_GET(PS_TARGET, saved) &&
	    FIELD_GET(PS_ACTUAL, cur) >= FIELD_GET(PS_ACTUAL, saved))
		return;
	cur &= ~(T6000_PS_LATCHES | PS_AUTO_ENABLE | PS_TARGET | T6000_PS_AUTO);
	cur |= saved & T6000_PS_AUTO;
	if (t->domain_pass && !FIELD_GET(PS_ACTUAL, readl(d->reg))) {
		writel(cur & ~PS_ACTUAL, d->reg);
		udelay(100);
	}
	writel(cur | (saved & PS_TARGET), d->reg);
	for (n = 0; n < 2000 && FIELD_GET(PS_ACTUAL, readl(d->reg)) <
				FIELD_GET(PS_ACTUAL, saved); n++)
		udelay(10);
	cur = readl(d->reg) & ~(T6000_PS_LATCHES | PS_ACTUAL);
	if ((saved & PS_AUTO_ENABLE) && !t->domain_pass)
		writel(cur | PS_AUTO_ENABLE, d->reg);
	cur = readl(d->reg);
	if ((cur & T6000_PS_RESET) && !(saved & T6000_PS_RESET) && FIELD_GET(PS_ACTUAL, cur)) {
		writel(cur & ~(T6000_PS_RESET | T6000_PS_BUSY | PS_FLAGS | PS_ACTUAL), d->reg);
		udelay(1);
		cur = readl(d->reg);
	}
	if (FIELD_GET(PS_ACTUAL, cur) < FIELD_GET(PS_ACTUAL, saved))
		t->domains_stuck++;
}

/* Until every domain is back, or T6000_DOMAIN_PASSES passes 10 ms apart */
static void t6000_domains_restore(void)
{
	struct t6000_sleep *t = &t6000_sleep;
	DECLARE_BITMAP(seen, T6000_DOMAINS_MAX);
	unsigned int i, pass;

	if (!t->domains_saved)
		return;
	t->domains_saved = false;
	for (pass = 0; pass < T6000_DOMAIN_PASSES; pass++) {
		if (pass)
			mdelay(10);
		t->domain_pass = pass;
		t->domains_stuck = 0;
		bitmap_zero(seen, T6000_DOMAINS_MAX);
		for (i = 0; i < t->ndomains; i++)
			t6000_domain_on(i, 0, seen);
		if (!t->domains_stuck)
			break;
	}
}

static int t6000_sleep_map_regs(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	static const char *const acc_names[T6000_ACCS] = { "acc0", "acc1", "acc2" };
	static const char *const amcc_names[T6000_AMCCS] = { "amcc0", "amcc1", "amcc2", "amcc3" };
	struct resource res, ps_east, ps_aon, amcc;
	struct device_node *np;
	unsigned int i, j, a, k;
	int ret;

	s->ps = t6000_map(s, "ps", &s->ps_res);
	t->ps_east = t6000_map(s, "ps-east", &ps_east);
	s->ps_aon = t6000_map(s, "ps-aon", &ps_aon);
	s->cpu_start = t6000_map(s, "cpu-start", &res);
	s->cluster = t6000_map(s, "cluster", &res);
	s->commit = t6000_map(s, "commit", &res);
	s->wdt = t6000_map(s, "wdt", &res);
	s->wake = t6000_map(s, "wake", &res);
	s->spmi = t6000_map(s, "spmi", &res);
	t->sep_pg = t6000_map(s, "sep-pg", &res);
	t->clkgen = t6000_map(s, "clkgen", &res);
	t->pll0 = t6000_map(s, "pll0", &res);
	t->dcs = t6000_map(s, "dcs", &res);
	t->apcie_q = t6000_map(s, "apcie-q", &res);
	t->ptd = t6000_map(s, "ptd", &res);
	if (IS_ERR(s->ps) || IS_ERR(t->ps_east) || IS_ERR(s->ps_aon) || IS_ERR(s->cpu_start) ||
	    IS_ERR(s->cluster) || IS_ERR(s->commit) || IS_ERR(s->wdt) || IS_ERR(s->wake) ||
	    IS_ERR(s->spmi) || IS_ERR(t->sep_pg) || IS_ERR(t->clkgen) || IS_ERR(t->pll0) ||
	    IS_ERR(t->dcs) || IS_ERR(t->apcie_q) || IS_ERR(t->ptd))
		return dev_err_probe(s->dev, -EINVAL,
				     "ps, ps-east, ps-aon, cpu-start, cluster, commit, wdt, wake, spmi, sep-pg, clkgen, pll0, dcs, apcie-q and ptd must all be mapped\n");
	t->block_start[T6000_PS] = s->ps_res.start;
	t->block_start[T6000_PS_EAST] = ps_east.start;
	t->block_start[T6000_PS_AON] = ps_aon.start;

	ret = apple_sleep_read_wake_sources(s);
	if (ret)
		return ret;

	for (i = 0; i < T6000_ACCS; i++) {
		t->acc[i] = t6000_map(s, acc_names[i], &res);
		if (IS_ERR(t->acc[i]))
			return dev_err_probe(s->dev, PTR_ERR(t->acc[i]), "no %s registers\n",
					     acc_names[i]);
	}
	for (a = 0; a < T6000_AMCCS; a++) {
		t->amcc[a] = t6000_map(s, amcc_names[a], &amcc);
		if (IS_ERR(t->amcc[a]))
			return dev_err_probe(s->dev, PTR_ERR(t->amcc[a]), "no %s registers\n",
					     amcc_names[a]);
		/* Its DCS channel blocks, above the controller's own: left alone if unmapped */
		for (k = 0; k < T6000_DCS_PER_AMCC; k++)
			t->dcs_block[a][k] = t6000_ioremap(s, amcc.start + T6000_DCS_BLOCK(k),
							   T6000_DCS_BLOCK_LEN);
	}

	/*
	 * The blocks with no register entry here. Each is used only if it mapped, as the
	 * steps that write them say.
	 */
	for (i = 0; i < ARRAY_SIZE(t6000_disp_bridge_pa); i++)
		t->disp_bridge[i] = t6000_ioremap(s, t6000_disp_bridge_pa[i], T6000_BRIDGE_LEN);
	t->gfx_bridge = t6000_ioremap(s, T6000_GFX_BRIDGE_PA, T6000_BRIDGE_LEN);
	t->afr_gate = t6000_ioremap(s, T6000_AFR_GATE, 4);
	for (i = 0; i < ARRAY_SIZE(t6000_sio_stop_pa); i++)
		t->sio_stop[i] = t6000_ioremap(s, t6000_sio_stop_pa[i], 4);
	for (i = 0; i < ARRAY_SIZE(t6000_quiesce_zero_pa); i++)
		t->zero[i] = t6000_ioremap(s, t6000_quiesce_zero_pa[i], 4);

	s->driver_down = devm_bitmap_zalloc(s->dev, resource_size(&s->ps_res) / 4, GFP_KERNEL);
	if (!s->driver_down)
		return -ENOMEM;

	t->pmp_dapf = t6000_ioremap(s, T6000_PMP_DAPF,
				    T6000_PMP_DAPF_ENTRIES * T6000_PMP_DAPF_STRIDE);
	if (t->pmp_dapf) {
		for (i = 0; i < T6000_PMP_DAPF_ENTRIES; i++) {
			void __iomem *entry = t->pmp_dapf + i * T6000_PMP_DAPF_STRIDE;

			if (!(readl(entry) & T6000_PMP_DAPF_IN_USE))
				break;
			for (j = 0; j < T6000_PMP_DAPF_WORDS; j++)
				t->pmp_dapf_saved[i][j] = readl(entry + 4 * j);
		}
		t->pmp_dapf_n = i;
	}
	t->pmp_iorvbar = t6000_ioremap(s, T6000_PMP_IORVBAR, 8);
	if (t->pmp_iorvbar) {
		t->pmp_iorvbar_saved = readq(t->pmp_iorvbar);
		t->pmp_iorvbar_valid = true;
	}

	np = of_find_compatible_node(NULL, NULL, "apple,t6000-nco");
	if (np && !of_address_to_resource(np, 0, &res) &&
	    resource_size(&res) >= T6000_NCO_CHANNELS * T6000_NCO_STRIDE)
		t->nco = t6000_ioremap(s, res.start, T6000_NCO_CHANNELS * T6000_NCO_STRIDE);
	of_node_put(np);

	np = of_find_compatible_node(NULL, NULL, "apple,t6000-pmgr-misc");
	if (np && !of_address_to_resource(np, 0, &res))
		t->fabric_ps = t6000_ioremap(s, res.start, 4);
	of_node_put(np);

	t6000_domains_init(s);

	return apple_sleep_alloc_books(s);
}

/* @secs from now. What WD1 had before the first arm of a sleep is kept for .finish. */
static void t6000_wdt_arm(struct apple_sleep *s, unsigned int secs)
{
	struct t6000_sleep *t = &t6000_sleep;

	if (!t->wdt_saved) {
		t->wdt_saved_ctl = readl(s->wdt + WDT_WD1_CTL);
		t->wdt_saved_bite = readl(s->wdt + WDT_WD1_BITE);
		t->wdt_saved = true;
	}
	writel(0, s->wdt + WDT_WD1_CTL);
	writel(0, s->wdt + WDT_WD1_COUNT);
	writel(secs * WDT_TICKS_PER_S, s->wdt + WDT_WD1_BITE);
	writel(WDT_WD1_RESET_EN, s->wdt + WDT_WD1_CTL);
	dsb(sy);
}

static void t6000_wdt_restore(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;

	if (!t->wdt_saved)
		return;
	writel(0, s->wdt + WDT_WD1_CTL);
	writel(0, s->wdt + WDT_WD1_COUNT);
	writel(t->wdt_saved_bite, s->wdt + WDT_WD1_BITE);
	writel(t->wdt_saved_ctl, s->wdt + WDT_WD1_CTL);
	t->wdt_saved = false;
}

/*
 * The L1 data cache written back by set and way, the EL02 timers masked, and the overrides
 * for a WFI without retention set, as XNU has them on T6000.
 */
static void t6000_deep_wfi_arm(void)
{
	u64 acc, cyc;

	if (is_kernel_in_hyp_mode()) {
		write_sysreg_s(0, SYS_CNTP_CTL_EL02);
		write_sysreg_s(0, SYS_CNTV_CTL_EL02);
	}

	acc = read_sysreg_s(T6000_ACC_OVRD);
	acc &= ~(T6000_ACC_PWR_DN_SRM | T6000_ACC_DIS_L2_FLUSH |
		 T6000_ACC_TRAIN_DOWN_LINK | T6000_ACC_POWER_DOWN_CPM);
	acc |= FIELD_PREP(T6000_ACC_PWR_DN_SRM, 3) |
	       FIELD_PREP(T6000_ACC_DIS_L2_FLUSH, 2) |
	       FIELD_PREP(T6000_ACC_TRAIN_DOWN_LINK, 3) |
	       FIELD_PREP(T6000_ACC_POWER_DOWN_CPM, 3) |
	       T6000_ACC_DIS_PIO_WFI | T6000_ACC_DEEP_SLEEP;

	cyc = read_sysreg_s(T6000_CYC_OVRD);
	cyc &= ~(T6000_CYC_WFI_MODE | T6000_CYC_IRQ_MODE | T6000_CYC_FIQ_MODE);
	cyc |= FIELD_PREP(T6000_CYC_WFI_MODE, 3) | T6000_CYC_DISABLE_WFI_RET;

	/*
	 * No return once SCTLR_EL1.C is clear: the write-back only reaches L2, so a stack pop
	 * with the data cache off could read stale DRAM before the WFI.
	 */
	asm volatile(
		"msr\tdaifset, #0xf\n\t"
		"mrs\tx3, sctlr_el1\n\t"
		"bic\tx3, x3, %[cbit]\n\t"
		"msr\tsctlr_el1, x3\n\t"
		"isb\n\t"
		"msr\tcsselr_el1, xzr\n\t"
		"isb\n\t"
		"mrs\tx3, ccsidr_el1\n\t"
		"and\tx4, x3, #7\n\t"
		"add\tx4, x4, #4\n\t"
		"ubfx\tx5, x3, #3, #10\n\t"
		"ubfx\tx6, x3, #13, #15\n\t"
		"clz\tw7, w5\n\t"
		"1:\n\t"
		"mov\tx8, x6\n\t"
		"2:\n\t"
		"lsl\tx9, x5, x7\n\t"
		"lsl\tx10, x8, x4\n\t"
		"orr\tx9, x9, x10\n\t"
		"dc\tcisw, x9\n\t"
		"subs\tx8, x8, #1\n\t"
		"b.ge\t2b\n\t"
		"subs\tx5, x5, #1\n\t"
		"b.ge\t1b\n\t"
		"dsb\tsy\n\t"
		"isb\n\t"
		"msr\tcntp_ctl_el0, xzr\n\t"
		"msr\tcntv_ctl_el0, xzr\n\t"
		"msr\ts3_1_c15_c0_0, xzr\n\t"
		"msr\ts3_5_c15_c6_0, %[acc]\n\t"
		"msr\ts3_5_c15_c5_0, %[cyc]\n\t"
		"isb\n\t"
		"mov\tx3, #1\n\t"
		"3:\n\t"
		"dsb\tsy\n\t"
		"isb\n\t"
		"wfi\n\t"
		"msr\ts3_5_c15_c1_1, x3\n\t"
		"b\t3b\n\t"
		:
		: [cbit] "i" (SCTLR_ELx_C), [acc] "r" (acc), [cyc] "r" (cyc)
		: "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "memory"
	);
	unreachable();
}

/* Masked once, before the timer: the shared commit's call after it then changes nothing. */
static void t6000_aic_mask_all(void)
{
	if (t6000_sleep.aic_masked)
		return;
	aic_sleep_mask_all();
	t6000_sleep.aic_masked = true;
}

static void t6000_aic_restore(void)
{
	if (!t6000_sleep.aic_masked)
		return;
	aic_sleep_restore();
	t6000_sleep.aic_masked = false;
}

/* The bridge's handshake, see T6000_BRIDGE_8014 */
static void t6000_bridge_quiesce(void __iomem *bridge)
{
	writel(1, bridge + T6000_BRIDGE_8014);
	writel(1, bridge + T6000_BRIDGE_82BC);
	writel(2, bridge + T6000_BRIDGE_82BC);
	writel(4, bridge + T6000_BRIDGE_82BC);
}

/* A device's request to the PMP set or cleared, and its acknowledgment waited for */
static int t6000_ptd_request(u64 request, bool on)
{
	void __iomem *ptd = t6000_sleep.ptd;
	u64 val = readq(ptd + T6000_PTD_PS_REQ_RD);

	writeq(on ? val | request : val & ~request, ptd + T6000_PTD_PS_REQ_WR);
	return readq_poll_timeout_atomic(ptd + T6000_PTD_PS_ACK_RD, val,
					 on ? val & request : !(val & request), 10, 200000);
}

/*
 * The GPU's way down: AFR and GFX up, GFX released from the PMP, the bridge quiesced, then
 * GFX and AFR lowered again.
 */
static void t6000_gfx_down(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	void __iomem *gfx = t->ps_east + T6000_PS_EAST_GFX;
	void __iomem *afr = s->ps + T6000_PS_AFR;
	void __iomem *afr_cluster = s->cluster + 4 * T6000_CLUSTER_AFR;
	bool gfx_on = true;
	u32 val;

	if (!t->gfx_bridge)
		return;

	t->afr_cluster_before = readl(afr_cluster);
	t->afr_cluster_valid = true;
	writel(CLUSTER_ON, afr_cluster);
	readl_poll_timeout_atomic(afr_cluster, val, val & CLUSTER_UP_DONE, 1, 50000);
	writel(CLUSTER_ON | CLUSTER_UP_DONE, afr_cluster);

	if (t->afr_gate)
		writel(1, t->afr_gate);
	writel(T6000_PS_WORD_MIN(false, 0xf, 4, 0, 0), afr);
	writel(T6000_PS_WORD_MIN(false, 0xf, 4, 0, 0xf), afr);
	writel(T6000_PS_WORD_MIN(true,  0xf, 4, 0, 0xf), afr);
	readl_poll_timeout_atomic(afr, val, FIELD_GET(PS_ACTUAL, val), 1, 50000);

	writel(T6000_PS_WORD(false, 0xf, 0, 0), gfx);
	writel(T6000_PS_WORD(false, 0xf, 0, 0xf), gfx);
	writel(T6000_PS_WORD(true,  0xf, 0, 0xf), gfx);

	t6000_ptd_request(T6000_PTD_REQ_GFX, true);

	if (readl_poll_timeout_atomic(gfx, val, FIELD_GET(PS_ACTUAL, val), 1, 50000))
		gfx_on = false;

	t6000_ptd_request(T6000_PTD_REQ_GFX, false);

	if (gfx_on && FIELD_GET(PS_ACTUAL, readl(gfx)))
		t6000_bridge_quiesce(t->gfx_bridge);

	writel(T6000_PS_WORD(false, 0xf, 0, 0xf), gfx);
	writel(T6000_PS_WORD(false, 0xf, PS_FLAGS, 0), gfx);
	readl_poll_timeout_atomic(gfx, val, !FIELD_GET(PS_ACTUAL, val), 1, 10000);

	writel(T6000_PS_WORD_MIN(false, 4,   4,   0, 0xf), afr);
	writel(T6000_PS_WORD_MIN(false, 0xf, 4,   PS_FLAGS, 0xf), afr);
	writel(T6000_PS_WORD_MIN(true,  0xf, 4,   0, 0xf), afr);
	writel(T6000_PS_WORD_MIN(true,  4,   0xf, T6000_PS_WAS_CLKGATED, 0xf), afr);
	writel(T6000_PS_WORD_MIN(false, 0xf, 0xf, T6000_PS_WAS_CLKGATED, 0xf), afr);
	writel(T6000_PS_WORD_MIN(false, 4,   4,   PS_FLAGS, 0), afr);
	readl_poll_timeout_atomic(afr, val, !FIELD_GET(PS_ACTUAL, val), 1, 10000);

	if (t->afr_gate)
		writel(0, t->afr_gate);
	writel(0, afr_cluster);
}

static int t6000_sleep_begin(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;

	t->ptd_sleep_set = false;
	if (s->dry_run)
		return 0;
	/* The display drivers power their domains off in their own suspend. */
	t6000_domains_save(true);
	t->ptd_sleep_saved = readl(t->ptd + T6000_PTD_SLEEP);
	writel(T6000_PTD_SLEEP_BEGIN, t->ptd + T6000_PTD_SLEEP);
	t->ptd_sleep_set = true;
	return 0;
}

/* SEP answers its sleep request only with the GPIO domain on. */
static void t6000_before_enclave(struct apple_sleep *s)
{
	sleep_ps_raise(s, T6000_PS_GPIO);
	sleep_note(s, 0x29);
}

static int t6000_sleep_display_off(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int k, i;
	u32 val, stuck = 0;

	/* Before anything in .enter touches the hardware */
	t6000_wdt_arm(s, 20);

	/* As macOS: SEP gets up to 5 s to power-gate after its sleep request's answer. */
	if (apple_sleep_enclave.stop) {
		void __iomem *sep_ps = t->ps_east + T6000_PS_EAST_SEP;

		sleep_note(s, 0x2a);
		if (readl_poll_timeout_atomic(sep_ps, val, !FIELD_GET(PS_ACTUAL, val), 100,
					      5000000))
			t->incomplete |= T6000_INCOMPLETE_SEP;
		sleep_note(s, 0x2b);
	}

	/* Every PMGR domain as the drivers left it, before the steps below lower any */
	t6000_domains_save(false);

	/* The internal display's coprocessor, if its driver left it powered: its CPU off */
	if (s->display[0].asc &&
	    FIELD_GET(PS_ACTUAL, readl(t6000_ps_reg(s, t6000_disp_ps[2])))) {
		writel(readl(s->display[0].asc + ASC_CPU_CONTROL) & ~ASC_CPU_CONTROL_RUN,
		       s->display[0].asc + ASC_CPU_CONTROL);
		dsb(sy);
	}

	t6000_wdt_arm(s, 35);
	t6000_gfx_down(s);
	t6000_wdt_arm(s, 50);

	for (k = 0; k < ARRAY_SIZE(t6000_disp_ps); k++) {
		void __iomem *reg, *fe, *bridge;
		unsigned int disp;
		bool cpu;

		i = t6000_disp_order[k];
		disp = i / 2;
		cpu = !(i & 1);
		reg = t6000_ps_reg(s, t6000_disp_ps[i]);
		fe = t6000_ps_reg(s, t6000_disp_ps[i | 1]);
		bridge = t->disp_bridge[disp];

		/* dispext0 is brought up first: its front end if it is off, then its CPU */
		if (i == 0) {
			if (!FIELD_GET(PS_ACTUAL, readl(fe)))
				sleep_ps_raise_start(s, T6000_PS_DISPEXT0_FE);
			if (FIELD_GET(PS_ACTUAL, readl(reg)) != 0xf)
				sleep_ps_raise_start(s, T6000_PS_DISPEXT0_CPU0);
		}
		/* A front end that is off has no bridge to quiesce. */
		if (bridge && !FIELD_GET(PS_ACTUAL, readl(fe)))
			bridge = NULL;

		if (cpu) {
			t6000_ptd_request(t6000_disp_request[disp], false);
			writel(T6000_PS_WORD_MIN(false, 0xf, 4, 0, 0xf), reg);
			writel(T6000_PS_WORD_MIN(false, 0xf, 4, PS_FLAGS, 0), reg);
		} else {
			if (bridge)
				t6000_bridge_quiesce(bridge);
			writel(T6000_PS_WORD(false, 0, PS_FLAGS, 0), reg);
		}
		if (readl_poll_timeout_atomic(reg, val, !FIELD_GET(PS_ACTUAL, val), 1, 10000))
			stuck |= BIT(i);
	}

	if (stuck) {
		dev_err(s->dev, "T6000: display power words still on (mask %#x): refusing the sleep\n",
			stuck);
		t6000_domains_restore();
		t6000_wdt_restore(s);
		return -EBUSY;
	}
	t6000_wdt_arm(s, 80);
	return 0;
}

static int t6000_sleep_final_check(struct apple_sleep *s)
{
	t6000_wdt_arm(s, 90);
	return 0;
}

static void t6000_sleep_prepare_regs(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int i, n;
	u64 val;

	/* A hang in the PMP's or the SMC's last steps or in the quiesce resets the SoC. */
	t6000_wdt_arm(s, 100);
	sleep_note(s, 0x70);

	for (i = 0; i < ARRAY_SIZE(t6000_aon_commit_off); i++)
		t->aon_saved[i] = readl(s->ps_aon + t6000_aon_commit_off[i].offset);
	t->aon_saved_valid = true;

	/*
	 * Only a SEP that took its sleep request in this entry gives the sequencer the power
	 * gate it waits for. An idle, running SEP reads an actual state of 0 as well.
	 */
	t->sep_pg_saved = readl(t->sep_pg);
	t->sep_pg_cleared = !apple_sleep_enclave.stop && (t->sep_pg_saved & T6000_SEP_PG_WAIT);
	if (t->sep_pg_cleared)
		writel(t->sep_pg_saved & ~T6000_SEP_PG_WAIT, t->sep_pg);

	sleep_note(s, 0x71);
	/* The PMP's sleep tail, right before the coprocessors' last stop idles it */
	t->ptd_req_saved = readq(t->ptd + T6000_PTD_PS_REQ_RD);
	writeq(0, t->ptd + T6000_PTD_PS_REQ_WR);
	readq_poll_timeout_atomic(t->ptd + T6000_PTD_PS_ACK_RD, val, !(u32)val, 100, 200000);
	for (i = 0; i < 4; i++)
		t->dvfs_saved[i] = readq(t->ptd + T6000_PTD_DVFS_RD(i));
	/* As recorded: all four twice, the sleep word, then the first three once more */
	for (n = 0; n < 2; n++)
		for (i = 0; i < 4; i++)
			writeq(T6000_PTD_DVFS_IDLE, t->ptd + T6000_PTD_DVFS_WR(i));
	writel(T6000_PTD_SLEEP_COMMIT, t->ptd + T6000_PTD_SLEEP);
	for (i = 0; i < 3; i++)
		writeq(T6000_PTD_DVFS_IDLE, t->ptd + T6000_PTD_DVFS_WR(i));
	t->ptd_written = true;
	sleep_note(s, 0x72);
}

static void t6000_sleep_undo_regs(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;

	if (t->sep_pg_cleared) {
		writel(t->sep_pg_saved, t->sep_pg);
		t->sep_pg_cleared = false;
	}
}

/* A complex's target goes off only with every domain it powers reading off. */
static bool t6000_cluster_gate_ok(struct apple_sleep *s, unsigned int cluster)
{
	const struct t6000_ps_word *word;

	switch (cluster) {
	case T6000_CLUSTER_ANE0:
		word = t6000_gate_ane0;
		break;
	case T6000_CLUSTER_AFR:
		word = t6000_gate_afr;
		break;
	case T6000_CLUSTER_DISPLAY:
		word = t6000_gate_display;
		break;
	case T6000_CLUSTER_AVEMSR:
		word = t6000_gate_avemsr;
		break;
	default:
		return true;
	}

	for (; word->offset; word++)
		if (FIELD_GET(PS_ACTUAL, readl(t6000_ps_reg(s, *word))))
			return false;
	return true;
}

static bool t6000_dcs_set(u32 state)
{
	void __iomem *dcs = t6000_sleep.dcs;
	u32 val = readl(dcs);

	writel((val & ~T6000_DCS_PS_TARGET) | FIELD_PREP(T6000_DCS_PS_TARGET, state), dcs);
	return !readl_poll_timeout_atomic(dcs, val, !(val & T6000_DCS_PS_BUSY), 1, 8000);
}

/*
 * The fields of a dcs_* word that are compared with its first sleep's: the state it has
 * reached only while it isn't automatic in both.
 */
static u32 t6000_dcs_word_mask(u32 a, u32 b)
{
	u32 mask = PS_TARGET | PS_ACTUAL | PS_DEV_DISABLE | T6000_PS_MIN | PS_AUTO_ENABLE;

	if (a & b & PS_AUTO_ENABLE)
		mask &= ~PS_ACTUAL;
	return mask;
}

/*
 * The dcs_* words before the commit, as the reference has them: the wake waits for each to
 * reach 0. The first sleep clears a minimum state and keeps what each word reads; later
 * sleeps put a differing word back to that.
 */
static void t6000_dcs_words_commit(void)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int i;
	u32 val, ref;

	for (i = 0; i < t->ndomains; i++) {
		struct t6000_domain *d = &t6000_domains[i];

		if (strncmp(d->name, "dcs_", 4))
			continue;
		val = readl(d->reg);
		if (!t->dcs_ref_taken) {
			if (val & T6000_PS_MIN) {
				writel(val & ~(T6000_PS_MIN | T6000_PS_RESET | PS_FLAGS), d->reg);
				udelay(100);
				t->dcs_floors++;
				val = readl(d->reg);
			}
			d->commit_ref = val;
			continue;
		}
		ref = d->commit_ref;
		if (!((val ^ ref) & t6000_dcs_word_mask(val, ref)) || (ref & T6000_PS_MIN))
			continue;
		writel((val & ~(PS_TARGET | PS_AUTO_ENABLE | T6000_PS_MIN | T6000_PS_RESET |
				PS_FLAGS)) |
		       (ref & (PS_TARGET | PS_AUTO_ENABLE | T6000_PS_MIN)), d->reg);
		udelay(100);
		if (!t->dcs_changed++) {
			t->dcs_changed_first = d;
			t->dcs_changed_was = val;
		}
	}
	t->dcs_ref_taken = true;
}

/* A channel enabled for the commit; every word as it was, for the wake */
static void t6000_nco_enable(void)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int ch, w;

	if (!t->nco)
		return;
	for (ch = 0; ch < T6000_NCO_CHANNELS; ch++) {
		void __iomem *chan = t->nco + ch * T6000_NCO_STRIDE;

		for (w = 0; w < T6000_NCO_WORDS; w++)
			t->nco_saved[ch][w] = readl(chan + 4 * w);
		if (!(t->nco_saved[ch][T6000_NCO_CTRL] & T6000_NCO_CTRL_ENABLE))
			writel(t->nco_saved[ch][T6000_NCO_CTRL] | T6000_NCO_CTRL_ENABLE,
			       chan + T6000_NCO_CTRL);
	}
	t->nco_saved_valid = true;
}

/* The control word with the enable off, the other four words, then the control word */
static void t6000_nco_restore(void)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int ch, w;

	if (!t->nco_saved_valid)
		return;
	t->nco_saved_valid = false;
	for (ch = 0; ch < T6000_NCO_CHANNELS; ch++) {
		void __iomem *chan = t->nco + ch * T6000_NCO_STRIDE;

		writel(t->nco_saved[ch][T6000_NCO_CTRL] & ~T6000_NCO_CTRL_ENABLE,
		       chan + T6000_NCO_CTRL);
		for (w = 1; w < T6000_NCO_WORDS; w++)
			writel(t->nco_saved[ch][w], chan + 4 * w);
		writel(t->nco_saved[ch][T6000_NCO_CTRL], chan + T6000_NCO_CTRL);
	}
}

/*
 * Final PMGR, clock, fabric p-state and SIO commit programming immediately before the S2R
 * timer. Only domains and muxes whose state differs from the running system are written.
 */
static void t6000_commit_late_state(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int i;
	u32 val;

	writel(T6000_PS_WORD(true, 0xf, 0, 0xf), t->ps_east + T6000_PS_EAST_AFNC2_LW1);
	writel(T6000_PS_WORD(true, 0xf, 0, 0xf), t->ps_east + T6000_PS_EAST_SIO);
	udelay(2000);

	for (i = 0; i < ARRAY_SIZE(t6000_aon_commit_off); i++)
		writel(t6000_aon_commit_off[i].value,
		       s->ps_aon + t6000_aon_commit_off[i].offset);

	for (i = 0; i < ARRAY_SIZE(t6000_ps_commit_state); i++)
		writel(t6000_ps_commit_state[i].value,
		       t6000_block(s, t6000_ps_commit_state[i].block) +
		       t6000_ps_commit_state[i].offset);

	for (i = 0; i < ARRAY_SIZE(t6000_clkgen_late_mux); i++) {
		void __iomem *reg = t->clkgen + t6000_clkgen_late_mux[i];

		val = readl(reg);
		writel((val & ~T6000_CLKGEN_MUX_SEL) |
		       FIELD_PREP(T6000_CLKGEN_MUX_SEL, T6000_CLKGEN_LATE_SEL), reg);
		readl_poll_timeout_atomic(reg, val, !(val & T6000_CLKGEN_MUX_BUSY), 1, 8000);
	}

	if (t->fabric_ps) {
		writel(SOC_SLEEP_STATE, t->fabric_ps);
		sleep_perf_wait(t->fabric_ps, SOC_SLEEP_STATE);
	}
	udelay(2000);

	writel(T6000_PS_WORD(false, 0xf, 0, 0xf), t->ps_east + T6000_PS_EAST_SIO_CPU);
	if (!readl_poll_timeout_atomic(t->ps_east + T6000_PS_EAST_SIO_CPU, val,
				       FIELD_GET(PS_ACTUAL, val) == 0xf, 1, 2000)) {
		for (i = 0; i < ARRAY_SIZE(t6000_sio_stop_pa); i++)
			if (t->sio_stop[i])
				writel(0, t->sio_stop[i]);
	}

	writel(0, s->ps + T6000_PS_PCIE0_REF);
	writel(0, s->ps + T6000_PS_APCIE_GP_SYS);

	for (i = 0; i < ARRAY_SIZE(t6000_clkgen_off_mux); i++)
		writel(0, t->clkgen + t6000_clkgen_off_mux[i]);
	udelay(100);
}

static void t6000_sleep_commit_regs(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int a, i, r, n;
	u32 val;
	u64 qval;

	sleep_note(s, 0x73);

	/* The halted PMP and its SRAM down, so the memory clock and the sequencer can follow */
	sleep_ps_lower(s, T6000_PS_PMP);
	if (sleep_ps_wait_off(s, T6000_PS_PMP))
		t->incomplete |= T6000_INCOMPLETE_PMP;
	sleep_ps_lower(s, T6000_PS_PMS_SRAM);
	if (sleep_ps_wait_off(s, T6000_PS_PMS_SRAM))
		t->incomplete |= T6000_INCOMPLETE_PMS_SRAM;

	/* 1. The memory controllers: enable DCS low-power gating, then drain */
	t->mcc_done = true;
	for (a = 0; a < T6000_AMCCS; a++)
		for (i = 0; i < T6000_DCS_PER_AMCC; i++)
			if (t->dcs_block[a][i]) {
				t->dcs_ctl_saved[a][i] =
					readl(t->dcs_block[a][i] + T6000_DCS_CTL);
				writel(t->dcs_ctl_saved[a][i] | T6000_DCS_CTL_LP,
				       t->dcs_block[a][i] + T6000_DCS_CTL);
			}
	for (a = 0; a < T6000_AMCCS; a++)
		writel(1, t->amcc[a] + T6000_AMCC_LOCK);
	for (a = 0; a < T6000_AMCCS; a++)
		writel(0, t->amcc[a] + T6000_AMCC_DRAIN_CTL);
	for (a = 0; a < T6000_AMCCS; a++)
		writel(0, t->amcc[a] + T6000_AMCC_LOCK);
	for (a = 0; a < T6000_AMCCS; a++)
		readl_poll_timeout_atomic(t->amcc[a] + T6000_AMCC_DRAIN_COUNT, val, !val,
					  1, 20000);

	/* 2. The error handlers, counters and voltage monitors */
	t->zeros_done = true;
	for (i = 0; i < ARRAY_SIZE(t6000_quiesce_zero_pa); i++) {
		if (!t->zero[i])
			continue;
		t->zero_saved[i] = readl(t->zero[i]);
		writel(0, t->zero[i]);
	}

	sleep_note(s, 0x74);
	/* 3. The PCIe bridge */
	t->apcie_done = true;
	t->apcie_saved[0] = readl(t->apcie_q + T6000_BRIDGE_8014);
	t->apcie_saved[1] = readl(t->apcie_q + T6000_BRIDGE_82BC);
	t6000_bridge_quiesce(t->apcie_q);

	sleep_note(s, 0x75);
	/* 4. The CPU complexes: counters and throttlers off, the PLL relock, APSC */
	t->acc_done = true;
	for (i = 0; i < T6000_ACCS; i++) {
		for (r = 0; r < T6000_ACCR_DVFS; r++) {
			void __iomem *reg = t->acc[i] + t6000_acc_regs[r];

			t->acc_saved[i][r] = readq(reg);
			writeq(t->acc_saved[i][r] & ~t6000_acc_enable[r], reg);
		}
	}
	t->relock_done = true;
	for (i = 0; i < 2; i++) {
		void __iomem *reg = t->acc[i] + T6000_ACC_DVFS_CMD;

		t->relock_saved[i] = readq(reg);
		writeq(T6000_ACC_DVFS_RELOCK, reg);
		readq_poll_timeout_atomic(reg, qval, !(qval & T6000_ACC_DVFS_BUSY), 1, 8000);
	}
	for (i = 0; i < T6000_ACCS; i++) {
		void __iomem *reg = t->acc[i] + T6000_ACC_DVFS_CMD;

		t->acc_saved[i][T6000_ACCR_DVFS] = readq(reg);
		for (n = 0; n < 2; n++) {
			writeq(T6000_ACC_DVFS_APSC, reg);
			readq_poll_timeout_atomic(reg, qval, !(qval & T6000_ACC_DVFS_BUSY),
						  1, 8000);
		}
	}

	sleep_note(s, 0x76);
	/* 5. The memory clock's state down, only with the PMP off, and AFR */
	t->dcs_done = false;
	if (!FIELD_GET(PS_ACTUAL, readl(s->ps + T6000_PS_PMP))) {
		t->dcs_saved = readl(t->dcs);
		t->dcs_done = true;
		if (!t6000_dcs_set(T6000_DCS_PS_STEP))
			t->incomplete |= T6000_INCOMPLETE_DCS_BUSY;
		if (!t6000_dcs_set(T6000_DCS_PS_SLEEP))
			t->incomplete |= T6000_INCOMPLETE_DCS_BUSY;
	} else {
		t->incomplete |= T6000_INCOMPLETE_DCS_KEPT;
	}
	t->afr_saved = readl(s->ps + T6000_PS_AFR);
	t->afr_done = true;
	writel(0, s->ps + T6000_PS_AFR);
	readl_poll_timeout_atomic(s->ps + T6000_PS_AFR, val, !(val & PS_ACTUAL), 1, 8000);

	sleep_note(s, 0x77);
	/* 6. The complexes' targets, all but the E cluster's, which goes last */
	t->cluster_written = 0;
	for (i = 1; i < T6000_CLUSTERS; i++) {
		void __iomem *reg = s->cluster + 4 * i;

		if (!(T6000_CLUSTERS_VALID & BIT(i)))
			continue;
		if (!t6000_cluster_gate_ok(s, i)) {
			t->clusters_kept |= BIT(i);
			continue;
		}
		t->cluster_saved[i] = readl(reg);
		writel(t->cluster_saved[i] & ~T6000_CLUSTER_PS_TARGET, reg);
		t->cluster_written |= BIT(i);
		if (!(T6000_CLUSTERS_CPU & BIT(i)))
			readl_poll_timeout_atomic(reg, val, !(val & T6000_CLUSTER_PS_BUSY),
						  1, 10000);
	}

	sleep_note(s, 0x78);
	/*
	 * 7. The SoC clock muxes onto the reference clock, then PLL0 off, the three writes
	 * twice over as recorded. A mux that stays busy leaves PLL0 on.
	 */
	t->mux_n = 0;
	for (i = 0; i < ARRAY_SIZE(t6000_clkgen_mux); i++) {
		void __iomem *reg = t->clkgen + t6000_clkgen_mux[i];

		val = readl(reg);
		t->mux_saved[i] = val;
		writel((val & ~T6000_CLKGEN_MUX_SEL) | T6000_CLKGEN_MUX_ON, reg);
		t->mux_n = i + 1;
		if (readl_poll_timeout_atomic(reg, val, !(val & T6000_CLKGEN_MUX_BUSY), 1, 8000)) {
			t->incomplete |= T6000_INCOMPLETE_PLL_KEPT;
			goto skip_pll;
		}
	}
	t->pll_saved = readl(t->pll0);
	t->pll_done = true;
	for (n = 0; n < 2; n++) {
		val = readl(t->pll0);
		writel((val & ~T6000_PLL_OFF_MODE) | FIELD_PREP(T6000_PLL_OFF_MODE, 3), t->pll0);
		readl_poll_timeout_atomic(t->pll0, val, !(val & T6000_PLL_BUSY), 1, 8000);
		val = readl(t->pll0);
		writel(val | T6000_PLL_OFF_OK, t->pll0);
		readl_poll_timeout_atomic(t->pll0, val, !(val & T6000_PLL_BUSY), 1, 8000);
		val = readl(t->pll0);
		writel(val & ~T6000_PLL_ON, t->pll0);
		readl_poll_timeout_atomic(t->pll0, val, !(val & T6000_PLL_BUSY), 1, 8000);
	}
skip_pll:

	sleep_note(s, 0x79);
	/* 8. The other cores stopped, as macOS does it here, then the E cluster's target */
	s->cpus_stop_ret = spin_table_stop_parked();
	t->cluster_saved[T6000_CLUSTER_EACC] = readl(s->cluster);
	writel(t->cluster_saved[T6000_CLUSTER_EACC] & ~T6000_CLUSTER_PS_TARGET, s->cluster);
	t->cluster_written |= BIT(T6000_CLUSTER_EACC);
	dsb(sy);

	/* 9. The dcs_* domains' words as the first sleep had them, and NCO channels enabled */
	t6000_dcs_words_commit();
	t6000_nco_enable();

	sleep_note(s, 0x7a);
	t6000_wdt_arm(s, 125);

	/* 10. Final PMGR, clock, fabric p-state and SIO state for the S2R sequencer */
	t6000_commit_late_state(s);
	dsb(sy);
	mdelay(T6000_LATE_SETTLE_MS);

	/* 11. The PMU RTC alarm's SPMI vector: its latched status cleared, then enabled */
	for (i = 0; i < T6000_SPMI_VEC_WORDS; i++)
		t->spmi_saved[i] = readl(s->spmi + T6000_SPMI_VEC_ENABLE + 4 * i);
	writel(T6000_SPMI_VEC_RTC, s->spmi + T6000_SPMI_VEC_ACK);
	writel(t->spmi_saved[0] | T6000_SPMI_VEC_RTC, s->spmi + T6000_SPMI_VEC_ENABLE);
	t->spmi_armed = true;

	/* 12. Every interrupt line masked here, so the timer's is the last write */
	t6000_aic_mask_all();
	t6000_wdt_arm(s, 150);
}

static void t6000_sleep_wake(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int a, i, j, r;
	u32 val;
	u64 qval;

	sleep_note(s, 0x7b);
	/* For the rest of the wake, until .finish puts WD1 back */
	t6000_wdt_arm(s, T6000_WDT_WAKE_SECS);

	if (t->sep_pg_cleared) {
		writel(t->sep_pg_saved, t->sep_pg);
		t->sep_pg_cleared = false;
	}

	if (t->spmi_armed) {
		for (i = 0; i < T6000_SPMI_VEC_WORDS; i++)
			writel(t->spmi_saved[i], s->spmi + T6000_SPMI_VEC_ENABLE + 4 * i);
		t->spmi_armed = false;
	}

	/* The DCS channels' control as before the commit */
	if (t->mcc_done) {
		for (a = 0; a < T6000_AMCCS; a++)
			for (i = 0; i < T6000_DCS_PER_AMCC; i++)
				if (t->dcs_block[a][i])
					writel(t->dcs_ctl_saved[a][i],
					       t->dcs_block[a][i] + T6000_DCS_CTL);
		t->mcc_done = false;
	}

	/* PLL0 on, then the SoC clock muxes back, last first */
	if (t->pll_done) {
		val = readl(t->pll0);
		writel(val | T6000_PLL_ON, t->pll0);
		readl_poll_timeout_atomic(t->pll0, val, !(val & T6000_PLL_BUSY), 1, 8000);
		writel(t->pll_saved, t->pll0);
		readl_poll_timeout_atomic(t->pll0, val, !(val & T6000_PLL_BUSY), 1, 8000);
		t->pll_done = false;
	}
	while (t->mux_n) {
		void __iomem *reg = t->clkgen + t6000_clkgen_mux[--t->mux_n];

		writel(t->mux_saved[t->mux_n], reg);
		readl_poll_timeout_atomic(reg, val, !(val & T6000_CLKGEN_MUX_BUSY), 1, 8000);
	}

	/*
	 * The complexes' targets. AFR's was already 0 when the commit read it, after the GPU's
	 * way down: it goes back to what it was before that.
	 */
	for (i = 0; i < T6000_CLUSTERS; i++) {
		void __iomem *reg = s->cluster + 4 * i;

		if (!(t->cluster_written & BIT(i)))
			continue;
		if (i == T6000_CLUSTER_AFR && t->afr_cluster_valid &&
		    !(t->cluster_saved[i] & T6000_CLUSTER_PS_TARGET)) {
			if ((readl(reg) ^ t->afr_cluster_before) & T6000_CLUSTER_PS_TARGET)
				writel(t->afr_cluster_before, reg);
		} else {
			writel(t->cluster_saved[i], reg);
		}
		readl_poll_timeout_atomic(reg, val, !(val & T6000_CLUSTER_PS_BUSY), 1, 10000);
	}
	t->cluster_written = 0;

	if (t->afr_done) {
		writel(t->afr_saved, s->ps + T6000_PS_AFR);
		readl_poll_timeout_atomic(s->ps + T6000_PS_AFR, val,
					  FIELD_GET(PS_ACTUAL, val) == FIELD_GET(PS_TARGET, val),
					  1, 8000);
		t->afr_done = false;
	}
	if (t->dcs_done) {
		t6000_dcs_set(T6000_DCS_PS_STEP);
		t6000_dcs_set(FIELD_GET(T6000_DCS_PS_TARGET, t->dcs_saved));
		t->dcs_done = false;
	}

	/* The CPU complexes: APSC as it was, then the other words, last first */
	if (t->acc_done) {
		for (i = 0; i < T6000_ACCS; i++) {
			for (r = T6000_ACCR_N; r-- > 0;) {
				void __iomem *reg = t->acc[i] + t6000_acc_regs[r];

				if (r == T6000_ACCR_DVFS) {
					qval = readq(reg) & ~T6000_ACC_DVFS_APSC_OFF;
					writeq(qval | (t->acc_saved[i][r] &
						       T6000_ACC_DVFS_APSC_OFF), reg);
					readq_poll_timeout_atomic(reg, qval,
								  !(qval & T6000_ACC_DVFS_BUSY),
								  1, 8000);
				} else {
					writeq(t->acc_saved[i][r], reg);
				}
			}
		}
		t->acc_done = false;
	}
	if (t->relock_done) {
		for (i = 2; i-- > 0;) {
			void __iomem *reg = t->acc[i] + T6000_ACC_DVFS_CMD;

			writeq(t->relock_saved[i], reg);
			readq_poll_timeout_atomic(reg, qval, !(qval & T6000_ACC_DVFS_BUSY),
						  1, 8000);
		}
		t->relock_done = false;
	}

	if (t->zeros_done) {
		for (i = ARRAY_SIZE(t6000_quiesce_zero_pa); i-- > 0;)
			if (t->zero[i])
				writel(t->zero_saved[i], t->zero[i]);
		t->zeros_done = false;
	}

	/* The always-on power words the commit lowered: they kept that through the sleep */
	if (t->aon_saved_valid) {
		for (i = ARRAY_SIZE(t6000_aon_commit_off); i-- > 0;) {
			void __iomem *reg = s->ps_aon + t6000_aon_commit_off[i].offset;
			u32 saved = t->aon_saved[i];

			writel(saved & ~(PS_FLAGS | PS_ACTUAL), reg);
			readl_poll_timeout_atomic(reg, val,
						  FIELD_GET(PS_ACTUAL, val) ==
						  FIELD_GET(PS_TARGET, saved), 1, 5000);
		}
		t->aon_saved_valid = false;
	}

	/* Every other PMGR domain that was on, back on */
	t6000_domains_restore();

	/* The PMP's address filter and reset vector, before its driver restarts it */
	for (i = 0; t->pmp_dapf && i < t->pmp_dapf_n; i++) {
		void __iomem *entry = t->pmp_dapf + i * T6000_PMP_DAPF_STRIDE;

		for (j = 1; j < T6000_PMP_DAPF_WORDS; j++)
			writel(t->pmp_dapf_saved[i][j], entry + 4 * j);
		writel(t->pmp_dapf_saved[i][0], entry);
	}
	if (t->pmp_iorvbar_valid &&
	    !(readq(t->pmp_iorvbar) & T6000_PMP_IORVBAR_VALID))
		writeq(t->pmp_iorvbar_saved, t->pmp_iorvbar);

	t6000_nco_restore();

	/* SEP's domain up before its DART resumes: automatic state and target 0xf */
	if (apple_sleep_enclave.stop) {
		void __iomem *sep_ps = t->ps_east + T6000_PS_EAST_SEP;

		val = readl(sep_ps);
		writel((val & ~(T6000_PS_AUTO | PS_DEV_DISABLE | PS_FLAGS)) |
		       FIELD_PREP(T6000_PS_AUTO, 0xf) | T6000_PS_WAS_CLKGATED | PS_TARGET, sep_ps);
		readl_poll_timeout_atomic(sep_ps, val, FIELD_GET(PS_ACTUAL, val) == 0xf,
					  10, 50000);
	}

	/* SIO, then GPIO, on before the AP pin state is put back */
	val = readl(t->ps_east + T6000_PS_EAST_SIO);
	writel((val & ~GENMASK(11, 0)) | PS_TARGET, t->ps_east + T6000_PS_EAST_SIO);
	readl_poll_timeout_atomic(t->ps_east + T6000_PS_EAST_SIO, val,
				  FIELD_GET(PS_ACTUAL, val) == 0xf, 10, 50000);
	sleep_ps_raise(s, T6000_PS_GPIO);
	readl_poll_timeout_atomic(s->ps + T6000_PS_GPIO, val, FIELD_GET(PS_ACTUAL, val) == 0xf,
				  10, 50000);

	/*
	 * The sleep resets every AP pin with the gpio domain, and the pin controller's driver
	 * restores only pins it configured: put all of them back now, before drivers resume.
	 */
	s->pins_lost = s->pins_saved_valid;
	apple_sleep_restore_pins();

	sleep_note(s, 0x7d);
}

/* One line for the steps of the entry that were left out or did not finish, if any */
static void t6000_report_incomplete(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	char steps[192] = "";
	unsigned int i, len = 0;

	if (!t->incomplete && !t->clusters_kept)
		return;
	for (i = 0; i < ARRAY_SIZE(t6000_incomplete_names); i++)
		if (t->incomplete & BIT(i))
			len += scnprintf(steps + len, sizeof(steps) - len, "%s%s",
					 len ? ", " : "", t6000_incomplete_names[i]);
	if (t->clusters_kept)
		scnprintf(steps + len, sizeof(steps) - len, "%scomplexes %#x left on",
			  len ? ", " : "", t->clusters_kept);
	dev_warn(s->dev, "T6000: sleep entry incomplete: %s\n", steps);
	t->incomplete = 0;
	t->clusters_kept = 0;
}

/* At .wake and again at .end: what the entry changed in the PMP and the watchdog, once */
static void t6000_sleep_finish(struct apple_sleep *s)
{
	struct t6000_sleep *t = &t6000_sleep;
	unsigned int i;

	if (t->ptd_sleep_set) {
		writel(t->ptd_sleep_saved, t->ptd + T6000_PTD_SLEEP);
		t->ptd_sleep_set = false;
	}

	/* The devices' requests and the fabric DVFS words, with the PMP running again */
	if (t->ptd_written) {
		u64 cur = readq(t->ptd + T6000_PTD_PS_REQ_RD);

		if (t->ptd_req_saved & ~cur)
			writeq(cur | t->ptd_req_saved, t->ptd + T6000_PTD_PS_REQ_WR);
		for (i = 0; i < 4; i++)
			writeq(t->dvfs_saved[i], t->ptd + T6000_PTD_DVFS_WR(i));
		mdelay(10);
		t->ptd_written = false;
	}

	t6000_wdt_restore(s);

	t6000_report_incomplete(s);
	if (t->dcs_floors) {
		dev_dbg(s->dev, "T6000: minimum state cleared in %u dcs power words at the first sleep\n",
			t->dcs_floors);
		t->dcs_floors = 0;
	}
	if (t->dcs_changed) {
		dev_info(s->dev, "T6000: %u dcs power words put back to their first sleep's state, %s from %#010x to %#010x\n",
			 t->dcs_changed, t->dcs_changed_first->name, t->dcs_changed_was,
			 readl(t->dcs_changed_first->reg));
		t->dcs_changed = 0;
	}
}

/*
 * Nothing per core before its stop or after its start: on the deep sleep path the reference
 * leaves each core's interrupt routing as it is.
 */
static int t6000_sleep_cpu_prepare(u64 mpidr)
{
	return 0;
}

static void t6000_sleep_cpu_restore(u64 mpidr)
{
}

static const struct apple_sleep_ops t6000_sleep_ops = {
	.deep_wfi_arm  = t6000_deep_wfi_arm,
	.resume        = cpu_resume,
	.aic_mask_all  = t6000_aic_mask_all,
	.aic_ack       = aic_sleep_ack,
	.aic_restore   = t6000_aic_restore,
	.map_regs      = t6000_sleep_map_regs,
	.begin         = t6000_sleep_begin,
	.before_enclave = t6000_before_enclave,
	.display_off   = t6000_sleep_display_off,
	.final_check   = t6000_sleep_final_check,
	.prepare_regs  = t6000_sleep_prepare_regs,
	.undo_regs     = t6000_sleep_undo_regs,
	.commit_regs   = t6000_sleep_commit_regs,
	.wake          = t6000_sleep_wake,
	.finish        = t6000_sleep_finish,
	.cpu_prepare   = t6000_sleep_cpu_prepare,
	.cpu_restore   = t6000_sleep_cpu_restore,
};

static const struct apple_sleep_soc t6000_sleep_soc = {
	.ops = &t6000_sleep_ops,
	.early_context = true,
	/* The safety net is WD1, armed by the operations themselves. */
	.no_wd2 = true,
	.no_pmu_notes = true,
	.smc_gen1_keys = true,
	.enclave_first = true,
	.commit_stops_cpus = true,
	.pmu_usid = 0xf,
	.ndisp = 1,
	/* MPIDR affinity 1 is the cluster: E, P0, P1; start bits 4 * cluster + core */
	.cpu_cluster_affinity = 1,
	.cpu_clusters = 3,
	.cpu_cores_per_cluster = 4,
	.cpu_start_stride = 4,
	.cluster_cpu = { 4 * T6000_CLUSTER_EACC, 4 * T6000_CLUSTER_PACC0, 4 * T6000_CLUSTER_PACC1 },
	.ncluster_cpu = 3,
	.wake_mask = T6000_WAKE_MASK,
	.wake_status = T6000_WAKE_STATUS,
	.wake_words = T6000_WAKE_WORDS,
	.wake_last_word = T6000_WAKE_LAST_WORD,
	.wake_names = t6000_wake_names,
	.nwake_names = ARRAY_SIZE(t6000_wake_names),
	.ps = {
		.gpio = T6000_PS_GPIO,
	},
};
