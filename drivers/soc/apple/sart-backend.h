/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_SART_BACKEND_H
#define APPLE_SART_BACKEND_H

#include <linux/soc/apple/sart.h>

/* Alternate providers retain the existing consumer API. */
struct apple_sart_backend {
	int (*add)(struct apple_sart *sart, phys_addr_t address, size_t size);
	int (*remove)(struct apple_sart *sart, phys_addr_t address,
		      size_t size);
	bool (*inherited)(struct apple_sart *sart, phys_addr_t address,
			  size_t size);
};

struct apple_sart *
apple_sart_create_backend(struct device *dev,
			  const struct apple_sart_backend *backend, void *data);
void *apple_sart_backend_data(struct apple_sart *sart);

#endif
