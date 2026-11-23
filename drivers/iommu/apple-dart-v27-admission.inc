/* SPDX-License-Identifier: GPL-2.0-only */
/* Track real clients from probe; lease acquisition still validates every owner. */
static bool apple_dart_v27_provider(struct device *dev, resource_size_t base)
{
	static const resource_size_t ids[] = {
		0x48a340000ULL, 0x48a300000ULL, 0x2b2340000ULL, 0x2b2300000ULL,
		0x392340000ULL, 0x392300000ULL, 0x302340000ULL, 0x302300000ULL,
		0x306340000ULL, 0x306300000ULL,
	};
	unsigned int i;

	/* Where the T6050 sleep is described: its display DARTs are leased over it. */
	if (!IS_ENABLED(CONFIG_APPLE_SOC_SLEEP) || !apple_t6050_sleep_described() ||
	    !of_device_is_compatible(dev->of_node, "apple,t6050-dart"))
		return false;
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		if (base == ids[i])
			return true;
	return false;
}

static int apple_dart_v27_publish(struct apple_dart *d,
	struct apple_dart_stream_map *map, struct device *dev, unsigned int sid, bool fresh)
{
	guard(spinlock_irqsave)(&d->lock);
	if (d->v27_provider && (d->m5_sleep_owner || d->v27_lost))
		return -EBUSY;
	if (fresh) {
		map->dart = d;
		if (d->v27_provider) {
			struct apple_dart_stream_map *old, *tmp;

			/*
			 * A device has one stream map per DART. One already listed for this
			 * device is from a configuration that was dropped half way, when a
			 * later DART of its iommus was not there yet: the core frees the
			 * device's IOMMU data then without a release, once per attempt. It
			 * leaves the list here, or every attempt would count as a client.
			 */
			list_for_each_entry_safe(old, tmp, &d->v27_clients, v27_link)
				if (old->v27_dev == dev)
					list_del_init(&old->v27_link);
			map->v27_dev = dev;
			list_add_tail(&map->v27_link, &d->v27_clients);
		}
	}
	set_bit(sid, map->sidmap);
	return 0;
}

static void apple_dart_v27_client_remove(struct apple_dart_stream_map *map)
{
	struct apple_dart *d = map->dart;

	if (!d->v27_provider)
		return;
	guard(spinlock_irqsave)(&d->lock);
	if (d->m5_sleep_owner)
		d->v27_lost = true;
	list_del(&map->v27_link);
}

static int apple_dart_v27_enter(struct apple_dart *d, bool unmap)
{
	unsigned int n;
	unsigned long flags;

	if (!d || !d->v27_provider)
		return 0;
	for (n = 0; n < 10000; n++) {
		spin_lock_irqsave(&d->lock, flags);
		if (!unmap && (d->m5_sleep_owner || d->v27_lost)) {
			spin_unlock_irqrestore(&d->lock, flags);
			return -EBUSY;
		}
		if (!d->v27_restoring) {
			atomic_inc(&d->v27_ops);
			spin_unlock_irqrestore(&d->lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&d->lock, flags);
		udelay(1);
	}
	/* Never let a caller free backing while provider restore is uncertain. */
	panic("DCP DART unmap raced an unresolved provider restore; DMA retained");
}

static void apple_dart_v27_leave(struct apple_dart *d)
{
	if (d && d->v27_provider)
		atomic_dec_return_release(&d->v27_ops);
}

static int apple_dart_v27_cfg_enter(struct apple_dart_master_cfg *cfg)
{
	int i, ret;

	for (i = 0; i < MAX_DARTS_PER_DEVICE && cfg->stream_maps[i].dart; i++) {
		ret = apple_dart_v27_enter(cfg->stream_maps[i].dart, false);
		if (ret) {
			while (i--)
				apple_dart_v27_leave(cfg->stream_maps[i].dart);
			return ret;
		}
	}
	return 0;
}

static void apple_dart_v27_cfg_leave(struct apple_dart_master_cfg *cfg)
{
	int i;

	for (i = 0; i < MAX_DARTS_PER_DEVICE && cfg->stream_maps[i].dart; i++)
		apple_dart_v27_leave(cfg->stream_maps[i].dart);
}

static void apple_dart_v27_domain_hold(struct apple_dart_domain *dd)
{
	unsigned int i;

	for (i = 0; i < MAX_DARTS_PER_DEVICE && dd->stream_maps[i].dart; i++)
		apple_dart_v27_enter(dd->stream_maps[i].dart, true);
}

static void apple_dart_v27_domain_drop(struct apple_dart_domain *dd)
{
	unsigned int i;

	for (i = 0; i < MAX_DARTS_PER_DEVICE && dd->stream_maps[i].dart; i++)
		apple_dart_v27_leave(dd->stream_maps[i].dart);
}
