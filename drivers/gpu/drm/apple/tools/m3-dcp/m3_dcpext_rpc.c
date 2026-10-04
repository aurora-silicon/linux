// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* M4 transport snapshot bce0fd0d685485 adapted for M3 D589 completion
 * and D201 20-byte output. M3 external startup remains a bounded probe. */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "../../dcp-link.h"
#include "m3_dcpext_rpc.h"
#define MAX_EVENTS 32
#define MAX_CALLBACKS 8
struct record_header {
	__le32 kind;
	__le32 size;
	__le64 message;
};

struct bridge_event {
	struct list_head list;
	struct record_header header;
	u8 data[];
};

struct m3_dcpext_rpc {
	struct device *dev;
	const struct m3_dcpext_rpc_ops *ops;
	void *cookie;
	void *rpc;
	struct mutex lock;
	struct list_head events;
	unsigned int event_count, callback_count;
	bool failed;
	bool enabled;
	u32 kernel_callback_depth;
	struct {
		bool active;
		bool remote;
		u32 offset;
		u32 size;
		struct apple_dcp_link_rpc_header header;
	} calls[2];
	struct {
		u64 message;
		void *output;
		u32 size;
		u32 call;
		u64 allocation_size;
		u64 mapping_id;
	} callbacks[MAX_CALLBACKS];
};

static void fail(struct m3_dcpext_rpc *b)
{
	b->failed = true;
	dev_err(b->dev,
		"DCP bridge stopped; retain firmware buffers until reboot\n");
}

static void event(struct m3_dcpext_rpc *b, u32 kind, u64 message,
		  const void *data, u32 size)
{
	struct bridge_event *e;

	if (b->ops->record &&
	    b->ops->record(b->cookie, kind, message, data, size)) {
		fail(b);
		return;
	}
	if (b->event_count >= MAX_EVENTS ||
	    size > APPLE_DCP_LINK_STREAM_BUFFER_SIZE) {
		fail(b);
		return;
	}
	e = kmalloc(sizeof(*e) + size, GFP_KERNEL);
	if (!e) {
		fail(b);
		return;
	}
	e->header = (struct record_header){
		.kind = cpu_to_le32(kind),
		.size = cpu_to_le32(size),
		.message = cpu_to_le64(message),
	};
	if (size)
		memcpy(e->data, data, size);
	list_add_tail(&e->list, &b->events);
	b->event_count++;
}

void m3_dcpext_rpc_crashed(struct m3_dcpext_rpc *b)
{
	mutex_lock(&b->lock);
	fail(b);
	mutex_unlock(&b->lock);
}

void m3_dcpext_rpc_receive(struct m3_dcpext_rpc *b, u64 message)
{
	struct apple_dcp_link_stream_layout layout;
	struct apple_dcp_link_rpc_view view;
	u8 stream = apple_dcp_link_message_stream(message);
	bool remote = apple_dcp_link_message_is_remote(message);
	bool nested;
	int ret;

	mutex_lock(&b->lock);
	if (b->failed)
		goto out;
	if ((message & 3) != APPLE_DCP_LINK_MSG_RPC) {
		event(b, 3, message, NULL, 0);
		goto out;
	}
	dma_rmb();
	if (message & BIT_ULL(6)) {
		nested = b->calls[1].active;
		if (stream || !b->calls[nested].active ||
		    b->calls[nested].remote != remote ||
		    apple_dcp_link_stream_layout(APPLE_DCP_LINK_SIDE_AP, 0,
						 remote, &layout))
			goto error;
		if (memcmp(b->rpc + layout.local_offset +
				   b->calls[nested].offset,
			   &b->calls[nested].header,
			   sizeof(b->calls[nested].header)))
			goto error;
		event(b, 1, message,
		      b->rpc + layout.local_offset + b->calls[nested].offset,
		      b->calls[nested].size);
		b->calls[nested].active = false;
		goto out;
	}
	if (b->callback_count >= MAX_CALLBACKS ||
	    apple_dcp_link_stream_layout(APPLE_DCP_LINK_SIDE_AP, stream, remote,
					 &layout))
		goto error;
	ret = apple_dcp_link_rpc_decode(message, b->rpc + layout.remote_offset,
					layout.capacity, &view);
	if (ret)
		goto error;
	if ((view.call >> 24) != 'D') {
		u32 off;

		/* Preserve stream-placement evidence without acknowledging bad data. */
		for (off = 0; off < APPLE_DCP_LINK_HEAP_OFFSET;
		     off += APPLE_DCP_LINK_STREAM_BUFFER_SIZE) {
			const __le32 *h = b->rpc + off;

			dev_err(b->dev,
				"M3 RPC header offset=%#x words=%#x/%#x/%#x\n",
				off, le32_to_cpu(h[0]), le32_to_cpu(h[1]),
				le32_to_cpu(h[2]));
		}
		goto error;
	}
	b->callbacks[b->callback_count].message = message;
	b->callbacks[b->callback_count].output = view.output;
	b->callbacks[b->callback_count].size = view.output_size;
	b->callbacks[b->callback_count].call = view.call;
	b->callbacks[b->callback_count].allocation_size = 0;
	b->callbacks[b->callback_count].mapping_id = 0;
	if (view.call == 0x44343531 && view.input_size == 20 &&
	    view.output_size == 28) {
		__le64 allocation_size;

		memcpy(&allocation_size, view.input + 4,
		       sizeof(allocation_size));
		b->callbacks[b->callback_count].allocation_size =
			le64_to_cpu(allocation_size);
	}
	if (view.call == 0x44323031 && view.input_size == 12 &&
	    view.output_size == 20) {
		__le64 mapping_id;

		memcpy(&mapping_id, view.input, sizeof(mapping_id));
		b->callbacks[b->callback_count].mapping_id =
			le64_to_cpu(mapping_id);
	}
	b->callback_count++;
	dev_dbg(b->dev, "M3 DCP callback tag=%#x input=%u output=%u\n",
		view.call, view.input_size, view.output_size);
	event(b, 2, message, b->rpc + layout.remote_offset + view.offset,
	      view.total_size);
	goto out;
error:
	fail(b);
out:
	mutex_unlock(&b->lock);
}

static ssize_t bridge_request(struct m3_dcpext_rpc *b,
			      struct record_header *record, size_t count)
{
	struct apple_dcp_link_stream_layout layout;
	struct apple_dcp_link_rpc_header *rpc;
	u64 message, control;
	u32 kind, size, total, offset = 0;
	bool nested, remote = false;
	int ret;

	kind = le32_to_cpu(record->kind);
	size = le32_to_cpu(record->size);
	control = le64_to_cpu(record->message);
	ret = -EINVAL;
	if (size != count - sizeof(*record))
		goto free_record;
	mutex_lock(&b->lock);
	if (!b->enabled) {
		ret = -EAGAIN;
		goto out;
	}
	if (b->failed) {
		ret = -EIO;
		goto out;
	}
	if (kind == 2) {
		unsigned int top;

		if (!b->callback_count)
			goto out;
		top = b->callback_count - 1;
		if (control != b->callbacks[top].message ||
		    size != b->callbacks[top].size)
			goto out;
		if (b->ops->record &&
		    b->ops->record(b->cookie, 4, control, record + 1, size)) {
			ret = -ENOSPC;
			fail(b);
			goto out;
		}
		memcpy(b->callbacks[top].output, record + 1, size);
		dma_wmb();
		ret = b->ops->send(b->cookie, APPLE_DCP_LINK_ENDPOINT,
				   apple_dcp_link_rpc_reply(control));
		if (ret)
			fail(b);
		else
			b->callback_count--;
		goto out;
	}
	if (kind != 1 || control > 1 || size < sizeof(*rpc))
		goto out;
	nested = control;
	if (b->calls[nested].active || (nested && !b->callback_count) ||
	    (!nested && b->callback_count)) {
		ret = -EBUSY;
		goto out;
	}
	if (nested) {
		u64 callback = b->callbacks[b->callback_count - 1].message;

		if (apple_dcp_link_message_stream(callback)) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		/* Re-entry uses the callback's stream without the new-stream IRQ flag. */
		remote = apple_dcp_link_message_is_remote(callback);
		if (b->calls[0].active && b->calls[0].remote == remote)
			offset = ALIGN(b->calls[0].size, 64);
	}
	rpc = (void *)(record + 1);
	ret = apple_dcp_link_rpc_payload_size(le32_to_cpu(rpc->input_size),
					      le32_to_cpu(rpc->output_size),
					      &total);
	if (ret || total != size) {
		ret = -EMSGSIZE;
		goto out;
	}
	ret = apple_dcp_link_stream_layout(APPLE_DCP_LINK_SIDE_AP, 0, remote,
					   &layout);
	if (ret)
		goto out;
	if (offset + size > layout.capacity) {
		ret = -EMSGSIZE;
		goto out;
	}
	if (b->ops->record &&
	    b->ops->record(b->cookie, 3, control, rpc, size)) {
		ret = -ENOSPC;
		fail(b);
		goto out;
	}
	memcpy(b->rpc + layout.local_offset + offset, rpc, size);
	memset(b->rpc + layout.local_offset + offset + sizeof(*rpc) +
		       le32_to_cpu(rpc->input_size),
	       0, le32_to_cpu(rpc->output_size));
	b->calls[nested].header = *rpc;
	b->calls[nested].size = size;
	b->calls[nested].offset = offset;
	b->calls[nested].remote = remote;
	b->calls[nested].active = true;
	/* M3 bit 9 requires a free stream; it is not a recursive-call marker. */
	apple_dcp_link_rpc_message(offset, size, false, &message);
	apple_dcp_link_stream_message(message, 0, remote, &message);
	dma_wmb();
	ret = b->ops->send(b->cookie, APPLE_DCP_LINK_ENDPOINT, message);
	if (ret)
		fail(b);
out:
	mutex_unlock(&b->lock);
free_record:
	kfree(record);
	return ret ? ret : count;
}

struct m3_dcpext_rpc *m3_dcpext_rpc_create(struct device *dev, void *rpc,
					   const struct m3_dcpext_rpc_ops *ops,
					   void *cookie)
{
	struct m3_dcpext_rpc *b;
	if (!dev || !rpc || !ops || !ops->send || !ops->poll)
		return ERR_PTR(-EINVAL);
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return ERR_PTR(-ENOMEM);
	b->dev = dev;
	b->rpc = rpc;
	b->ops = ops;
	b->cookie = cookie;
	b->enabled = true;
	mutex_init(&b->lock);
	INIT_LIST_HEAD(&b->events);
	return b;
}
void m3_dcpext_rpc_destroy(struct m3_dcpext_rpc *b)
{
	struct bridge_event *e, *next;
	list_for_each_entry_safe(e, next, &b->events, list) {
		list_del(&e->list);
		kfree(e);
	}
	kfree(b);
}
static struct bridge_event *kernel_event(struct m3_dcpext_rpc *b,
					 unsigned long timeout)
{
	struct bridge_event *e;
	long ret;

	unsigned long deadline = jiffies + timeout;

	while (!READ_ONCE(b->event_count) && !READ_ONCE(b->failed)) {
		long remaining = deadline - jiffies;

		if (!timeout || remaining <= 0)
			return ERR_PTR(-ETIMEDOUT);
		ret = b->ops->poll(b->cookie, remaining);
		if (ret)
			return ERR_PTR(ret);
	}
	mutex_lock(&b->lock);
	if (b->failed || list_empty(&b->events)) {
		mutex_unlock(&b->lock);
		return ERR_PTR(-EIO);
	}
	e = list_first_entry(&b->events, struct bridge_event, list);
	list_del(&e->list);
	b->event_count--;
	mutex_unlock(&b->lock);
	return e;
}

static int kernel_send(struct m3_dcpext_rpc *b, u32 kind, u64 message,
		       const void *data, u32 size)
{
	struct record_header *record;
	ssize_t ret;

	if (size > APPLE_DCP_LINK_STREAM_BUFFER_SIZE)
		return -EMSGSIZE;
	record = kmalloc(sizeof(*record) + size, GFP_KERNEL);
	if (!record)
		return -ENOMEM;
	*record = (struct record_header){
		.kind = cpu_to_le32(kind),
		.size = cpu_to_le32(size),
		.message = cpu_to_le64(message),
	};
	if (size)
		memcpy(record + 1, data, size);
	ret = bridge_request(b, record, sizeof(*record) + size);
	return ret < 0 ? ret : 0;
}

static int kernel_callback(struct m3_dcpext_rpc *b, struct bridge_event *e,
			   m3_dcpext_callback_fn callback, void *cookie,
			   u32 *completed)
{
	struct apple_dcp_link_rpc_header *h = (void *)e->data;
	u32 size = le32_to_cpu(e->header.size), in, out, tag;
	int ret;

	if (le32_to_cpu(e->header.kind) != 2 || size < sizeof(*h))
		return -EPROTO;
	in = le32_to_cpu(h->input_size);
	out = le32_to_cpu(h->output_size);
	tag = le32_to_cpu(h->call);
	if (in > size - sizeof(*h) || out != size - sizeof(*h) - in ||
	    !callback)
		return -EPROTO;
	memset(e->data + sizeof(*h) + in, 0, out);
	b->kernel_callback_depth++;
	ret = callback(b, cookie, tag, h + 1, in, e->data + sizeof(*h) + in,
		       out);
	b->kernel_callback_depth--;
	if (ret)
		return ret;
	if (tag == 0x44353839 && in == 0x6f0 && completed)
		*completed = le32_to_cpup((__le32 *)(h + 1));
	return kernel_send(b, 2, le64_to_cpu(e->header.message),
			   e->data + sizeof(*h) + in, out);
}

int m3_dcpext_rpc_pump(struct m3_dcpext_rpc *b, unsigned long timeout,
		       m3_dcpext_callback_fn callback, void *cookie)
{
	struct bridge_event *e;
	int ret;

	e = kernel_event(b, timeout);
	if (IS_ERR(e))
		return PTR_ERR(e);
	ret = kernel_callback(b, e, callback, cookie, NULL);
	kfree(e);
	return ret;
}

static int rpc_call(struct m3_dcpext_rpc *b, u32 tag, const void *input,
		    u32 input_size, void *output, u32 output_size,
		    u32 completion_id, m3_dcpext_callback_fn callback,
		    void *cookie, bool (*valid)(void *), bool *rejected)
{
	struct apple_dcp_link_rpc_header *packet;
	unsigned long deadline = jiffies + 5 * HZ;
	u32 size, completed = 0;
	u64 expected = 0x42;
	bool nested, replied = false;
	int ret;

	ret = apple_dcp_link_rpc_payload_size(input_size, output_size, &size);
	if (ret || size > APPLE_DCP_LINK_STREAM_BUFFER_SIZE)
		return -EMSGSIZE;
	packet = kzalloc(size, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;
	apple_dcp_link_rpc_header_encode(packet, tag, input_size, output_size);
	if (input_size)
		memcpy(packet + 1, input, input_size);
retry:
	/* Drain asynchronous notifications before starting an outer request.
	 * A queued callback is not recursive entry until its handler is running.
	 */
	if (!b->kernel_callback_depth) {
		while (READ_ONCE(b->event_count)) {
			if (time_after_eq(jiffies, deadline)) {
				kfree(packet);
				return -ETIMEDOUT;
			}
			ret = m3_dcpext_rpc_pump(b, 0, callback, cookie);
			if (ret) {
				kfree(packet);
				return ret;
			}
		}
	}
	mutex_lock(&b->lock);
	nested = b->kernel_callback_depth != 0;
	if (nested && !b->callback_count) {
		mutex_unlock(&b->lock);
		kfree(packet);
		return -EPROTO;
	}
	if (nested)
		expected |= b->callbacks[b->callback_count - 1].message &
			    BIT_ULL(8);
	mutex_unlock(&b->lock);
	if (valid && !valid(cookie)) {
		*rejected = true;
		kfree(packet);
		return -ESTALE;
	}
	ret = kernel_send(b, 1, nested, packet, size);
	if (ret == -EBUSY && !nested && time_before(jiffies, deadline))
		goto retry; /* A callback arrived between draining and submitting. */
	kfree(packet);
	if (ret)
		return ret;
	while (time_before(jiffies, deadline)) {
		long remaining = deadline - jiffies;
		struct bridge_event *e = kernel_event(b, max(remaining, 0L));
		struct apple_dcp_link_rpc_header *h;

		if (IS_ERR(e))
			return PTR_ERR(e);
		h = (void *)e->data;
		if (le32_to_cpu(e->header.kind) == 1) {
			if (replied || le32_to_cpu(e->header.size) != size ||
			    le64_to_cpu(e->header.message) != expected ||
			    le32_to_cpu(h->call) != tag ||
			    le32_to_cpu(h->input_size) != input_size ||
			    le32_to_cpu(h->output_size) != output_size) {
				ret = -EPROTO;
			} else {
				if (output_size)
					memcpy(output,
					       e->data + sizeof(*h) +
						       input_size,
					       output_size);
				replied = true;
			}
		} else {
			ret = kernel_callback(b, e, callback, cookie,
					      &completed);
			if (completion_id && completed &&
			    completed != completion_id)
				ret = -EPROTO;
		}
		kfree(e);
		if (ret)
			return ret;
		if (replied && (!completion_id || completed == completion_id))
			return 0;
	}
	return -ETIMEDOUT;
}

int m3_dcpext_rpc_alloc(struct m3_dcpext_rpc *b, struct m3_dcpext_buffer *info)
{
	u32 top;
	int ret = -EINVAL;
	mutex_lock(&b->lock);
	if (!b->failed && b->callback_count && b->ops->alloc) {
		top = b->callback_count - 1;
		if (b->callbacks[top].call == 0x44343531 && info->size &&
		    b->callbacks[top].allocation_size == info->size &&
		    !info->id && !info->flags && !info->dva &&
		    !info->physical) {
			ret = b->ops->alloc(b->cookie, info);
			if (!ret)
				b->callbacks[top].allocation_size = 0;
		}
	}
	mutex_unlock(&b->lock);
	return ret;
}
int m3_dcpext_rpc_map(struct m3_dcpext_rpc *b, struct m3_dcpext_buffer *info)
{
	u32 top;
	int ret = -EINVAL;
	mutex_lock(&b->lock);
	if (!b->failed && b->callback_count && b->ops->map) {
		top = b->callback_count - 1;
		if (b->callbacks[top].call == 0x44323031 && info->id &&
		    b->callbacks[top].mapping_id == info->id && !info->flags &&
		    !info->size && !info->dva && !info->physical)
			ret = b->ops->map(b->cookie, info);
	}
	mutex_unlock(&b->lock);
	return ret;
}
int m3_dcpext_rpc_dart_power(struct m3_dcpext_rpc *b, bool on)
{
 int ret = -EOPNOTSUPP;
 mutex_lock(&b->lock);
 if (!b->failed && b->callback_count && b->ops->dart_power &&
     b->callbacks[b->callback_count - 1].call == 0x44353734)
  ret = b->ops->dart_power(b->cookie, on);
 mutex_unlock(&b->lock);
 return ret;
}

int m3_dcpext_rpc_retire(struct m3_dcpext_rpc *b, u32 id)
{
	if (!id || !b->ops->retire)
		return -EINVAL;
	return b->ops->retire(b->cookie, id);
}

int m3_dcpext_rpc_call(struct m3_dcpext_rpc *b, u32 tag, const void *input,
		       u32 input_size, void *output, u32 output_size,
		       u32 completion_id, m3_dcpext_callback_fn callback,
		       void *cookie)
{
	return m3_dcpext_rpc_call_checked(b, tag, input, input_size, output,
					output_size, completion_id, callback, cookie, NULL);
}

int m3_dcpext_rpc_call_checked(struct m3_dcpext_rpc *b, u32 tag, const void *input,
			     u32 input_size, void *output, u32 output_size,
			     u32 completion_id, m3_dcpext_callback_fn callback,
			     void *cookie, bool (*valid)(void *))
{
	bool rejected = false;
	int ret;

	if ((input_size && !input) || (output_size && !output))
		return -EINVAL;
	ret = rpc_call(b, tag, input, input_size, output, output_size,
		       completion_id, callback, cookie, valid, &rejected);
	if (ret && !rejected)
		m3_dcpext_rpc_crashed(b);
	return ret;
}
