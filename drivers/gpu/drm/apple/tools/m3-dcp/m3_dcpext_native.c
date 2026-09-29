// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Hash-qualified J514S DCP protocol; a single kernel client owns all RPCs. */
#include <linux/device.h>
#include <linux/ktime.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include "m3_dcpext_rpc.h"
#include "m3_dcpext_native.h"
#include "m3_dcpext_modes.h"
#include "m3_dcpext_property.h"

#define A(n) M3_DCP_TAG('A', n)
#define D(n) M3_DCP_TAG('D', n)
#define MAX_PROPERTIES 256
#define MAX_RAW_PROPERTIES 32

struct m3_dcp_property {
	u32 service;
	char key[64];
	u64 value;
};

struct m3_dcpext_native {
	struct device *dev;
	struct m3_dcpext_rpc *bridge;
	struct mutex lock;
	bool failed;
	bool opened;
	u32 stride;
 u64 mode_generation;
	u32 diagnostic_plane;
	bool diagnostic_flat_surface;
	struct m3_dcp_property properties[MAX_PROPERTIES];
	u32 property_count;
	struct { u32 service; char key[64]; void *data; u32 size; } raw[MAX_RAW_PROPERTIES];
	u32 raw_count, raw_bytes;
	struct m3_property_transfer chunk;
	int metadata_error;
	u64 completion_count;
	u8 completion_last[0x6f0];
	u64 analytics_count;
	u8 analytics_last[4164];
};


static int callback(struct m3_dcpext_rpc *b, void *cookie, u32 tag,
		    const void *input, u32 in_size, void *output, u32 out_size);

static int call(struct m3_dcpext_native *dcp, u32 tag, const void *in, u32 ins,
		void *out, u32 outs, u32 completion)
{
	int ret;

	if (dcp->failed)
		return -EIO;
	dev_dbg(dcp->dev, "M3 kernel RPC %#x input=%u output=%u\n", tag, ins, outs);
	ret = m3_dcpext_rpc_call(dcp->bridge, tag, in, ins, out, outs,
				completion, callback, dcp);
	if (ret) {
		dcp->failed = true;
		dev_err(dcp->dev, "M3 kernel RPC %#x failed: %d; recovery requires reboot\n", tag, ret);
	}
	return ret;
}

static int simple_call(struct m3_dcpext_native *dcp, u32 tag, bool input, bool reply, bool expected)
{
	__le32 one = cpu_to_le32(1), result = 0;
	int ret = call(dcp, tag, input ? &one : NULL, input ? 4 : 0,
		       reply ? &result : NULL, reply ? 4 : 0, 0);

	if (!ret && reply && le32_to_cpu(result) != expected)
		return -EPROTO;
	return ret;
}

static struct m3_dcp_property *property(struct m3_dcpext_native *dcp,
				       u32 service, const u8 *key, bool create)
{
	struct m3_dcp_property *p;
	u32 i;

	if (!memchr(key, 0, 64))
		return ERR_PTR(-EINVAL);
	for (i = 0; i < dcp->property_count; i++) {
		p = &dcp->properties[i];
		if (p->service == service && !strcmp(p->key, key))
			return p;
	}
	if (!create)
		return NULL;
	if (dcp->property_count == MAX_PROPERTIES)
		return ERR_PTR(-ENOSPC);
	p = &dcp->properties[dcp->property_count++];
	p->service = service;
	strscpy(p->key, key, sizeof(p->key));
	return p;
}

static int raw_property(struct m3_dcpext_native *dcp, u32 service, const u8 *key,
			const void *data, u32 size)
{
	void *copy;
	u32 i, old_size = 0;

	if (!memchr(key, 0, 64) || size > M3_DCPEXT_MAX_PROPERTY_BYTES)
		return -EINVAL;
	for (i = 0; i < dcp->raw_count; i++)
		if (dcp->raw[i].service == service && !strcmp(dcp->raw[i].key, key))
			break;
	if (i == MAX_RAW_PROPERTIES)
		return -ENOSPC;
	if (i < dcp->raw_count)
		old_size = dcp->raw[i].size;
	if (dcp->raw_bytes - old_size + size > SZ_8M)
		return -ENOSPC;
	copy = kvmemdup(size ? data : "", size ?: 1, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	if (i == dcp->raw_count)
		dcp->raw_count++;
	kvfree(dcp->raw[i].data);
	dcp->raw[i].service = service;
	strscpy(dcp->raw[i].key, key, 64);
	dcp->raw[i].data = copy;
	dcp->raw[i].size = size;
	dcp->raw_bytes += size - old_size;
 if(!service && !strcmp(key,"TimingElements")){dcp->mode_generation++;dcp->metadata_error=0;}
	return 0;
}

static int callback(struct m3_dcpext_rpc *b, void *cookie, u32 tag,
		    const void *input, u32 in_size, void *output, u32 out_size)
{
	struct m3_dcpext_native *dcp = cookie;
	const u8 *in = input;
	u8 *out = output;
	struct m3_dcp_property *p;
	struct m3_dcpext_buffer buffer = {};
	u32 count, offset, service, key_offset;
	int ret;

	dev_dbg(dcp->dev, "M3 kernel callback %#x input=%u output=%u\n", tag, in_size, out_size);
#define SHAPE(i, o) (in_size == (i) && out_size == (o))
	if (tag == D(591) && SHAPE(20, 0)) {
        /* Old DCP protocol: packed id/bool/intent/width/height, then padding.
         * Firmware-generated intent swaps do not retire an A407 framebuffer.
         * The first captured M3 notification has intent 15 at 1920x1080. */
        u32 intent = get_unaligned_le32(in + 5);
        if (in[4] > 1 || (intent != 6 && intent != 7 && intent != 15))
            return -EOPNOTSUPP;
        dev_info(dcp->dev, "M3 intent swap id=%u flag=%u intent=%u size=%ux%u; no submitted buffer retired\n",
            get_unaligned_le32(in), in[4], intent,
            get_unaligned_le32(in + 9), get_unaligned_le32(in + 13));
        return 0;
    }
    /* D002 is the external will-power-off notification; no DMA ownership release. */
	if (tag == D(2) && SHAPE(0, 0))
		return 0;
	if (tag == D(574) && SHAPE(4, 4)) {
		/* M3 TEXT13e810 stores one bool byte; the other three are padding.
		 * m1n1's matching old protocol names this powerUpDART. */
		if (in[0] > 1)
			return -EINVAL;
		return m3_dcpext_rpc_dart_power(b, in[0]);
	}
	if (tag == D(209) && SHAPE(0, 8)) {
		put_unaligned_le64(ktime_to_ms(ktime_get_real()), out);
		return 0;
	}
	if (tag == D(596) && SHAPE(0, 4))
		return 0; /* No default framebuffer was allocated by this host. */
	/* J514S 1e5e14/1e5e9c: event, parameter, u32 value, nullable byte.
	 * The getter copies output[0] back and returns output[4] as a boolean.
	 * Match the M2 host: no display tiling, and no setter implementation.
	 */
	if ((tag == D(115) && SHAPE(16, 8)) ||
	    (tag == D(116) && SHAPE(16, 4))) {
		if (in[12] > 1)
			return -EINVAL;
		dev_info(dcp->dev, "M3 tiling callback %#x event=%u parameter=%u value=%u null=%u\n",
			 tag, get_unaligned_le32(in), get_unaligned_le32(in + 4),
			 get_unaligned_le32(in + 8), in[12]);
		if (tag == D(115))
			out[4] = 1;
		return 0;
	}
	/* The transport zeroes output, so absent optional metadata stays absent. */
	if (tag == D(114) && SHAPE(4164, 4100)) {
		/* J514S CoreAnalyticsSendEvent: name[64], serialized dictionary[4096],
		 * nullable flag. Retain the latest event locally. The dictionary is
		 * in/out: firmware parses it even on failure, so a zero-filled reply
		 * is invalid. Return its own serialized dictionary without changes.
		 * The final u32 is the method status (zero = accepted).
		 */
		if (!memchr(in, 0, 64) || in[4160] > 1 ||
		    (!in[4160] && in[64] != 'd'))
			return -EINVAL;
		memcpy(dcp->analytics_last, in, sizeof(dcp->analytics_last));
		dcp->analytics_count++;
		if (!in[4160])
			memcpy(out, in + 64, 4096);
		put_unaligned_le32(0, out + 4096);
		dev_info(dcp->dev, "M3 analytics event %llu: %.64s (retained locally)\n",
			 dcp->analytics_count, in);
		return 0;
	}
	if (tag == D(589) && SHAPE(0x6f0, 0)) {
		memcpy(dcp->completion_last, in, sizeof(dcp->completion_last));
		dcp->completion_count++;
		return 0;
	}
	if ((tag == D(588) && SHAPE(8, 0)) ||
	    (tag == D(598) && SHAPE(0, 0)))
		return 0;
	if (tag == D(599) && SHAPE(0, 0))
		return simple_call(dcp, A(410), false, true, false);
	if ((tag == D(108) || tag == D(109) || tag == D(110) || tag == D(111) ||
	     tag == D(112) || tag == D(113) || tag == D(0) || tag == D(1)) && SHAPE(0, 4)) {
		out[0] = 1;
		return 0;
	}
	if (tag == D(576) && SHAPE(88, 76)) {
		if (!(in[84] & 1))
			memcpy(out, in + 8, 76);
		return 0;
	}
	if ((tag == D(577) && SHAPE(4, 0)) || (tag == D(300) && SHAPE(16, 0)))
		return 0;
	if (tag == D(127) && SHAPE(4, 4)) {
		ret = m3_property_begin(&dcp->chunk, get_unaligned_le32(in),
				M3_DCPEXT_MAX_PROPERTY_BYTES - dcp->raw_bytes);
		goto property_reply;
	}
	if (tag == D(128) && SHAPE(0x1008, 4)) {
		ret = m3_property_append(&dcp->chunk, get_unaligned_le32(in + 0x1000),
				in, get_unaligned_le32(in + 0x1004));
		goto property_reply;
	}
	if (tag == D(129) && SHAPE(64, 4)) {
		ret = m3_property_complete(&dcp->chunk);
		if (!ret && !memchr(in, 0, 64))
			ret = -EINVAL;
		if (!ret)
			ret = raw_property(dcp, 0, in, dcp->chunk.data, dcp->chunk.size);
		if (!ret)
			dev_info(dcp->dev, "M3 property %.64s: %u bytes\n", in, dcp->chunk.size);
		m3_property_reset(&dcp->chunk);
		goto property_reply;
	}
	if (tag == D(408) && SHAPE(8, 8)) {
		count = get_unaligned_le32(in + 4);
		if (memcmp(in, "VORP", 4) || count > 1)
			return -EINVAL;
		/* J514S boot ADT: dispext0 IDs353/414 (indices97/158),
		 * dispext1 IDs356/416 (indices100/160), both 935MHz/0. */
		put_unaligned_le64(count ? 0 : 935000000, out);
		dev_info(dcp->dev, "M3 display clock %u: %llu Hz (external boot ADT)\n",
			 count, get_unaligned_le64(out));
		return 0;
	}
	if (tag == D(125) && SHAPE(100, 36)) {
		count = get_unaligned_le32(in + 64);
		if (count > 8)
			return -EINVAL;
		memcpy(out, in + 68, count * 4);
		return 0;
	}
	if (tag == D(6) && SHAPE(60, 56)) {
		if (!(in[56] & 1))
			memcpy(out, in, 56);
		return 0;
	}
	if ((tag == D(122) || tag == D(123)) && SHAPE(0, 4))
		return 0;
	if (tag == D(121) && SHAPE(0, 4)) {
		static const struct { u32 tag; bool in, reply, expected; } sequence[] = {
			{A(444), false, true, false}, {A(29)},
			{A(466), true}, {A(0), true, true, true}, {A(463), false, true, true},
		};
		for (count = 0; count < ARRAY_SIZE(sequence); count++) {
			ret = simple_call(dcp, sequence[count].tag, sequence[count].in,
					  sequence[count].reply, sequence[count].expected);
			if (ret)
				return ret;
		}
		out[0] = 1;
		return 0;
	}
	if (tag == D(101) && SHAPE(0, 4)) {
		put_unaligned_le32(dcp->stride, out);
		return 0;
	}
	if (tag == D(3) && SHAPE(4, 60)) {
		put_unaligned_le32(1, out + 56); /* Explicitly decline bandwidth service. */
		return 0;
	}
	if (tag == D(451) && SHAPE(20, 28)) {
		u32 alignment = get_unaligned_le32(in + 12);

		dev_info(dcp->dev, "M3 allocation flags=%#x size=%llu alignment=%u null=%u/%u/%u\n",
			 get_unaligned_le32(in), get_unaligned_le64(in + 4), alignment,
			 in[16], in[17], in[18]);
		if (get_unaligned_le32(in) != 0x703 || !is_power_of_2(alignment) || alignment > SZ_16K ||
		    in[16] || in[17] || in[18])
			return -EINVAL;
		buffer.size = get_unaligned_le64(in + 4);
		ret = m3_dcpext_rpc_alloc(b, &buffer);
		if (ret)
			return ret;
		if ((buffer.physical | buffer.dva) & (alignment - 1))
			return -EINVAL;
		put_unaligned_le64(buffer.physical, out);
		put_unaligned_le64(buffer.dva, out + 8);
		put_unaligned_le64(buffer.size, out + 16);
		put_unaligned_le32(buffer.id, out + 24);
		return 0;
	}
	if (tag == D(201) && SHAPE(12, 20)) {
		u64 id = get_unaligned_le64(in);

		if (id > U32_MAX || get_unaligned_le32(in + 8) != 1)
			return -EINVAL;
		buffer.id = id;
		ret = m3_dcpext_rpc_map(b, &buffer);
		if (ret)
			return ret;
		/* Same vaddr/dva/status layout as M2, verified at TEXT 0x18b330.
		 * The AP virtual-address output is unused by this host.
		 */
		put_unaligned_le64(0, out);
		put_unaligned_le64(buffer.dva, out + 8);
		put_unaligned_le32(0, out + 16);
		return 0;
	}
	if (tag == D(454) && SHAPE(4, 4)) {
		ret = m3_dcpext_rpc_retire(b, get_unaligned_le32(in));
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if (tag == D(582) && SHAPE(8, 4))
		return 0; /* Optional default surface is not allocated by this host. */
	if (tag == D(400) && SHAPE(76, 0xc04)) {
		count = get_unaligned_le32(in + 68);
		service = get_unaligned_le32(in);
		if (count > 0xc00 || (in[72] & 1) || !memchr(in + 4, 0, 64))
			return -EINVAL;
		for (offset = 0; offset < dcp->raw_count; offset++) {
			if (dcp->raw[offset].service != service || strcmp(dcp->raw[offset].key, in + 4))
				continue;
			if (dcp->raw[offset].size > count)
				return -ENOSPC;
			memcpy(out, dcp->raw[offset].data, dcp->raw[offset].size);
			put_unaligned_le32(dcp->raw[offset].size, out + 0xc00);
			break;
		}
		return 0;
	}
	if (tag == D(401) && SHAPE(80, 12)) {
		/* Match the existing M2 mini-LED host's PMUS compatibility
		 * response. This is a fixed calibration placeholder, NOT a
		 * measured M3 temperature and never a thermal-guard input.
		 * Firmware 19a4c4..19a4e8 converts centidegrees to 16.16.
		 */
		if (!memcmp(in, "SUMP", 4) &&
		    !strncmp(in + 4, "Temperature", 64)) {
			put_unaligned_le64(3029, out);
			out[8] = 1;
			dev_info_ratelimited(dcp->dev,
				"M3 PMUS Temperature: M2 compatibility placeholder 3029\n");
			return 0;
		}
		p = property(dcp, get_unaligned_le32(in), in + 4, false);
		if (IS_ERR(p))
			return PTR_ERR(p);
		if (p) {
			put_unaligned_le64(p->value, out);
			out[8] = 1;
		} else
			dev_info_ratelimited(dcp->dev,
				"M3 missing host property service=%#x key=%.64s\n",
				get_unaligned_le32(in), in + 4);
		return 0;
	}
	if ((tag == D(413) && SHAPE(4168, 4)) ||
	    ((tag == D(552) || tag == D(561)) && SHAPE(4164, 4)) ||
	    (tag == D(567) && SHAPE(128, 4))) {
		key_offset = tag == D(413) ? 4 : 0;
		service = key_offset ? get_unaligned_le32(in) : 0;
		if (tag != D(567) && (in[key_offset + 4160] & 1)) {
			out[0] = 1;
			return 0;
		}
		count = tag == D(567) ? strnlen(in + 64, 64) : 4096;
		ret = raw_property(dcp, service, in + key_offset, in + key_offset + 64, count);
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if ((tag == D(563) && SHAPE(76, 4)) || (tag == D(565) && SHAPE(72, 4)) ||
	    (tag == D(414) && SHAPE(80, 4)) || (tag == D(415) && SHAPE(76, 4)) ||
	    ((tag == D(102) || tag == D(104)) && SHAPE(68, 0))) {
		key_offset = (tag == D(414) || tag == D(415)) ? 4 : 0;
		service = key_offset ? get_unaligned_le32(in) : 0;
		count = (tag == D(563) || tag == D(414)) ? 8 : 4;
		if (out_size && (in[key_offset + 64 + count] & 1)) {
			out[0] = 1;
			return 0;
		}
		p = property(dcp, service, in + key_offset, true);
		if (IS_ERR(p))
			return PTR_ERR(p);
		p->value = count == 8 ? get_unaligned_le64(in + key_offset + 64) :
				       get_unaligned_le32(in + key_offset + 64);
		if (tag == D(104))
			p->value = in[64];
		if (out_size)
			out[0] = 1;
		return 0;
	}
	if (tag == D(406) && SHAPE(72, 0))
		return 0;
	if (tag == D(107) && SHAPE(64, 0)) {
        if(!memchr(in,0,64))return -EINVAL;
        if(!strcmp(in,"TimingElements"))dcp->mode_generation++;
        for(u32 i=0;i<dcp->raw_count;i++) {
         if(dcp->raw[i].service || strcmp(dcp->raw[i].key,in))continue;
         dcp->raw_bytes-=dcp->raw[i].size;kvfree(dcp->raw[i].data);
         dcp->raw[i]=dcp->raw[--dcp->raw_count];
         memset(&dcp->raw[dcp->raw_count],0,sizeof(dcp->raw[0]));break;
        }
		p = property(dcp, 0, in, false);
		if (IS_ERR(p))
			return PTR_ERR(p);
		if (p) {
			*p = dcp->properties[--dcp->property_count];
			memset(&dcp->properties[dcp->property_count], 0, sizeof(*p));
		}
		return 0;
	}
	if (tag == D(207) && SHAPE(0, 4)) {
		/* HDMI has no panel backlight. Match iomfb_template.c's external
		 * display path: acknowledge the request without A132, which
		 * registers a panel backlight service with firmware.
		 */
		out[0] = 1;
		return 0;
	}
	if (tag == D(206) && SHAPE(0, 4)) {
		ret = simple_call(dcp, A(131), false, true, true);
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if (tag == D(100) && SHAPE(0, 0))
		{
		__le32 result;
		return call(dcp, A(374), NULL, 0, &result, 4, 0);
	}
	dev_err(dcp->dev, "M3 unhandled kernel callback %#x %u/%u\n", tag, in_size, out_size);
	return -EOPNOTSUPP;
#undef SHAPE
property_reply:
	if (ret) {
		dev_warn_ratelimited(dcp->dev, "M3 property rejected: %d; keeping RPC alive\n", ret);
		dcp->metadata_error = ret;
		m3_property_reset(&dcp->chunk);
	}
	/* D127/D128/D129 return a bool. False rejects data, not the transport. */
	out[0] = !ret;
	return 0;

}

int m3_dcpext_native_open(struct m3_dcpext_native *dcp)
{
	int ret;

	mutex_lock(&dcp->lock);
	if (dcp->opened) {
		ret = -EALREADY;
	} else {
		ret = simple_call(dcp, A(455), false, false, false);
		if (!ret) {
			dcp->opened = true;
			dev_info(dcp->dev, "M3 first-client-open complete\n");
		}
	}
	mutex_unlock(&dcp->lock);
	return ret;
}

struct m3_dcpext_native *m3_dcpext_native_start(struct device *dev, struct m3_dcpext_rpc *bridge,
				       bool defer_open)
{
	struct m3_dcpext_native *dcp;
	int ret;

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return ERR_PTR(-ENOMEM);
	dcp->dev = dev;
	dcp->bridge = bridge;
	mutex_init(&dcp->lock);
	dcp->stride = 7680;
	ret = simple_call(dcp, A(401), false, true, true);
	if (!ret && !defer_open)
		ret = m3_dcpext_native_open(dcp);
	if (ret)
		return ERR_PTR(ret);
	if (defer_open)
		dev_info(dev, "M3 DCP kernel startup complete; first-client-open deferred\n");
	else
		dev_info(dev, "M3 DCP kernel startup and first-client-open complete\n");
	return dcp;
}

/* J514S wrappers: A409@13f7d4 (4/4), A472@1421f4 (12/8),
 * A411@13f8a8 (8/4). A472's output status is at byte 4; bytes 9/10
 * are additional boolean inputs and byte 11 is the output-pointer null flag.
 * Follow M2's display-handle 0, then power-state 1 sequence explicitly.
 */
int m3_dcpext_native_panel(struct m3_dcpext_native *dcp, unsigned int action)
{
	u8 input[80] = {}, output[8] = {};
	int ret;

	mutex_lock(&dcp->lock);
	switch (action) {
	case 1:
		ret = call(dcp, A(409), input, 4, output, 4, 0);
		/* Existing internal-display handle 0 returns 2. */
		if (!ret && get_unaligned_le32(output) != 2)
			ret = -EIO;
		if (!ret) {
			put_unaligned_le64(1, input);
			ret = call(dcp, A(472), input, 12, output, 8, 0);
			if (!ret && get_unaligned_le32(output + 4))
				ret = -EIO;
		}
		break;
	case 2:
		/* PreferredTimingElements: color 1, timing 2, 3024x1964. */
		put_unaligned_le32(1, input);
		put_unaligned_le32(2, input + 4);
		ret = call(dcp, A(411), input, 8, output, 4, 0);
		if (!ret && get_unaligned_le32(output))
			ret = -EIO;
		break;
	case 3:
		ret = call(dcp, A(479), input, 4, output, 8, 0);
		break;
	case 4:
		/* Native stub kernel ab5a53c; firmware wrapper 140fe4.
		 * Same initialization used by the M2 host before client open.
		 */
		ret = call(dcp, A(448), input, 4, output, 4, 0);
		if (!ret && get_unaligned_le32(output))
			ret = -EIO;
		break;
	case 5:
		/* Native stub kernel ab57948; firmware wrapper 13fe2c.
		 * Location 9 is the identity CTM used by the M2 KMS path.
		 */
		put_unaligned_le32(9, input);
		put_unaligned_le64(1ULL << 32, input + 4);
		put_unaligned_le64(1ULL << 32, input + 4 + 4 * 8);
		put_unaligned_le64(1ULL << 32, input + 4 + 8 * 8);
		ret = call(dcp, A(421), input, 80, output, 4, 0);
		if (!ret && get_unaligned_le32(output))
			ret = -EIO;
		break;
	case 6:
	case 7:
		/* Reversible diagnostic: M2's linear primary uses IOMFB plane 2.
		 * The next swap clears the other two primary slots atomically.
		 */
		dcp->diagnostic_plane = action == 6 ? 2 : 0;
		put_unaligned_le32(dcp->diagnostic_plane, output);
		ret = 0;
		break;
	case 8:
	case 9:
		/* Captured 26A428 linear BGRA uses the non-planar IOSurface
		 * representation. SurfaceContents fields match the stub driver.
		 */
		dcp->diagnostic_flat_surface = action == 8;
		ret = 0;
		break;
	default:
		ret = -EINVAL;
	}
	dev_info(dcp->dev, "M3 panel action %u ret=%d output=%#x/%#x\n",
		 action, ret, get_unaligned_le32(output), get_unaligned_le32(output + 4));
	if (ret)
		dcp->failed = true;
	mutex_unlock(&dcp->lock);
	return ret;
}

/* External mode IDs come from this session's TimingElements publication. */
int m3_dcpext_native_mode(struct m3_dcpext_native *dcp, u32 color, u32 timing)
{
 u8 input[8], output[4]; int ret;
 put_unaligned_le32(color, input); put_unaligned_le32(timing, input + 4);
 mutex_lock(&dcp->lock);
 ret = call(dcp, A(411), input, sizeof(input), output, sizeof(output), 0);
 if (!ret && get_unaligned_le32(output)) ret = -EIO;
 if (ret) dcp->failed = true;
 mutex_unlock(&dcp->lock);
 return ret;
}
int m3_dcpext_native_power(struct m3_dcpext_native *dcp, bool on)
{
 u8 input[12] = {}, output[8] = {}; int ret;
 put_unaligned_le64(on, input);
 mutex_lock(&dcp->lock);
 ret = call(dcp, A(472), input, sizeof(input), output, sizeof(output), 0);
 if (!ret && get_unaligned_le32(output + 4)) ret = -EIO;
 if (ret) dcp->failed = true;
 mutex_unlock(&dcp->lock);
 return ret;
}

/* Last 32 serialized swaps: ID, begin/A406-ack/completion monotonic us.
 * Fixed-size overwrite telemetry; no allocation or logging in the frame loop. */
static unsigned long long swap_timings[32 * 4];
static unsigned int swap_timing_index;
module_param_array(swap_timings, ullong, NULL, 0400);
static int native_swap(struct m3_dcpext_native *dcp, const void *surface, u64 dva,
		       u32 width, u32 height, u32 background)
{
	__le32 start[4] = {}, started[2], result[3];
	u8 *swap;
	u32 id;
	unsigned int ti;
	int ret;

	if (surface && (!dva || !width || width > M3_DCPEXT_MAX_WIDTH || !height || height > M3_DCPEXT_MAX_HEIGHT))
		return -EINVAL;
	swap = kzalloc(0x1b58, GFP_KERNEL);
	if (!swap)
		return -ENOMEM;
	mutex_lock(&dcp->lock);
	ti = (swap_timing_index++ % 32) * 4;
	swap_timings[ti + 1] = ktime_to_us(ktime_get());
	ret = call(dcp, A(406), start, sizeof(start), started, sizeof(started), 0);
	if (ret || le32_to_cpu(started[1])) {
		ret = ret ?: -EIO;
		goto out;
	}
	id = le32_to_cpu(started[0]);
	swap_timings[ti] = id;
	swap_timings[ti + 2] = ktime_to_us(ktime_get());
	if (!id) {
		ret = -EPROTO;
		goto out;
	}
	put_unaligned_le32(id, swap + 0x98);
	put_unaligned_le32(0x80000007, swap + 0x14c);
	put_unaligned_le32(0x80000007, swap + 0x150);
	/* A disabled plane must blank to black, including compositor DPMS. */
	put_unaligned_le32(background, swap + 0x154);
	memset(swap + 0x1b4b, 1, 10);
	swap[0x1b56] = swap[0x1b57] = 1;
	if (surface) {
		u32 plane = dcp->diagnostic_plane;
		u8 *s = swap + 0x508 + plane * 0x22c;

		memcpy(s, surface, 0x22c);
		if (dcp->diagnostic_flat_surface) {
			put_unaligned_le32(0, s + 3);
			put_unaligned_le32(0, s + 7);
			s[0x14] = 1; /* Captured sRGB BGRA surface. */
			put_unaligned_le16(4, s + 0x19);
			memset(s + 0x39, 0, 0x1ed - 0x39);
			put_unaligned_le64(1, s + 0x51);
			put_unaligned_le64(1, s + 0x149);
		}
		put_unaligned_le64(dva, swap + 0xdb8 + plane * 8);
		put_unaligned_le32(1, swap + 0x9c + plane * 4);
		put_unaligned_le32(width, swap + 0xb4 + plane * 16);
		put_unaligned_le32(height, swap + 0xb8 + plane * 16);
		put_unaligned_le32(width, swap + 0x114 + plane * 16);
		put_unaligned_le32(height, swap + 0x118 + plane * 16);
		swap[0x1b4b + plane] = 0;
	}
	ret = call(dcp, A(407), swap, 0x1b58, result, sizeof(result), id);
	swap_timings[ti + 3] = ktime_to_us(ktime_get());
	if (!ret && get_unaligned_le32((u8 *)result + 5))
		ret = -EIO;
	if (!ret && !surface)
		dev_info(dcp->dev, "M3 kernel swap %u completed, surface=%d\n", id, !!surface);
out:
	if (ret)
		dcp->failed = true;
	mutex_unlock(&dcp->lock);
	kfree(swap);
	return ret;
}

int m3_dcpext_native_swap(struct m3_dcpext_native *dcp, const void *surface, u64 dva,
		       u32 width, u32 height)
{
	return native_swap(dcp, surface, dva, width, height, 0xff000000);
}

int m3_dcpext_native_background(struct m3_dcpext_native *dcp, u32 color)
{
	return native_swap(dcp, NULL, 0, 0, 0, color);
}

void *m3_dcpext_native_property(struct m3_dcpext_native *dcp, const char *key, u32 *size)
{
	void *copy = NULL;
	u32 i;

	mutex_lock(&dcp->lock);
	for (i = 0; i < dcp->raw_count; i++) {
		if (!dcp->raw[i].service && !strcmp(dcp->raw[i].key, key)) {
			*size = dcp->raw[i].size;
			copy = kvmemdup(*size ? dcp->raw[i].data : "", *size ?: 1, GFP_KERNEL);
			if (!copy) copy = ERR_PTR(-ENOMEM);
			break;
		}
	}
	mutex_unlock(&dcp->lock);
	return copy;
}

bool m3_dcpext_native_property_ready(struct m3_dcpext_native *dcp,const char *key)
{
	bool ready = false;

	mutex_lock(&dcp->lock);
	for (u32 i = 0; i < dcp->raw_count; i++) {
		if (!dcp->raw[i].service && !strcmp(dcp->raw[i].key, key)) {
			ready = dcp->raw[i].size != 0;
			break;
		}
	}
	mutex_unlock(&dcp->lock);
	return ready;
}

void m3_dcpext_native_analytics_show(struct m3_dcpext_native *dcp, struct seq_file *seq)
{
	mutex_lock(&dcp->lock);
	seq_printf(seq, "events %llu\n", dcp->analytics_count);
	if (dcp->analytics_count) {
		seq_printf(seq, "last_event %.64s\ndictionary_present %u\n",
			   dcp->analytics_last, !dcp->analytics_last[4160]);
		if (!dcp->analytics_last[4160])
			seq_hex_dump(seq, "", DUMP_PREFIX_OFFSET, 16, 1,
				     dcp->analytics_last + 64, 4096, false);
	}
	mutex_unlock(&dcp->lock);
}

void m3_dcpext_native_completion_show(struct m3_dcpext_native *dcp, struct seq_file *seq)
{
	mutex_lock(&dcp->lock);
	seq_printf(seq, "callbacks %llu\n", dcp->completion_count);
	if (dcp->completion_count)
		seq_hex_dump(seq, "", DUMP_PREFIX_OFFSET, 16, 1,
			     dcp->completion_last, sizeof(dcp->completion_last), false);
	mutex_unlock(&dcp->lock);
}

int m3_dcpext_native_pump(struct m3_dcpext_native *dcp, unsigned long timeout)
{
 int ret;
 mutex_lock(&dcp->lock);
 ret=m3_dcpext_rpc_pump(dcp->bridge, timeout, callback, dcp);
 mutex_unlock(&dcp->lock);
 return ret;
}

void m3_dcpext_native_invalidate_sink(struct m3_dcpext_native *dcp)
{
 mutex_lock(&dcp->lock);
 dcp->mode_generation++;
 dcp->metadata_error=0;
 m3_property_reset(&dcp->chunk);
 for(u32 i=0;i<dcp->raw_count;) {
  if(dcp->raw[i].service ||
     (strcmp(dcp->raw[i].key,"TimingElements") &&
      strcmp(dcp->raw[i].key,"DisplayAttributes"))){i++;continue;}
  dcp->raw_bytes-=dcp->raw[i].size;kvfree(dcp->raw[i].data);
  dcp->raw[i]=dcp->raw[--dcp->raw_count];
  memset(&dcp->raw[dcp->raw_count],0,sizeof(dcp->raw[0]));
 }
 mutex_unlock(&dcp->lock);
}

u64 m3_dcpext_native_generation(struct m3_dcpext_native *dcp)
{
 u64 generation;mutex_lock(&dcp->lock);generation=dcp->mode_generation;mutex_unlock(&dcp->lock);return generation;
}

int m3_dcpext_native_metadata_error(struct m3_dcpext_native *dcp)
{
 return READ_ONCE(dcp->metadata_error);
}
