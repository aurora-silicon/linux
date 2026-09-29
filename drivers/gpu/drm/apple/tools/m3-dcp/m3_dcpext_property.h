/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCPEXT_PROPERTY_H
#define M3_DCPEXT_PROPERTY_H
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

/* Allocate on receipt, not on the firmware's announced length. The budget is
 * shared with retained properties; a rejected transfer never becomes an RPC
 * transport error. All sizes and offsets are validated before addition. */
struct m3_property_transfer {
 void *data;
 u32 size, used, capacity, budget;
 bool active;
};
static void m3_property_reset(struct m3_property_transfer *p)
{
 kvfree(p->data);
 *p = (struct m3_property_transfer){};
}
static int m3_property_begin(struct m3_property_transfer *p, u32 count, u32 budget)
{
 bool interrupted = p->active;
 m3_property_reset(p);
 if (interrupted || !count) return -EINVAL;
 if (count - 1 > budget) return -ENOSPC;
 p->size = count - 1;
 p->budget = budget;
 p->active = true;
 return 0;
}
static int m3_property_append(struct m3_property_transfer *p, u32 offset,
                              const void *data, u32 count)
{
 u32 need, capacity;
 void *grown;
 if (!p->active || offset != p->used || count > 4096 || !count ||
     offset > p->size || count > p->size - offset) return -EINVAL;
 need = offset + count;
 if (need > p->capacity) {
  capacity = p->capacity ? p->capacity : 4096;
  while (capacity < need) {
   if (capacity > p->budget / 2) { capacity = p->budget; break; }
   capacity *= 2;
  }
  if (capacity > p->budget) capacity = p->budget;
  grown = kvrealloc(p->data, capacity, GFP_KERNEL);
  if (!grown) return -ENOMEM;
  p->data = grown;
  p->capacity = capacity;
 }
 memcpy(p->data + offset, data, count);
 p->used = need;
 return 0;
}
static int m3_property_complete(const struct m3_property_transfer *p)
{
 return p->active && p->used == p->size ? 0 : -EINVAL;
}
#endif
