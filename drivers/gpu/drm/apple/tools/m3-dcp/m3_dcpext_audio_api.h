/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCPEXT_AUDIO_H
#define M3_DCPEXT_AUDIO_H
#include <linux/types.h>
#define M3_AUDIO_ELEMENTS_MAX (256 * 1024)

/* Calls serialize with scanout/hotplug. Generation binds a PCM stream to one
 * connected sink; an unplug/replug must never reuse its opaque audio cookie.
 */
struct m3_dcpext_audio_ops {
	int (*call)(u16 group, u32 command, void *data, u32 bytes, u64 *generation);
	int (*identify)(char *name, size_t size, u64 *generation);
};
const struct m3_dcpext_audio_ops *m3_dcpext_audio_get(unsigned int port);
#endif
