// SPDX-License-Identifier: GPL-2.0-only
/* Included by path.c. Preserve reservations while the routers lose power. */
int tb_path_sleep_prepare(struct tb_path *path)
{
	if (!path->activated || path->sleep_hops || path->path_length <= 0 ||
	    path->path_length > TB_PATH_MAX_HOPS)
		return -EINVAL;
	path->sleep_hops = kcalloc(path->path_length, sizeof(*path->sleep_hops), GFP_KERNEL);
	return path->sleep_hops ? 0 : -ENOMEM;
}

int tb_path_sleep(struct tb_path *path)
{
	int i, ret;

	if (!path->sleep_hops || path->sleep_valid)
		return -EINVAL;
	/* Capture the whole path before changing any hop in it. */
	for (i = 0; i < path->path_length; i++) {
		const struct tb_path_hop *hop = &path->hops[i];
		struct tb_regs_hop *saved = &path->sleep_hops[i];

		ret = tb_port_read(hop->in_port, saved, TB_CFG_HOPS, 2 * hop->in_hop_index, 2);
		if (ret)
			return ret;
		if (!saved->enable || saved->out_port != hop->out_port->port ||
		    saved->next_hop != hop->next_hop_index)
			return -EUCLEAN;
		saved->pending = 0;
	}
	/* Even a partial drain must be replayed on abort. Credits stay allocated. */
	path->sleep_valid = true;
	for (i = 0; i < path->path_length; i++) {
		ret = __tb_path_deactivate_hop(path->hops[i].in_port,
					       path->hops[i].in_hop_index, false);
		if (ret)
			return ret;
	}
	return 0;
}

int tb_path_wake(struct tb_path *path)
{
	int i, ret;

	if (!path->sleep_valid)
		return 0;
	if (tb_path_is_invalid(path))
		return -ENODEV;
	/* Restore the absolute credit total; shared ports must not accumulate it again. */
	for (i = path->path_length - 1; i >= 0; i--) {
		struct tb_path_hop *hop = &path->hops[i];
		struct tb_port *port = hop->in_port;

		if (hop->nfc_credits &&
		    (!tb_switch_is_usb4(port->sw) || tb_port_is_null(port) ||
		     tb_port_needs_host_dp_credits(port))) {
			ret = tb_port_write(port, &port->config.nfc_credits,
					    TB_CFG_PORT, ADP_CS_4, 1);
			if (ret)
				return ret;
		}
		if (hop->in_counter_index >= 0) {
			ret = tb_port_clear_counter(port, hop->in_counter_index);
			if (ret)
				return ret;
		}
	}
	for (i = 0; i < path->path_length; i++) {
		const struct tb_path_hop *hop = &path->hops[i];
		struct tb_regs_hop observed;

		/* USB4 requires a path read before its write. */
		ret = tb_port_read(hop->in_port, &observed, TB_CFG_HOPS,
				   2 * hop->in_hop_index, 2);
		if (!ret)
			ret = tb_port_write(hop->in_port, &path->sleep_hops[i], TB_CFG_HOPS,
					    2 * hop->in_hop_index, 2);
		if (ret)
			return ret;
	}
	path->sleep_valid = false;
	return 0;
}

void tb_path_sleep_complete(struct tb_path *path)
{
	kfree(path->sleep_hops);
	path->sleep_hops = NULL;
	path->sleep_valid = false;
}
