/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Compile the production consumer glue against the maintained PMP owner. */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>

struct device { void *of_node; };
static bool board = true, compatible = true;
static int links, votes, link_result, vote_result;
static int display_calls, display_result;
static bool display_admitted;
static bool asahi_g15_board(void) { return board; }
static bool of_device_is_compatible(void *node, const char *name)
{
	(void)node;
	(void)name;
	return compatible;
}
static int apple_drm_t6030_link_device(struct device *dev)
{
	assert(dev->of_node);
	display_calls++;
	display_admitted = display_result == 0;
	return display_result;
}
static int apple_pmp_link_device(struct device *dev)
{
	assert(dev->of_node);
	assert(display_admitted);
	display_admitted = false;
	links++;
	return link_result;
}
static int apple_pmp_set_device_power(unsigned char command, unsigned short device,
				    unsigned int enabled)
{
	assert(command == 0x0f && device == 5 && enabled == 1);
	votes++;
	return vote_result;
}
#include "pmp-production.h"

int main(void)
{
	struct device dev = { .of_node = &dev };

	board = false;
	assert(asahi_g15_pmp_prepare(&dev) == -ENODEV);
	assert(asahi_g15_pmp_get(&dev) == -ENODEV);
	board = true;
	compatible = false;
	assert(asahi_g15_pmp_prepare(&dev) == -ENODEV);
	assert(asahi_g15_pmp_get(&dev) == -ENODEV);
	compatible = true;
	dev.of_node = NULL;
	assert(asahi_g15_pmp_prepare(&dev) == -ENODEV);
	assert(asahi_g15_pmp_get(&dev) == -ENODEV);
	assert(display_calls == 0 && links == 0 && votes == 0);
	dev.of_node = &dev;
	display_result = -517;
	assert(asahi_g15_pmp_prepare(&dev) == -517);
	assert(links == 0 && votes == 0);
	display_result = -EINVAL;
	assert(asahi_g15_pmp_prepare(&dev) == -EINVAL);
	assert(links == 0 && votes == 0);
	display_result = 0;
	link_result = -517; /* EPROBE_DEFER */
	assert(asahi_g15_pmp_prepare(&dev) == -517);
	assert(votes == 0);
	link_result = 0;
	assert(asahi_g15_pmp_prepare(&dev) == 0);
	vote_result = -ETIMEDOUT;
	assert(asahi_g15_pmp_get(&dev) == -ETIMEDOUT);
	vote_result = -EIO;
	assert(asahi_g15_pmp_get(&dev) == -EIO);
	vote_result = 0;
	assert(asahi_g15_pmp_get(&dev) == 0);
	assert(display_calls == 4 && links == 2 && votes == 3);
	puts("production PMP bridge: board rejection, display-before-PMP admission, defer, ACK errors and exact AGX vote: PASS");
}
