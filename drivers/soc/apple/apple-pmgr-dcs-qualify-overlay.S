/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
.section .rodata,"a"
.balign 8
.global dcs_qualify_overlay_begin
dcs_qualify_overlay_begin:
.incbin "drivers/soc/apple/apple-pmgr-dcs-qualify.dtbo"
.global dcs_qualify_overlay_end
dcs_qualify_overlay_end:
.section .note.GNU-stack,"",%progbits
