// SPDX-License-Identifier: GPL-2.0-only
/* Native stopped-epoch T8140 PMP owner with optional Linux thermal policy. */
#include <crypto/sha2.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/dma-direct.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/soc/apple/mailbox.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/workqueue.h>

#include "pmp-v2-protocol.h"
#include <linux/suspend.h>
#include "pmp-v2-resident.h"
#include "pmp-v2-profile.h"
#include "pmp-v2-dart.h"

#define PMP_CPU_CONTROL 0x44
#define PMP_CPU_RUN BIT(4)
#define PMP_ENDPOINT 0x20

struct pmp_v2 {
	struct device *dev;
	struct device_node *nub;
	struct apple_rtkit *rtk;
	struct mutex lock;
	struct mutex lifecycle;
	struct notifier_block reboot;
	struct delayed_work health;
	struct pmp_v2_protocol protocol;
	struct pmp_v2_dart dart;
	void __iomem *control, *resident, *mailbox_status;
	struct resource firmware;
	struct pmp_v2_segment segments[64];
	unsigned int segment_count;
	u64 dram_mask;
	u64 messages, last_message_ns;
	bool attempted, pinned, running, stopping;
	bool private_profile;
	const struct pmp_v2_profile *profile;
	struct dentry *debug;
};

struct pmp_v2_dma {
	dma_addr_t handle;
	u64 iova;
	bool mapped;
};

static struct pmp_v2_dart *pmp_v2_dart_of(struct pmp_v2 *pmp)
{
	return &pmp->dart;
}

static bool pmp_v2_dart_owner_running(struct pmp_v2 *pmp)
{
	return readl(pmp->control + PMP_CPU_CONTROL) & PMP_CPU_RUN;
}

/* A2I must be empty with no error flags; I2A must have no pending count. */
static bool pmp_v2_dart_owner_mailbox_idle(struct pmp_v2 *pmp)
{
	u32 tx = readl(pmp->mailbox_status);
	u32 rx = readl(pmp->mailbox_status + 4);

	return !((tx | rx) & GENMASK(19, 18)) && (tx & BIT(17)) && !(rx & GENMASK(23, 20));
}

static void pmp_v2_fail(struct pmp_v2 *pmp, int error)
{
	if (!pmp->protocol.failed)
		dev_err(pmp->dev, "PMP epoch failed (%d); retaining published DMA, no restart\n", error);
	pmp->protocol.failed = true;
}

/* Coherent host memory mapped at an owned SID0 DVA of the PMP DART. */
static int pmp_v2_dma_allocate(struct pmp_v2_protocol *p, struct pmp_v2_buffer *b)
{
	struct pmp_v2 *pmp = p->private;
	struct pmp_v2_dma *dma;
	int ret = pmp_v2_dart_check(pmp);

	if (ret)
		return ret;
	dma = kzalloc_obj(*dma);
	if (!dma)
		return -ENOMEM;
	b->allocation_size = PAGE_ALIGN(b->size);
	b->data = dma_alloc_coherent(pmp->dev, b->allocation_size, &dma->handle, GFP_KERNEL);
	if (!b->data) {
		kfree(dma);
		return -ENOMEM;
	}
	ret = pmp_v2_dart_map(pmp, dma_to_phys(pmp->dev, dma->handle), b->allocation_size,
			      &dma->iova);
	if (ret) {
		dma_free_coherent(pmp->dev, b->allocation_size, b->data, dma->handle);
		kfree(dma);
		return ret;
	}
	dma->mapped = true;
	b->private = dma;
	b->address = dma->iova | pmp->dram_mask;
	return 0;
}

static void pmp_v2_dma_release(struct pmp_v2_protocol *p, struct pmp_v2_buffer *b)
{
	struct pmp_v2 *pmp = p->private;
	struct pmp_v2_dma *dma = b->private;
	int ret = 0;

	if (dma->mapped)
		ret = pmp_v2_dart_unmap(pmp, dma->iova, b->allocation_size);
	if (ret) {
		/* Still translated: quarantine the memory rather than reuse it. */
		dev_err(pmp->dev, "PMP DMA %016llx retained after unmap failure (%d)\n",
			dma->iova, ret);
		kfree(dma);
		return;
	}
	dma_free_coherent(pmp->dev, b->allocation_size, b->data, dma->handle);
	kfree(dma);
}

static const void *pmp_v2_property(struct pmp_v2_protocol *p, const char *name, int *size)
{
	struct pmp_v2 *pmp = p->private;

	return of_get_property(pmp->nub, name, size);
}

static const struct pmp_v2_protocol_ops pmp_v2_native_ops = {
	.allocate = pmp_v2_dma_allocate,
	.release = pmp_v2_dma_release,
	.property = pmp_v2_property,
};

static int pmp_v2_shmem_setup(void *cookie, struct apple_rtkit_shmem *memory)
{
	struct pmp_v2 *pmp = cookie;
	struct pmp_v2_buffer *b;
	int ret = 0;

	mutex_lock(&pmp->lock);
	/* A supplied address requires an inherited ledger, which we do not have. */
	if (memory->iova || memory->is_mapped || pmp->protocol.failed ||
	    READ_ONCE(pmp->stopping)) {
		ret = -EINVAL;
		goto fault;
	}
	b = pmp_v2_allocate(&pmp->protocol, memory->size, false);
	if (IS_ERR(b)) {
		ret = PTR_ERR(b);
		goto fault;
	}
	/* The common unshifted system format has the narrowest address field. */
	if (b->address + b->size - 1 > GENMASK_ULL(43, 0)) {
		pmp_v2_release(&pmp->protocol, b); /* not yet published */
		ret = -ERANGE;
		goto fault;
	}
	memory->buffer = b->data;
	memory->iova = b->address;
	memory->private = b;
	dma_wmb();
	goto out;
fault:
	pmp_v2_fail(pmp, ret);
out:
	mutex_unlock(&pmp->lock);
	return ret;
}

static void pmp_v2_shmem_destroy(void *cookie, struct apple_rtkit_shmem *memory)
{
	struct pmp_v2 *pmp = cookie;

	mutex_lock(&pmp->lock);
	/* No system-buffer revocation is qualified for a started epoch. */
	if (pmp->pinned)
		pmp_v2_fail(pmp, -EBUSY);
	else if (memory->private)
		pmp_v2_release(&pmp->protocol, memory->private);
	mutex_unlock(&pmp->lock);
}

static void pmp_v2_crashed(void *cookie, const void *log, size_t size)
{
	struct pmp_v2 *pmp = cookie;

	mutex_lock(&pmp->lock);
	pmp_v2_fail(pmp, -EIO);
	mutex_unlock(&pmp->lock);
}

static void pmp_v2_receive(void *cookie, u8 endpoint, u64 word)
{
	struct pmp_v2 *pmp = cookie;
	u64 reply;
	int ret;

	mutex_lock(&pmp->lock);
	if (pmp->protocol.failed || READ_ONCE(pmp->stopping))
		goto out;
	if (!pmp->pinned || endpoint != PMP_ENDPOINT) {
		ret = -EPROTO;
		goto fault;
	}
	ret = pmp_v2_dart_check(pmp);
	if (ret)
		goto fault;
	dma_rmb();
	ret = pmp_v2_handle(&pmp->protocol, word, &reply);
	if (ret) {
		dev_err(pmp->dev, "PMP rejected application word %016llx (%d)\n", word, ret);
		goto fault;
	}
	dma_wmb();
	ret = apple_rtkit_send_message(pmp->rtk, endpoint, reply, NULL, false);
	if (ret)
		goto fault;
	pmp->messages++;
	pmp->last_message_ns = ktime_get_boottime_ns();
	goto out;
fault:
	pmp_v2_fail(pmp, ret);
out:
	mutex_unlock(&pmp->lock);
}

static const struct apple_rtkit_ops pmp_v2_rtkit_ops = {
	.protocol_version = 12,
	.shmem_setup = pmp_v2_shmem_setup,
	.shmem_destroy = pmp_v2_shmem_destroy,
	.recv_message = pmp_v2_receive,
	.crashed = pmp_v2_crashed,
};

/* Device mapping: aligned 32-bit accesses only, including packed DATA fields. */
static int pmp_v2_resident_read(void *cookie, u64 address, void *data, size_t size)
{
	struct pmp_v2 *pmp = cookie;
	u64 end, first = round_down(address, 4), last;
	unsigned int i;
	size_t n;

	if (!size || check_add_overflow(address, (u64)size, &end) || end > U64_MAX - 3)
		return -EINVAL;
	last = round_up(end, 4);
	for (i = 0; i < pmp->segment_count; i++) {
		struct pmp_v2_segment *s = &pmp->segments[i];

		if (first >= s->physical && last <= s->physical + s->size)
			break;
	}
	if (i == pmp->segment_count)
		return -ERANGE;
	for (n = 0; n < size; n++) {
		u64 a = address + n;
		u32 value = readl(pmp->resident + round_down(a, 4) - pmp->firmware.start);

		((u8 *)data)[n] = value >> (8 * (a & 3));
	}
	return 0;
}

static int pmp_v2_resident_fixup(struct pmp_v2 *pmp, bool apply)
{
	static const u32 keys[] = {
		0x42444944, 0x44564944, 0x44434150, 0x44434844, 0x504d435f,
		0x504d4356, 0x504d4342, 0x504d4358, 0x43564152, 0x504d434d,
	};
	struct pmp_v2_patch patches[ARRAY_SIZE(keys)] = {};
	u32 values[ARRAY_SIZE(keys) * 2];
	u64 physical;
	size_t size;
	u8 *snapshot;
	unsigned int i, n, changed = 0;
	int ret;

	if (readl(pmp->control + PMP_CPU_CONTROL) & PMP_CPU_RUN)
		return -EBUSY;
	if (of_property_count_u32_elems(pmp->dev->of_node, "apple,patchbay-values") != ARRAY_SIZE(values))
		return -EINVAL;
	ret = of_property_read_u32_array(pmp->dev->of_node, "apple,patchbay-values", values,
					 ARRAY_SIZE(values));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(keys); i++) {
		if (values[2 * i] != keys[i])
			return -EINVAL;
		patches[i].key = keys[i];
		patches[i].value = values[2 * i + 1];
	}
	ret = pmp_v2_locate(pmp->segments, pmp->segment_count, pmp_v2_resident_read,
			    pmp, &physical, &size);
	if (ret)
		return ret;
	snapshot = kmalloc(size, GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;
	ret = pmp_v2_resident_read(pmp, physical, snapshot, size);
	if (ret)
		goto out;
	ret = pmp_v2_patch_plan(snapshot, size, patches, ARRAY_SIZE(patches));
	if (ret)
		goto out;
	for (i = 0; i < ARRAY_SIZE(patches); i++) {
		struct pmp_v2_patch *patch = &patches[i];
		u8 verify[4];

		if (!patch->present || get_unaligned_le32(snapshot + patch->offset) == patch->value)
			continue;
		changed++;
		if (!apply)
			continue;
		if (readl(pmp->control + PMP_CPU_CONTROL) & PMP_CPU_RUN) {
			ret = -EBUSY;
			goto out;
		}
		for (n = 0; n < 4; n++) {
			u64 a = physical + patch->offset + n;
			void __iomem *word = pmp->resident + round_down(a, 4) - pmp->firmware.start;
			u32 shift = 8 * (a & 3), old = readl(word);

			writel((old & ~(0xffU << shift)) |
			       (((patch->value >> (8 * n)) & 0xff) << shift), word);
		}
		/* Posted device writes must reach resident DATA before RUN. */
		wmb();
		ret = pmp_v2_resident_read(pmp, physical + patch->offset, verify, 4);
		if (ret || get_unaligned_le32(verify) != patch->value) {
			ret = -EIO;
			goto out;
		}
	}
	dev_info(pmp->dev, "PMP resident DATA: %u %s payload changes, %zu bytes\n",
		 changed, apply ? "verified" : "proposed", size);
out:
	kfree(snapshot);
	return ret;
}

static void pmp_v2_health(struct work_struct *work)
{
	struct pmp_v2 *pmp = container_of(to_delayed_work(work), struct pmp_v2, health);
	int ret;

	/* Read-only DART checks and the mailbox poll need no registry lock:
	 * received words are dispatched to pmp_v2_receive(), which takes it.
	 * Holding the lock here made the 25 ms sampler's trylock fail whenever
	 * the two timer expiries aligned, clamping both clusters for nothing.
	 */
	ret = pmp_v2_dart_check(pmp);
	if (!ret)
		ret = min(apple_rtkit_poll(pmp->rtk), 0);
	if (!ret && apple_rtkit_is_crashed(pmp->rtk))
		ret = -EIO;
	mutex_lock(&pmp->lock);
	if (ret)
		pmp_v2_fail(pmp, ret);
	/* Transport status only; this period is NOT a temperature-freshness ABI. */
	if (!pmp->protocol.failed && !READ_ONCE(pmp->stopping))
		schedule_delayed_work(&pmp->health, HZ);
	mutex_unlock(&pmp->lock);
}

static int pmp_v2_start_epoch(struct pmp_v2 *pmp)
{
	u8 digest[SHA256_DIGEST_SIZE];
	u32 control;
	int ret;

	mutex_lock(&pmp->lock);
	if (pmp->attempted) {
		mutex_unlock(&pmp->lock);
		return -EBUSY;
	}
	pmp->attempted = true;
	control = readl(pmp->control + PMP_CPU_CONTROL);
	if ((control & PMP_CPU_RUN) || !pmp_v2_dart_owner_mailbox_idle(pmp)) {
		ret = -EBUSY;
		goto fail_locked;
	}
	ret = pmp_v2_dart_check(pmp);
	if (ret)
		goto fail_locked;
	/* Identity belongs to this stopped epoch, not to the unit serial or DT
	 * claim. Unknown builds may use the generic protocol, never fixed DATA
	 * offsets. Do not normalize an unexpected resident digest.
	 */
	pmp->private_profile = false;
	pmp->profile = NULL;
	ret = pmp_v2_profile_digest(pmp->segments, pmp->segment_count,
				    pmp_v2_resident_read, pmp, digest, &pmp->profile);
	if (!ret) {
		pmp->private_profile = pmp->profile != NULL;
		dev_info(pmp->dev, "PMP resident TEXT SHA256=%*phN private-profile=%u (%s)\n",
			 (int)sizeof(digest), digest, pmp->private_profile,
			 pmp->profile ? pmp->profile->build : "unknown build");
	} else {
		dev_warn(pmp->dev, "PMP private profile unavailable (%d); private reader disabled\n", ret);
	}
	ret = pmp_v2_resident_fixup(pmp, true);
	if (ret)
		goto fail_locked;
	/* RTKit starts RX; callbacks take our mutex, so do not hold it here. */
	mutex_unlock(&pmp->lock);
	pmp->rtk = apple_rtkit_init(pmp->dev, pmp, NULL, 0, &pmp_v2_rtkit_ops);
	if (IS_ERR(pmp->rtk)) {
		ret = PTR_ERR(pmp->rtk);
		pmp->rtk = NULL;
		goto fail;
	}
	mutex_lock(&pmp->lock);
	ret = pmp->protocol.failed ? -EIO : pmp_v2_dart_pin_epoch(pmp);
	if (ret)
		goto fail_locked;
	pmp->pinned = true;
	/* Publish resident DATA and the prepared DMA context before execution. */
	wmb();
	control = readl(pmp->control + PMP_CPU_CONTROL);
	if (control & PMP_CPU_RUN) {
		ret = -EBUSY;
		goto fail_locked;
	}
	writel(control | PMP_CPU_RUN, pmp->control + PMP_CPU_CONTROL);
	if (!(readl(pmp->control + PMP_CPU_CONTROL) & PMP_CPU_RUN)) {
		ret = -EIO;
		goto fail_locked;
	}
	mutex_unlock(&pmp->lock);
	ret = apple_rtkit_boot(pmp->rtk);
	if (ret)
		goto fail;
	if (!apple_rtkit_has_endpoint(pmp->rtk, PMP_ENDPOINT)) {
		ret = -ENODEV;
		goto fail;
	}
	ret = apple_rtkit_start_ep(pmp->rtk, PMP_ENDPOINT);
	if (ret)
		goto fail;
	mutex_lock(&pmp->lock);
	if (pmp->protocol.failed) {
		ret = -EIO;
		goto fail_locked;
	}
	pmp->running = true;
	schedule_delayed_work(&pmp->health, HZ);
	dev_info(pmp->dev, "PMP RTKit12 endpoint enabled; awaiting registry traffic (no thermal admission)\n");
	mutex_unlock(&pmp->lock);
	return 0;
fail:
	mutex_lock(&pmp->lock);
fail_locked:
	pmp_v2_fail(pmp, ret);
	mutex_unlock(&pmp->lock);
	return ret;
}

static int pmp_v2_start(struct pmp_v2 *pmp)
{
	/* Reboot joins startup without holding the callback/registry mutex. */
	guard(mutex)(&pmp->lifecycle);
	if (READ_ONCE(pmp->stopping))
		return -ESHUTDOWN;
	return pmp_v2_start_epoch(pmp);
}

static ssize_t pmp_v2_start_write(struct file *file, const char __user *data,
				 size_t size, loff_t *position)
{
	bool start;
	int ret = kstrtobool_from_user(data, size, &start);

	if (ret || !start)
		return -EINVAL;
	ret = pmp_v2_start(file->private_data);
	return ret ? ret : size;
}

static const struct file_operations pmp_v2_start_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = pmp_v2_start_write,
};

static int pmp_v2_status_show(struct seq_file *s, void *unused)
{
	struct pmp_v2 *pmp = s->private;

	mutex_lock(&pmp->lock);
	seq_printf(s, "attempted=%u pinned=%u running=%u failed=%u profile=%s\n",
		   pmp->attempted, pmp->pinned, pmp->running, pmp->protocol.failed,
		   pmp->profile ? pmp->profile->build : "none");
	seq_printf(s, "messages=%llu last_message_boottime_ns=%llu buffers=%u bytes=%zu entries=%u\n",
		   pmp->messages, pmp->last_message_ns, pmp->protocol.buffer_count,
		   pmp->protocol.allocated, pmp->protocol.entry_count);
	seq_printf(s, "dart_admitted=%u apf_prepared=%u root=%08x epoch_pinned=%u dart_failed=%u dva_next=%llx\n",
		   pmp->dart.admitted, pmp->dart.apf.prepared, pmp->dart.installed_root,
		   pmp->dart.epoch_pinned, READ_ONCE(pmp->dart.failed), pmp->dart.dva_next);
	seq_puts(s, "thermal_producer_qualified=0; CPU admission cap unchanged\n");
	mutex_unlock(&pmp->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pmp_v2_status);

static int pmp_v2_registry_show(struct seq_file *s, void *unused)
{
	struct pmp_v2 *pmp = s->private;
	struct pmp_v2_entry *entry;
	u8 hash[SHA256_DIGEST_SIZE];

	mutex_lock(&pmp->lock);
	list_for_each_entry(entry, &pmp->protocol.entries, link) {
		sha256(entry->value, entry->size, hash);
		seq_printf(s, "id=%u name=%s format=%s unit=%s flags=%02x length=%u sha256=%*phN\n",
			   entry->id, entry->name, entry->format, entry->unit,
			   entry->flags, entry->size, SHA256_DIGEST_SIZE, hash);
		seq_printf(s, " initial_source=%s updates=%llu registered_ns=%llu last_update_ns=%llu\n",
			   entry->default_from_adt ? "own-adt" : "firmware",
			   entry->updates, entry->registered_ns, entry->last_update_ns);
	}
	mutex_unlock(&pmp->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pmp_v2_registry);

#ifdef CONFIG_APPLE_PMP_V2_THERMAL
#include "pmp-v2-data-access.h"
#include "pmp-v2-sample.h"
#include "pmp-v2-thermal.h"
#endif

static int pmp_v2_reboot(struct notifier_block *notifier,
			 unsigned long action, void *unused)
{
	struct pmp_v2 *pmp = container_of(notifier, struct pmp_v2, reboot);
	int ret = -EOPNOTSUPP;

	/* Blocking reboot notifiers run before device_shutdown(), while CPU
	 * policies, SMC and interrupts can still complete minimum-state requests.
	 * This is inhibition, not a firmware DMA-drain or a veto of reboot.
	 */
	WRITE_ONCE(pmp->stopping, true);
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
	/* Join automatic startup before taking the lifecycle mutex it uses. */
	ret = pt_shutdown();
#endif
	mutex_lock(&pmp->lifecycle);
	cancel_delayed_work_sync(&pmp->health);
	/* Join any application callback already past its stopping check. The
	 * RTKit core and all published storage still remain resident until reset.
	 */
	mutex_lock(&pmp->lock);
	mutex_unlock(&pmp->lock);
	dev_info(pmp->dev, "PMP reboot: policy inhibited, minima status=%d, DMA pinned=%u until reset\n",
		 ret, pmp->pinned);
	mutex_unlock(&pmp->lifecycle);
	/* No RUN clear, RTKit sleep, DMA unmap or power-domain release. */
	return NOTIFY_DONE;
}

static void pmp_v2_node_put(void *node)
{
	of_node_put(node);
}

static int pmp_v2_probe_owner(struct platform_device *pdev)
{
	static const char * const power_names[] = { "pmp", "sram" };
	const struct dev_pm_domain_attach_data pd = {
		.pd_names = power_names, .num_pd_names = ARRAY_SIZE(power_names),
		.pd_flags = PD_FLAG_DEV_LINK_ON | PD_FLAG_ATTACH_POWER_ON,
	};
	struct device *dev = &pdev->dev;
	struct dev_pm_domain_list *domains;
	struct device_node *mailbox;
	struct apple_mbox *transport;
	struct resource *resource, mbox;
	const void *raw;
	struct pmp_v2 *pmp;
	u32 metadata_version;
	int ret, length;
	unsigned int i;

	if (!of_machine_is_compatible("apple,j700") ||
	    of_property_read_u32(dev->of_node, "apple,metadata-version", &metadata_version) ||
	    metadata_version != 1)
		return -EINVAL;
	pmp = devm_kzalloc(dev, sizeof(*pmp), GFP_KERNEL);
	if (!pmp)
		return -ENOMEM;
	pmp->dev = dev;
	mutex_init(&pmp->lock);
	mutex_init(&pmp->lifecycle);
	INIT_DELAYED_WORK(&pmp->health, pmp_v2_health);
	pmp_v2_protocol_init(&pmp->protocol, &pmp_v2_native_ops, pmp);
	pmp->nub = of_get_child_by_name(dev->of_node, "firmware-data");
	if (!pmp->nub)
		return -EINVAL;
	ret = devm_add_action_or_reset(dev, pmp_v2_node_put, pmp->nub);
	if (ret)
		return ret;
	if (!of_property_present(pmp->nub, "pre-loaded") ||
	    !of_property_present(pmp->nub, "no-shutdown") ||
	    !of_property_present(pmp->nub, "user-power-managed") ||
	    of_property_present(pmp->nub, "running") ||
	    of_property_present(pmp->nub, "no-firmware-service") ||
	    of_property_present(pmp->nub, "wait-for") ||
	    of_property_present(pmp->nub, "pio-reg-index") ||
	    of_property_present(pmp->nub, "cpu-ctrl-filtered"))
		return -EINVAL;
	raw = of_get_property(pmp->nub, "asc-dram-mask", &length);
	if (raw) {
		if (length != 8)
			return -EINVAL;
		pmp->dram_mask = get_unaligned_le64(raw);
		if (pmp->dram_mask & ~GENMASK_ULL(43, 0))
			return -ERANGE;
	}
	ret = devm_pm_domain_attach_list(dev, &pd, &domains);
	if (ret < 0)
		return ret;
	if (!domains || domains->num_pds != 2)
		return -EINVAL;
	/* Host memory is reached through the owned SID0 translation; the direct
	 * DMA mask only has to cover physical memory for the coherent allocator.
	 */
	if (device_iommu_mapped(dev))
		return dev_err_probe(dev, -EINVAL, "PMP owns its translation instance; no iommus\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "control");
	if (!resource || resource->start != 0x300e00000ULL || resource_size(resource) != 0x48)
		return -EINVAL;
	pmp->control = devm_ioremap_resource(dev, resource);
	if (IS_ERR(pmp->control))
		return PTR_ERR(pmp->control);
	if (readl(pmp->control + PMP_CPU_CONTROL) & PMP_CPU_RUN)
		return -EBUSY;
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "firmware");
	if (!resource || resource->start != 0x300500000ULL || resource_size(resource) != 0xa0000)
		return -EINVAL;
	pmp->firmware = *resource;
	pmp->resident = devm_ioremap_resource(dev, resource);
	if (IS_ERR(pmp->resident))
		return PTR_ERR(pmp->resident);
	raw = of_get_property(dev->of_node, "apple,resident-segments", &length);
	if (!raw || length <= 0)
		return -EINVAL;
	ret = pmp_v2_segments(raw, length, pmp->segments, ARRAY_SIZE(pmp->segments));
	if (ret < 0)
		return ret;
	pmp->segment_count = ret;
	for (i = 0; i < pmp->segment_count; i++) {
		struct pmp_v2_segment *s = &pmp->segments[i];

		if (!IS_ALIGNED(s->physical, 4) || !IS_ALIGNED(s->size, 4) ||
		    s->physical < pmp->firmware.start || s->physical + s->size - 1 > pmp->firmware.end)
			return -EINVAL;
	}
	mailbox = of_parse_phandle(dev->of_node, "mboxes", 0);
	if (!mailbox)
		return -EINVAL;
	ret = of_address_to_resource(mailbox, 0, &mbox);
	of_node_put(mailbox);
	if (ret || mbox.start != 0x300e08000ULL || resource_size(&mbox) < 0x1000)
		return -EINVAL;
	/* The transport must exist before the one-shot start can be attempted;
	 * defer the whole probe rather than failing the epoch later.
	 */
	transport = apple_mbox_get(dev, 0);
	if (IS_ERR(transport))
		return PTR_ERR(transport) == -EPROBE_DEFER ? -EPROBE_DEFER : -ENODEV;
	/* Non-consuming status view; the mailbox driver alone owns its FIFOs. */
	pmp->mailbox_status = devm_ioremap(dev, mbox.start + 0x110, 8);
	if (!pmp->mailbox_status)
		return -ENOMEM;
	ret = pmp_v2_dart_probe(pmp, pdev);
	if (ret)
		return dev_err_probe(dev, ret, "PMP DART resource validation\n");
	ret = pmp_v2_resident_fixup(pmp, false);
	if (ret)
		return dev_err_probe(dev, ret, "PMP resident metadata/own-input validation\n");
	/* Own the stopped translation instance: admission, APF, SID0 root. */
	ret = pmp_v2_dart_start(pmp);
	if (ret)
		return ret;
	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret) {
		pm_runtime_put_noidle(dev);
		return ret;
	}
	platform_set_drvdata(pdev, pmp);
	pmp->reboot.notifier_call = pmp_v2_reboot;
	ret = devm_register_reboot_notifier(dev, &pmp->reboot);
	if (ret)
		return ret;
	pmp->debug = debugfs_create_dir("apple-pmp-v2", NULL);
#ifndef CONFIG_APPLE_PMP_V2_THERMAL
	debugfs_create_file("start", 0200, pmp->debug, pmp, &pmp_v2_start_fops);
#endif
	debugfs_create_file("status", 0400, pmp->debug, pmp, &pmp_v2_status_fops);
	debugfs_create_file("registry", 0400, pmp->debug, pmp, &pmp_v2_registry_fops);
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
	ret = pt_init(pmp);
	if (ret)
		return ret;
	dev_info(dev, "PMP stopped-epoch preflight passed; awaiting capped thermal dependencies\n");
#else
	dev_info(dev, "PMP stopped-epoch preflight passed; explicit one-shot debugfs start required\n");
#endif
	return 0;
}

static bool pmp_v2_probed;

static int pmp_v2_probe(struct platform_device *pdev)
{
	int ret;

	pmp_v2_probed = true;
	ret = pmp_v2_probe_owner(pdev);
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
	/* The cpufreq boot caps exist for this policy; do not leave them behind. */
	if (ret && ret != -EPROBE_DEFER)
		pt_owner_failed(ret);
#endif
	return ret;
}

#ifdef CONFIG_APPLE_PMP_V2_THERMAL
/* No owner device at all (no or disabled DT node): same as a failed owner. */
static int __init pmp_v2_late_check(void)
{
	if (of_machine_is_compatible("apple,j700") && !pmp_v2_probed)
		pt_owner_failed(-ENODEV);
	return 0;
}
late_initcall_sync(pmp_v2_late_check);
#endif

static int pmp_v2_suspend(struct device *dev)
{
	return -EBUSY; /* Shared power/firmware quiescence not qualified. */
}

static int pmp_v2_suspend_system(struct device *dev)
{
	/*
	 * The blanket refusal above exists because quiescing the PMP means
	 * quiescing a firmware that owns a resident window this driver has
	 * patched, and nothing has qualified doing that on this part.
	 *
	 * Suspend-to-idle does not ask for any of it.  The SoC is not powered
	 * down, the resident window stays mapped and valid, the firmware keeps
	 * running and keeps servicing the thermal policy -- which matters on a
	 * fanless machine -- so the correct behaviour is to do nothing and
	 * succeed.  Refusing it instead aborts the whole cycle for every other
	 * device (measured on J700, 2026-09-21: "apple-pmp-v2 ... failed to
	 * suspend: error -16" then "PM: Some devices failed to suspend").
	 *
	 * Deeper states still have to quiesce the firmware, and that is the
	 * part that is unqualified, so they keep the refusal.
	 */
	if (pm_suspend_target_state == PM_SUSPEND_TO_IDLE)
		return 0;

	return pmp_v2_suspend(dev);
}

static const struct dev_pm_ops pmp_v2_pm_ops = {
	.suspend = pmp_v2_suspend_system,
	.freeze = pmp_v2_suspend,
	.poweroff = pmp_v2_suspend,
	.runtime_suspend = pmp_v2_suspend,
};

static const struct of_device_id pmp_v2_match[] = {
	{ .compatible = "apple,t8140-pmp-v2" },
	{}
};

static struct platform_driver pmp_v2_driver = {
	.probe = pmp_v2_probe,
	.driver = {
		.name = "apple-pmp-v2",
		.of_match_table = pmp_v2_match,
		.suppress_bind_attrs = true,
		.pm = &pmp_v2_pm_ops,
	},
};
builtin_platform_driver(pmp_v2_driver);

MODULE_LICENSE("GPL");
