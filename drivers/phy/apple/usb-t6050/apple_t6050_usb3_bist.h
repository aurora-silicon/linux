/* SPDX-License-Identifier: MIT */
/* T6050-specific native PHY recipe; no Windows/Linux frontend policy. */
#ifndef APPLE_T6050_USB3_BIST_H
#define APPLE_T6050_USB3_BIST_H
struct apple_t6050_usb_io {
    void *context;
    unsigned int (*read)(void *, unsigned int bank, unsigned int offset);
    void (*write)(void *, unsigned int bank, unsigned int offset, unsigned int value);
    int (*poll)(void *, unsigned int bank, unsigned int offset, unsigned int mask, unsigned int value, unsigned int usec);
    void (*delay)(void *, unsigned int usec);
};
static inline void apple_t6050_usb_mask(const struct apple_t6050_usb_io *io, unsigned int bank, unsigned int offset, unsigned int mask, unsigned int value)
{ io->write(io->context, bank, offset, (io->read(io->context, bank, offset) & ~mask) | value); }
static inline int apple_t6050_usb_bist_off(const struct apple_t6050_usb_io *io)
{
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x110, 0x660, 0x0);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x4, 0x8, 0x8);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x4, 0x8, 0x0);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x4, 0x1, 0x0);
    return 0;
}
static inline int apple_t6050_usb_bist_on(const struct apple_t6050_usb_io *io)
{
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x4, 0x1, 0x1);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x10c, 0x1e100000, 0x4100000);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x110, 0x660, 0x220);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x4, 0x8, 0x8);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x4, 0x8, 0x0);
    if (io->poll(io->context, 35, 0x104, 0x100, 0x0, 20000)) return -1;
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x8, 0x20, 0x20);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x8, 0x20, 0x0);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x10c, 0x1e000000, 0x6000000);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x110, 0x600, 0x200);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x4, 0x8, 0x8);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x4, 0x8, 0x0);
    if (io->poll(io->context, 35, 0x104, 0x40, 0x40, 130000)) return -1;
    if (io->poll(io->context, 35, 0x104, 0x100, 0x0, 1000)) return -1;
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x8, 0x20, 0x20);
    /* native sequence step */
    apple_t6050_usb_mask(io, 35, 0x8, 0x20, 0x0);
    return 0;
}
#endif
