/* SPDX-License-Identifier: MIT */
/* Platform backend for native direct USB3, not a Type-C policy engine.
 * The caller owns the port, has powered the PHY/firmware for native USB3,
 * completed DWC3 core init and enabled SUSPHY, and has valid DART mappings.
 * Bank 35 is T6050's ADT PHY reg[35]. Bank 44 is usb-drdN reg[3].
 * The first left/debug port must never reach this backend.
 */
#ifndef APPLE_T6050_USB3_PIPE_H
#define APPLE_T6050_USB3_PIPE_H
#include "apple_t6050_usb3_bist.h"

/* Cold controller setup while RESET_N is clear. Linux's existing ATC
 * frontend supplies a dummy PIPE clock before DWC3 reads its core registers.
 * T6050 uses the native-RE 6:4 clock field and dummy encoding 0x42.
 * This is not an active producer handoff; never use it for USB4 teardown.
 */
static inline int
apple_t6050_usb3_pipe_cold_dummy(const struct apple_t6050_usb_io *io)
{
    unsigned int mux;
    if (!io || !io->read || !io->write || !io->delay)
        return -1;
    if (io->read(io->context, 44, 0x1c) & 1)
        return -1;
    mux = io->read(io->context, 44, 0x0c) & 0x73;
    if (mux != 0 && mux != 2 && mux != 0x42)
        return -1;
    if (mux == 0x42)
        return 0;
    apple_t6050_usb_mask(io, 44, 0x0c, 0x70, 0);
    io->delay(io->context, 10);
    apple_t6050_usb_mask(io, 44, 0x0c, 3, 2);
    io->delay(io->context, 10);
    apple_t6050_usb_mask(io, 44, 0x0c, 0x70, 0x40);
    io->delay(io->context, 10);
    return (io->read(io->context, 44, 0x0c) & 0x73) == 0x42 ? 0 : -1;
}

static inline int
apple_t6050_usb3_pipe_switch(const struct apple_t6050_usb_io *io)
{
    unsigned int aon, mux;
    int result = -1, locked = 0, bist = 0;

    if (!io || !io->read || !io->write || !io->poll || !io->delay)
        return -1;
    aon = io->read(io->context, 44, 0x1c);
    mux = io->read(io->context, 44, 0x0c) & 0x73;
    /* Never reroute an active USB4 producer or guess an unknown state. */
    if (!(aon & 1) || (mux != 0x42 && mux != 0x10))
        return -1;

    /* Native T8142 xHCI USB3 mode switch, native27 T6050-compatible path. */
    apple_t6050_usb_mask(io, 44, 0x04, 0x0e, 0);
    apple_t6050_usb_mask(io, 44, 0x00, 1, 1);
    apple_t6050_usb_mask(io, 44, 0x10, 1, 1);
    locked = 1;
    if (io->poll(io->context, 44, 0x14, 1, 1, 6000))
        goto out;

    bist = 1;
    if (apple_t6050_usb_bist_on(io))
        goto out;
    apple_t6050_usb_mask(io, 44, 0x20, 0x100f, 3);
    if (io->poll(io->context, 44, 0x20, 0xf, 3, 1000))
        goto out;
    if (apple_t6050_usb_bist_off(io))
        goto out;
    bist = 0;

    /* T6050 clock bits are 6:4, not the older 5:3. Native USB3 is 0x10. */
    apple_t6050_usb_mask(io, 44, 0x0c, 0x70, 0);
    io->delay(io->context, 5);
    apple_t6050_usb_mask(io, 44, 0x0c, 0x73, 0);
    apple_t6050_usb_mask(io, 44, 0x0c, 0x73, 0x10);
    result = (io->read(io->context, 44, 0x0c) & 0x73) == 0x10 ? 0 : -1;

out:
    if (bist && apple_t6050_usb_bist_off(io))
        result = -1;
    apple_t6050_usb_mask(io, 44, 0x00, 5, 0);
    if (locked) {
        apple_t6050_usb_mask(io, 44, 0x10, 1, 0);
        if (io->poll(io->context, 44, 0x14, 1, 0, 6000))
            result = -1;
    }
    return result;
}

/* The native close() call quiesces the ACIO PHY while PIPE is locked,
 * before selecting the dummy producer. The caller supplies that lifecycle
 * operation; merely changing MUX is not a complete disconnect sequence.
 */
static inline int
apple_t6050_usb3_pipe_stop(const struct apple_t6050_usb_io *io,
                          int (*quiesce)(void *), void *context)
{
    unsigned int mux;
    int result = -1;

    if (!io || !io->read || !io->write || !io->poll || !io->delay || !quiesce)
        return -1;
    mux = io->read(io->context, 44, 0x0c) & 0x73;
    if (mux != 0x10 && mux != 0x42)
        return -1;
    apple_t6050_usb_mask(io, 44, 0x04, 0x0e, 0);
    apple_t6050_usb_mask(io, 44, 0x00, 1, 1);
    apple_t6050_usb_mask(io, 44, 0x00, 4, 4);
    apple_t6050_usb_mask(io, 44, 0x10, 1, 1);
    /* Native mode0-from2 waits 150 ms, logs a missing lock ACK, and
     * continues teardown, as the saved native driver does.
     * The caller has already stopped the controller's DMA/PIPE consumer;
     * this is not the active-producer handoff used by pipe_switch().
     * The nonselected override, final mux readback and unlock checks below
     * remain mandatory, as does successful PHY quiescence.
     */
    io->poll(io->context, 44, 0x14, 1, 1, 150000);
    if (quiesce(context))
        goto out;
    apple_t6050_usb_mask(io, 44, 0x20, 0x100f, 0x1002);
    if (io->poll(io->context, 44, 0x20, 0xf, 2, 1000))
        goto out;
    apple_t6050_usb_mask(io, 44, 0x0c, 0x70, 0);
    io->delay(io->context, 5);
    apple_t6050_usb_mask(io, 44, 0x0c, 3, 2);
    apple_t6050_usb_mask(io, 44, 0x0c, 0x70, 0x40);
    result = (io->read(io->context, 44, 0x0c) & 0x73) == 0x42 ? 0 : -1;
out:
    apple_t6050_usb_mask(io, 44, 0x00, 1, 0);
    apple_t6050_usb_mask(io, 44, 0x00, 4, 0);
    apple_t6050_usb_mask(io, 44, 0x10, 1, 0);
    if (io->poll(io->context, 44, 0x14, 1, 0, 6000))
        result = -1;
    return result;
}
#endif
