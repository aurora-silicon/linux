// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Root-only RPC transport for bounded firmware diagnostics, not a display ABI. */
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/iommu.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/poll.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "../../dcp-link.h"
#include "m3_dcp_bridge.h"

#define MAX_EVENTS 32
#define MAX_CALLBACKS 8
#define MAX_BUFFERS 64

/* Diagnostic ABI: allocations are retained until reboot, including releases. */
#define BRIDGE_ALLOC_BUFFER _IOWR('M', 0x40, struct m3_dcp_buffer)
#define BRIDGE_RETIRE_BUFFER _IOW('M', 0x41, __u32)
#define BRIDGE_MAP_PIODMA _IOWR('M', 0x42, struct m3_dcp_buffer)

struct scanout_request {
	__u32 width, height, stride, format;
	__u64 size, dva;
};
#define BRIDGE_ALLOC_SCANOUT _IOWR('M', 0x43, struct scanout_request)

/* Write op 1: submit, message=0 (normal) or 1 (nested), data=RPC packet.
 * Write op 2: callback reply, message=original message, data=output bytes.
 * Read kind 1: command reply, kind 2: callback, kind 3: unexpected message.
 * All headers are little endian. One complete record per read/write.
 */
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

struct m3_dcp_bridge {
	struct miscdevice misc;
	struct device *dev;
	struct apple_rtkit *rtkit;
	void *rpc;
	struct mutex lock;
	wait_queue_head_t wait;
	struct list_head events;
	unsigned int event_count, callback_count;
	bool failed;
	bool enabled;
	bool kernel_client;
	u32 kernel_callback_depth;
	struct work_struct *kernel_work;
	u32 buffer_count;
	u64 buffer_bytes;
	struct platform_device *piodma;
	struct iommu_domain *piodma_domain;
	struct platform_device *scanout;
	void *scanout_cpu;
	struct scanout_request scanout_info;
	struct {
		void *cpu;
		struct m3_dcp_buffer info;
		bool retired;
		bool piodma_mapped;
	} buffers[MAX_BUFFERS];
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

static void fail(struct m3_dcp_bridge *b)
{
	b->failed = true;
	dev_err(b->dev, "DCP bridge stopped; retain firmware buffers until reboot\n");
	wake_up_interruptible(&b->wait);
}

static void event(struct m3_dcp_bridge *b, u32 kind, u64 message,
		  const void *data, u32 size)
{
	struct bridge_event *e;

	if (b->event_count >= MAX_EVENTS || size > APPLE_DCP_LINK_STREAM_BUFFER_SIZE) {
		fail(b);
		return;
	}
	e = kmalloc(sizeof(*e) + size, GFP_KERNEL);
	if (!e) {
		fail(b);
		return;
	}
	e->header = (struct record_header) {
		.kind = cpu_to_le32(kind), .size = cpu_to_le32(size),
		.message = cpu_to_le64(message),
	};
	if (size)
		memcpy(e->data, data, size);
	list_add_tail(&e->list, &b->events);
	b->event_count++;
	wake_up_interruptible(&b->wait);
	if (b->kernel_work)
		queue_work(system_unbound_wq, b->kernel_work);
}

void m3_dcp_bridge_crashed(struct m3_dcp_bridge *b)
{
	mutex_lock(&b->lock);
	fail(b);
	mutex_unlock(&b->lock);
}

void m3_dcp_bridge_receive(struct m3_dcp_bridge *b, u64 message)
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
		if (stream || !b->calls[nested].active || b->calls[nested].remote != remote ||
		    apple_dcp_link_stream_layout(APPLE_DCP_LINK_SIDE_AP, 0, remote, &layout))
			goto error;
		if (memcmp(b->rpc + layout.local_offset + b->calls[nested].offset,
			   &b->calls[nested].header, sizeof(b->calls[nested].header)))
			goto error;
		event(b, 1, message, b->rpc + layout.local_offset + b->calls[nested].offset,
		      b->calls[nested].size);
		b->calls[nested].active = false;
		goto out;
	}
	if (b->callback_count >= MAX_CALLBACKS ||
	    apple_dcp_link_stream_layout(APPLE_DCP_LINK_SIDE_AP, stream, remote, &layout))
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

			dev_err(b->dev, "M3 RPC header offset=%#x words=%#x/%#x/%#x\n",
				off, le32_to_cpu(h[0]), le32_to_cpu(h[1]), le32_to_cpu(h[2]));
		}
		goto error;
	}
	b->callbacks[b->callback_count].message = message;
	b->callbacks[b->callback_count].output = view.output;
	b->callbacks[b->callback_count].size = view.output_size;
	b->callbacks[b->callback_count].call = view.call;
	b->callbacks[b->callback_count].allocation_size = 0;
	b->callbacks[b->callback_count].mapping_id = 0;
	if (view.call == 0x44343531 && view.input_size == 20 && view.output_size == 28) {
		__le64 allocation_size;

		memcpy(&allocation_size, view.input + 4, sizeof(allocation_size));
		b->callbacks[b->callback_count].allocation_size = le64_to_cpu(allocation_size);
	}
	if (view.call == 0x44323031 && view.input_size == 12 && view.output_size == 20) {
		__le64 mapping_id;

		memcpy(&mapping_id, view.input, sizeof(mapping_id));
		b->callbacks[b->callback_count].mapping_id = le64_to_cpu(mapping_id);
	}
	b->callback_count++;
	dev_dbg(b->dev, "M3 DCP callback tag=%#x input=%u output=%u\n",
		 view.call, view.input_size, view.output_size);
	event(b, 2, message, b->rpc + layout.remote_offset + view.offset, view.total_size);
	goto out;
error:
	fail(b);
out:
	mutex_unlock(&b->lock);
}

static ssize_t bridge_read(struct file *file, char __user *user, size_t count, loff_t *pos)
{
	struct m3_dcp_bridge *b = container_of(file->private_data, struct m3_dcp_bridge, misc);
	struct bridge_event *e;
	size_t size;
	int ret;

again:
	mutex_lock(&b->lock);
	if (list_empty(&b->events)) {
		ret = b->failed ? -EIO : -EAGAIN;
		mutex_unlock(&b->lock);
		if (ret == -EIO || file->f_flags & O_NONBLOCK)
			return ret;
		ret = wait_event_interruptible(b->wait, READ_ONCE(b->event_count) || READ_ONCE(b->failed));
		if (ret)
			return ret;
		goto again;
	}
	e = list_first_entry(&b->events, struct bridge_event, list);
	size = sizeof(e->header) + le32_to_cpu(e->header.size);
	ret = -EMSGSIZE;
	if (count < size)
		goto out;
	ret = -EFAULT;
	if (copy_to_user(user, &e->header, size))
		goto out;
	list_del(&e->list);
	b->event_count--;
	kfree(e);
	ret = size;
out:
	mutex_unlock(&b->lock);
	return ret;
}

static ssize_t bridge_request(struct m3_dcp_bridge *b, struct record_header *record, size_t count)
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
		if (control != b->callbacks[top].message || size != b->callbacks[top].size)
			goto out;
		memcpy(b->callbacks[top].output, record + 1, size);
		dma_wmb();
		ret = apple_rtkit_send_message(b->rtkit, APPLE_DCP_LINK_ENDPOINT,
					      apple_dcp_link_rpc_reply(control), NULL, false);
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
					    le32_to_cpu(rpc->output_size), &total);
	if (ret || total != size) {
		ret = -EMSGSIZE;
		goto out;
	}
	ret = apple_dcp_link_stream_layout(APPLE_DCP_LINK_SIDE_AP, 0, remote, &layout);
	if (ret)
		goto out;
	if (offset + size > layout.capacity) {
		ret = -EMSGSIZE;
		goto out;
	}
	memcpy(b->rpc + layout.local_offset + offset, rpc, size);
	memset(b->rpc + layout.local_offset + offset + sizeof(*rpc) + le32_to_cpu(rpc->input_size),
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
	ret = apple_rtkit_send_message(b->rtkit, APPLE_DCP_LINK_ENDPOINT, message, NULL, false);
	if (ret)
		fail(b);
out:
	mutex_unlock(&b->lock);
free_record:
	kfree(record);
	return ret ? ret : count;
}

static ssize_t bridge_write(struct file *file, const char __user *user, size_t count, loff_t *pos)
{
	struct m3_dcp_bridge *b = container_of(file->private_data, struct m3_dcp_bridge, misc);
	struct record_header *record;

	if (count < sizeof(*record) || count > sizeof(*record) + APPLE_DCP_LINK_STREAM_BUFFER_SIZE)
		return -EMSGSIZE;
	record = memdup_user(user, count);
	if (IS_ERR(record))
		return PTR_ERR(record);
	return bridge_request(b, record, count);
}

static __poll_t bridge_poll(struct file *file, poll_table *wait)
{
	struct m3_dcp_bridge *b = container_of(file->private_data, struct m3_dcp_bridge, misc);
	__poll_t mask = 0;

	poll_wait(file, &b->wait, wait);
	if (READ_ONCE(b->event_count))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(b->failed))
		mask |= EPOLLERR;
	return mask;
}

static int map_piodma_buffer(struct m3_dcp_bridge *b, u32 id)
{
	struct m3_dcp_buffer *info = &b->buffers[id - 1].info;
	struct device_node *node;
	u64 offset;
	int ret;

	if (b->buffers[id - 1].piodma_mapped)
		return 0;
	if (!b->piodma) {
		node = of_get_child_by_name(b->dev->of_node, "piodma");
		if (!node || !of_device_is_available(node) ||
		    !of_property_read_bool(node, "apple,j514s-inherited-mappings")) {
			of_node_put(node);
			return -ENODEV;
		}
		b->piodma = of_platform_device_create(node, NULL, b->dev);
		if (!b->piodma) {
			of_node_put(node);
			return -ENOMEM;
		}
		ret = dma_set_mask_and_coherent(&b->piodma->dev, DMA_BIT_MASK(42));
		if (!ret)
			ret = of_dma_configure(&b->piodma->dev, node, true);
		of_node_put(node);
		/* Never destroy an attached locked DART stream on an error path. */
		if (ret)
			return ret;
		b->piodma_domain = iommu_get_domain_for_dev(&b->piodma->dev);
	}
	if (IS_ERR_OR_NULL(b->piodma_domain))
		return -EIO;
	ret = iommu_map(b->piodma_domain, info->dva, info->physical, info->size,
			IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret)
		return ret;
	for (offset = 0; offset < info->size; offset += SZ_16K) {
		if (iommu_iova_to_phys(b->piodma_domain, info->dva + offset) != info->physical + offset) {
			fail(b);
			return -EIO;
		}
	}
	b->buffers[id - 1].piodma_mapped = true;
	dev_info(b->dev, "M3 PIODMA mapped buffer %u size=%llu dva=%#llx\n",
		 id, info->size, info->dva);
	return 0;
}

static int allocate_scanout(struct m3_dcp_bridge *b, struct scanout_request *request)
{
	static const u32 colors[] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
		0xffff00ff, 0xffff0000, 0xff0000ff, 0xff202020,
	};
	struct scanout_request info = *request;
	struct device_node *node;
	dma_addr_t dva;
	u32 *pixels;
	u32 x, y;
	int ret;

	/* One diagnostic allocation per boot; native J514S panel dimensions. */
	if (b->scanout_cpu || b->calls[0].active || b->calls[1].active || b->callback_count ||
	    info.width != 3024 || info.height != 1964 || info.stride || info.format || info.size || info.dva)
		return -EINVAL;
	if (!b->scanout) {
		node = of_find_node_by_path("disp0");
		if (!node || !of_device_is_available(node) ||
		    !of_device_is_compatible(node, "apple,t6030-display-diagnostics") ||
		    !of_property_read_bool(node, "apple,j514s-inherited-mappings")) {
			of_node_put(node);
			return -ENODEV;
		}
		b->scanout = of_find_device_by_node(node);
		if (!b->scanout) {
			of_node_put(node);
			return -ENODEV;
		}
		ret = dma_set_mask_and_coherent(&b->scanout->dev, DMA_BIT_MASK(42));
		if (!ret)
			ret = of_dma_configure(&b->scanout->dev, node, true);
		of_node_put(node);
		/* Retain the device reference and inherited mappings until reboot. */
		if (ret)
			return ret;
	}
	info.stride = info.width * 4;
	info.size = ALIGN((u64)info.stride * info.height, SZ_16K);
	info.format = 0x42475241; /* DCP BGRA8: wire FourCC ARGB */
	pixels = dma_alloc_coherent(&b->scanout->dev, info.size, &dva, GFP_KERNEL);
	if (!pixels)
		return -ENOMEM;
	info.dva = dva;
	/* Fill once; there is no per-frame CPU framebuffer copy. */
	for (y = 0; y < info.height; y++) {
		for (x = 0; x < info.width; x++) {
			u32 color = colors[x * ARRAY_SIZE(colors) / info.width];

			if (x < 4 || y < 4 || x >= info.width - 4 || y >= info.height - 4)
				color = 0xffffffff;
			pixels[y * info.width + x] = cpu_to_le32(color);
		}
	}
	dma_wmb();
	b->scanout_cpu = pixels;
	b->scanout_info = info;
	dev_info(b->dev, "M3 native scanout allocated %ux%u stride=%u size=%llu dva=%#llx\n",
		 info.width, info.height, info.stride, info.size, info.dva);
	*request = info;
	return 0;
}

static int bridge_buffer_request(struct m3_dcp_bridge *b, unsigned int command,
				 struct m3_dcp_buffer *info)
{
	struct m3_dcp_buffer request = *info;
	struct sg_table table;
	dma_addr_t dva;
	void *cpu;
	u32 id;
	size_t size;
	int ret = -EINVAL;

	if (command != BRIDGE_ALLOC_BUFFER && command != BRIDGE_RETIRE_BUFFER &&
	    command != BRIDGE_MAP_PIODMA && command != BRIDGE_ALLOC_SCANOUT)
		return -ENOTTY;
	mutex_lock(&b->lock);
	if (b->failed || !b->enabled) {
		ret = -EIO;
		goto out;
	}
	if (command == BRIDGE_ALLOC_SCANOUT) {
		ret = allocate_scanout(b, (struct scanout_request *)info);
		goto out;
	}
	if (command == BRIDGE_RETIRE_BUFFER) {
		id = request.id;
		ret = -EINVAL;
		if (!id || id > b->buffer_count || b->buffers[id - 1].retired)
			goto out;
		/* Keep mappings and memory pinned: this diagnostic has no DMA fence. */
		b->buffers[id - 1].retired = true;
		ret = 0;
		goto out;
	}
	ret = -EINVAL;
	if (command == BRIDGE_MAP_PIODMA) {
		id = request.id;
		if (!b->callback_count || !id || id > b->buffer_count ||
		    b->buffers[id - 1].retired || request.size || request.physical || request.dva || request.flags ||
		    b->callbacks[b->callback_count - 1].call != 0x44323031 ||
		    b->callbacks[b->callback_count - 1].mapping_id != id)
			goto out;
		ret = map_piodma_buffer(b, id);
		if (!ret)
			*info = b->buffers[id - 1].info;
		goto out;
	}
	if (!b->callback_count || request.flags || request.id || request.dva || request.physical ||
	    !request.size || request.size > SZ_16M ||
	    b->callbacks[b->callback_count - 1].call != 0x44343531 ||
	    b->callbacks[b->callback_count - 1].allocation_size != request.size)
		goto out;
	size = ALIGN(request.size, SZ_16K);
	ret = -ENOSPC;
	if (b->buffer_count == MAX_BUFFERS || b->buffer_bytes + size > SZ_64M)
		goto out;
	/* D451 requests both a physical address and a DCP address. Require a
	 * physically contiguous backing allocation before advertising either.
	 */
	cpu = dma_alloc_attrs(b->dev, size, &dva, GFP_KERNEL, DMA_ATTR_FORCE_CONTIGUOUS);
	ret = -ENOMEM;
	if (!cpu)
		goto out;
	ret = dma_get_sgtable_attrs(b->dev, &table, cpu, dva, size, DMA_ATTR_FORCE_CONTIGUOUS);
	if (ret)
		goto free_dma;
	ret = -ERANGE;
	if (table.orig_nents != 1 || table.sgl->length < size) {
		sg_free_table(&table);
		goto free_dma;
	}
	request.physical = sg_phys(table.sgl);
	sg_free_table(&table);
	request.size = size;
	request.dva = dva;
	request.id = b->buffer_count + 1; /* firmware treats zero as failure */
	memset(cpu, 0, size);
	dma_wmb();
	b->buffers[b->buffer_count].cpu = cpu;
	b->buffers[b->buffer_count].info = request;
	b->buffer_count++;
	b->buffer_bytes += size;
	/* One allocation per callback. Retain even on copy_to_user failure. */
	b->callbacks[b->callback_count - 1].allocation_size = 0;
	dev_info(b->dev, "M3 DCP buffer id=%u size=%zu dma=%pad\n", request.id, size, &dva);
	*info = request;
	ret = 0;
	goto out;
free_dma:
	dma_free_attrs(b->dev, size, cpu, dva, DMA_ATTR_FORCE_CONTIGUOUS);
out:
	mutex_unlock(&b->lock);
	return ret;
}

static long bridge_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct m3_dcp_bridge *b = container_of(file->private_data, struct m3_dcp_bridge, misc);
	void __user *user = (void __user *)argument;
	struct m3_dcp_buffer request = {};
	int ret;

	if (command == BRIDGE_RETIRE_BUFFER) {
		if (copy_from_user(&request.id, user, sizeof(request.id)))
			return -EFAULT;
	} else if (command == BRIDGE_ALLOC_BUFFER || command == BRIDGE_MAP_PIODMA ||
		   command == BRIDGE_ALLOC_SCANOUT) {
		if (copy_from_user(&request, user, sizeof(request)))
			return -EFAULT;
	} else {
		return -ENOTTY;
	}
	ret = bridge_buffer_request(b, command, &request);
	if (!ret && command != BRIDGE_RETIRE_BUFFER && copy_to_user(user, &request, sizeof(request)))
		ret = -EFAULT;
	return ret;
}

static const struct file_operations bridge_fops = {
	.owner = THIS_MODULE,
	.read = bridge_read,
	.write = bridge_write,
	.poll = bridge_poll,
	.unlocked_ioctl = bridge_ioctl,
};

struct m3_dcp_bridge *m3_dcp_bridge_create(struct device *dev,
					struct apple_rtkit *rtkit, void *rpc, bool kernel_client)
{
	struct m3_dcp_bridge *b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	int ret;

	if (!b)
		return ERR_PTR(-ENOMEM);
	b->dev = dev;
	b->kernel_client = kernel_client;
	b->rtkit = rtkit;
	b->rpc = rpc;
	mutex_init(&b->lock);
	init_waitqueue_head(&b->wait);
	INIT_LIST_HEAD(&b->events);
	b->misc = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR, .name = "m3-dcp-rpc", .mode = 0600,
		.fops = &bridge_fops, .parent = dev,
	};
	ret = kernel_client ? 0 : misc_register(&b->misc);
	if (ret)
		return ERR_PTR(ret);
	return b;
}

void m3_dcp_bridge_activate(struct m3_dcp_bridge *b)
{
	mutex_lock(&b->lock);
	b->enabled = true;
	mutex_unlock(&b->lock);
	dev_info(b->dev, "M3 DCP %s ready\n", b->kernel_client ? "kernel RPC client" : "RPC bridge /dev/m3-dcp-rpc");
}

/* One kernel worker owns this client, including recursive callbacks. */
static struct bridge_event *kernel_event(struct m3_dcp_bridge *b, unsigned long timeout)
{
	struct bridge_event *e;
	long ret;

	ret = wait_event_interruptible_timeout(b->wait,
		READ_ONCE(b->event_count) || READ_ONCE(b->failed), timeout);
	if (ret <= 0)
		return ERR_PTR(ret ?: -ETIMEDOUT);
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

static int kernel_send(struct m3_dcp_bridge *b, u32 kind, u64 message,
		       const void *data, u32 size)
{
	struct record_header *record;
	ssize_t ret;

	if (size > APPLE_DCP_LINK_STREAM_BUFFER_SIZE)
		return -EMSGSIZE;
	record = kmalloc(sizeof(*record) + size, GFP_KERNEL);
	if (!record)
		return -ENOMEM;
	*record = (struct record_header) {
		.kind = cpu_to_le32(kind), .size = cpu_to_le32(size),
		.message = cpu_to_le64(message),
	};
	if (size)
		memcpy(record + 1, data, size);
	ret = bridge_request(b, record, sizeof(*record) + size);
	return ret < 0 ? ret : 0;
}

static int kernel_callback(struct m3_dcp_bridge *b, struct bridge_event *e,
			   m3_dcp_callback_fn callback, void *cookie, u32 *completed)
{
	struct apple_dcp_link_rpc_header *h = (void *)e->data;
	u32 size = le32_to_cpu(e->header.size), in, out, tag;
	int ret;

	if (le32_to_cpu(e->header.kind) != 2 || size < sizeof(*h))
		return -EPROTO;
	in = le32_to_cpu(h->input_size);
	out = le32_to_cpu(h->output_size);
	tag = le32_to_cpu(h->call);
	if (in > size - sizeof(*h) || out != size - sizeof(*h) - in || !callback)
		return -EPROTO;
	memset(e->data + sizeof(*h) + in, 0, out);
	b->kernel_callback_depth++;
	ret = callback(b, cookie, tag, h + 1, in, e->data + sizeof(*h) + in, out);
	b->kernel_callback_depth--;
	if (ret)
		return ret;
	if (tag == 0x44353839 && in == 0x6f0 && completed)
		*completed = le32_to_cpup((__le32 *)(h + 1));
	return kernel_send(b, 2, le64_to_cpu(e->header.message), e->data + sizeof(*h) + in, out);
}

int m3_dcp_bridge_pump(struct m3_dcp_bridge *b, unsigned long timeout,
		      m3_dcp_callback_fn callback, void *cookie)
{
	struct bridge_event *e;
	int ret;

	if (!b->kernel_client)
		return -EBUSY;
	e = kernel_event(b, timeout);
	if (IS_ERR(e))
		return PTR_ERR(e);
	ret = kernel_callback(b, e, callback, cookie, NULL);
	kfree(e);
	return ret;
}

int m3_dcp_bridge_call(struct m3_dcp_bridge *b, u32 tag,
		      const void *input, u32 input_size, void *output, u32 output_size,
		      u32 completion_id, m3_dcp_callback_fn callback, void *cookie)
{
	struct apple_dcp_link_rpc_header *packet;
	unsigned long deadline = jiffies + 5 * HZ;
	u32 size, completed = 0;
	u64 expected = 0x42;
	bool nested, replied = false;
	int ret;

	if (!b->kernel_client)
		return -EBUSY;
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
			ret = m3_dcp_bridge_pump(b, 0, callback, cookie);
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
		expected |= b->callbacks[b->callback_count - 1].message & BIT_ULL(8);
	mutex_unlock(&b->lock);
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
			    le32_to_cpu(h->call) != tag || le32_to_cpu(h->input_size) != input_size ||
			    le32_to_cpu(h->output_size) != output_size) {
				ret = -EPROTO;
			} else {
				if (output_size)
					memcpy(output, e->data + sizeof(*h) + input_size, output_size);
				replied = true;
			}
		} else {
			ret = kernel_callback(b, e, callback, cookie, &completed);
			if (completion_id && completed && completed != completion_id)
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

int m3_dcp_bridge_alloc(struct m3_dcp_bridge *b, struct m3_dcp_buffer *info)
{
	return bridge_buffer_request(b, BRIDGE_ALLOC_BUFFER, info);
}

int m3_dcp_bridge_map(struct m3_dcp_bridge *b, struct m3_dcp_buffer *info)
{
	return bridge_buffer_request(b, BRIDGE_MAP_PIODMA, info);
}

int m3_dcp_bridge_retire(struct m3_dcp_bridge *b, u32 id)
{
	struct m3_dcp_buffer info = { .id = id };

	return bridge_buffer_request(b, BRIDGE_RETIRE_BUFFER, &info);
}

void m3_dcp_bridge_set_work(struct m3_dcp_bridge *b, struct work_struct *work)
{
	mutex_lock(&b->lock);
	b->kernel_work = work;
	if (b->event_count)
		queue_work(system_unbound_wq, work);
	mutex_unlock(&b->lock);
}
