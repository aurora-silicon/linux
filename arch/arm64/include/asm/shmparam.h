/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_SHMPARAM_H
#define __ASM_SHMPARAM_H

/*
 * For IPC syscalls from compat tasks, we need to use the legacy 16k
 * alignment value. Since we don't have aliasing D-caches, the rest of
 * the time we can safely use PAGE_SIZE.
 */
#define COMPAT_SHMLBA	(4 * PAGE_SIZE)

/* SHMLBA also aligns kernel allocations; scale only SysV attachments. */
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
#define arch_shm_attach_align(mm, align) \
	(((align) >> PAGE_SHIFT) << mm_page_shift(mm))
#endif

#include <asm-generic/shmparam.h>

#endif /* __ASM_SHMPARAM_H */
