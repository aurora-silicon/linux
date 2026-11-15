// SPDX-License-Identifier: GPL-2.0-only
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
#if IS_ENABLED(CONFIG_MT7932_RF_KUNIT_TEST)
		if (m->cal_test_send)
			ret = m->cal_test_send(m, body, ret);
		else
#endif
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
	mt_rf_fail_locked(m, ret);
	spin_unlock_irqrestore(&m->response_lock, flags);
	dev_err(&m->pdev->dev, "CAL_PROCEDURE_FAILED: %d; epoch latched, no replay\n", ret);
	return ret;
}

void mt_cal_input_error(struct mt7932 *m, int error, unsigned int tag, const char *stage)
{
	if (error == -ENOENT)
		dev_err(&m->pdev->dev, "local input mediatek/mt7932/oca2.bin missing section 0x%04x during %s: %d\n",
			tag, stage, error);
	else if (error == -EOPNOTSUPP)
		dev_err(&m->pdev->dev, "CAL_PROFILE_UNSUPPORTED: phase=%s section=0x%04x error=%d\n",
			stage, tag, error);
	else if (tag)
		dev_err(&m->pdev->dev, "local input mediatek/mt7932/oca2.bin section 0x%04x rejected during %s: %d\n",
			tag, stage, error);
	else
		dev_err(&m->pdev->dev, "local input mediatek/mt7932/oca2.bin container format rejected during %s: %d\n",
			stage, error);
}

int mt_calibration_gate(struct mt7932 *m)
{
	const struct firmware *wcal, *oca;
	const char *phase = "own input validation";
	struct mt7932_cal_piece pieces[14];
	u8 body[1028], request[16];
	unsigned int at, count, group, input_tag;
	int ret, planned;

	if (!m->six_ghz_valid || m->smart_version != 12 || !m->preload_version) {
		dev_err(&m->pdev->dev, "CAL_CAPABILITY_MISMATCH: six-ghz-valid=%u smart-version=%u preload-version=%u\n",
			m->six_ghz_valid, m->smart_version, m->preload_version);
		return -EOPNOTSUPP;
	}
	ret = mt_request_input(m, &wcal, "mediatek/mt7932/wcal.bin");
	if (ret)
		return ret;
	ret = mt_request_input(m, &oca, "mediatek/mt7932/oca2.bin");
	if (ret)
		goto out_wcal;
	ret = -EINVAL;
	if (!wcal->size || wcal->size > 1024) {
		dev_err(&m->pdev->dev, "local input mediatek/mt7932/wcal.bin has invalid size %zu\n", wcal->size);
		goto out;
	}
	planned = mt7932_cal_power_on(oca->data, oca->size, m->six_ghz, pieces, &input_tag);
	if (planned < 0) {
		ret = planned;
		mt_cal_input_error(m, ret, input_tag, "power-on plan");
		goto out;
	}
	/* Validate every potential PL request locally before changing firmware state. */
	memset(request, 0, sizeof(request));
	put_unaligned_le32(0x1000, request + 4);
	for (at = 1; at <= 13; at++) {
		struct mt7932_cal_piece check[7];

		put_unaligned_le32(at, request + 12);
		ret = mt7932_cal_requested(oca->data, oca->size, request, sizeof(request),
					  m->smart_version, m->module_byte, check, &input_tag);
		if (ret < 0) {
			mt_cal_input_error(m, ret, input_tag, "D7 plan");
			goto out;
		}
	}
	phase = "WCAL command/reply";
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
		ret = -EPROTO;
		goto out;
	}
	dev_dbg(&m->pdev->dev, "OWN_WCAL_ACCEPTED: %zu bytes\n", wcal->size);
	phase = "power-on D6 command/completion";
	for (at = 0, group = 0; at < planned; at += count, group++) {
		count = pieces[at].fragment >> 4;
		if (!count || count > planned - at) {
			ret = -EINVAL;
			goto out;
		}
		ret = mt_cal_procedure(m, pieces + at, count, 1, m->preload_version);
		if (ret)
			goto out;
		dev_dbg(&m->pdev->dev, "POWER_ON_CAL_GROUP_OK: %u type=%u parameter=%08x fragments=%u\n",
			group, pieces[at].type, pieces[at].parameter, count);
	}
	dev_dbg(&m->pdev->dev, "POWER_ON_CAL_COMPLETE: %u groups, %d fragments\n", group, planned);
	phase = "queued D7 planning/command/completion";
	ret = mt_cal_drain_requests(m, oca);
out:
	if (ret)
		dev_err(&m->pdev->dev, "CALIBRATION_FAILED: phase=%s error=%d\n", phase, ret);
	release_firmware(oca);
out_wcal:
	release_firmware(wcal);
	return ret;
}

/* Shared startup queue drain, entered by the serialized command owner. */
int mt_cal_drain_requests(struct mt7932 *m, const struct firmware *oca)
{
	struct mt7932_cal_piece pieces[7];
	u8 request[16];
	unsigned long flags;
	unsigned int at, count, input_tag;
	int ret;

	lockdep_assert_held(&m->command_mutex);
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
					  m->smart_version, m->module_byte, pieces, &input_tag);
		if (ret < 0) {
			mt_cal_input_error(m, ret, input_tag, "D7 plan");
			return ret;
		}
		ret = mt_cal_procedure(m, pieces, ret,
				       mt7932_cal_request_replies(request), m->preload_version);
		if (ret)
			return ret;
		dev_dbg(&m->pdev->dev, "CAL_REQUEST_SERVICED: channel=%u\n",
			get_unaligned_le32(request + 12));
	}
	return 0;
}
