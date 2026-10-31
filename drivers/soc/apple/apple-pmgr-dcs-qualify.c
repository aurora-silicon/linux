// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by apple-pmgr-misc.c. Qualification only, never a production ABI. */
#if IS_ENABLED(CONFIG_APPLE_PMGR_DCS_QUALIFY)
/* Existing internal name lookup, built-in diagnostics only, no exported ABI. */
extern struct icc_path *icc_get(struct device *dev, const char *src, const char *dst);
extern const u8 dcs_qualify_overlay_begin[], dcs_qualify_overlay_end[];

static int __init dcs_qualify_dt_init(void)
{
	struct device_node *np __free(device_node) = NULL;
	struct device_node *pmp __free(device_node) = NULL;
	int overlay_id;

	if (!of_machine_is_compatible("apple,j316s") ||
	    !of_machine_is_compatible("apple,t6000"))
		return 0;
	pmp = of_find_compatible_node(NULL, NULL, "apple,t6000-pmp-v2");
	/* Do not disable PMP, unbind a driver, or claim firmware ownership. */
	if (!pmp || of_device_is_available(pmp)) {
		pr_err("DCSQUAL: host ownership not established; leaving DT unchanged\n");
		return -EBUSY;
	}
	np = of_find_node_by_path("/soc/power-management@28e20c000");
	if (!np || !of_device_is_compatible(np, "apple,t6000-pmgr-misc") ||
	    of_property_present(np, "operating-points-v2"))
		return -EINVAL;
	return of_overlay_fdt_apply(dcs_qualify_overlay_begin,
			dcs_qualify_overlay_end - dcs_qualify_overlay_begin, &overlay_id, NULL);
}
arch_initcall(dcs_qualify_dt_init);

static void dcs_qualify_snapshot(struct apple_pmgr_misc *m, const char *phase)
{
	u32 d = readl(m->devices[DEV_DCS].base);
	u32 f = readl(m->devices[DEV_FABRIC].base);
	int drc, frc;

	if (!m->qualify_dir)
		return;
	drc = readl_poll_timeout_atomic(m->devices[DEV_DCS].base, d,
			 ((d >> 4) & 15) == (d & 15), 10, 1000);
	frc = readl_poll_timeout_atomic(m->devices[DEV_FABRIC].base, f,
			 ((f >> 4) & 15) == (f & 15), 10, 1000);

	if (m->qualify_dir)
		dev_info(m->dev, "DCSQUAL %s dcs=%#x fabric=%#x floor=7 saved=%u failed=%u dcs_rc=%d fabric_rc=%d\n",
			 phase, d, f, m->devices[DEV_DCS].active_state, m->dcs_failed, drc, frc);
}

static int dcs_qualify_status_show(struct seq_file *s, void *unused)
{
	struct apple_pmgr_misc *m = s->private;
	u32 d, f;
	unsigned int i;

	guard(mutex)(&m->lock);
	d = readl(m->devices[DEV_DCS].base);
	f = readl(m->devices[DEV_FABRIC].base);
	seq_printf(s, "dcs_raw=%#x desired=%u actual=%u saved=%u floor=7\n",
		   d, d & 15, (d >> 4) & 15, m->devices[DEV_DCS].active_state);
	seq_printf(s, "fabric_raw=%#x desired=%u actual=%u boot=%u holds=%u bw_hold=%u\n",
		   f, f & 15, (f >> 4) & 15, m->devices[DEV_FABRIC].active_state,
		   m->holds, m->dcs_bw_active);
	seq_printf(s, "pmp_owned=%u sleeping=%u failed=%u vote0=%u vote1=%u\n",
		   m->pmp_owned, m->sleeping, m->dcs_failed,
		   READ_ONCE(m->qualify_vote[0]), READ_ONCE(m->qualify_vote[1]));
	for (i = 0; i < m->dcs_num_opps; i++)
		seq_printf(s, "selector=%u raw_state=%u SYNTHETIC_NOT_BANDWIDTH\n",
			   m->dcs_opps[i].bw, m->dcs_opps[i].state);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dcs_qualify_status);

static int dcs_qualify_vote_get(void *data, u64 *value)
{
	struct apple_pmgr_misc *m = data;
	*value = READ_ONCE(m->qualify_vote[0]);
	return 0;
}

static int dcs_qualify_set(struct apple_pmgr_misc *m, unsigned int client, u64 value)
{
	int ret;

	if (value > U32_MAX)
		return -ERANGE;
	/* icc_set_bw takes the provider lock itself. Do not take m->lock here. */
	guard(mutex)(&m->qualify_lock);
	ret = icc_set_bw(m->qualify_path[client], value, value);
	if (!ret)
		WRITE_ONCE(m->qualify_vote[client], value);
	dcs_qualify_snapshot(m, ret ? "vote-error" : "vote");
	return ret;
}

static int dcs_qualify_vote_set(void *data, u64 value)
{
	return dcs_qualify_set(data, 0, value);
}
DEFINE_DEBUGFS_ATTRIBUTE(dcs_qualify_vote_fops, dcs_qualify_vote_get,
			 dcs_qualify_vote_set, "%llu\n");

static int dcs_qualify_vote1_get(void *data, u64 *value)
{
	struct apple_pmgr_misc *m = data;
	*value = READ_ONCE(m->qualify_vote[1]);
	return 0;
}

static int dcs_qualify_vote1_set(void *data, u64 value)
{
	return dcs_qualify_set(data, 1, value);
}
DEFINE_DEBUGFS_ATTRIBUTE(dcs_qualify_vote1_fops, dcs_qualify_vote1_get,
			 dcs_qualify_vote1_set, "%llu\n");

static void dcs_qualify_cleanup(void *data)
{
	struct apple_pmgr_misc *m = data;

	debugfs_remove_recursive(m->qualify_dir);
	icc_put(m->qualify_path[1]);
	icc_put(m->qualify_path[0]);
}

static int dcs_qualify_init(struct apple_pmgr_misc *m)
{
	unsigned int i;
	int ret;

	if (!of_property_read_bool(m->dev->of_node, "aurora,dcs-qualify"))
		return 0;
	if (!m->dcs_data || m->pmp_owned)
		return -EINVAL;
	mutex_init(&m->qualify_lock);
	for (i = 0; i < 2; i++) {
		m->qualify_path[i] = icc_get(m->dev, "t600x-dcs-clients", "t600x-dcs-memory");
		if (IS_ERR(m->qualify_path[i])) {
			ret = PTR_ERR(m->qualify_path[i]);
			m->qualify_path[i] = NULL;
			goto fail;
		}
	}
	/* Retain the boot vote before sync_state can drop boot constraints. */
	m->qualify_vote[0] = m->dcs_opps[m->dcs_num_opps - 1].bw;
	ret = icc_set_bw(m->qualify_path[0], m->qualify_vote[0], m->qualify_vote[0]);
	if (ret)
		goto fail;
	m->qualify_dir = debugfs_create_dir("apple-dcs-qualify", NULL);
	if (IS_ERR_OR_NULL(m->qualify_dir)) {
		ret = m->qualify_dir ? PTR_ERR(m->qualify_dir) : -ENODEV;
		m->qualify_dir = NULL;
		goto fail;
	}
	debugfs_create_file("status", 0400, m->qualify_dir, m, &dcs_qualify_status_fops);
	debugfs_create_file("vote0", 0600, m->qualify_dir, m, &dcs_qualify_vote_fops);
	debugfs_create_file("vote1", 0600, m->qualify_dir, m, &dcs_qualify_vote1_fops);
	dcs_qualify_snapshot(m, "boot-held");
	return devm_add_action_or_reset(m->dev, dcs_qualify_cleanup, m);
fail:
	icc_put(m->qualify_path[1]);
	icc_put(m->qualify_path[0]);
	return ret;
}
#else
static inline int dcs_qualify_init(struct apple_pmgr_misc *m) { return 0; }
static inline void dcs_qualify_snapshot(struct apple_pmgr_misc *m, const char *phase) {}
#endif
