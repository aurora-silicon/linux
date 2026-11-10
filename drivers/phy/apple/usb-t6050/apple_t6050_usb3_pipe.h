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
    /* Unset, or already on the dummy producer (1:0 = 2) with any clock: when nothing
     * has reset the controller domain since boot, the mux can read 0x22. */
    if (mux != 0 && (mux & 3) != 2)
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

/*
 * Returns 0, or the failing step: -2 the PIPE was not parked on a known producer, -4..-6 the
 * BIST, -7 the mux did not read back USB3, -8 the BIST did not stop. Up to -6 the mux is
 * untouched. A lock or unlock that is not acknowledged within 6 ms is not fatal: it is logged
 * and the switch carries on. On J716c the lock ACK does not come.
 */
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
        return -2;

    /* Native T8142 xHCI USB3 mode switch, native27 T6050-compatible path. */
    apple_t6050_usb_mask(io, 44, 0x04, 0x0e, 0);
    apple_t6050_usb_mask(io, 44, 0x00, 1, 1);
    apple_t6050_usb_mask(io, 44, 0x10, 1, 1);
    locked = 1;
    io->poll(io->context, 44, 0x14, 1, 1, 6000);

    bist = 1;
    result = -4;
    if (apple_t6050_usb_bist_on(io))
        goto out;
    apple_t6050_usb_mask(io, 44, 0x20, 0x100f, 3);
    result = -5;
    if (io->poll(io->context, 44, 0x20, 0xf, 3, 1000))
        goto out;
    result = -6;
    if (apple_t6050_usb_bist_off(io))
        goto out;
    bist = 0;

    /* T6050 clock bits are 6:4, not the older 5:3. Native USB3 is 0x10. */
    apple_t6050_usb_mask(io, 44, 0x0c, 0x70, 0);
    io->delay(io->context, 5);
    apple_t6050_usb_mask(io, 44, 0x0c, 0x73, 0);
    apple_t6050_usb_mask(io, 44, 0x0c, 0x73, 0x10);
    result = (io->read(io->context, 44, 0x0c) & 0x73) == 0x10 ? 0 : -7;

out:
    if (bist && apple_t6050_usb_bist_off(io))
        result = -8;
    apple_t6050_usb_mask(io, 44, 0x00, 5, 0);
    if (locked) {
        apple_t6050_usb_mask(io, 44, 0x10, 1, 0);
        io->poll(io->context, 44, 0x14, 1, 0, 6000);
    }
    return result;
}

/*
 * Move a running DWC3 from the dummy producer (0x42) to a USB4 router's USB3 tunnel (0x21: PIPE mode 1,
 * clock 2), once the tunnel is active. As for native USB3, the PIPE is locked only when DWC3 is out of
 * reset, and a lock that is not acknowledged within 6 ms is not fatal.
 * Returns 0, -2 if the PIPE is on another producer, or -7 if the mux did not read back the tunnel.
 */
static inline int
apple_t6050_usb3_pipe_tunnel(const struct apple_t6050_usb_io *io)
{
    unsigned int mux;
    int locked;

    if (!io || !io->read || !io->write || !io->poll || !io->delay)
        return -1;
    mux = io->read(io->context, 44, 0x0c) & 0x73;
    if (mux == 0x21)
        return 0;
    if (mux != 0x42)
        return -2;
    locked = io->read(io->context, 44, 0x1c) & 1;

    apple_t6050_usb_mask(io, 44, 0x04, 0x0e, 0);
    apple_t6050_usb_mask(io, 44, 0x00, 1, 1);
    if (locked) {
        apple_t6050_usb_mask(io, 44, 0x10, 1, 1);
        io->poll(io->context, 44, 0x14, 1, 1, 6000);
    }
    apple_t6050_usb_mask(io, 44, 0x0c, 0x70, 0);
    io->delay(io->context, 5);
    apple_t6050_usb_mask(io, 44, 0x0c, 3, 1);
    apple_t6050_usb_mask(io, 44, 0x0c, 0x70, 0x20);
    apple_t6050_usb_mask(io, 44, 0x00, 1, 0);
    apple_t6050_usb_mask(io, 44, 0x00, 4, 0);
    if (locked) {
        apple_t6050_usb_mask(io, 44, 0x10, 1, 0);
        io->poll(io->context, 44, 0x14, 1, 0, 6000);
    }
    return (io->read(io->context, 44, 0x0c) & 0x73) == 0x21 ? 0 : -7;
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
    if (mux != 0x10 && mux != 0x21 && mux != 0x42)
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
