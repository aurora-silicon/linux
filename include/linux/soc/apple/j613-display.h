/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_J613_25G83_H
#define APPLE_J613_25G83_H

#include <linux/kconfig.h>
#include <linux/string.h>
#include <linux/types.h>

#define J613_25G83_DCP_UUID "C042E95C-B9D8-3F0E-94B3-582A08AA6FDD"
#define J613_25G83_PMP_UUID "EFD60284-7C58-33A9-8522-CB3366AF6040"
#define J613_25G83_HANDOFF "apple,j613-25g83-mapping-handoff"

#if IS_ENABLED(CONFIG_APPLE_T6030_DISPLAY_GATE)
bool apple_t6030_display_gate_enabled(void);
#else
static inline bool apple_t6030_display_gate_enabled(void)
{
	return true;
}
#endif

static inline bool j613_25g83_identity(bool j613, u32 profile,
				     const u32 *version, size_t cells,
				     const char *uuid)
{
	return j613 && profile == 1 && version && cells == 3 &&
	       version[0] == 26 && version[1] == 6 && version[2] == 2 &&
	       uuid && !strcmp(uuid, J613_25G83_DCP_UUID);
}

static inline bool j613_25g83_clock(const u32 *witness, size_t cells)
{
	return witness && cells == 5 && witness[0] == 1 && witness[1] == 345 &&
	       witness[2] == 416 && witness[3] && !witness[4];
}

static inline bool j613_25g83_mappings(const u32 *markers, size_t cells)
{
	return markers && cells == 3 && markers[0] == 1 && markers[1] == 1 &&
	       markers[2] == 1;
}

#if IS_ENABLED(CONFIG_APPLE_J613_DISPLAY_GATE)
unsigned int apple_j613_25g83_clock_hz(void);
#else
static inline unsigned int apple_j613_25g83_clock_hz(void)
{
	return 0;
}
#endif

#endif
