// SPDX-License-Identifier: GPL-2.0-only
/*
 * ave_sess_input() hands the encoder one DART address per frame. The
 * encoder reads p->ch luma lines from it and p->ch / 2 chroma lines from
 * p->height lines further on (ave_build_process()). Every accepted address
 * must have all of that inside the OUTPUT buffer or the slot's bounce copy,
 * for any data_offset and payload vb2 lets through
 * (data_offset < bytesused <= length).
 */
static struct device dev;
static struct ave_dev ave = { .dev = &dev };
static struct ave_session sess;
static struct ave_ctx ctx = { .ave = &ave, .sess = &sess };
/* Not static: the simulated encoder reads must not be optimised away. */
u8 sink;

static void geometry(u32 width, u32 height)
{
	memset(&sess, 0, sizeof(sess));
	sess.p.width = width;
	sess.p.height = height;
	sess.p.cw = ALIGN(width, 16);
	sess.p.ch = ALIGN(height, 16);
	sess.p.stride = ALIGN(width, 64);
	sess.p.sample_bytes = 1;
}

static size_t sizeimage(void)
{
	return (size_t)sess.p.stride * sess.p.ch * 3 / 2;
}

static size_t encoder_extent(void)
{
	const struct ave_params *p = &sess.p;

	return max((size_t)p->stride * p->ch,
		   (size_t)p->stride * (p->height + p->ch / 2));
}

static bool inside(dma_addr_t a, size_t n, const void *base, size_t len)
{
	uintptr_t b = (uintptr_t)base;

	return base && a >= b && a - b <= len && n <= len - (a - b);
}

/* What the encoder does with the address: read the whole picture. */
static void encoder_read(dma_addr_t input)
{
	const struct ave_params *p = &sess.p;
	const u8 *y = (const u8 *)(uintptr_t)input;
	const u8 *uv = y + (size_t)p->stride * p->height;
	u8 sum = 0;

	for (size_t i = 0; i < (size_t)p->stride * p->ch; i++)
		sum += y[i];
	for (size_t i = 0; i < (size_t)p->stride * (p->ch / 2); i++)
		sum += uv[i];
	sink = sum;
}

static struct vb2_v4l2_buffer *buffer(enum vb2_memory memory, size_t length)
{
	struct vb2_v4l2_buffer *b = calloc(1, sizeof(*b));

	assert(b);
	b->vb2_buf.memory = memory;
	b->vb2_buf.planes[0].length = length;
	/* Exactly @length bytes, so AddressSanitizer sees a read past the end. */
	assert(!posix_memalign(&b->vb2_buf.planes[0].mem, SZ_16K_ALIGNED, length));
	memset(b->vb2_buf.planes[0].mem, 0x5a, length);
	return b;
}

static void put(struct vb2_v4l2_buffer *b)
{
	free(b->vb2_buf.planes[0].mem);
	free(b);
}

static void release_bounce(void)
{
	ave_free(&ave, &sess.slot[0].bounce);
}

/*
 * Feed one buffer through the production code; if it is accepted, check
 * the address and read the picture there as the encoder would.
 */
static int submit(struct vb2_v4l2_buffer *b, unsigned int off, unsigned int used)
{
	struct vb2_plane *pl = &b->vb2_buf.planes[0];
	const struct ave_buf *bounce = &sess.slot[0].bounce;
	dma_addr_t input = 0;
	int ret;

	assert(off < used && used <= pl->length);
	pl->data_offset = off;
	pl->bytesused = used;
	ret = ave_sess_input(&ctx, 0, b, &input);
	if (ret)
		return ret;
	if (!inside(input, encoder_extent(), pl->mem, pl->length) &&
	    !inside(input, encoder_extent(), bounce->cpu, bounce->size)) {
		fprintf(stderr,
			"FAIL: data_offset %u payload %u (%s, %u byte buffer): encoder input %#llx reads %zu bytes outside the buffer\n",
			off, used, b->vb2_buf.memory == VB2_MEMORY_MMAP ? "MMAP" : "DMABUF",
			pl->length, (unsigned long long)input, encoder_extent());
		exit(1);
	}
	if (!IS_ALIGNED(input, 64)) {
		fprintf(stderr,
			"FAIL: data_offset %u payload %u (%s, %u byte buffer): encoder input %#llx is not 64-byte aligned\n",
			off, used, b->vb2_buf.memory == VB2_MEMORY_MMAP ? "MMAP" : "DMABUF",
			pl->length, (unsigned long long)input);
		exit(1);
	}
	encoder_read(input);
	return 0;
}

/* Every 64-byte data_offset vb2 accepts, for both payload extremes. */
static void sweep(enum vb2_memory memory, size_t length)
{
	struct vb2_v4l2_buffer *b = buffer(memory, length);
	unsigned int accepted = 0, refused = 0;

	for (unsigned int k = 0; k < 2; k++) {
		unsigned int used = k ? length : sizeimage();

		for (unsigned int off = 0; off < used; off += 64) {
			if (submit(b, off, used))
				refused++;
			else
				accepted++;
			release_bounce();
		}
	}
	/* Offset 0 with the S_FMT payload always works. */
	assert(accepted);
	assert(refused);
	put(b);
}

/* A whole picture after a non-zero data_offset still goes straight in. */
static void offset_with_room(enum vb2_memory memory)
{
	unsigned int off = 4 * SZ_16K_ALIGNED;
	struct vb2_v4l2_buffer *b = buffer(memory, off + sizeimage());
	dma_addr_t input;

	b->vb2_buf.planes[0].data_offset = off;
	b->vb2_buf.planes[0].bytesused = off + sizeimage();
	assert(!ave_sess_input(&ctx, 0, b, &input));
	assert(input == (uintptr_t)b->vb2_buf.planes[0].mem + off);
	assert(!sess.slot[0].bounce.cpu);
	put(b);
}

/* Tightly packed lines are moved to the encoder's stride, unchanged. */
static void packed(enum vb2_memory memory, unsigned int off, bool in_place)
{
	const struct ave_params *p = &sess.p;
	u32 rows = p->height + DIV_ROUND_UP(p->height, 2), line = p->width;
	struct vb2_v4l2_buffer *b = buffer(memory, sizeimage());
	u8 *from = (u8 *)b->vb2_buf.planes[0].mem + off;
	const u8 *to;

	for (u32 y = 0; y < rows; y++)
		for (u32 x = 0; x < line; x++)
			from[y * line + x] = (u8)(y * 7 + x);
	assert(!submit(b, off, off + rows * line));
	to = in_place ? from : sess.slot[0].bounce.cpu;
	assert(to && (bool)sess.slot[0].bounce.cpu == !in_place);
	for (u32 y = 0; y < rows; y++)
		for (u32 x = 0; x < line; x++)
			assert(to[(size_t)y * p->stride + x] == (u8)(y * 7 + x));
	release_bounce();
	put(b);
}

/*
 * Packed lines at every byte data_offset of a buffer with room to spare:
 * each picture is accepted at an aligned address (submit() checks) and
 * its lines arrive there unchanged, in place or in the copy.
 */
static void packed_any_offset(enum vb2_memory memory)
{
	const struct ave_params *p = &sess.p;
	u32 rows = p->height + DIV_ROUND_UP(p->height, 2), line = p->width;
	struct vb2_v4l2_buffer *b = buffer(memory, sizeimage() + 2 * 64);

	for (unsigned int off = 0; off < 2 * 64; off++) {
		u8 *from = (u8 *)b->vb2_buf.planes[0].mem + off;
		const u8 *to;

		for (u32 y = 0; y < rows; y++)
			for (u32 x = 0; x < line; x++)
				from[y * line + x] = (u8)(y * 7 + x + off);
		assert(!submit(b, off, off + rows * line));
		to = sess.slot[0].bounce.cpu ? sess.slot[0].bounce.cpu : from;
		for (u32 y = 0; y < rows; y++)
			for (u32 x = 0; x < line; x++)
				assert(to[(size_t)y * p->stride + x] == (u8)(y * 7 + x + off));
		release_bounce();
	}
	put(b);
}

int main(void)
{
	static const u32 sizes[][2] = { { 128, 64 }, { 100, 60 }, { 320, 180 } };

	for (unsigned int i = 0; i < ARRAY_SIZE(sizes); i++) {
		geometry(sizes[i][0], sizes[i][1]);
		sweep(VB2_MEMORY_MMAP, sizeimage());
		sweep(VB2_MEMORY_DMABUF, sizeimage());
		sweep(VB2_MEMORY_MMAP, sizeimage() + 3 * 64);
		offset_with_room(VB2_MEMORY_MMAP);
		offset_with_room(VB2_MEMORY_DMABUF);
	}
	geometry(100, 60);
	packed(VB2_MEMORY_MMAP, 0, true);
	packed(VB2_MEMORY_MMAP, 64, false);
	packed(VB2_MEMORY_DMABUF, 0, false);
	packed(VB2_MEMORY_DMABUF, 128, false);
	packed_any_offset(VB2_MEMORY_MMAP);
	packed_any_offset(VB2_MEMORY_DMABUF);
	assert(!dma_allocs && jiffies);
	puts("ok: OUTPUT data_offset sweep (MMAP, DMABUF), offset with room, packed repack at any offset");
	return 0;
}
