// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple RTKit: hand a running co-processor session from the kernel that loads
 * a hibernation image (the boot kernel) to the kernel restored from it.
 *
 * A co-processor that the boot kernel started keeps running across the jump
 * into the image, but everything the boot kernel knew about it is overwritten
 * by the image. The few facts the restored kernel needs to take the session
 * over (where the firmware's shared buffers are, how far a receive ring got)
 * are left in __nosavedata: the image never saves those pages, so after the
 * restore they still hold what the boot kernel wrote, provided both kernels
 * are the same build loaded at the same physical address. Each record carries
 * its own physical address and a checksum, so a record from a kernel loaded
 * elsewhere, or no record at all, reads as absent.
 *
 * The storage must be in the kernel image, so this only works with RTKit
 * built in. A taken record is cleared.
 */

#include <linux/export.h>
#include <linux/notifier.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <asm/memory.h>

#if IS_BUILTIN(CONFIG_APPLE_RTKIT) && IS_ENABLED(CONFIG_HIBERNATION)

#define HANDOFF_MAGIC	0x52544b48 /* "RTKH" */
#define HANDOFF_VERSION	1
#define HANDOFF_SLOTS	4
#define HANDOFF_TAG	32

struct apple_rtkit_hib_slot {
	u32 magic;
	u32 version;
	u64 self_phys;
	char tag[HANDOFF_TAG];
	u32 len;
	u32 sum;
	u8 data[APPLE_RTKIT_HIB_HANDOFF_MAX];
};

static struct apple_rtkit_hib_slot handoff[HANDOFF_SLOTS] __nosavedata;
static DEFINE_SPINLOCK(handoff_lock);
static bool restoring;

/* FNV-1a over everything but the checksum itself. */
static u32 handoff_sum(const struct apple_rtkit_hib_slot *s)
{
	const u8 *p = (const u8 *)s;
	size_t off = offsetof(struct apple_rtkit_hib_slot, sum);
	u32 h = 0x811c9dc5;
	size_t i;

	for (i = 0; i < sizeof(*s); i++) {
		if (i >= off && i < off + sizeof(s->sum))
			continue;
		h ^= p[i];
		h *= 0x01000193;
	}
	return h;
}

static bool handoff_valid(const struct apple_rtkit_hib_slot *s)
{
	return s->magic == HANDOFF_MAGIC && s->version == HANDOFF_VERSION &&
	       s->self_phys == __pa_symbol(s) &&
	       s->len <= APPLE_RTKIT_HIB_HANDOFF_MAX &&
	       s->tag[HANDOFF_TAG - 1] == '\0' && s->sum == handoff_sum(s);
}

static struct apple_rtkit_hib_slot *handoff_find(const char *tag)
{
	int i;

	for (i = 0; i < HANDOFF_SLOTS; i++)
		if (handoff_valid(&handoff[i]) && !strcmp(handoff[i].tag, tag))
			return &handoff[i];
	return NULL;
}

bool apple_rtkit_hib_restoring(void)
{
	return READ_ONCE(restoring);
}
EXPORT_SYMBOL_GPL(apple_rtkit_hib_restoring);

int apple_rtkit_hib_handoff_put(const char *tag, const void *data, size_t len)
{
	struct apple_rtkit_hib_slot *s;
	unsigned long flags;
	int i, ret = 0;

	if (len > APPLE_RTKIT_HIB_HANDOFF_MAX || strlen(tag) >= HANDOFF_TAG)
		return -EINVAL;

	spin_lock_irqsave(&handoff_lock, flags);
	s = handoff_find(tag);
	for (i = 0; !s && i < HANDOFF_SLOTS; i++)
		if (!handoff_valid(&handoff[i]))
			s = &handoff[i];
	if (s) {
		memset(s, 0, sizeof(*s));
		s->magic = HANDOFF_MAGIC;
		s->version = HANDOFF_VERSION;
		s->self_phys = __pa_symbol(s);
		strscpy(s->tag, tag, sizeof(s->tag));
		s->len = len;
		memcpy(s->data, data, len);
		s->sum = handoff_sum(s);
	} else {
		ret = -ENOSPC;
	}
	spin_unlock_irqrestore(&handoff_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_rtkit_hib_handoff_put);

int apple_rtkit_hib_handoff_take(const char *tag, void *data, size_t len)
{
	struct apple_rtkit_hib_slot *s;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&handoff_lock, flags);
	s = handoff_find(tag);
	if (!s) {
		ret = -ENOENT;
	} else if (s->len != len) {
		ret = -EINVAL;
	} else {
		memcpy(data, s->data, len);
		ret = len;
	}
	if (s)
		memset(s, 0, sizeof(*s));
	spin_unlock_irqrestore(&handoff_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_rtkit_hib_handoff_take);

void apple_rtkit_hib_handoff_drop(const char *tag)
{
	struct apple_rtkit_hib_slot *s;
	unsigned long flags;

	spin_lock_irqsave(&handoff_lock, flags);
	s = handoff_find(tag);
	if (s)
		memset(s, 0, sizeof(*s));
	spin_unlock_irqrestore(&handoff_lock, flags);
}
EXPORT_SYMBOL_GPL(apple_rtkit_hib_handoff_drop);

static int apple_rtkit_hib_notify(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	switch (action) {
	case PM_RESTORE_PREPARE:
		WRITE_ONCE(restoring, true);
		break;
	case PM_POST_RESTORE:
		/* The image was not restored: this kernel goes on. */
		WRITE_ONCE(restoring, false);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block apple_rtkit_hib_nb = {
	.notifier_call = apple_rtkit_hib_notify,
};

static int __init apple_rtkit_hib_init(void)
{
	/*
	 * A kernel restored from an image runs this only on its own first
	 * boot, and the flag is saved false in the image: in the restored
	 * kernel it reads false.
	 */
	return register_pm_notifier(&apple_rtkit_hib_nb);
}
core_initcall(apple_rtkit_hib_init);

#else

bool apple_rtkit_hib_restoring(void)
{
	return false;
}
EXPORT_SYMBOL_GPL(apple_rtkit_hib_restoring);

int apple_rtkit_hib_handoff_put(const char *tag, const void *data, size_t len)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(apple_rtkit_hib_handoff_put);

int apple_rtkit_hib_handoff_take(const char *tag, void *data, size_t len)
{
	return -ENOENT;
}
EXPORT_SYMBOL_GPL(apple_rtkit_hib_handoff_take);

void apple_rtkit_hib_handoff_drop(const char *tag)
{
}
EXPORT_SYMBOL_GPL(apple_rtkit_hib_handoff_drop);

#endif
