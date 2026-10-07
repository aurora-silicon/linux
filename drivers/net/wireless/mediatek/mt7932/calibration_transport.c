/* SPDX-License-Identifier: GPL-2.0-only */
#include "mt7932.h"



/* Called only by the serialized command owner, never from the RX consumer. */
int mt_cal_procedure(struct mt7932 *m, struct mt7932_cal_piece *pieces,
				    unsigned int count, unsigned int logical, u8 context_version)
{
	u8 body[1100];
	unsigned long flags;
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);
	unsigned int i;
	int ret;

	reinit_completion(&m->cal_response);
	spin_lock_irqsave(&m->response_lock, flags);
	ret = mt7932_cal_begin(&m->cal_state, logical);
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (ret)
		return ret;
	for (i = 0; i < count; i++) {
		ret = mt7932_cal_body(body, sizeof(body), pieces + i, context_version);
		if (ret < 0)
			goto fail;
		ret = mt_request(m, 0xd6, true, true, false, body, ret);
		if (ret)
			goto fail;
	}
	if (time_after_eq(jiffies, deadline) ||
	    !wait_for_completion_timeout(&m->cal_response, deadline - jiffies)) {
		ret = -ETIMEDOUT;
		goto fail;
	}
	spin_lock_irqsave(&m->response_lock, flags);
	ret = m->cal_state.error ?: (m->cal_state.done ? 0 : -EPROTO);
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (!ret)
		return 0;
fail:
	spin_lock_irqsave(&m->response_lock, flags);
	m->cal_state.error = ret;
	m->cal_state.active = false;
	spin_unlock_irqrestore(&m->response_lock, flags);
	dev_err(&m->pdev->dev, "CAL_PROCEDURE_FAILED: %d; epoch latched, no replay\n", ret);
	return ret;
}

int mt_calibration_gate(struct mt7932 *m)
{
	const struct firmware *wcal, *oca;
	struct mt7932_cal_piece pieces[14];
	u8 body[1028], request[16];
	unsigned long flags;
	unsigned int at, count, group;
	int ret, planned;

	if (!m->six_ghz_valid || m->smart_version != 12 || !m->preload_version)
		return -EOPNOTSUPP;
	ret = request_firmware_direct(&wcal, "mediatek/mt7932/wcal.bin", &m->pdev->dev);
	if (ret)
		return ret;
	ret = request_firmware_direct(&oca, "mediatek/mt7932/oca2.bin", &m->pdev->dev);
	if (ret)
		goto out_wcal;
	ret = -EINVAL;
	if (!wcal->size || wcal->size > 1024)
		goto out;
	planned = mt7932_cal_power_on(oca->data, oca->size, m->six_ghz, pieces);
	if (planned < 0) {
		ret = planned;
		goto out;
	}
	/* Validate every potential PL request locally before changing firmware state. */
	memset(request, 0, sizeof(request));
	put_unaligned_le32(0x1000, request + 4);
	for (at = 1; at <= 13; at++) {
		struct mt7932_cal_piece check[7];

		put_unaligned_le32(at, request + 12);
		ret = mt7932_cal_requested(oca->data, oca->size, request, sizeof(request),
					  m->smart_version, m->module_byte, check);
		if (ret < 0)
			goto out;
	}
	body[0] = 3;
	body[1] = 0;
	put_unaligned_le16(wcal->size, body + 2);
	memcpy(body + 4, wcal->data, wcal->size);
	ret = mt_request_ext(m, 0xed, 0x21, true, true, true, body, 4 + wcal->size);
	memzero_explicit(body, sizeof(body));
	if (ret)
		goto out;
	if (m->reply_length < 44 || m->reply[28] != 0xed || m->reply[32] ||
	    get_unaligned_le32(m->reply + 40)) {
		dev_err(&m->pdev->dev, "WCAL_REPLY_REJECTED: length=%zu eid=%02x ext=%02x\n",
			m->reply_length, m->reply[28], m->reply[32]);
		print_hex_dump(KERN_INFO, "WCAL_RESULT: ", DUMP_PREFIX_OFFSET, 16, 1,
			       m->reply, min_t(size_t, m->reply_length, 64), false);
		ret = -EPROTO;
		goto out;
	}
	dev_info(&m->pdev->dev, "OWN_WCAL_ACCEPTED: %zu bytes\n", wcal->size);
	for (at = 0, group = 0; at < planned; at += count, group++) {
		count = pieces[at].fragment >> 4;
		if (!count || count > planned - at) {
			ret = -EINVAL;
			goto out;
		}
		ret = mt_cal_procedure(m, pieces + at, count, 1, m->preload_version);
		if (ret)
			goto out;
		dev_info(&m->pdev->dev, "POWER_ON_CAL_GROUP_OK: %u type=%u parameter=%08x fragments=%u\n",
			 group, pieces[at].type, pieces[at].parameter, count);
	}
	dev_info(&m->pdev->dev, "POWER_ON_CAL_COMPLETE: %u groups, %d fragments\n", group, planned);
	/* Drain already queued genuine requests without ever manufacturing one. */
	for (at = 0; at < ARRAY_SIZE(m->cal_requests); at++) {
		spin_lock_irqsave(&m->response_lock, flags);
		count = m->cal_request_count;
		if (count) {
			memcpy(request, m->cal_requests[m->cal_request_head], 16);
			m->cal_request_head = (m->cal_request_head + 1) % ARRAY_SIZE(m->cal_requests);
			m->cal_request_count--;
		}
		spin_unlock_irqrestore(&m->response_lock, flags);
		if (!count)
			break;
		ret = mt7932_cal_requested(oca->data, oca->size, request, sizeof(request),
					  m->smart_version, m->module_byte, pieces);
		if (ret < 0)
			goto out;
		ret = mt_cal_procedure(m, pieces, ret, 4, m->preload_version);
		if (ret)
			goto out;
		dev_info(&m->pdev->dev, "CAL_REQUEST_SERVICED: channel=%u\n", get_unaligned_le32(request + 12));
	}
	ret = 0;
out:
	release_firmware(oca);
out_wcal:
	release_firmware(wcal);
	return ret;
}
