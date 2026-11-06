// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in Aura PV-KVM v0 diagnostic client. No device exists without the Aura DT node. */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <uapi/linux/aura_pv.h>

struct aura_pv_page {
	u32 major;
	u32 minor;
	u32 bytes;
	u32 layout_id;
	u64 request_seq;
	u64 response_seq;
	u64 input_dirty;
	u64 output_valid;
	u64 guest_magic;
	u64 program_words;
	u64 initial;
	u64 result;
	u64 program[AURA_PV_MAX_PROGRAM];
	u64 host_sctlr_before;
	u64 host_sctlr_after;
	u64 host_cntkctl_before;
	u64 host_cntkctl_after;
	u64 host_sentinel_status;
	u64 exit_reason;
	u64 exit_flags;
	u8 reserved[AURA_PV_PAGE_BYTES - 200];
} __packed __aligned(64);

static_assert(sizeof(struct aura_pv_page) == AURA_PV_PAGE_BYTES);

struct aura_pv_dev {
	struct miscdevice misc;
	struct aura_pv_caps caps;
	bool available;
};

static struct aura_pv_dev aura_pv;

static void aura_pv_hvc(unsigned long fid, unsigned long arg1,
			unsigned long arg2, unsigned long arg3,
			unsigned long *ret0, unsigned long *ret1,
			unsigned long *ret2, unsigned long *ret3)
{
	register unsigned long x0 asm("x0") = fid;
	register unsigned long x1 asm("x1") = arg1;
	register unsigned long x2 asm("x2") = arg2;
	register unsigned long x3 asm("x3") = arg3;
	register unsigned long x19 asm("x19") = 0x1919191919191919ULL;
	register unsigned long x20 asm("x20") = 0x2020202020202020ULL;
	asm volatile("hvc #0"
		     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x19),
		       "+r"(x20)
		     :
		     : "memory");
	*ret0 = x0;
	*ret1 = x1;
	*ret2 = x2;
	*ret3 = x3;
}

static int aura_pv_discover(struct aura_pv_dev *dev)
{
	BUILD_BUG_ON(sizeof(struct aura_pv_page) != AURA_PV_PAGE_BYTES);
	unsigned long r0, r1, r2, r3;
	struct page *page;
	struct aura_pv_caps *caps;
	phys_addr_t pa;
	int ret = -ENODEV;

	aura_pv_hvc(AURA_PV_FUNC_UID, 0, 0, 0, &r0, &r1, &r2, &r3);
	if ((long)r0)
		return (long)r0 == -38 ? -ENODEV : -EPROTO;
	if (r1 != 0x4155524f52502b56ULL || r2 != 0x504b564d2d454c31ULL)
		return -EPROTO;
	aura_pv_hvc(AURA_PV_FUNC_VERSION, 0, 0, 0, &r0, &r1, &r2, &r3);
	if ((long)r0 || r1 != AURA_PV_ABI_MAJOR || r2 != AURA_PV_ABI_MINOR)
		return -EPROTO;

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	caps = page_address(page);
	memset(caps, 0, PAGE_SIZE);
	pa = page_to_phys(page);
	/* Aura's ABI block is exactly 4 KiB even when Linux uses 16 KiB pages. */
	aura_pv_hvc(AURA_PV_FUNC_GET_CAPS, pa, AURA_PV_PAGE_BYTES, 0, &r0, &r1,
		    &r2, &r3);
	if (!(long)r0 && caps->layout_id == AURA_PV_LAYOUT_CAPS &&
	    caps->bytes == sizeof(*caps)) {
		memcpy(&dev->caps, caps, sizeof(dev->caps));
		dev->available = true;
		ret = 0;
	} else {
		ret = -EPROTO;
	}
	__free_page(page);
	return ret;
}

static long aura_pv_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct aura_pv_dev *dev = file->private_data;
	struct aura_pv_run req;
	struct aura_pv_page *page;
	unsigned long r0, r1, r2, r3;
	phys_addr_t pa;
	long status;

	if (!dev->available)
		return -ENODEV;
	if (cmd == AURA_PV_IOC_DISCOVER)
		return copy_to_user((void __user *)arg, &dev->caps,
				    sizeof(dev->caps)) ?
			       -EFAULT :
			       0;
	if (cmd != AURA_PV_IOC_RUN)
		return -ENOTTY;
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (!req.request_seq || req.program_words == 0 ||
	    req.program_words > AURA_PV_MAX_PROGRAM)
		return -EINVAL;

	page = (struct aura_pv_page *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return -ENOMEM;
	page->major = AURA_PV_ABI_MAJOR;
	page->minor = AURA_PV_ABI_MINOR;
	page->bytes = sizeof(*page);
	page->layout_id = AURA_PV_LAYOUT_RUN;
	page->request_seq = req.request_seq;
	page->guest_magic = AURA_PV_GUEST_MAGIC;
	page->program_words = req.program_words;
	page->initial = req.initial;
	memcpy(page->program, req.program, sizeof(req.program));
	pa = virt_to_phys(page);
	aura_pv_hvc(AURA_PV_FUNC_RUN, pa, AURA_PV_PAGE_BYTES, req.request_seq,
		    &r0, &r1, &r2, &r3);
	status = (long)r0;
	if (!status && (page->response_seq != req.request_seq ||
			!(page->output_valid & AURA_PV_OUTPUT_RESULT)))
		status = -EPROTO;
	if (!status && (page->host_sentinel_status & BIT(2)))
		status = -EIO;
	req.status = status;
	req.result = page->result;
	req.response_seq = page->response_seq;
	req.host_sentinel_status = page->host_sentinel_status;
	free_page((unsigned long)page);
	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;
	return status;
}

static int aura_pv_open(struct inode *inode, struct file *file)
{
	file->private_data = &aura_pv;
	return 0;
}

static const struct file_operations aura_pv_fops = {
	.owner = THIS_MODULE,
	.open = aura_pv_open,
	.unlocked_ioctl = aura_pv_ioctl,
	.compat_ioctl = aura_pv_ioctl,
};

static int __init aura_pv_init(void)
{
	struct device_node *node;
	int ret;

	node = of_find_compatible_node(NULL, NULL, "aurora,aura-kvm-pv-0");
	if (!node)
		return -ENODEV;
	of_node_put(node);
	ret = aura_pv_discover(&aura_pv);
	if (ret)
		return ret;
	aura_pv.misc.minor = MISC_DYNAMIC_MINOR;
	aura_pv.misc.name = "aura-pv";
	aura_pv.misc.fops = &aura_pv_fops;
	return misc_register(&aura_pv.misc);
}
module_init(aura_pv_init);

static void __exit aura_pv_exit(void)
{
	misc_deregister(&aura_pv.misc);
}
module_exit(aura_pv_exit);

MODULE_DESCRIPTION("Opt-in Aura PV-KVM v0 diagnostic client");
MODULE_LICENSE("GPL");
