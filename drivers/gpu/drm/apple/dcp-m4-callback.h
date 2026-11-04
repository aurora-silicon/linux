/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_CALLBACK_H
#define APPLE_DCP_M4_CALLBACK_H

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include "dcp-m4-profile.h"

/* All hooks execute under the serialized transaction owner, never in IRQ context. */
struct dcp_m4_callback_ops {
	int (*call)(void *cookie, unsigned int method, bool remote,
		    const void *input, u32 input_size, void *output, u32 output_size);
	int (*swap_done)(void *cookie, u32 id);
	/* Copy the bounded name and compute/retain any digest before packet ACK/reuse. */
	int (*event)(void *cookie, const char *name, const void *dictionary, size_t size);
	int (*property)(void *cookie, unsigned int method, const void *input, void *output);
	int (*memory)(void *cookie, unsigned int method, const void *input, void *output);
	/* Idle caching: D117 state, D578/D579 fence, D580 cache-surface release. */
	int (*idle)(void *cookie, unsigned int method, const void *input, void *output);
};

struct dcp_m4_callback_shape {
	u16 method;
	u16 input;
	u16 output;
};

static inline int dcp_m4_callback_result(int ret)
{
	return ret > 0 ? -EPROTO : ret;
}

/* Exact qualified J713 shapes. An absent behavior hook is failure, never an ACK. */
static const struct dcp_m4_callback_shape dcp_m4_callbacks[] = {
	{0, 0, 4}, {1, 0, 4}, {2, 0, 0}, {3, 4, 60}, {6, 84, 80}, {8, 4, 0},
	{100, 0, 0}, {101, 0, 4}, {102, 68, 0}, {104, 68, 0}, {106, 0, 4}, {107, 64, 0},
	{108, 0, 4}, {109, 0, 4}, {110, 0, 4}, {111, 0, 4}, {112, 0, 4}, {113, 0, 4},
	{114, 4164, 4100}, {117, 12, 0}, {121, 0, 4}, {122, 0, 4}, {123, 0, 4},
	{125, 100, 36}, {127, 4, 4}, {128, 4104, 4}, {129, 64, 4},
	{201, 12, 16}, {203, 0, 4}, {206, 0, 4}, {207, 0, 4}, {209, 0, 8}, {300, 16, 0},
	{400, 76, 3076}, {401, 80, 12}, {406, 72, 0}, {408, 8, 8},
	{413, 4168, 4}, {414, 80, 4}, {415, 76, 4}, {451, 20, 28}, {454, 4, 4},
	{552, 4164, 4}, {561, 4164, 4}, {563, 76, 4}, {565, 72, 4}, {567, 128, 4},
	{572, 4, 4}, {575, 88, 76}, {576, 4, 0}, {578, 4, 4}, {579, 0, 0}, {580, 0, 0},
	{582, 8, 4}, {588, 0, 0}, {590, 1840, 0},
	{592, 20, 0}, {594, 4, 0}, {599, 0, 0}, {700, 4, 4},
};

static inline int dcp_m4_callback_method(u32 tag)
{
	unsigned int method = 0;

	if ((tag & 0xff) != 'D')
		return -EPROTO;
	for (unsigned int shift = 8; shift < 32; shift += 8) {
		u8 digit = tag >> shift;

		if (digit < '0' || digit > '9')
			return -EPROTO;
		method = method * 10 + digit - '0';
	}
	return method;
}

static inline int dcp_m4_callback_shape(u32 tag, u32 input, u32 output)
{
	int method = dcp_m4_callback_method(tag);

	const struct dcp_m4_callback_shape *table = dcp_m4_callbacks;
	unsigned int count = ARRAY_SIZE(dcp_m4_callbacks);

	if (method < 0)
		return method;
	for (unsigned int i = 0; i < count; i++)
		if (table[i].method == method)
			return input == table[i].input &&
				output == table[i].output ? method : -EMSGSIZE;
	return -EOPNOTSUPP;
}

static inline int dcp_m4_nested_call(const struct dcp_m4_callback_ops *ops, void *cookie,
				    bool remote, unsigned int method, bool input_one,
				    int expected)
{
	u8 input[4], output[4] = {0};
	int ret;

	if (!ops->call)
		return -EOPNOTSUPP;
	put_unaligned_le32(1, input);
	/* expected: -1 no output, -2 four-byte output of any value. */
	ret = ops->call(cookie, method, remote, input_one ? input : NULL, input_one ? 4 : 0,
			expected == -1 ? NULL : output, expected == -1 ? 0 : 4);
	if (ret)
		return ret < 0 ? ret : -EPROTO;
	if (expected >= 0 && get_unaligned_le32(output) != (u32)expected)
		return -EPROTO;
	return 0;
}

/*
 * The transaction layer has already located and owns non-overlapping packet
 * input/output spans. It must poison the session on any error here, and send an
 * ACK only after successful dispatch and device-visible output publication.
 */
static inline int dcp_m4_callback_dispatch(const struct dcp_m4_callback_ops *ops, void *cookie,
					 u32 tag, bool remote, const void *input, u32 input_size,
					 void *output, u32 output_size)
{
	const u8 *in = input;
	u8 *out = output;
	int method, ret;
	u32 value;

	method = dcp_m4_callback_shape(tag, input_size, output_size);
	if (method < 0)
		return method;
	if (!ops || (input_size && !input) || (output_size && !output))
		return -EINVAL;
	if (output_size)
		memset(out, 0, output_size);

	switch (method) {
	case 0: case 1: case 108: case 109: case 110: case 111: case 112: case 113:
		put_unaligned_le32(1, out);
		return 0;
	case 3:
		put_unaligned_le32(1, out + 56); /* Decline optional bandwidth service. */
		return 0;
	case 6:
		if (!(in[80] & 1))
			memcpy(out, in, 80);
		return 0;
	case 575:
		if (!(in[84] & 1))
			memcpy(out, in + 8, 76);
		return 0;
	case 300: case 406: case 576: case 588:
		return 0;
	/*
	 * will_power_off_signal, set_power_trace_enable, swap_complete_intent,
	 * enable_backlight_message_ap_gated: notifications.
	 */
	case 2: case 8: case 592: case 594:
		return 0;
	/*
	 * set_idle_caching_state_ap, idle_fence_create, idle_fence_complete,
	 * idle_surface_release.
	 */
	case 117: case 578: case 579: case 580:
		if (!ops->idle)
			return -EOPNOTSUPP;
		return dcp_m4_callback_result(ops->idle(cookie, method, input, output));
	/*
	 * acknowledgeSetPowerState, powerUpDART, set_kernel_power_assert: success;
	 * aot_supported_peek: false.
	 */
	case 106: case 572: case 700: case 203:
		return 0;
	case 209: /* get_calendar_time_ms */
		put_unaligned_le64(ktime_get_real_ns() / NSEC_PER_MSEC, out);
		return 0;
	case 122: case 123: case 582:
		return 0; /* False, not an unimplemented service returning success. */
	case 101:
		put_unaligned_le32(dcp_m4_pf->boot_stride, out);
		return 0;
	case 125:
		value = get_unaligned_le32(in + 64);
		if (value > 8)
			return -EINVAL;
		memcpy(out, in + 68, value * 4);
		return 0;
	case 408:
		value = get_unaligned_le32(in + 4);
		if (memcmp(in, "VORP", 4) || value > 1)
			return -EINVAL;
		put_unaligned_le64(value ? 24000000 : 48000000, out);
		return 0;
	case 590:
		value = get_unaligned_le32(in);
		if (!value)
			return -EPROTO;
		return ops->swap_done ? dcp_m4_callback_result(ops->swap_done(cookie, value)) :
			-EOPNOTSUPP;
	case 599:
		return dcp_m4_nested_call(ops, cookie, remote, 412, false, 1);
	case 100:
		return dcp_m4_nested_call(ops, cookie, remote, 390, false, 0);
	case 206: case 207:
		ret = dcp_m4_nested_call(ops, cookie, remote, method == 206 ? 131 : 132, false, 0);
		if (ret)
			return ret;
		put_unaligned_le32(1, out);
		return 0;
	case 121:
		ret = dcp_m4_nested_call(ops, cookie, remote, 389, false, -1);
		if (!ret)
			ret = dcp_m4_nested_call(ops, cookie, remote, 446, false, 1);
		if (!ret)
			ret = dcp_m4_nested_call(ops, cookie, remote, 29, false, -1);
		if (!ret)
			ret = dcp_m4_nested_call(ops, cookie, remote, 468, true, -1);
		if (!ret)
			ret = dcp_m4_nested_call(ops, cookie, remote, 0, true, 1);
		if (!ret)
			ret = dcp_m4_nested_call(ops, cookie, remote, 465, false, 1);
		if (ret)
			return ret;
		put_unaligned_le32(1, out);
		return 0;
	case 114:
		if (!memchr(in, 0, 64) || in[4160] > 1 || (!in[4160] && in[64] != 'd'))
			return -EINVAL;
		if (!ops->event)
			return -EOPNOTSUPP;
		ret = ops->event(cookie, (const char *)in, (in[4160] & 1) ? NULL : in + 64,
				(in[4160] & 1) ? 0 : 4096);
		if (ret)
			return ret < 0 ? ret : -EPROTO;
		if (!(in[4160] & 1))
			memcpy(out, in + 64, 4096);
		return 0;
	case 201: case 451: case 454:
		return ops->memory ?
			dcp_m4_callback_result(ops->memory(cookie, method, input, output)) : -EOPNOTSUPP;
	default:
		return ops->property ?
			dcp_m4_callback_result(ops->property(cookie, method, input, output)) : -EOPNOTSUPP;
	}
}

#endif
