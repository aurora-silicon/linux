/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vb2 and DMA boundary for the production ave_sess_input(). A DMA address
 * is the host address of the memory behind it, so the simulated encoder
 * read below touches exactly what the hardware would and AddressSanitizer
 * reports a read past the buffer.
 */
#define PAGE_SIZE 4096UL
#define PAGE_ALIGN(x) ALIGN((size_t)(x), PAGE_SIZE)
#define min(x, y) ((x) < (y) ? (x) : (y))
#define SZ_16K_ALIGNED 0x4000UL

#define VB2_MAX_PLANES 8

enum vb2_memory { VB2_MEMORY_MMAP = 1, VB2_MEMORY_DMABUF = 4 };

struct vb2_plane {
	void *mem;
	unsigned int bytesused, length, data_offset;
};

struct vb2_buffer {
	enum vb2_memory memory;
	struct vb2_plane planes[VB2_MAX_PLANES];
};

struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct ave_vb2_mem;

static unsigned long vb2_get_plane_payload(struct vb2_buffer *vb, unsigned int p)
{
	return vb->planes[p].bytesused;
}

static unsigned long vb2_plane_size(struct vb2_buffer *vb, unsigned int p)
{
	return vb->planes[p].length;
}

static void *vb2_plane_vaddr(struct vb2_buffer *vb, unsigned int p)
{
	return vb->planes[p].mem;
}

static dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *vb, unsigned int p)
{
	return (uintptr_t)vb->planes[p].mem;
}

struct device { int unused; };

static unsigned int dma_allocs;

static void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma, int gfp,
			     unsigned long attrs)
{
	void *p = NULL;

	assert(!posix_memalign(&p, SZ_16K_ALIGNED, size));
	memset(p, 0, size);
	*dma = (uintptr_t)p;
	dma_allocs++;
	return p;
}

static void dma_free_attrs(struct device *dev, size_t size, void *cpu, dma_addr_t dma,
			   unsigned long attrs)
{
	assert((uintptr_t)cpu == dma);
	free(cpu);
	dma_allocs--;
}
