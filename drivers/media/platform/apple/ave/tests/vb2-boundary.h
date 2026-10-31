/* SPDX-License-Identifier: GPL-2.0-only */
/* DMA-contig boundary; real AVE memory operations run unchanged. */
typedef unsigned int refcount_t;
static void refcount_set(refcount_t *r, unsigned int n)
{
	*r = n;
}
static void refcount_inc(refcount_t *r)
{
	assert(*r);
	++*r;
}
static bool refcount_dec_and_test(refcount_t *r)
{
	assert(*r);
	return !--*r;
}
struct dma_buf { unsigned int refs; };
struct vm_area_struct { int unused; };
struct vb2_mem_ops {
	void *(*alloc)(struct vb2_buffer *vb, struct device *dev, unsigned long size);
	void (*put)(void *priv);
	struct dma_buf *(*get_dmabuf)(struct vb2_buffer *vb, void *priv, unsigned long flags);
	void *(*cookie)(struct vb2_buffer *vb, void *priv);
	void *(*vaddr)(struct vb2_buffer *vb, void *priv);
	int (*mmap)(void *priv, struct vm_area_struct *vma);
	void (*prepare)(void *priv);
	void (*finish)(void *priv);
	int (*map_dmabuf)(void *priv);
	void (*unmap_dmabuf)(void *priv);
	void *(*attach_dmabuf)(struct vb2_buffer *vb, struct device *dev,
			       struct dma_buf *dbuf, unsigned long size);
	void (*detach_dmabuf)(void *priv);
	unsigned int (*num_users)(void *priv);
};
static void get_dma_buf(struct dma_buf *b)
{
	assert(b->refs);
	b->refs++;
}
static void dma_buf_put(struct dma_buf *b)
{
	assert(b->refs);
	b->refs--;
}
static struct device *get_device(struct device *d)
{
	d->dma_refs++;
	return d;
}
static void put_device(struct device *d)
{
	assert(d->dma_refs);
	d->dma_refs--;
}
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define kzalloc_obj(p) calloc(1, sizeof(p))
struct test_mapping {
	bool firmware_visible, mapped;
	dma_addr_t dma;
};
static bool stop_seen, power_seen, dma_alloc_fail, dma_attach_fail, dma_map_fail;
static unsigned int mapping_frees, mapping_unmaps, mapping_detaches;
static void *test_dma_alloc(struct vb2_buffer *vb, struct device *dev, unsigned long size)
{
	struct test_mapping *m = calloc(1, sizeof(*m));

	assert(m);
	if (dma_alloc_fail) {
		free(m);
		return ERR_PTR(-ENOMEM);
	}
	m->dma = (dma_addr_t)(uintptr_t)m;
	return m;
}
static void test_dma_put(void *priv)
{
	struct test_mapping *m = priv;

	/* Every command-referenced mapping survives until stop AND DART reset. */
	assert(!m->firmware_visible || (stop_seen && power_seen));
	mapping_frees++;
	free(m);
}
static void *test_dma_attach(struct vb2_buffer *vb, struct device *dev,
			     struct dma_buf *dbuf, unsigned long size)
{
	return dma_attach_fail ? ERR_PTR(-ENOMEM) : test_dma_alloc(vb, dev, size);
}
static int test_dma_map(void *priv)
{
	struct test_mapping *m = priv;

	assert(!m->mapped);
	if (dma_map_fail)
		return -EIO;
	m->mapped = true;
	return 0;
}
static void test_dma_unmap(void *priv)
{
	struct test_mapping *m = priv;

	assert(m->mapped);
	assert(!m->firmware_visible || (stop_seen && power_seen));
	mapping_unmaps++;
	m->mapped = false;
}
static void test_dma_detach(void *priv)
{
	struct test_mapping *m = priv;

	assert(!m->mapped);
	mapping_detaches++;
	test_dma_put(priv);
}
static void *test_dma_cookie(struct vb2_buffer *vb, void *priv)
{
	return &((struct test_mapping *)priv)->dma;
}
static void *test_dma_vaddr(struct vb2_buffer *vb, void *priv)
{
	return priv;
}
static int test_dma_mmap(void *priv, struct vm_area_struct *vma)
{
	return 0;
}
static unsigned int test_dma_users(void *priv)
{
	return 1;
}
static void test_dma_sync(void *priv) { }
static struct dma_buf *test_dma_export(struct vb2_buffer *vb, void *priv, unsigned long flags)
{
	return NULL;
}
static const struct vb2_mem_ops vb2_dma_contig_memops = {
	.alloc = test_dma_alloc, .put = test_dma_put,
	.attach_dmabuf = test_dma_attach, .detach_dmabuf = test_dma_detach,
	.map_dmabuf = test_dma_map, .unmap_dmabuf = test_dma_unmap,
	.cookie = test_dma_cookie, .vaddr = test_dma_vaddr, .mmap = test_dma_mmap,
	.num_users = test_dma_users, .prepare = test_dma_sync, .finish = test_dma_sync,
	.get_dmabuf = test_dma_export,
};
