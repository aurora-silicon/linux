// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Copyright (C) The Asahi Linux Contributors
 */

#include "connector.h"

#include "linux/err.h"
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/string_helpers.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include <drm/drm_managed.h>

#include "dcp-internal.h"
#include "iomfb_m4.h"

void apple_connector_edid_init(struct apple_connector *connector)
{
	connector->edid_live = false;
	connector->edid_invalidation_pending = false;
}

/*
 * The EDID cache is generation fenced and owned under mode_config.mutex:
 * mark it stale here, and the hotplug work or the next get_modes() clears it.
 */
void apple_connector_invalidate_edid(struct apple_connector *connector)
{
	if (connector)
		WRITE_ONCE(connector->edid_invalidation_pending, true);
}

void apple_connector_edid_set_live(struct apple_connector *connector, bool live)
{
	if (!connector)
		return;
	if (!live || READ_ONCE(connector->edid_live) != live)
		apple_connector_invalidate_edid(connector);
	WRITE_ONCE(connector->edid_live, live);
}

void apple_connector_set_pipeline(struct apple_connector *connector,
				  struct platform_device *pdev)
{
	apple_connector_invalidate_edid(connector);
	WRITE_ONCE(connector->edid_live, !!pdev);
	WRITE_ONCE(connector->dcp, pdev);
}

enum dcp_chunk_type {
	DCP_CHUNK_COLOR_ELEMENTS,
	DCP_CHUNK_TIMING_ELELMENTS,
	DCP_CHUNK_DISPLAY_ATTRIBUTES,
	DCP_CHUNK_TRANSPORT,
	DCP_CHUNK_NUM_TYPES,
};

static int chunk_show(struct seq_file *m,
		      enum dcp_chunk_type chunk_type)
{
	struct apple_connector *apple_con = m->private;
	struct dcp_chunks *chunk = NULL;

	mutex_lock(&apple_con->chunk_lock);

	switch (chunk_type) {
	case DCP_CHUNK_COLOR_ELEMENTS:
		chunk = &apple_con->color_elements;
		break;
	case DCP_CHUNK_TIMING_ELELMENTS:
		chunk = &apple_con->timing_elements;
		break;
	case DCP_CHUNK_DISPLAY_ATTRIBUTES:
		chunk = &apple_con->display_attributes;
		break;
	case DCP_CHUNK_TRANSPORT:
		chunk = &apple_con->transport;
		break;
	default:
		break;
	}

	if (chunk)
		seq_write(m, chunk->data, chunk->length);

	mutex_unlock(&apple_con->chunk_lock);

	return 0;
}

#define CONNECTOR_DEBUGFS_ENTRY(name, type) \
static int chunk_ ## name ## _show(struct seq_file *m, void *data) \
{ \
	return chunk_show(m, type); \
} \
static int chunk_ ## name ## _open(struct inode *inode, struct file *file) \
{ \
	return single_open(file,  chunk_ ## name ## _show, inode->i_private); \
} \
static const struct file_operations chunk_ ## name ## _fops = { \
	.owner = THIS_MODULE, \
	.open = chunk_ ## name ## _open, \
	.read = seq_read, \
	.llseek = seq_lseek, \
	.release = single_release, \
}

CONNECTOR_DEBUGFS_ENTRY(color, DCP_CHUNK_COLOR_ELEMENTS);
CONNECTOR_DEBUGFS_ENTRY(timing, DCP_CHUNK_TIMING_ELELMENTS);
CONNECTOR_DEBUGFS_ENTRY(display_attribs, DCP_CHUNK_DISPLAY_ATTRIBUTES);
CONNECTOR_DEBUGFS_ENTRY(transport, DCP_CHUNK_TRANSPORT);

static const char *dcp_color_format_name(u8 format)
{
	switch (format) {
	case DCP_COLOR_FORMAT_RGB:
		return "RGB";
	case DCP_COLOR_FORMAT_YCBCR420:
		return "YCbCr420";
	case DCP_COLOR_FORMAT_YCBCR444:
		return "YCbCr444";
	case DCP_COLOR_FORMAT_YCBCR422:
		return "YCbCr422";
	case DCP_COLOR_FORMAT_YCBCR422_DP:
		return "YCbCr422_DP";
	case DCP_COLOR_FORMAT_YCBCR422_HDMI:
		return "YCbCr422_HDMI";
	default:
		return "other";
	}
}

static const char *dcp_color_eotf_name(u8 eotf)
{
	switch (eotf) {
	case DCP_EOTF_SDR_GAMMA:
		return "SDR";
	case DCP_EOTF_HDR_GAMMA:
		return "HDR_gamma";
	case DCP_EOTF_ST_2084:
		return "PQ";
	case DCP_EOTF_BT_2100:
		return "HLG";
	default:
		return "other";
	}
}

static const char *dcp_colorimetry_name(u8 colorimetry)
{
	switch (colorimetry) {
	case DCP_COLORIMETRY_BT601:
		return "BT601";
	case DCP_COLORIMETRY_BT709:
		return "BT709";
	case DCP_COLORIMETRY_XVYCC_601:
		return "xvYCC601";
	case DCP_COLORIMETRY_XVYCC_709:
		return "xvYCC709";
	case DCP_COLORIMETRY_SYCC_601:
		return "sYCC601";
	case DCP_COLORIMETRY_ADOBE_YCC_601:
		return "AdobeYCC601";
	case DCP_COLORIMETRY_BT2020_CYCC:
		return "BT2020_CYCC";
	case DCP_COLORIMETRY_BT2020_YCC:
		return "BT2020_YCC";
	case DCP_COLORIMETRY_BT2020_RGB:
		return "BT2020_RGB";
	case DCP_COLORIMETRY_SRGB:
		return "sRGB";
	case DCP_COLORIMETRY_SCRGB:
		return "scRGB";
	case DCP_COLORIMETRY_SCRGB_FIXED:
		return "scRGB_fixed";
	case DCP_COLORIMETRY_ADOBE_RGB:
		return "AdobeRGB";
	case DCP_COLORIMETRY_DCI_P3_RGB_D65:
		return "DCI_P3_D65";
	case DCP_COLORIMETRY_DCI_P3_RGB_THEATER:
		return "DCI_P3_Theater";
	case DCP_COLORIMETRY_RGB:
		return "Default_RGB";
	default:
		return "other";
	}
}

static int color_state_show(struct seq_file *m, void *data)
{
	struct apple_connector *apple_con = m->private;
	struct platform_device *pdev = READ_ONCE(apple_con->dcp);
	struct apple_dcp *dcp;
	struct dcp_color_mode cm;
	u8 sent[24] = {};
	bool hdr_service, hdr_infoframe, hdr_sent_valid;
	u32 timing_id, color_id;

	bool vrr_enabled, output_pq, output_hdr, output_edr;

	if (!pdev) {
		seq_puts(m, "pipeline: none\n");
		return 0;
	}
	dcp = platform_get_drvdata(pdev);
	if (!dcp) {
		seq_puts(m, "pipeline: unbound\n");
		return 0;
	}

	scoped_guard(spinlock_irqsave, &dcp->hdr.lock) {
		timing_id = dcp->mode.timing_mode_id;
		color_id = dcp->mode.color_mode_id;
		cm = dcp->active_color;
		vrr_enabled = dcp->vrr_enabled;
		output_pq = dcp->output_pq;
		output_hdr = dcp->output_hdr;
		output_edr = dcp->output_edr;
		hdr_service = !!dcp->hdr.service;
		hdr_infoframe = dcp->hdr.infoframe;
		hdr_sent_valid = dcp->hdr.sent_valid;
		if (hdr_sent_valid)
			memcpy(sent, dcp->hdr.sent, sizeof(sent));
	}

	seq_printf(m, "timing_id: %u\n", timing_id);
	seq_printf(m, "color_id: %u\n", color_id);
	if (cm.score >= 0) {
		seq_printf(m, "depth: %u\n", cm.depth);
		seq_printf(m, "format: %s (%u)\n", dcp_color_format_name(cm.format), cm.format);
		seq_printf(m, "colorimetry: %s (%u)\n",
			   dcp_colorimetry_name(cm.colorimetry), cm.colorimetry);
		seq_printf(m, "eotf: %s (%u)\n", dcp_color_eotf_name(cm.eotf), cm.eotf);
		seq_printf(m, "range: %s (%u)\n",
			   cm.range == DCP_COLOR_YCBCR_RANGE_FULL ? "Full" : "Limited",
			   cm.range);
	} else {
		seq_puts(m, "depth: none\n");
	}
	seq_printf(m, "vrr_enabled: %u\n", vrr_enabled);
	seq_printf(m, "output_pq: %u\n", output_pq);
	seq_printf(m, "output_hdr: %u\n", output_hdr);
	seq_printf(m, "output_edr: %u\n", output_edr);
	seq_printf(m, "hdr_service: %u\n", hdr_service);
	seq_printf(m, "hdr_infoframe: %u\n", hdr_infoframe);
	seq_printf(m, "hdr_sent_valid: %u\n", hdr_sent_valid);
	if (hdr_sent_valid) {
		seq_printf(m, "hdr_mastering_max: %u\n", get_unaligned_le16(sent + 16));
		seq_printf(m, "hdr_mastering_min: %u\n", get_unaligned_le16(sent + 18));
		seq_printf(m, "hdr_max_cll: %u\n", get_unaligned_le16(sent + 20));
		seq_printf(m, "hdr_max_fall: %u\n", get_unaligned_le16(sent + 22));
	}
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(color_state);

static void dcp_afk_debugfs_root(struct platform_device *pdev, int ep, struct dentry *root)
{
#if IS_ENABLED(CONFIG_DRM_APPLE_DEBUG)
	struct dentry *entry = NULL;
	struct apple_dcp *dcp;

	/* Type-C connectors can be registered before a pipeline is routed. */
	if (!pdev)
		return;
	dcp = platform_get_drvdata(pdev);
	if (!dcp)
		return;

	switch (ep) {
	case AV_ENDPOINT:
		entry = debugfs_create_dir("avep", root);
		break;
	default:
		break;
	}

	if (!IS_ERR_OR_NULL(entry))
		dcp->ep_debugfs[ep - 0x20] = entry;
#endif
}

/* The panel's notch state (dcp-notch.h), for checking what is on screen. */
static int notch_show(struct seq_file *m, void *v)
{
	struct apple_dcp *dcp = m->private;

	seq_printf(m, "rows %u\npanel_rows %u\npolicy %s\nowned %u\nscanout_y %u\n",
		   dcp->notch_rows, READ_ONCE(dcp->notch_panel_rows),
		   READ_ONCE(dcp->notch_policy) == DCP_NOTCH_SHOWN ? "shown" : "hidden",
		   READ_ONCE(dcp->notch_owned), READ_ONCE(dcp->notch_scanout_y));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(notch);

void apple_connector_debugfs_init(struct drm_connector *connector, struct dentry *root)
{
	struct apple_connector *apple_con = to_apple_connector(connector);
	struct apple_dcp *dcp = apple_con->dcp ?
		platform_get_drvdata(apple_con->dcp) : NULL;

	if (dcp && iomfb_is_m4(dcp))
		iomfb_m4_debugfs_init(dcp, root);
	if (dcp && dcp->notch_rows && dcp_notch_dynamic(dcp))
		debugfs_create_file("notch", 0444, root, dcp, &notch_fops);

        debugfs_create_file("ColorElements", 0444, root, apple_con,
                            &chunk_color_fops);
        debugfs_create_file("TimingElements", 0444, root, apple_con,
                            &chunk_timing_fops);
        debugfs_create_file("DisplayAttributes", 0444, root, apple_con,
                            &chunk_display_attribs_fops);
        debugfs_create_file("Transport", 0444, root, apple_con,
			    &chunk_transport_fops);
	debugfs_create_file("color_state", 0444, root, apple_con,
			    &color_state_fops);

	switch (connector->connector_type) {
	case DRM_MODE_CONNECTOR_DisplayPort:
	case DRM_MODE_CONNECTOR_HDMIA:
		dcp_afk_debugfs_root(READ_ONCE(apple_con->dcp), AV_ENDPOINT, root);
		break;
	default:
		break;
	}
}

static void dcp_connector_set_dict(struct apple_connector *connector,
				   struct dcp_chunks *dict,
				   struct dcp_chunks *chunks)
{
	/*
	 * Not devm: the buffer is allocated by whichever DCP received the
	 * chunks, but a Type-C port can move to another pipeline before it is
	 * replaced, and freeing it against the connector's current DCP then
	 * fails devres_destroy() -- one WARN and one leak per dictionary.
	 */
	kvfree(dict->data);

	*dict = *chunks;
}

void dcp_connector_update_dict(struct apple_connector *connector, const char *key,
			       struct dcp_chunks *chunks)
{
	mutex_lock(&connector->chunk_lock);
	if (!strcmp(key, "ColorElements"))
		dcp_connector_set_dict(connector, &connector->color_elements, chunks);
	else if (!strcmp(key, "TimingElements"))
		dcp_connector_set_dict(connector, &connector->timing_elements, chunks);
	else if (!strcmp(key, "DisplayAttributes"))
		dcp_connector_set_dict(connector, &connector->display_attributes, chunks);
	else if (!strcmp(key, "Transport"))
		dcp_connector_set_dict(connector, &connector->transport, chunks);
	else
		kvfree(chunks->data);

	chunks->data = NULL;
	chunks->length = 0;

	mutex_unlock(&connector->chunk_lock);
}
