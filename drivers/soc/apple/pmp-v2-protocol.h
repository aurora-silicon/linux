/* SPDX-License-Identifier: GPL-2.0-only */
/* Passive PMP-v2 protocol core. The adapter owns DMA visibility/lifetime. */
#ifndef APPLE_PMP_V2_PROTOCOL_H
#define APPLE_PMP_V2_PROTOCOL_H

#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/ktime.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define PMP_V2_ADDR_MASK GENMASK_ULL(47, 0)
#define PMP_V2_MAX_BUFFERS 256
#define PMP_V2_MAX_BYTES SZ_16M
#define PMP_V2_MAX_ENTRIES 1024
#define PMP_V2_MAX_VALUES SZ_1M

struct pmp_v2_buffer {
	struct list_head link;
	u64 address;
	void *data;
	void *private;
	size_t size, allocation_size;
	bool application;
};

struct pmp_v2_entry {
	struct list_head link;
	char name[48], format[8], unit[8];
	u16 id;
	u8 flags;
	u32 size;
	bool default_from_adt;
	u64 updates, registered_ns, last_update_ns;
	u8 value[];
};

struct pmp_v2_protocol;

struct pmp_v2_protocol_ops {
	int (*allocate)(struct pmp_v2_protocol *p, struct pmp_v2_buffer *buffer);
	void (*release)(struct pmp_v2_protocol *p, struct pmp_v2_buffer *buffer);
	const void *(*property)(struct pmp_v2_protocol *p, const char *name, int *size);
};

struct pmp_v2_protocol {
	const struct pmp_v2_protocol_ops *ops;
	void *private;
	struct list_head buffers, entries;
	struct pmp_v2_buffer *first, *second;
	size_t allocated, value_bytes;
	unsigned int buffer_count, entry_count;
	bool failed;
};

static void pmp_v2_protocol_init(struct pmp_v2_protocol *p,
				 const struct pmp_v2_protocol_ops *ops, void *private)
{
	memset(p, 0, sizeof(*p));
	p->ops = ops;
	p->private = private;
	INIT_LIST_HEAD(&p->buffers);
	INIT_LIST_HEAD(&p->entries);
}

static struct pmp_v2_buffer *pmp_v2_find(struct pmp_v2_protocol *p, u64 address,
				      size_t minimum)
{
	struct pmp_v2_buffer *b;

	list_for_each_entry(b, &p->buffers, link)
		if (b->application && b->address == address && b->size >= minimum)
			return b;
	return NULL;
}

static struct pmp_v2_buffer *pmp_v2_allocate(struct pmp_v2_protocol *p, size_t size,
					  bool application)
{
	struct pmp_v2_buffer *b, *old;
	u64 end;
	int ret;

	if (p->failed || !size || size > 0xffffff)
		return ERR_PTR(-EINVAL);
	if (p->buffer_count >= PMP_V2_MAX_BUFFERS || size > PMP_V2_MAX_BYTES - p->allocated)
		return ERR_PTR(-ENOMEM);
	b = kzalloc_obj(*b);
	if (!b)
		return ERR_PTR(-ENOMEM);
	b->size = size;
	b->application = application;
	ret = p->ops->allocate(p, b);
	if (ret) {
		kfree(b);
		return ERR_PTR(ret);
	}
	/* The adapter must allocate a real mapped, aligned, full-width IOVA. */
	if (!b->data || !b->address || !IS_ALIGNED(b->address, SZ_16K) ||
	    b->allocation_size < size || b->allocation_size > PMP_V2_MAX_BYTES - p->allocated ||
	    check_add_overflow(b->address, (u64)b->allocation_size, &end) ||
	    end - 1 > PMP_V2_ADDR_MASK) {
		ret = -ERANGE;
		goto release_unpublished;
	}
	list_for_each_entry(old, &p->buffers, link)
		if (b->address < old->address + old->allocation_size && old->address < end) {
			ret = -EEXIST;
			goto release_unpublished;
		}
	list_add_tail(&b->link, &p->buffers);
	p->allocated += b->allocation_size;
	p->buffer_count++;
	return b;
release_unpublished:
	p->ops->release(p, b);
	kfree(b);
	return ERR_PTR(ret);
}

static void pmp_v2_release(struct pmp_v2_protocol *p, struct pmp_v2_buffer *b)
{
	list_del(&b->link);
	p->allocated -= b->allocation_size;
	p->buffer_count--;
	p->ops->release(p, b);
	kfree(b);
}

static struct pmp_v2_entry *pmp_v2_entry_find(struct pmp_v2_protocol *p, u16 id)
{
	struct pmp_v2_entry *entry;

	list_for_each_entry(entry, &p->entries, link)
		if (entry->id == id)
			return entry;
	return NULL;
}

static int pmp_v2_register(struct pmp_v2_protocol *p, u64 address, u64 *reply)
{
	struct pmp_v2_buffer *b = pmp_v2_find(p, address, 0x51);
	struct pmp_v2_entry *entry, *old;
	const void *value;
	const u8 *d;
	u64 size, id;
	int property_size = 0;
	bool from_firmware;

	if (!b || b == p->first || b == p->second)
		return -EINVAL;
	d = b->data;
	if (!d[0] || !memchr(d, 0, 48) || !memchr(d + 0x30, 0, 8) ||
	    !memchr(d + 0x38, 0, 8))
		return -EINVAL;
	id = get_unaligned_le64(d + 0x48);
	if (id > U16_MAX)
		return -ERANGE;
	list_for_each_entry(old, &p->entries, link)
		/* Firmware repeats e.g. temp-sensor-version for distinct sensor IDs. */
		if (old->id == id)
			return -EEXIST;
	size = get_unaligned_le64(d + 0x40);
	from_firmware = size != 0;
	if (from_firmware) {
		if (size > U32_MAX || size > p->first->size)
			return -EOVERFLOW;
		value = p->first->data;
	} else {
		value = p->ops->property(p, d, &property_size);
		if (!value || property_size <= 0 || property_size > p->first->size)
			size = 0;
		else
			size = property_size;
	}
	*reply = (0x33ULL << 48) | size;
	if (!size)
		return 0;
	if (p->entry_count >= PMP_V2_MAX_ENTRIES || size > PMP_V2_MAX_VALUES - p->value_bytes)
		return -ENOMEM;
	entry = kzalloc(struct_size(entry, value, size), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	memcpy(entry->name, d, sizeof(entry->name));
	memcpy(entry->format, d + 0x30, sizeof(entry->format));
	memcpy(entry->unit, d + 0x38, sizeof(entry->unit));
	entry->id = id;
	entry->flags = d[0x50];
	entry->size = size;
	entry->default_from_adt = !from_firmware;
	entry->registered_ns = ktime_get_boottime_ns();
	memcpy(entry->value, value, size);
	if (!from_firmware)
		memcpy(p->first->data, value, size);
	list_add_tail(&entry->link, &p->entries);
	p->entry_count++;
	p->value_bytes += size;
	return 0;
}

/* Serialized caller: sync coherent data before this, publish ACK afterwards.
 * Error means quarantine. It does not free any previously published buffer.
 */
static int pmp_v2_handle(struct pmp_v2_protocol *p, u64 word, u64 *reply)
{
	u64 address = word & PMP_V2_ADDR_MASK;
	struct pmp_v2_buffer *b;
	struct pmp_v2_entry *entry;
	u8 op = word >> 48;
	int ret = -EINVAL;

	if (p->failed || word >> 56)
		goto fault;
	switch (op) {
	case 0x10:
		/* Admission requires absent pio-reg-index. */
		*reply = 0x11ULL << 48;
		return 0;
	case 0x12:
		b = pmp_v2_allocate(p, word & 0xffffff, true);
		if (IS_ERR(b)) {
			ret = PTR_ERR(b);
			if (ret == -ENOMEM) {
				*reply = 0x13ULL << 48;
				return 0;
			}
			goto fault;
		}
		*reply = (0x13ULL << 48) | b->address;
		return 0;
	case 0x14:
		b = pmp_v2_find(p, address, 1);
		if (b && (b == p->first || b == p->second))
			goto fault;
		if (b)
			pmp_v2_release(p, b);
		*reply = 0x15ULL << 48;
		return 0;
	case 0x30:
		b = pmp_v2_find(p, address, 16);
		if (!b || p->first || p->second)
			goto fault;
		p->first = pmp_v2_find(p, get_unaligned_le64(b->data), 1);
		p->second = pmp_v2_find(p, get_unaligned_le64(b->data + 8), 1);
		if (!p->first || !p->second || p->first == p->second ||
		    b == p->first || b == p->second)
			goto fault;
		*reply = 0x31ULL << 48;
		return 0;
	}
	if (!p->first || !p->second)
		goto fault;
	switch (op) {
	case 0x32:
		ret = pmp_v2_register(p, address, reply);
		if (ret)
			goto fault;
		return 0;
	case 0x34:
	case 0x36:
		entry = pmp_v2_entry_find(p, word & 0xffff);
		*reply = ((u64)(op + 1) << 48) | (entry ? entry->size : 0);
		if (!entry)
			return 0;
		if (op == 0x34) {
			list_del(&entry->link);
			p->value_bytes -= entry->size;
			p->entry_count--;
			kfree(entry);
		} else {
			if (entry->size > p->first->size)
				goto fault;
			memcpy(entry->value, p->first->data, entry->size);
			/* Host receipt evidence, not a sensor generation or freshness proof. */
			entry->updates++;
			entry->last_update_ns = ktime_get_boottime_ns();
		}
		return 0;
	}
fault:
	p->failed = true;
	return ret;
}

#endif
