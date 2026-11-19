// SPDX-License-Identifier: GPL-2.0-only
/* Included after the MMIO fixture and before the actual ownership methods. */
#include <limits.h>
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
/* Single-threaded fixture: model publication without CPU barriers. */
#define smp_store_release(p, v) (*(p) = (v))
#define dma_wmb() \
	do {      \
	} while (0)
#define spin_lock_irqsave(lock, flags) \
	do {                           \
		(void)(lock);          \
		(flags) = 0;           \
	} while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do {                                \
		(void)(lock);               \
		(void)(flags);              \
	} while (0)
#define APPLE_PS_PROT_SLICE(n) (0x100 + 0x40 * (n))
#define APPLE_PS_PROT_SLICE_ON BIT(4)
#define APPLE_DART_WRITER_COUNTED 1
#define APPLE_DART_WRITER_CLOSED 2
#define scoped_guard(type, lock) for (int _g = ((void)(lock), 1); _g; _g = 0)
#define bitmap_fill(map, n) ((void)sizeof(n), memset((map), 0xff, sizeof(map)))
#define PM_SUSPEND_MEM 3
static int pm_suspend_target_state = PM_SUSPEND_MEM;
struct device {
	int refs;
};
struct device_link {};
struct regmap {
	u32 value;
	int fail, fail_read;
	unsigned int offset;
};
struct apple_dart_sleep {
	int result;
};
struct apple_dart_cio;
struct apple_dart_stream_map;
struct apple_dart_hw {
	int (*invalidate_tlb)(struct apple_dart_stream_map *map);
};
struct apple_dart {
	struct device *dev;
	void *regs;
	unsigned int num_streams, sleep_writers;
	bool managed_sids, sleep_closed, commands_gated, cio_active;
	int managed_lock, lock;
	const struct apple_dart_hw *hw;
	struct apple_dart_cio *cio;
	struct apple_dart_sleep *sleep_record;
};
struct apple_dart_stream_map {
	struct apple_dart *dart;
	unsigned long sidmap[4];
};
static unsigned int invalidations, sleeps;
static bool fail_flush;
/* A flush issues commands, so it needs them reopened; it may precede the opening. */
static int model_invalidate(struct apple_dart_stream_map *map)
{
	assert(!map->dart->commands_gated);
	if (fail_flush)
		return -ETIMEDOUT;
	invalidations++;
	return 0;
}
/* The drain of memory-only writers: a hook lets one finish meanwhile. */
static void (*on_sleep)(void);
static void usleep_range(unsigned long min, unsigned long max)
{
	(void)min;
	(void)max;
	sleeps++;
	if (on_sleep)
		on_sleep();
}
static void (*on_read)(void);
static const struct apple_dart_hw model_hw = { .invalidate_tlb = model_invalidate };
static int pm_runtime_resume_and_get(struct device *dev)
{
	dev->refs++;
	return 0;
}
static void pm_runtime_put(struct device *dev)
{
	assert(dev->refs > 0);
	dev->refs--;
}
static int regmap_read(struct regmap *map, unsigned int off, u32 *value)
{
	assert(off == map->offset);
	if (on_read)
		on_read();
	if (map->fail_read)
		return -EIO;
	*value = map->value;
	return 0;
}
static int regmap_write(struct regmap *map, unsigned int off, u32 value)
{
	assert(off == map->offset);
	if (map->fail)
		return -EIO;
	map->value = value;
	return 0;
}
static int regmap_update_bits(struct regmap *map, unsigned int off, u32 mask,
			      u32 value)
{
	assert(off == map->offset && mask == BIT(4));
	if (map->fail)
		return -EIO;
	map->value = (map->value & ~mask) | value;
	return 0;
}
static struct apple_dart dart;
static struct device provider, owner;
static struct regmap gapf;
static int apple_dart_quiesce_commands(struct device *dev)
{
	assert(dev == &provider && dart.sleep_closed && !dart.sleep_writers);
	dart.commands_gated = true;
	return 0;
}

/* Arguments are evaluated: a diagnostic's register reads still happen. */
static inline void dev_log(void *dev, const char *fmt, ...)
{
	(void)dev;
	(void)fmt;
}
#define dev_err(dev, ...) dev_log(dev, __VA_ARGS__)
#define dev_info(dev, ...) dev_log(dev, __VA_ARGS__)
/* ACTUAL_OWNER */

static struct apple_dart_cio state;
static void reset_owner(unsigned int slice, bool partitions)
{
	reset(partitions ? 28 : 256, partitions);
	provider.refs = owner.refs = 0;
	/* A live DART: its slice enabled, protection on. */
	gapf = (struct regmap){ .offset = APPLE_PS_PROT_SLICE(slice),
				.value = BIT(1) | BIT(4) | BIT(7) };
	/* PCIe-C's software allocator covers 64 of the 256 hardware SIDs. */
	dart = (struct apple_dart){ .regs = regs,
				    .num_streams = partitions ? 28 : 64,
				    .dev = &provider,
				    .hw = &model_hw,
				    .cio = &state };
	state = (struct apple_dart_cio){ .dart = &dart,
					 .owner = &owner,
					 .gapf = &gapf,
					 .slice = slice };
	invalidations = sleeps = 0;
	fail_flush = false;
	on_sleep = on_read = NULL;
}
static struct apple_dart dart;
/* A writer that updates page tables in memory while the DART is closed. */
static void closed_writer(void)
{
	int gate = apple_dart_sleep_writer_get(&dart);

	assert(gate == APPLE_DART_WRITER_CLOSED);
	apple_dart_sleep_writer_put(&dart, gate);
}
static void finish_writer(void)
{
	apple_dart_sleep_writer_put(&dart, APPLE_DART_WRITER_CLOSED);
	on_sleep = NULL;
}
int main(void)
{
	unsigned int cases = 0;
	const unsigned int slices[] = { 14, 15, 18, 19 };

	for (unsigned int i = 0; i < ARRAY_SIZE(slices); i++) {
		reset_owner(slices[i], i % 2);
		for (unsigned int cycle = 0; cycle < 3; cycle++) {
			assert(!apple_dart_cio_prepare(&state) &&
			       provider.refs == 1 && dart.cio_active);
			assert(apple_dart_cio_prepare(&state) == -EBUSY &&
			       provider.refs == 1);
			assert(!apple_dart_cio_off(&state) && state.changed &&
			       !state.restored);
			assert(state.saved.streams == (i % 2 ? 28 : 256));
			assert(dart.commands_gated && dart.sleep_closed &&
			       gapf.value == (BIT(1) | BIT(7)));
			lose_power();
			assert(!apple_dart_cio_on(&state) && state.restored);
			assert(!dart.commands_gated && !dart.sleep_closed);
			assert(gapf.value == (BIT(1) | BIT(4) | BIT(7)));
			check_restored(&state.saved);
			unsigned int before = writes;

			assert(!apple_dart_cio_on(&state) && writes == before);
			assert(apple_dart_cio_off(&state) == -EINVAL);
			assert(!apple_dart_cio_finish(&state) &&
			       !dart.cio_active && !provider.refs);
			assert(apple_dart_cio_on(&state) == -EINVAL);
			assert(apple_dart_cio_finish(&state) == -EINVAL);
			cases++;
		}
	}
	reset_owner(15, true);
	assert(!apple_dart_cio_prepare(&state));
	assert(!apple_dart_cio_on(&state));
	assert(!apple_dart_cio_finish(&state) && !reads && !writes);
	cases++;
	assert(!apple_dart_cio_prepare(&state));
	assert(apple_dart_sleep_writer_get(&dart) == APPLE_DART_WRITER_COUNTED);
	assert(apple_dart_cio_off(&state) == -EBUSY && !reads && !writes);
	apple_dart_sleep_writer_put(&dart, APPLE_DART_WRITER_COUNTED);
	assert(!apple_dart_cio_finish(&state));
	cases++;
	for (unsigned int fault = 0; fault < 4; fault++) {
		reset_owner(14, false);
		assert(!apple_dart_cio_prepare(&state));
		if (fault == 0)
			gapf.fail_read = 1;
		if (fault == 1)
			gapf.value &= ~BIT(1);
		if (fault == 2)
			busy = 1;
		if (fault == 3)
			regs[2] = 0x204;
		assert(apple_dart_cio_off(&state) < 0);
		assert(!writes && !dart.commands_gated && !dart.sleep_closed &&
		       !state.changed);
		assert(!apple_dart_cio_finish(&state) && !provider.refs);
		cases++;
	}
	reset_owner(19, true);
	assert(!apple_dart_cio_prepare(&state));
	gapf.fail = 1;
	assert(apple_dart_cio_off(&state) == -EIO);
	assert(state.changed && dart.sleep_closed && dart.commands_gated);
	assert(apple_dart_cio_on(&state) == -EIO && !state.restored);
	gapf.fail = 0;
	ntrace = 0;
	assert(!apple_dart_cio_on(&state));
	check_restored(&state.saved);
	assert(!apple_dart_cio_finish(&state) && !provider.refs);
	cases++;
	reset_owner(18, false);
	assert(!apple_dart_cio_prepare(&state));
	assert(!apple_dart_cio_off(&state));
	lose_power();
	drop = 0x1400;
	assert(apple_dart_cio_on(&state) == -EIO && !state.restored);
	assert(dart.sleep_closed && dart.commands_gated && provider.refs == 1);
	drop = ~0U;
	assert(!apple_dart_cio_on(&state) && !apple_dart_cio_finish(&state));
	cases++;
	reset_owner(15, true);
	assert(!apple_dart_cio_prepare(&state));
	assert(!apple_dart_cio_off(&state));
	/* A map while closed goes to memory and is flushed once it reopens. */
	assert(apple_dart_sleep_writer_get(&dart) == APPLE_DART_WRITER_CLOSED &&
	       state.tlb_stale && !dart.sleep_writers && state.closed_writers == 1);
	apple_dart_sleep_writer_put(&dart, APPLE_DART_WRITER_CLOSED);
	lose_power();
	assert(!invalidations && !apple_dart_cio_on(&state) && invalidations == 1);
	assert(!state.tlb_stale && !apple_dart_cio_finish(&state));
	cases++;
	/* Without such a map, reopening issues no extra flush. */
	reset_owner(18, false);
	assert(!apple_dart_cio_prepare(&state) && !apple_dart_cio_off(&state));
	lose_power();
	assert(!apple_dart_cio_on(&state) && !invalidations);
	assert(!apple_dart_cio_finish(&state));
	cases++;
	/* Not restored: the transaction ends, the DART stays closed, sleeps are refused. */
	reset_owner(14, false);
	assert(!apple_dart_cio_prepare(&state) && !apple_dart_cio_off(&state));
	assert(apple_dart_cio_finish(&state) == -EIO && state.dead);
	assert(!dart.cio_active && !provider.refs);
	assert(dart.sleep_closed && dart.commands_gated);
	closed_writer();
	assert(apple_dart_cio_prepare(&state) == -EIO && !provider.refs);
	/* Still closed once its owner is gone. */
	state.owner = NULL;
	closed_writer();
	cases++;
	/* A memory-only writer still running at reopening is waited for, then flushed. */
	reset_owner(19, true);
	assert(!apple_dart_cio_prepare(&state) && !apple_dart_cio_off(&state));
	assert(apple_dart_sleep_writer_get(&dart) == APPLE_DART_WRITER_CLOSED);
	lose_power();
	on_sleep = finish_writer;
	assert(!apple_dart_cio_on(&state) && sleeps == 1 && invalidations == 1);
	assert(!dart.sleep_closed && !state.closed_writers && !state.tlb_stale);
	assert(!apple_dart_cio_finish(&state));
	cases++;
	/* One that never finishes keeps it closed; the transaction then ends dead. */
	reset_owner(15, true);
	assert(!apple_dart_cio_prepare(&state) && !apple_dart_cio_off(&state));
	assert(apple_dart_sleep_writer_get(&dart) == APPLE_DART_WRITER_CLOSED);
	lose_power();
	assert(apple_dart_cio_on(&state) == -EBUSY && !state.restored && dart.sleep_closed);
	assert(apple_dart_cio_finish(&state) == -EIO && state.dead);
	apple_dart_sleep_writer_put(&dart, APPLE_DART_WRITER_CLOSED);
	cases++;
	/* A flush that fails keeps it closed too. */
	reset_owner(14, false);
	assert(!apple_dart_cio_prepare(&state) && !apple_dart_cio_off(&state));
	closed_writer();
	lose_power();
	fail_flush = true;
	assert(apple_dart_cio_on(&state) == -ETIMEDOUT && dart.sleep_closed);
	assert(apple_dart_cio_finish(&state) == -EIO && state.dead);
	cases++;
	/* A snapshot that fails after a memory-only update reopens with a flush. */
	reset_owner(18, false);
	assert(!apple_dart_cio_prepare(&state));
	regs[2] = 0x204;
	on_read = closed_writer;
	assert(apple_dart_cio_off(&state) < 0 && !state.changed);
	assert(!dart.sleep_closed && invalidations == 1 && !state.dead);
	on_read = NULL;
	assert(!apple_dart_cio_finish(&state));
	cases++;
	/* A display firmware left protection off: it is turned on before the snapshot. */
	reset_owner(5, false);
	gapf.value = BIT(1) | BIT(7);
	assert(!apple_dart_cio_prepare(&state) && !apple_dart_cio_off(&state));
	assert(gapf.value == (BIT(1) | BIT(7)));
	lose_power();
	/* After the sleep the slice word reads zero: it is written back. */
	gapf.value = 0;
	assert(!apple_dart_cio_on(&state) && gapf.value == (BIT(1) | BIT(4) | BIT(7)));
	assert(!apple_dart_cio_finish(&state));
	cases++;
	printf("PASS %u CIO DART ownership cases\n", cases);
}
