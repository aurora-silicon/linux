// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding AArch32 type1 probe; no armhf sysroot or physical DMA. */
#define __user
#include "../../../../../include/uapi/linux/vfio.h"
#include "../../../../../drivers/vfio/vfio_iommu_type1_test.h"

typedef unsigned int u32;
typedef unsigned long long u64;
#define IOVA (256UL * 1024 * 1024)
static unsigned int stage;

static long call(long n, long a, long b, long c, long d, long e, long f)
{
	register long r0 __asm__("r0") = a, r1 __asm__("r1") = b,
			 r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d, r4 __asm__("r4") = e,
			 r5 __asm__("r5") = f;
	register long r7 __asm__("r7") = n;

	__asm__ volatile("svc 0"
			 : "+r"(r0)
			 : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
			 : "memory", "cc");
	return r0;
}

static u32 number(const char *s)
{
	u32 n = 0;

	while (*s >= '0' && *s <= '9')
		n = n * 10 + *s++ - '0';
	return n;
}

static void clear(void *data, u32 size)
{
	volatile unsigned char *p = data;

	for (u32 i = 0; i < size; i++)
		p[i] = 0;
}

/* Aggregate initialization may use these compiler-runtime entry points. */
void __aeabi_memclr8(void *data, u32 size)
{
	clear(data, size);
}
void __aeabi_memclr4(void *data, u32 size)
{
	clear(data, size);
}
void __aeabi_memclr(void *data, u32 size)
{
	clear(data, size);
}

_Noreturn static void finish(int code)
{
	if (code) {
		char msg[] = "not ok - AArch32 type1 stage=00\n";

		msg[29] = '0' + stage / 10;
		msg[30] = '0' + stage % 10;
		call(4, 1, (long)msg, sizeof(msg) - 1, 0, 0, 0);
	}
	call(1, code, 0, 0, 0, 0, 0);
	for (;;)
		;
}
#define CHECK(x)                       \
	do {                           \
		stage++;               \
		if (!(x))              \
			finish(stage); \
	} while (0)

static long ioctl_test(int fd, u32 request, void *arg)
{
	return call(54, fd, request, (long)arg, 0, 0, 0);
}

static int state(int fd, struct vfio_type1_test_cmd *cmd)
{
	clear(cmd, sizeof(*cmd));
	return ioctl_test(fd, VFIO_TYPE1_TEST_STATE, cmd);
}

static long map(int fd, u32 address, u32 size)
{
	struct vfio_iommu_type1_dma_map cmd;

	clear(&cmd, sizeof(cmd));
	cmd.argsz = sizeof(cmd);
	cmd.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	cmd.iova = IOVA;
	cmd.vaddr = address;
	cmd.size = size;
	return ioctl_test(fd, VFIO_IOMMU_MAP_DMA, &cmd);
}

static long unmap(int fd, u32 size)
{
	struct vfio_iommu_type1_dma_unmap cmd;
	long ret;

	clear(&cmd, sizeof(cmd));
	cmd.argsz = sizeof(cmd);
	cmd.iova = IOVA;
	cmd.size = size;
	ret = ioctl_test(fd, VFIO_IOMMU_UNMAP_DMA, &cmd);
	return ret ? ret : cmd.size == size ? 0 : -1;
}

static long rw(int fd, unsigned char *bytes, unsigned int flags)
{
	struct vfio_type1_test_cmd cmd;

	clear(&cmd, sizeof(cmd));
	cmd.iova = IOVA;
	cmd.buffer = (u32)bytes;
	cmd.size = 16;
	cmd.flags = flags;
	return ioctl_test(fd, VFIO_TYPE1_TEST_RW, &cmd);
}

_Noreturn void probe(u32 *sp)
{
	u32 argc = *sp++, pages = 0;
	char **argv = (void *)sp;
	struct vfio_type1_test_cmd cmd;
	struct vfio_iommu_type1_dirty_bitmap dirty;
	struct vfio_iommu_type1_info info;
	unsigned char bytes[16];
	long fd, file, data, pid;
	int status = -1;
	u32 native, length;

	sp += argc + 1;
	while (*sp)
		sp++;
	sp++;
	while (sp[0]) {
		if (sp[0] == 6)
			pages = sp[1];
		sp += 2;
	}
	CHECK(argc == 3 && pages == number(argv[2]));
	native = number(argv[1]);
	length = native + 4096;
	fd = call(5, (long)"/dev/vfio-test", 2, 0, 0, 0, 0);
	file = call(385, (long)"compat-vfio", 0, 0, 0, 0, 0);
	CHECK(fd >= 0 && file >= 0 && !call(93, file, 2 * native, 0, 0, 0, 0));
	data = call(192, 0, 2 * native, 3, 1, file, 0);
	CHECK((u32)data < 0xfffff001U);
	CHECK(call(125, data + 4096, 4096, 1, 0, 0, 0) ==
	      (pages == 4096 ? 0 : -22));
	CHECK(!call(125, data, 2 * native, 3, 0, 0, 0));
	for (u32 i = 0; i < 2 * native; i++)
		((volatile unsigned char *)data)[i] = 0x53;
	clear(&info, sizeof(info));
	info.argsz = sizeof(info);
	CHECK(!ioctl_test(fd, VFIO_IOMMU_GET_INFO, &info) &&
	      (info.iova_pgsizes & 4096));
	CHECK(!map(fd, data, 2 * native));
	CHECK(!state(fd, &cmd) && !cmd.locked_bytes);
	CHECK(!rw(fd, bytes, 0) && bytes[0] == 0x53 && bytes[15] == 0x53);
	clear(&dirty, sizeof(dirty));
	dirty.argsz = sizeof(dirty);
	dirty.flags = VFIO_IOMMU_DIRTY_PAGES_FLAG_START;
	CHECK(!ioctl_test(fd, VFIO_IOMMU_DIRTY_PAGES, &dirty));
	clear(&cmd, sizeof(cmd));
	cmd.iova = IOVA + 19;
	CHECK(!ioctl_test(fd, VFIO_TYPE1_TEST_PIN, &cmd));
	CHECK(!state(fd, &cmd) && cmd.locked_bytes == native &&
	      cmd.external_refs == 1);
	/* Compat pointer padding must not become part of the native pointer. */
	_Alignas(struct vfio_iommu_type1_dirty_bitmap_get) unsigned char
		buffer[sizeof(dirty) +
		       sizeof(struct vfio_iommu_type1_dirty_bitmap_get)];
	struct vfio_iommu_type1_dirty_bitmap *header = (void *)buffer;
	struct vfio_iommu_type1_dirty_bitmap_get *range = (void *)header->data;
	u64 bits = 0;

	clear(buffer, sizeof(buffer));
	header->argsz = sizeof(buffer);
	header->flags = VFIO_IOMMU_DIRTY_PAGES_FLAG_GET_BITMAP;
	range->iova = IOVA;
	range->size = 2 * native;
	range->bitmap.pgsize = 4096;
	range->bitmap.size = sizeof(bits);
	range->bitmap.data = &bits;
	CHECK(!ioctl_test(fd, VFIO_IOMMU_DIRTY_PAGES, buffer) &&
	      bits == ((1ULL << (native >> 12)) - 1));
	_Static_assert(
		sizeof(range->bitmap.data) == 4 && sizeof(range->bitmap) == 24,
		"AArch32 bitmap has a 32-bit pointer and trailing padding");
	((unsigned char *)&range
		 ->bitmap)[__builtin_offsetof(struct vfio_bitmap, data) +
			   sizeof(range->bitmap.data)] = 0xa5;
	bits = 0;
	CHECK(!ioctl_test(fd, VFIO_IOMMU_DIRTY_PAGES, buffer) &&
	      bits == ((1ULL << (native >> 12)) - 1));
	_Alignas(struct vfio_bitmap) unsigned char
		unmap_buffer[sizeof(struct vfio_iommu_type1_dma_unmap) +
			     sizeof(struct vfio_bitmap)];
	struct vfio_iommu_type1_dma_unmap *remove = (void *)unmap_buffer;
	struct vfio_bitmap *remove_bitmap = (void *)remove->data;

	clear(unmap_buffer, sizeof(unmap_buffer));
	remove->argsz = sizeof(unmap_buffer);
	remove->flags = VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP;
	remove->iova = IOVA;
	remove->size = 2 * native;
	remove_bitmap->pgsize = 4096;
	remove_bitmap->size = sizeof(bits);
	remove_bitmap->data = &bits;
	((unsigned char *)
		 remove_bitmap)[__builtin_offsetof(struct vfio_bitmap, data) +
				sizeof(remove_bitmap->data)] = 0x5a;
	bits = 0;
	CHECK(!ioctl_test(fd, VFIO_IOMMU_UNMAP_DMA, unmap_buffer) &&
	      remove->size == 2 * native &&
	      bits == ((1ULL << (native >> 12)) - 1));
	CHECK(!state(fd, &cmd) && !cmd.locked_bytes && !cmd.external_refs &&
	      cmd.notifications == 1);
	clear(&cmd, sizeof(cmd));
	cmd.size = 4096;
	CHECK(!ioctl_test(fd, VFIO_TYPE1_TEST_DOMAIN_ADD, &cmd));
	pid = call(2, 0, 0, 0, 0, 0, 0);
	if (!pid)
		finish(map(fd, data + 4096, length) ? 99 : 0);
	CHECK(pid > 0 && call(114, pid, (long)&status, 0, 0, 0, 0) == pid &&
	      !status);
	CHECK(!state(fd, &cmd) && !cmd.owner_users &&
	      cmd.locked_bytes == 2 * native && cmd.mapped_bytes[0] == length);
	CHECK(rw(fd, bytes, 0) == -14);
	CHECK(!rw(fd, bytes, VFIO_TYPE1_TEST_WORKER) && bytes[0] == 0x53 &&
	      bytes[15] == 0x53);
	CHECK(!unmap(fd, length));
	CHECK(!state(fd, &cmd) && !cmd.locked_bytes && !cmd.mapped_bytes[0]);
	CHECK(!call(91, data, 2 * native, 0, 0, 0, 0));
	CHECK(!call(6, file, 0, 0, 0, 0, 0) && !call(6, fd, 0, 0, 0, 0, 0));
	static const char ok[] =
		"ok - AArch32 type1 real ioctls, source exit and fallback alignment\n";

	call(4, 1, (long)ok, sizeof(ok) - 1, 0, 0, 0);
	finish(0);
}
__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov r0,sp\nbl probe\n");
