// SPDX-License-Identifier: GPL-2.0-only
/*
 * Keep OUTPUT mappings alive independently of vb2 buffer completion.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/dma-buf.h>
#include <linux/refcount.h>
#include <linux/slab.h>

#include <media/videobuf2-dma-contig.h>

#include "ave.h"

/*
 * vb2 owns one reference, each submitted Process owns another. A failed
 * session keeps its references in quarantine until the coprocessor and DART
 * have lost power. In particular, DQBUF/REQBUFS/close may release vb2's
 * reference without unmapping an imported attachment or freeing an MMAP IOVA.
 */
struct ave_vb2_mem {
	refcount_t refs;
	void *priv;
	struct dma_buf *dbuf;
	struct device *dev;
	bool mapped;
};

struct ave_vb2_mem *ave_vb2_get(struct vb2_buffer *vb)
{
	struct ave_vb2_mem *mem = vb->planes[0].mem_priv;

	refcount_inc(&mem->refs);
	return mem;
}

void ave_vb2_put(void *buf_priv)
{
	struct ave_vb2_mem *mem = buf_priv;

	if (!mem || !refcount_dec_and_test(&mem->refs))
		return;
	if (mem->dbuf) {
		if (mem->mapped)
			vb2_dma_contig_memops.unmap_dmabuf(mem->priv);
		vb2_dma_contig_memops.detach_dmabuf(mem->priv);
		dma_buf_put(mem->dbuf);
		put_device(mem->dev);
	} else {
		vb2_dma_contig_memops.put(mem->priv);
	}
	kfree(mem);
}

static void *ave_vb2_alloc(struct vb2_buffer *vb, struct device *dev,
			 unsigned long size)
{
	struct ave_vb2_mem *mem;

	mem = kzalloc_obj(*mem);
	if (!mem)
		return ERR_PTR(-ENOMEM);
	mem->priv = vb2_dma_contig_memops.alloc(vb, dev, size);
	if (IS_ERR(mem->priv)) {
		void *err = mem->priv;

		kfree(mem);
		return err;
	}
	refcount_set(&mem->refs, 1);
	return mem;
}

static void *ave_vb2_attach(struct vb2_buffer *vb, struct device *dev,
			  struct dma_buf *dbuf, unsigned long size)
{
	struct ave_vb2_mem *mem;

	mem = kzalloc_obj(*mem);
	if (!mem)
		return ERR_PTR(-ENOMEM);
	mem->priv = vb2_dma_contig_memops.attach_dmabuf(vb, dev, dbuf, size);
	if (IS_ERR(mem->priv)) {
		void *err = mem->priv;

		kfree(mem);
		return err;
	}
	/* vb2 can drop its dma_buf and device references before quarantine ends. */
	get_dma_buf(dbuf);
	mem->dbuf = dbuf;
	mem->dev = get_device(dev);
	refcount_set(&mem->refs, 1);
	return mem;
}

static int ave_vb2_map(void *buf_priv)
{
	struct ave_vb2_mem *mem = buf_priv;
	int ret;

	if (mem->mapped)
		return 0;
	ret = vb2_dma_contig_memops.map_dmabuf(mem->priv);
	if (!ret)
		mem->mapped = true;
	return ret;
}

static void ave_vb2_unmap(void *buf_priv)
{
	/* The final put unmaps, after both vb2 and firmware have let go. */
}

static void *ave_vb2_cookie(struct vb2_buffer *vb, void *buf_priv)
{
	struct ave_vb2_mem *mem = buf_priv;

	return vb2_dma_contig_memops.cookie(vb, mem->priv);
}

static void *ave_vb2_vaddr(struct vb2_buffer *vb, void *buf_priv)
{
	struct ave_vb2_mem *mem = buf_priv;

	return vb2_dma_contig_memops.vaddr(vb, mem->priv);
}

static int ave_vb2_mmap(void *buf_priv, struct vm_area_struct *vma)
{
	struct ave_vb2_mem *mem = buf_priv;

	return vb2_dma_contig_memops.mmap(mem->priv, vma);
}

static struct dma_buf *ave_vb2_export(struct vb2_buffer *vb, void *buf_priv,
				      unsigned long flags)
{
	struct ave_vb2_mem *mem = buf_priv;

	return vb2_dma_contig_memops.get_dmabuf(vb, mem->priv, flags);
}

static unsigned int ave_vb2_num_users(void *buf_priv)
{
	struct ave_vb2_mem *mem = buf_priv;

	return vb2_dma_contig_memops.num_users(mem->priv);
}

static void ave_vb2_prepare(void *buf_priv)
{
	struct ave_vb2_mem *mem = buf_priv;

	vb2_dma_contig_memops.prepare(mem->priv);
}

static void ave_vb2_finish(void *buf_priv)
{
	struct ave_vb2_mem *mem = buf_priv;

	vb2_dma_contig_memops.finish(mem->priv);
}

/* OUTPUT supports MMAP and DMABUF; CAPTURE never goes to the firmware. */
const struct vb2_mem_ops ave_vb2_memops = {
	.alloc = ave_vb2_alloc,
	.put = ave_vb2_put,
	.get_dmabuf = ave_vb2_export,
	.cookie = ave_vb2_cookie,
	.vaddr = ave_vb2_vaddr,
	.mmap = ave_vb2_mmap,
	.prepare = ave_vb2_prepare,
	.finish = ave_vb2_finish,
	.map_dmabuf = ave_vb2_map,
	.unmap_dmabuf = ave_vb2_unmap,
	.attach_dmabuf = ave_vb2_attach,
	.detach_dmabuf = ave_vb2_put,
	.num_users = ave_vb2_num_users,
};
