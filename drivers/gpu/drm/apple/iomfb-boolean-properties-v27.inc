/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Aurora Silicon */

struct dcp_boolean_property_v27 {
	struct list_head node;
	char name[64];
	bool value;
};

static bool trampoline_set_boolean_property_v27(struct apple_dcp *dcp, int tag,
						void *out, void *in)
{
	const u8 *request = in;
	struct dcp_boolean_property_v27 *property;
	char name[64];

	trace_iomfb_callback(dcp, tag, __func__);

	memcpy(name, request, sizeof(name));
	name[sizeof(name) - 1] = '\0';
	put_unaligned_le32(0, out);

	list_for_each_entry(property, &dcp->iomfb_boolean_properties, node)
		if (!strcmp(property->name, name))
			goto update;
	if (dcp->iomfb_boolean_property_count >= 64)
		return true;
	property = kzalloc(sizeof(*property), GFP_KERNEL);
	if (!property)
		return true;
	memcpy(property->name, name, sizeof(name));
	dcp->iomfb_boolean_property_count++;
	list_add_tail(&property->node, &dcp->iomfb_boolean_properties);
update:
	property->value = request[64] & 1;
	put_unaligned_le32(1, out);
	return true;
}

void iomfb_firmware_state_cleanup_v27(void *data)
{
	struct apple_dcp *dcp = data;
	struct dcp_boolean_property_v27 *property, *next;

	external_counts_cleanup_v27(dcp);
	list_for_each_entry_safe(property, next, &dcp->iomfb_boolean_properties, node) {
		list_del(&property->node);
		kfree(property);
		dcp->iomfb_boolean_property_count--;
	}
}
