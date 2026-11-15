/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Private disposable-guest test protocol, not a supported VFIO ABI. */
#ifndef VFIO_IOMMU_TYPE1_TEST_H
#define VFIO_IOMMU_TYPE1_TEST_H
#include <linux/ioctl.h>
#include <linux/types.h>

struct vfio_type1_test_cmd {
	__u64 iova;
	__u64 size;
	__u64 buffer;
	__u64 fail_after;
	__u64 locked_bytes;
	__u64 owner_locked_bytes;
	__u64 mapped_bytes[2];
	__u64 native_page_size;
	__u64 minimum_page_size;
	__u32 domain;
	__u32 flags;
	__u32 owner_users;
	__u32 notifications;
	__u32 external_refs;
	__u32 reserved;
};

#define VFIO_TYPE1_TEST_DOMAIN_ADD _IOWR('V', 1, struct vfio_type1_test_cmd)
#define VFIO_TYPE1_TEST_DOMAIN_DEL _IOWR('V', 2, struct vfio_type1_test_cmd)
#define VFIO_TYPE1_TEST_RW         _IOWR('V', 3, struct vfio_type1_test_cmd)
#define VFIO_TYPE1_TEST_PIN        _IOWR('V', 4, struct vfio_type1_test_cmd)
#define VFIO_TYPE1_TEST_UNPIN      _IOWR('V', 5, struct vfio_type1_test_cmd)
#define VFIO_TYPE1_TEST_STATE      _IOWR('V', 6, struct vfio_type1_test_cmd)
#define VFIO_TYPE1_TEST_EMULATED _IOWR('V', 7, struct vfio_type1_test_cmd)
#define VFIO_TYPE1_TEST_STATE_FRAGMENTS 1U
#define VFIO_TYPE1_TEST_WRITE 1U
#define VFIO_TYPE1_TEST_WORKER 2U
#endif
