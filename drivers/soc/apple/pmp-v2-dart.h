/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Native ownership of the T8140 PMP DART translation instance (TRAD) and its
 * address filter (FPAD) by the stopped-epoch PMP owner. Included by pmp-v2.c.
 *
 * This is the qualified J700 stopped-PMP SID0 profile of the cleanroom tree
 * (drivers/iommu/apple-dart-pmp.h and apple-dart-pmp-apf.h there), kept as a
 * private translation instance of the PMP owner instead of a profile inside
 * the shared apple-dart driver: the J700 tree's apple-dart.c carries the DCP
 * display handoff and is not modified for the PMP. The semantics are the
 * same: admission is read-only and never adopts an inherited valid SID0 root,
 * every stream other than SID0 keeps the state iBoot left, only the 26 SID0
 * APF rows are ever written (after the own-ADT table hash is verified), the
 * SID0 root is a Linux four-level DART2 table with the ADT DVA window, and any
 * fault or unexpected register change latches the epoch failed so that all
 * published DMA is retained until platform reset.
 *
 * Register layout: T8110 DART as documented in drivers/iommu/apple-dart.c.
 */
#include <crypto/sha2.h>
#include <linux/hex.h>
#include <linux/interrupt.h>
#include <linux/io-pgtable.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>

#define PMP_DART_PARAMS1		0x00
#define PMP_DART_PARAMS1_PAGE_SHIFT	GENMASK(27, 24)
#define PMP_DART_PARAMS3		0x08
#define PMP_DART_PARAMS3_PA_WIDTH	GENMASK(29, 24)
#define PMP_DART_PARAMS3_VA_WIDTH	GENMASK(21, 16)
#define PMP_DART_PARAMS3_VERSION	GENMASK(15, 0)
#define PMP_DART_PARAMS4		0x0c
#define PMP_DART_PARAMS4_NUM_SIDS	GENMASK(8, 0)
#define PMP_DART_TLB_CMD		0x80
#define PMP_DART_TLB_CMD_BUSY		BIT(31)
#define PMP_DART_TLB_CMD_V2		BIT(15)
#define PMP_DART_TLB_CMD_OP		GENMASK(10, 8)
#define PMP_DART_TLB_CMD_OP_FLUSH_SID	1
#define PMP_DART_TLB_CMD_STREAM		GENMASK(7, 0)
#define PMP_DART_ERROR			0x100
#define PMP_DART_ERROR_MASK		0x104
#define PMP_DART_ERROR_ADDR_LO		0x170
#define PMP_DART_ERROR_ADDR_HI		0x174
#define PMP_DART_ERROR_STREAMS		0x1c0
#define PMP_DART_PROTECT		0x200
#define PMP_DART_PROTECT_LOCK		0x208
#define PMP_DART_PROTECT_TTBR_TCR	BIT(0)
#define PMP_DART_BLOCK_ERROR		0x704
#define PMP_DART_ENABLE_STREAMS		0xc00
#define PMP_DART_TCR(sid)		(0x1000 + 4 * (sid))
#define PMP_DART_TCR_FOUR_LEVEL		BIT(3)
#define PMP_DART_TCR_TRANSLATE_ENABLE	BIT(0)
#define PMP_DART_TTBR(sid)		(0x1400 + 4 * (sid))
#define PMP_DART_TTBR_VALID		BIT(0)
#define PMP_DART_EXCEPTION(i)		(0x4000 + 4 * (i))
#define PMP_DART_ERR_SID(sid)		(0x8000 + 0x40 * (sid))
#define PMP_DART_COMMAND_BUSY_US	100
#define PMP_DART_FLUSH_BUSY_US		2000

#define PMP_DART_STREAMS		16
#define PMP_DART_VERSION		0x0203
#define PMP_DART_DVA_START		0x10000000000ULL
#define PMP_DART_DVA_END		0x100bfefffffULL

/* Own-ADT dart-pmp/dapf-instance-0 and dart-tunables-instance-0 records. */
#define PMP_APF_ROWS 27
#define PMP_APF_CAPACITY 32
#define PMP_APF_FIELDS 6

struct apple_pmp_apf {
	void __iomem *regs;
	u32 values[PMP_APF_ROWS][PMP_APF_FIELDS];
	u8 source_byte33[PMP_APF_ROWS];
	bool decoded, prepared;
};

static const struct {
	u32 offset, mask, value;
} apple_pmp_tunables[] = {
	{ 0x20c, 0x80000521, 0x80000521 },
	{ 0x220, 0x000f0f0f, 0x000f0f0f },
	{ 0x224, 0x00ffffff, 0x00080808 },
	{ 0x300, 0x00001f31, 0x00000001 },
	{ 0x308, 0x3ffffffc, 0x10000000 },
	{ 0x310, 0x3ffffffc, 0x3ffffffc },
	{ 0x318, 0x00001f31, 0x00000001 },
	{ 0x320, 0x3ffffffc, 0x0ff00000 },
	{ 0x328, 0x3ffffffc, 0x0ff017fc },
};

static const u32 apple_pmp_apf_offsets[] = { 8, 12, 16, 20, 32, 0 };
static const u32 apple_pmp_apf_masks[] = {
	0xfffffffc, 0xf, 0xfffffffc, 0xf, 0xffff, 0x333,
};

static bool apple_pmp_data_hash(const void *raw, size_t size, const char *expected)
{
	u8 hash[SHA256_DIGEST_SIZE];
	char hex[2 * SHA256_DIGEST_SIZE];

	sha256(raw, size, hash);
	bin2hex(hex, hash, sizeof(hash));
	return !memcmp(hex, expected, sizeof(hex));
}

static int apple_pmp_apf_decode(struct apple_pmp_apf *apf, const void *raw,
			       size_t size, const void *tunables, size_t tunable_size)
{
	const u8 *data = raw, *t = tunables;
	unsigned int row, i;

	apf->decoded = false;
	if (!data || size != PMP_APF_ROWS * 52 || !t || tunable_size != 9 * 24 ||
	    !apple_pmp_data_hash(data, size,
		"febc4d2910ffca684887378e5c06b30db4407d0e678a33232c9298aa300df571") ||
	    !apple_pmp_data_hash(t, tunable_size,
		"f0b7dc1c1fa9b485029beac01e56e1b23062990481f86c60e0192e09161dbf50"))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(apple_pmp_tunables); i++, t += 24)
		if (get_unaligned_le32(t) != apple_pmp_tunables[i].offset ||
		    get_unaligned_le32(t + 4) != 4 ||
		    get_unaligned_le64(t + 8) != apple_pmp_tunables[i].mask ||
		    get_unaligned_le64(t + 16) != apple_pmp_tunables[i].value)
			return -EINVAL;
	for (row = 0; row < PMP_APF_ROWS; row++, data += 52) {
		u64 start = get_unaligned_le64(data), end = get_unaligned_le64(data + 8);
		u32 sid = get_unaligned_le32(data + 16);
		u32 *value = apf->values[row];

		if (start > end || end >= BIT_ULL(36) || sid != (row == 7 ? BIT(2) : BIT(0)))
			return -EINVAL;
		for (i = 1; i < 8; i++)
			if (get_unaligned_le32(data + 16 + 4 * i))
				return -EINVAL;
		value[0] = lower_32_bits(start);
		value[1] = upper_32_bits(start);
		value[2] = lower_32_bits(end);
		value[3] = upper_32_bits(end);
		value[4] = sid;
		value[5] = ((data[48] & 3) << 8) | ((data[49] & 3) << 4) | (data[50] & 3);
		apf->source_byte33[row] = data[51];
	}
	apf->decoded = true;
	return 0;
}

static int apple_pmp_apf_verify(struct apple_pmp_apf *apf)
{
	unsigned int row, field;

	if (!apf->decoded || !apf->regs)
		return -EINVAL;
	for (row = 0; row < PMP_APF_ROWS; row++) {
		if (row == 7)
			continue;
		for (field = 0; field < PMP_APF_FIELDS; field++)
			if ((readl(apf->regs + 64 * row + apple_pmp_apf_offsets[field]) &
			     apple_pmp_apf_masks[field]) !=
			    (apf->values[row][field] & apple_pmp_apf_masks[field]))
				return -EIO;
	}
	return 0;
}

/* Caller serializes a boot-epoch latch across deferred probe and failures. */
static int apple_pmp_apf_initialize(struct apple_pmp_apf *apf, void __iomem *trad,
				    int (*admit)(void *), void *cookie, bool verify_only)
{
	u32 before[PMP_APF_CAPACITY][PMP_APF_FIELDS];
	u32 tunables[ARRAY_SIZE(apple_pmp_tunables)];
	unsigned int row, field, i;
	int ret;

	apf->prepared = false;
	if (!apf->decoded || !apf->regs)
		return -EINVAL;
	ret = admit(cookie);
	if (ret)
		return ret;
	if (readl(trad + 0x200) != 2 || readl(trad + 0x208) != 2)
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(apple_pmp_tunables); i++) {
		tunables[i] = readl(trad + apple_pmp_tunables[i].offset);
		if ((tunables[i] & apple_pmp_tunables[i].mask) != apple_pmp_tunables[i].value)
			return -EINVAL; /* Never write TRAD, even a nominal no-op. */
	}
	for (row = 0; row < PMP_APF_CAPACITY; row++) {
		for (field = 0; field < PMP_APF_FIELDS; field++)
			before[row][field] = readl(apf->regs + row * 64 + apple_pmp_apf_offsets[field]);
		if (!verify_only && row < PMP_APF_ROWS && (before[row][5] & 3))
			return -EBUSY;
	}
	/* Resolve all comparisons and admission before the first possible store. */
	ret = admit(cookie);
	if (ret)
		return ret;
	if (readl(trad + 0x200) != 2 || readl(trad + 0x208) != 2)
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(apple_pmp_tunables); i++)
		if (readl(trad + apple_pmp_tunables[i].offset) != tunables[i])
			return -EIO;
	if (!verify_only) {
		for (row = 0; row < PMP_APF_ROWS; row++) {
			if (row == 7)
				continue;
			for (field = 0; field < PMP_APF_FIELDS; field++)
				writel(apf->values[row][field],
				       apf->regs + row * 64 + apple_pmp_apf_offsets[field]);
			/* Complete this control-last slot before reading its fields back. */
			mb();
			for (field = 0; field < PMP_APF_FIELDS; field++)
				if ((readl(apf->regs + row * 64 + apple_pmp_apf_offsets[field]) &
				     apple_pmp_apf_masks[field]) !=
				    (apf->values[row][field] & apple_pmp_apf_masks[field]))
					return -EIO;
		}
		/* No DMA root or firmware start may precede completed APF writes. */
		mb();
	}
	ret = apple_pmp_apf_verify(apf);
	if (ret)
		return ret;
	for (row = 0; row < PMP_APF_CAPACITY; row++) {
		if (row != 7 && row < PMP_APF_ROWS)
			continue;
		for (field = 0; field < PMP_APF_FIELDS; field++)
			if (readl(apf->regs + row * 64 + apple_pmp_apf_offsets[field]) !=
			    before[row][field])
				return -EIO;
	}
	for (i = 0; i < ARRAY_SIZE(apple_pmp_tunables); i++)
		if (readl(trad + apple_pmp_tunables[i].offset) != tunables[i])
			return -EIO;
	if (readl(trad + 0x200) != 2 || readl(trad + 0x208) != 2)
		return -EIO;
	ret = admit(cookie);
	if (!ret)
		apf->prepared = true;
	return ret;
}

struct pmp_v2_dart {
	struct device *dev;
	void __iomem *regs, *gate_status;
	struct apple_pmp_apf apf;
	struct io_pgtable_ops *pgtbl_ops;
	struct io_pgtable_cfg pgtbl_cfg;
	spinlock_t lock;
	int irq;
	u32 pgsize, ias, oas, version, num_streams;
	u32 tcr[PMP_DART_STREAMS], ttbr[PMP_DART_STREAMS];
	u32 enable, protect, protect_lock, error_mask;
	u32 installed_root;
	u64 dva_next;
	bool admitted, failed, epoch_pinned;
};

/* Set by the owner: read-only views of the CPU control word and mailbox. */
struct pmp_v2;
static bool pmp_v2_dart_owner_running(struct pmp_v2 *pmp);
static bool pmp_v2_dart_owner_mailbox_idle(struct pmp_v2 *pmp);
static struct pmp_v2_dart *pmp_v2_dart_of(struct pmp_v2 *pmp);

/* Qualified read-only v2.3 TRAD diagnostics, not a fault-clear protocol. */
static void pmp_v2_dart_report_fault(struct pmp_v2_dart *dart)
{
	u32 streams = readl(dart->regs + PMP_DART_EXCEPTION(0));
	unsigned int sid;

	dev_err(dart->dev, "PMP DART fault: legacy status=%08x streams=%08x address=%08x:%08x block=%08x pending=%08x (no clears; epoch failed)\n",
		readl(dart->regs + PMP_DART_ERROR),
		readl(dart->regs + PMP_DART_ERROR_STREAMS),
		readl(dart->regs + PMP_DART_ERROR_ADDR_HI),
		readl(dart->regs + PMP_DART_ERROR_ADDR_LO),
		readl(dart->regs + PMP_DART_BLOCK_ERROR), streams);
	for (sid = 0; sid < PMP_DART_STREAMS; sid++) {
		void __iomem *record = dart->regs + PMP_DART_ERR_SID(sid);

		/* SID0 is observed even without a summary bit; label that separately. */
		if (sid && !(streams & BIT(sid)))
			continue;
		dev_err(dart->dev, "PMP DART SID%u indicated=%u status=%08x metadata=%08x address=%08x:%08x\n",
			sid, !!(streams & BIT(sid)), readl(record), readl(record + 4),
			readl(record + 12), readl(record + 8));
	}
}

static irqreturn_t pmp_v2_dart_irq(int irq, void *cookie)
{
	struct pmp_v2_dart *dart = cookie;

	/* This is an exclusive host IRQ for the retained instance. Preserve
	 * evidence; the owner must quarantine all published DMA on failure.
	 */
	WRITE_ONCE(dart->failed, true);
	disable_irq_nosync(irq);
	pmp_v2_dart_report_fault(dart);
	return IRQ_HANDLED;
}

/* Everything outside SID0 must still read as admitted. */
static int pmp_v2_dart_preserved(struct pmp_v2_dart *dart)
{
	unsigned int i;

	if (READ_ONCE(dart->failed))
		return -EIO;
	if (readl(dart->regs + PMP_DART_PROTECT) != dart->protect ||
	    readl(dart->regs + PMP_DART_PROTECT_LOCK) != dart->protect_lock ||
	    readl(dart->regs + PMP_DART_ERROR_MASK) != dart->error_mask ||
	    readl(dart->regs + PMP_DART_ERROR) ||
	    ((readl(dart->regs + PMP_DART_ENABLE_STREAMS) ^ dart->enable) & ~BIT(0)) ||
	    readl(dart->regs + PMP_DART_ERROR_STREAMS))
		goto fault;
	for (i = 1; i < PMP_DART_STREAMS; i++)
		if (readl(dart->regs + PMP_DART_TCR(i)) != dart->tcr[i] ||
		    readl(dart->regs + PMP_DART_TTBR(i)) != dart->ttbr[i])
			goto fault;
	return 0;
fault:
	WRITE_ONCE(dart->failed, true);
	return -EIO;
}

static int pmp_v2_dart_wait_idle(struct pmp_v2_dart *dart, unsigned int timeout_us)
{
	u32 command;

	return readl_poll_timeout_atomic(dart->regs + PMP_DART_TLB_CMD, command,
					 !(command & PMP_DART_TLB_CMD_BUSY), 1, timeout_us);
}

/* SID0 only. TLB_CMD is a single shared port: a write while BUSY is dropped. */
static int pmp_v2_dart_flush(struct pmp_v2_dart *dart)
{
	unsigned long flags;
	u32 command = FIELD_PREP(PMP_DART_TLB_CMD_OP, PMP_DART_TLB_CMD_OP_FLUSH_SID) |
		      FIELD_PREP(PMP_DART_TLB_CMD_STREAM, 0);
	int ret;

	if (dart->version >= 0x0202)
		command |= PMP_DART_TLB_CMD_V2;
	spin_lock_irqsave(&dart->lock, flags);
	ret = pmp_v2_dart_preserved(dart);
	if (!ret)
		ret = pmp_v2_dart_wait_idle(dart, PMP_DART_COMMAND_BUSY_US);
	if (!ret) {
		writel(command, dart->regs + PMP_DART_TLB_CMD);
		ret = pmp_v2_dart_wait_idle(dart, PMP_DART_FLUSH_BUSY_US);
	}
	if (ret)
		WRITE_ONCE(dart->failed, true);
	spin_unlock_irqrestore(&dart->lock, flags);
	if (ret)
		dev_err(dart->dev, "PMP DART SID0 flush failed: %d\n", ret);
	return ret;
}

/* Admission is read-only. Never take over an inherited valid root. */
static int pmp_v2_dart_admit(struct pmp_v2 *pmp)
{
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);
	unsigned int i;
	int ret;

	if (pmp_v2_dart_owner_running(pmp) ||
	    (readl(dart->regs + PMP_DART_TTBR(0)) & PMP_DART_TTBR_VALID))
		return -EBUSY;
	ret = pmp_v2_dart_wait_idle(dart, PMP_DART_COMMAND_BUSY_US);
	if (ret)
		return ret;
	dart->protect = readl(dart->regs + PMP_DART_PROTECT);
	dart->protect_lock = readl(dart->regs + PMP_DART_PROTECT_LOCK);
	dart->error_mask = readl(dart->regs + PMP_DART_ERROR_MASK);
	if (dart->protect & PMP_DART_PROTECT_TTBR_TCR)
		return -EBUSY;
	dart->enable = readl(dart->regs + PMP_DART_ENABLE_STREAMS);
	for (i = 1; i < PMP_DART_STREAMS; i++) {
		dart->tcr[i] = readl(dart->regs + PMP_DART_TCR(i));
		dart->ttbr[i] = readl(dart->regs + PMP_DART_TTBR(i));
	}
	dart->admitted = true;
	return pmp_v2_dart_preserved(dart);
}

static int pmp_v2_dart_apf_admit(void *cookie)
{
	struct pmp_v2 *pmp = cookie;
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);

	if (dart->epoch_pinned || pmp_v2_dart_owner_running(pmp) ||
	    !pmp_v2_dart_owner_mailbox_idle(pmp) ||
	    ((readl(dart->gate_status) >> 4) & 15) != 15 ||
	    ((readl(dart->gate_status + 8) >> 4) & 15) != 15 ||
	    readl(dart->regs + PMP_DART_ENABLE_STREAMS) ||
	    (readl(dart->regs + PMP_DART_TTBR(0)) & PMP_DART_TTBR_VALID) ||
	    readl(dart->regs + PMP_DART_BLOCK_ERROR) ||
	    readl(dart->regs + PMP_DART_EXCEPTION(0)))
		return -EBUSY;
	return pmp_v2_dart_preserved(dart);
}

static int pmp_v2_dart_enable_sid0(struct pmp_v2_dart *dart)
{
	unsigned long flags;
	u32 enable;
	int ret;

	spin_lock_irqsave(&dart->lock, flags);
	ret = pmp_v2_dart_preserved(dart);
	if (!ret) {
		enable = readl(dart->regs + PMP_DART_ENABLE_STREAMS);
		writel(enable | BIT(0), dart->regs + PMP_DART_ENABLE_STREAMS);
		if (readl(dart->regs + PMP_DART_ENABLE_STREAMS) != (enable | BIT(0))) {
			WRITE_ONCE(dart->failed, true);
			ret = -EIO;
		}
	}
	spin_unlock_irqrestore(&dart->lock, flags);
	return ret;
}

static int pmp_v2_dart_ttbr(struct pmp_v2_dart *dart, phys_addr_t root, u32 *value)
{
	u64 encoded;

	if (!IS_ALIGNED(root, SZ_16K) || root > DMA_BIT_MASK(dart->oas))
		return -ERANGE;
	encoded = ((root >> 14) << 2) | PMP_DART_TTBR_VALID;
	if (encoded > U32_MAX)
		return -ERANGE;
	*value = encoded;
	return 0;
}

/* Install the Linux four-level SID0 root; unowned streams are retained. */
static int pmp_v2_dart_install_root(struct pmp_v2 *pmp)
{
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);
	struct io_pgtable_cfg *cfg = &dart->pgtbl_cfg;
	u32 expected, tcr = PMP_DART_TCR_TRANSLATE_ENABLE | PMP_DART_TCR_FOUR_LEVEL;
	int ret;

	if (dart->epoch_pinned || dart->installed_root || pmp_v2_dart_owner_running(pmp))
		return -EBUSY;
	if (cfg->apple_dart_cfg.n_ttbrs != 1 || cfg->apple_dart_cfg.n_levels != 4)
		return -EINVAL;
	ret = pmp_v2_dart_ttbr(dart, virt_to_phys(cfg->apple_dart_cfg.ttbr[0]), &expected);
	if (ret)
		return ret;
	ret = pmp_v2_dart_enable_sid0(dart);
	if (ret)
		return ret;
	writel(expected, dart->regs + PMP_DART_TTBR(0));
	writel(tcr, dart->regs + PMP_DART_TCR(0));
	ret = pmp_v2_dart_flush(dart);
	if (ret)
		return ret;
	if (readl(dart->regs + PMP_DART_TTBR(0)) != expected ||
	    readl(dart->regs + PMP_DART_TCR(0)) != tcr) {
		WRITE_ONCE(dart->failed, true);
		return -EIO;
	}
	ret = pmp_v2_dart_preserved(dart);
	if (ret)
		return ret;
	dart->installed_root = expected;
	dev_info(dart->dev, "PMP DART SID0 four-level root=%08x; unowned state retained\n",
		 expected);
	return 0;
}

/* The owner's per-operation check; equivalent to apple_dart_pmp_check(). */
static int pmp_v2_dart_check(struct pmp_v2 *pmp)
{
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);

	if (!dart || !dart->admitted)
		return -ENODEV;
	if (!dart->apf.prepared)
		return -ENXIO;
	if (pmp_v2_dart_preserved(dart))
		return -EIO;
	if (readl(dart->regs + PMP_DART_TCR(0)) !=
	    (PMP_DART_TCR_TRANSLATE_ENABLE | PMP_DART_TCR_FOUR_LEVEL) ||
	    !dart->installed_root ||
	    readl(dart->regs + PMP_DART_TTBR(0)) != dart->installed_root ||
	    !(readl(dart->regs + PMP_DART_ENABLE_STREAMS) & BIT(0)))
		return -ENXIO;
	return 0;
}

static int pmp_v2_dart_pin_epoch(struct pmp_v2 *pmp)
{
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);
	int ret = pmp_v2_dart_check(pmp);

	if (ret)
		return ret;
	if (dart->epoch_pinned || pmp_v2_dart_owner_running(pmp))
		return -EBUSY;
	ret = apple_pmp_apf_verify(&dart->apf);
	if (ret) {
		WRITE_ONCE(dart->failed, true);
		return ret;
	}
	/* No restart/unpin operation until firmware quiescence is qualified. */
	dart->epoch_pinned = true;
	return 0;
}

/* Map a physically contiguous, 16 KiB-aligned buffer at the next free DVA. */
static int pmp_v2_dart_map(struct pmp_v2 *pmp, phys_addr_t phys, size_t size, u64 *iova)
{
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);
	size_t mapped = 0;
	u64 dva, end;
	int ret;

	if (!size || !IS_ALIGNED(size, SZ_16K) || !IS_ALIGNED(phys, SZ_16K))
		return -EINVAL;
	ret = pmp_v2_dart_check(pmp);
	if (ret)
		return ret;
	dva = dart->dva_next;
	if (check_add_overflow(dva, (u64)size, &end) || end - 1 > PMP_DART_DVA_END)
		return -ENOSPC;
	ret = dart->pgtbl_ops->map_pages(dart->pgtbl_ops, dva, phys, SZ_16K, size / SZ_16K,
					 IOMMU_READ | IOMMU_WRITE, GFP_KERNEL, &mapped);
	if (!ret && mapped != size)
		ret = -EIO;
	if (ret) {
		if (mapped)
			dart->pgtbl_ops->unmap_pages(dart->pgtbl_ops, dva, SZ_16K,
						     mapped / SZ_16K, NULL);
		return ret;
	}
	/* Leaf PTE writes must be visible to the DART before the flush. */
	dma_wmb();
	ret = pmp_v2_dart_flush(dart);
	if (ret)
		return ret;
	dart->dva_next = end;
	*iova = dva;
	return 0;
}

/* Startup rollback or a firmware-requested free. A failed epoch retains
 * every published buffer instead, since no quiescence boundary exists.
 */
static int pmp_v2_dart_unmap(struct pmp_v2 *pmp, u64 iova, size_t size)
{
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);
	size_t unmapped;

	if (READ_ONCE(dart->failed))
		return -EBUSY;
	unmapped = dart->pgtbl_ops->unmap_pages(dart->pgtbl_ops, iova, SZ_16K,
						size / SZ_16K, NULL);
	if (unmapped != size) {
		WRITE_ONCE(dart->failed, true);
		return -EIO;
	}
	dma_wmb();
	return pmp_v2_dart_flush(dart);
}

static int pmp_v2_dart_probe(struct pmp_v2 *pmp, struct platform_device *pdev)
{
	static const char instance[] = "TRAD\0\0\0\0DART\0\0\0\0FPAD\0\0\0\0DART\0\0\0\0"
				       "FPAG\0\0\0\0PS_PROT\0";
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);
	struct device *dev = &pdev->dev;
	struct resource *res;
	const void *raw, *tunables, *association;
	u64 range[2];
	u32 params, capacity;
	int ret, length, tunable_length, association_length;

	dart->dev = dev;
	spin_lock_init(&dart->lock);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dart");
	if (!res || res->start != 0x300300000ULL || resource_size(res) != 0xc000)
		return -EINVAL;
	dart->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(dart->regs))
		return PTR_ERR(dart->regs);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "apf");
	if (!res || res->start != 0x300310000ULL || resource_size(res) != SZ_16K)
		return -EINVAL;
	dart->apf.regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(dart->apf.regs))
		return PTR_ERR(dart->apf.regs);
	/* Read-only PMGR view of the PMP and PMS_SRAM gates; pmgr owns writes. */
	dart->gate_status = devm_ioremap(dev, 0x3007003d0ULL, 12);
	if (!dart->gate_status)
		return -ENOMEM;
	dart->irq = platform_get_irq_byname(pdev, "dart");
	if (dart->irq < 0)
		return dart->irq;
	if (of_property_read_u64_array(dev->of_node, "apple,dma-range", range, 2) ||
	    range[0] != PMP_DART_DVA_START || range[0] + range[1] - 1 != PMP_DART_DVA_END)
		return -EINVAL;
	association = of_get_property(dev->of_node, "apple,apf-instance", &association_length);
	if (!association || association_length != sizeof(instance) - 1 ||
	    memcmp(association, instance, sizeof(instance) - 1) ||
	    of_property_read_u32(dev->of_node, "apple,apf-capacity", &capacity) ||
	    capacity != PMP_APF_CAPACITY)
		return -EINVAL;
	raw = of_get_property(dev->of_node, "apple,apf-table", &length);
	tunables = of_get_property(dev->of_node, "apple,dart-tunables", &tunable_length);
	if (!raw || !tunables)
		return -EINVAL;
	ret = apple_pmp_apf_decode(&dart->apf, raw, length, tunables, tunable_length);
	if (ret)
		return dev_err_probe(dev, ret, "PMP APF table/tunables are not the qualified own-ADT records\n");

	params = readl(dart->regs + PMP_DART_PARAMS1);
	dart->pgsize = 1 << FIELD_GET(PMP_DART_PARAMS1_PAGE_SHIFT, params);
	params = readl(dart->regs + PMP_DART_PARAMS3);
	dart->ias = FIELD_GET(PMP_DART_PARAMS3_VA_WIDTH, params);
	dart->oas = FIELD_GET(PMP_DART_PARAMS3_PA_WIDTH, params);
	dart->version = FIELD_GET(PMP_DART_PARAMS3_VERSION, params);
	dart->num_streams = FIELD_GET(PMP_DART_PARAMS4_NUM_SIDS,
				      readl(dart->regs + PMP_DART_PARAMS4));
	if (dart->pgsize != SZ_16K || dart->num_streams != PMP_DART_STREAMS ||
	    dart->version != PMP_DART_VERSION || dart->ias < 41 || dart->ias > 47 ||
	    (dart->oas != 36 && dart->oas != 42))
		return dev_err_probe(dev, -EINVAL,
				     "PMP DART is not the qualified T8110 v2.3 instance (pgsize %x, %u streams, version %04x, AS %u -> %u)\n",
				     dart->pgsize, dart->num_streams, dart->version,
				     dart->ias, dart->oas);
	dart->dva_next = PMP_DART_DVA_START;
	return 0;
}

/* Read-only evidence for a refused admission; nothing is cleared or written. */
static void pmp_v2_dart_dump(struct pmp_v2 *pmp, const char *why)
{
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);

	dev_err(dart->dev, "%s: run=%u mailbox_idle=%u gates=%08x/%08x protect=%08x lock=%08x error_mask=%08x enable=%08x tcr0=%08x ttbr0=%08x tcr2=%08x ttbr2=%08x block=%08x pending=%08x apf0=%08x/%08x apf7=%08x\n",
		why, pmp_v2_dart_owner_running(pmp), pmp_v2_dart_owner_mailbox_idle(pmp),
		readl(dart->gate_status), readl(dart->gate_status + 8),
		readl(dart->regs + PMP_DART_PROTECT), readl(dart->regs + PMP_DART_PROTECT_LOCK),
		readl(dart->regs + PMP_DART_ERROR_MASK), readl(dart->regs + PMP_DART_ENABLE_STREAMS),
		readl(dart->regs + PMP_DART_TCR(0)), readl(dart->regs + PMP_DART_TTBR(0)),
		readl(dart->regs + PMP_DART_TCR(2)), readl(dart->regs + PMP_DART_TTBR(2)),
		readl(dart->regs + PMP_DART_BLOCK_ERROR), readl(dart->regs + PMP_DART_EXCEPTION(0)),
		readl(dart->apf.regs + 0), readl(dart->apf.regs + 8), readl(dart->apf.regs + 7 * 64));
}

/* Admit the stopped instance, prepare the APF, then install the SID0 root. */
static int pmp_v2_dart_start(struct pmp_v2 *pmp)
{
	struct pmp_v2_dart *dart = pmp_v2_dart_of(pmp);
	int ret;

	ret = pmp_v2_dart_admit(pmp);
	if (ret) {
		pmp_v2_dart_dump(pmp, "PMP DART admission refused");
		return dev_err_probe(dart->dev, ret, "PMP DART admission (inherited SID0 root or protected)\n");
	}
	ret = apple_pmp_apf_initialize(&dart->apf, dart->regs, pmp_v2_dart_apf_admit, pmp, false);
	if (ret) {
		WRITE_ONCE(dart->failed, true);
		pmp_v2_dart_dump(pmp, "PMP APF preparation refused");
		return dev_err_probe(dart->dev, ret, "PMP APF failed; no DMA root or firmware start\n");
	}
	dev_info(dart->dev, "PMP APF prepared: 26 SID0 rows verified; TRAD/SID2 retained, ASC stopped\n");
	ret = devm_request_irq(dart->dev, dart->irq, pmp_v2_dart_irq, 0,
			       "apple-pmp-v2 dart", dart);
	if (ret)
		return ret;
	dart->pgtbl_cfg = (struct io_pgtable_cfg){
		.pgsize_bitmap = SZ_16K,
		.ias = 41,
		.oas = dart->oas,
		.coherent_walk = 1,
		.iommu_dev = dart->dev,
	};
	dart->pgtbl_ops = alloc_io_pgtable_ops(APPLE_DART2, &dart->pgtbl_cfg, dart);
	if (!dart->pgtbl_ops)
		return -ENOMEM;
	ret = pmp_v2_dart_install_root(pmp);
	if (ret) {
		dev_err(dart->dev, "PMP DART SID0 root installation failed: %d\n", ret);
		return ret;
	}
	dev_info(dart->dev, "PMP DART stopped; owning SID0 only (version %04x, AS %u -> %u)\n",
		 dart->version, dart->ias, dart->oas);
	return 0;
}
