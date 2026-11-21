/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* The T6050's PMGR power words by bank, from the ADT. Generated; do not edit. */

/* J714s power words except the cores and CPMs, by bank. */
static const u32 t6050_ps_all[] = {
#include "sleep-t6050-common-power.inc"
};

/* J716c also has the second encoder and all 32 AMCC power domains. */
static const u32 t6050_j716c_ps_all[] = {
#include "sleep-t6050-common-power.inc"
	T6050_W(T6050_PS_MEDIA, 0x001f0),	/* VENC1_SYS */
	T6050_W(T6050_PS_MEDIA, 0x00418),	/* AMCC16 */
	T6050_W(T6050_PS_MEDIA, 0x00420),	/* AMCC17 */
	T6050_W(T6050_PS_MEDIA, 0x00428),	/* AMCC18 */
	T6050_W(T6050_PS_MEDIA, 0x00430),	/* AMCC19 */
	T6050_W(T6050_PS_MEDIA, 0x00498),	/* AMCC20 */
	T6050_W(T6050_PS_MEDIA, 0x004a0),	/* AMCC21 */
	T6050_W(T6050_PS_MEDIA, 0x004a8),	/* AMCC22 */
	T6050_W(T6050_PS_MEDIA, 0x004b0),	/* AMCC23 */
	T6050_W(T6050_PS_MEDIA, 0x00518),	/* AMCC24 */
	T6050_W(T6050_PS_MEDIA, 0x00520),	/* AMCC25 */
	T6050_W(T6050_PS_MEDIA, 0x00528),	/* AMCC26 */
	T6050_W(T6050_PS_MEDIA, 0x00530),	/* AMCC27 */
	T6050_W(T6050_PS_MEDIA, 0x00598),	/* AMCC28 */
	T6050_W(T6050_PS_MEDIA, 0x005a0),	/* AMCC29 */
	T6050_W(T6050_PS_MEDIA, 0x005a8),	/* AMCC30 */
	T6050_W(T6050_PS_MEDIA, 0x005b0),	/* AMCC31 */
	T6050_W(T6050_PS_MEDIA, 0x08028),	/* VENC1_DMA */
	T6050_W(T6050_PS_MEDIA, 0x08030),	/* VENC1_PIPE4 */
	T6050_W(T6050_PS_MEDIA, 0x08038),	/* VENC1_PIPE5 */
	T6050_W(T6050_PS_MEDIA, 0x08040),	/* VENC1_ME0 */
	T6050_W(T6050_PS_MEDIA, 0x08048),	/* VENC1_ME1 */
};
