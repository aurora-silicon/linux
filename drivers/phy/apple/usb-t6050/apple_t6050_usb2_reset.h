/* SPDX-License-Identifier: MIT */
#ifndef APPLE_T6050_USB2_RESET_H
#define APPLE_T6050_USB2_RESET_H
#include "apple_t6050_usb3_bist.h"

/* Native T6050 Type-C PHY USB2 port reset block, native27:
 * bank0+4 bit1 assert, sleep 5 ms, release.
 * Caller holds exclusive PHY ownership with its USB power domains active.
 * sleep_ms is supplied by the OS frontend; there is no packet-path polling. */
static inline int apple_t6050_usb2_port_reset(
    const struct apple_t6050_usb_io *io, void *sleep_context,
    void (*sleep_ms)(void *, unsigned int))
{
    if (!io || !io->read || !io->write || !sleep_ms)
        return -1;
    apple_t6050_usb_mask(io, 0, 4, 2U, 2U);
    if (!(io->read(io->context, 0, 4) & 2U))
        return -1;
    sleep_ms(sleep_context, 5);
    apple_t6050_usb_mask(io, 0, 4, 2U, 0);
    return io->read(io->context, 0, 4) & 2U ? -1 : 0;
}
#endif
