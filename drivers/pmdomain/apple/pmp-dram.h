/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_PMP_DRAM_H
#define _APPLE_PMP_DRAM_H

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/string.h>

#define APPLE_PMP_DRAM_MAX_STATES 32

struct apple_pmp_dram {
	const char *type;
	u32 width;
	u32 state_count;
	u64 max_rate;
	u64 peak_bandwidth;
	u64 bandwidth_class;
};

/*
 * Read metadata only: never register these OPPs with a clock/voltage provider.
 * Unknown lower-state rates are deliberately absent. Firmware state IDs, not
 * child order or frequency sorting, identify the entries.
 */
static inline int apple_pmp_dram_parse(const struct fwnode_handle *node,
				       struct apple_pmp_dram *dram)
{
	struct apple_pmp_dram parsed = {};
	struct fwnode_reference_args ref;
	struct fwnode_handle *opp;
	u64 rates[APPLE_PMP_DRAM_MAX_STATES] = {};
	u64 seen = 0, peak;
	u32 level;
	unsigned int i;
	int ret;

	/* Old DTs with no memory description retain the existing interface. */
	if (!fwnode_property_present(node, "apple,dram-type") &&
	    !fwnode_property_present(node, "apple,data-bus-width") &&
	    !fwnode_property_present(node, "apple,dcs-state-count") &&
	    !fwnode_property_present(node, "apple,peak-bandwidth-bytes-per-second") &&
	    !fwnode_property_present(node, "apple,bandwidth-class-bytes-per-second") &&
	    !fwnode_property_present(node, "operating-points-v2"))
		return -ENODATA;

	if (fwnode_property_read_string(node, "apple,dram-type", &parsed.type) ||
	    strcmp(parsed.type, "lpddr5") ||
	    fwnode_property_read_u32(node, "apple,data-bus-width", &parsed.width) ||
	    !parsed.width || parsed.width % 8 ||
	    fwnode_property_read_u32(node, "apple,dcs-state-count", &parsed.state_count) ||
	    !parsed.state_count || parsed.state_count > APPLE_PMP_DRAM_MAX_STATES ||
	    fwnode_property_read_u64(node, "apple,peak-bandwidth-bytes-per-second",
				     &parsed.peak_bandwidth) ||
	    fwnode_property_read_u64(node, "apple,bandwidth-class-bytes-per-second",
				     &parsed.bandwidth_class) || !parsed.bandwidth_class)
		return -EINVAL;

	ret = fwnode_property_get_reference_args(node, "operating-points-v2", NULL, 0, 0, &ref);
	if (ret)
		return ret;
	if (fwnode_property_match_string(ref.fwnode, "compatible",
					 "operating-points-v2-apple-dram") != 0) {
		ret = -EINVAL;
		goto out;
	}
	fwnode_for_each_available_child_node(ref.fwnode, opp) {
		u64 rate, ck;

		if (fwnode_property_read_u32(opp, "opp-level", &level) ||
		    level >= parsed.state_count || (seen & BIT_ULL(level)))
			goto bad_opp;
		seen |= BIT_ULL(level);
		if (!fwnode_property_present(opp, "apple,data-rate")) {
			if (fwnode_property_present(opp, "opp-hz"))
				goto bad_opp;
			continue;
		}
		if (fwnode_property_read_u64(opp, "apple,data-rate", &rate) || !rate ||
		    fwnode_property_read_u64(opp, "opp-hz", &ck) || !ck ||
		    ck != rate / 2 + rate % 2)
			goto bad_opp;
		rates[level] = rate;
	}
	if (seen != GENMASK_ULL(parsed.state_count - 1, 0)) {
		ret = -EINVAL;
		goto out;
	}
	parsed.max_rate = rates[parsed.state_count - 1];
	if (!parsed.max_rate ||
	    check_mul_overflow(parsed.max_rate, (u64)parsed.width / 8, &peak) ||
	    peak != parsed.peak_bandwidth) {
		ret = -EINVAL;
		goto out;
	}
	for (i = 0; i + 1 < parsed.state_count; i++) {
		if (rates[i] >= parsed.max_rate) {
			ret = -EINVAL;
			goto out;
		}
	}
	/* Publish only a complete, internally consistent description. */
	*dram = parsed;
out:
	fwnode_handle_put(ref.fwnode);
	return ret;
bad_opp:
	fwnode_handle_put(opp);
	ret = -EINVAL;
	goto out;
}

#endif
