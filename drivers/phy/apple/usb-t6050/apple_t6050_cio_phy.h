/* SPDX-License-Identifier: MIT */
/* ACIO80 PHY CIO (USB4/Thunderbolt) mode.
 *
 * The bring-up runs the mode-independent power-up (the USB recipes' first
 * block), the common and CIO/CIO80 tunables, the crossbar, the sleep-control
 * release and the firmware start. CIO applies no DP or USB lane masks,
 * no DP clock switch and no orientation bit in ATCPHY_CFG +0x8; orientation
 * only selects ACIOPHY_TOP +0x78 bit 0. Both lanes are released.
 * Spread-spectrum stays on.
 */
#ifndef APPLE_T6050_CIO_PHY_H
#define APPLE_T6050_CIO_PHY_H
#include "apple_t6050_usb_phy.h"

/* The power-up and reset release, identical to the USB recipes. */
#define APPLE_T6050_CIO_POWERUP \
    { T6050_MASK, 39, 0x0, 0x1, 0x1, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_POLL, 39, 0x4, 0x1, 0x1, 1000, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 39, 0x0, 0x2, 0x2, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_POLL, 39, 0x4, 0x2, 0x2, 1000, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 39, 0x0, 0x4, 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_POWER, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 39, 0x2c, 0x1, 0x0, 0, 0 }, \
    { T6050_POLL, 39, 0x4, 0x100, 0x0, 1000, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 39, 0x0, 0x8, 0x8, 0, 0 }

/* The common tunables, then the CIO set and the CIO80 set. An absent CIO80
 * tunable is skipped.
 */
#define APPLE_T6050_CIO_TUNABLES \
    { T6050_TUNABLE, 41, 0x0, 0x0, 0x0, 0, "tunable_ATC0AXI2AF" }, \
    { T6050_TUNABLE, 40, 0x0, 0x0, 0x0, 0, "tunable_ATC_FABRIC" }, \
    { T6050_TUNABLE, 43, 0x0, 0x0, 0x0, 0, "tunable_ATC_COMMON_CFG" }, \
    { T6050_TUNABLE, 42, 0x0, 0x0, 0x0, 0, "tunable_ATC0AXI2AF_LIOA" }, \
    { T6050_TUNABLE, 20, 0x0, 0x0, 0x0, 0, "tunable_LN0_RX_CFG_TX_OF_RXCLK" }, \
    { T6050_TUNABLE, 29, 0x0, 0x0, 0x0, 0, "tunable_LN1_RX_CFG_TX_OF_RXCLK" }, \
    { T6050_TUNABLE, 18, 0x0, 0x0, 0x0, 0, "tunable_LN0_RX_EQ_DFLT" }, \
    { T6050_TUNABLE, 27, 0x0, 0x0, 0x0, 0, "tunable_LN1_RX_EQ_DFLT" }, \
    { T6050_TUNABLE, 5, 0x0, 0x0, 0x0, 0, "tunable_AUS40CMN_SHM" }, \
    { T6050_TUNABLE, 8, 0x0, 0x0, 0x0, 0, "tunable_AUSPLL_CORE" }, \
    { T6050_TUNABLE, 10, 0x0, 0x0, 0x0, 0, "tunable_CIO4PLL_CORE" }, \
    { T6050_TUNABLE, 3, 0x0, 0x0, 0x0, 0, "tunable_UC_REGS_CIO_DFLT" }, \
    { T6050_TUNABLE, 16, 0x0, 0x0, 0x0, 0, "tunable_LN0_RX_TOP_CIO_DFLT" }, \
    { T6050_TUNABLE, 25, 0x0, 0x0, 0x0, 0, "tunable_LN1_RX_TOP_CIO_DFLT" }, \
    { T6050_TUNABLE, 18, 0x0, 0x0, 0x0, 0, "tunable_LN0_RX_EQ_CIO_DFLT" }, \
    { T6050_TUNABLE, 27, 0x0, 0x0, 0x0, 0, "tunable_LN1_RX_EQ_CIO_DFLT" }

/* The crossbar: clear the lane selects, set the orientation bit (+0x78)
 * and select CIO.
 */
#define APPLE_T6050_CIO_XBAR(reverse) \
    { T6050_MASK, 2, 0x6c, 0xfc, 0x0, 0, 0 }, \
    { T6050_MASK, 2, 0x78, 0x1f, (reverse), 0, 0 }, \
    { T6050_MASK, 2, 0x6c, 0x3, 0x2, 0, 0 }

/* Power a lane's receiver up, then release its transmitter overrides. */
#define APPLE_T6050_CIO_LANE(l) \
    { T6050_MASK, 2, 0x10, 0x10 << (l), 0x10 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x10, 0x40 << (l), 0x40 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x10, 0x1 << (l), 0x1 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x10, 0x4 << (l), 0x4 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0xc, 0x400 << (l), 0x400 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0xc, 0x1000 << (l), 0x1000 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0xc, 0x40 << (l), 0x40 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0xc, 0x100 << (l), 0x100 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0xc, 0x4000 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0xc, 0x10000 << (l), 0x10000 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x120, 0x40 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x120, 0x4 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x120, 0x400 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }

/* The sleep-control release: the common block's overrides, then both lanes. */
#define APPLE_T6050_CIO_SLPCTRL \
    { T6050_MASK, 2, 0xc, 0x2a, 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    APPLE_T6050_CIO_LANE(0), \
    APPLE_T6050_CIO_LANE(1)

/* Start the PHY firmware, wait for UC_TNBL_DONE and
 * deassert the ACIOPHY reset. No PIPE BIST and no ATCPHY_CFG +0x8 write.
 */
#define APPLE_T6050_CIO_START \
    { T6050_DELAY, 0, 0x0, 0x0, 0xa, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0xa, 0, 0 }, \
    { T6050_MASK, 38, 0x40, 0x1, 0x1, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0xa, 0, 0 }, \
    { T6050_FIRMWARE, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0xa, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1388, 0, 0 }, \
    { T6050_POLL, 2, 0x260, 0x400, 0x400, 1000000, 0 }, \
    { T6050_MASK, 39, 0x0, 0x10, 0x10, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x64, 0, 0 }

static const struct apple_t6050_usb_step apple_t6050_cio_init_normal[] = {
    APPLE_T6050_CIO_POWERUP,
    APPLE_T6050_CIO_TUNABLES,
    APPLE_T6050_CIO_XBAR(0x0),
    APPLE_T6050_CIO_SLPCTRL,
    APPLE_T6050_CIO_START,
};

static const struct apple_t6050_usb_step apple_t6050_cio_init_reverse[] = {
    APPLE_T6050_CIO_POWERUP,
    APPLE_T6050_CIO_TUNABLES,
    APPLE_T6050_CIO_XBAR(0x1),
    APPLE_T6050_CIO_SLPCTRL,
    APPLE_T6050_CIO_START,
};

/* Power a lane's transmitter down. */
#define APPLE_T6050_CIO_TX_DOWN(l) \
    { T6050_MASK, 2, 0x120, 0x100 << (l), 0x100 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x120, 0x1 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x120, 0x10 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }

/* Power a lane's receiver down; CIO powers the receivers down, USB does not. */
#define APPLE_T6050_CIO_RX_DOWN(l) \
    { T6050_MASK, 2, 0xc, 0x4000 << (l), 0x4000 << (l), 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0xc, 0x40 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0xc, 0x400 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x10, 0x1 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }, \
    { T6050_MASK, 2, 0x10, 0x10 << (l), 0x0, 0, 0 }, \
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 }

/* The shutdown: both lanes down without the PIPE BIST, the firmware
 * power-down, then the USB recipes' sleep, clamp and reset sequence.
 */
static const struct apple_t6050_usb_step apple_t6050_cio_shutdown[] = {
    APPLE_T6050_CIO_TX_DOWN(0),
    APPLE_T6050_CIO_TX_DOWN(1),
    { T6050_MASK, 5, 0x0, 0x2, 0x0, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_MASK, 2, 0xc, 0x10, 0x10, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_MASK, 2, 0xc, 0x1, 0x0, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_MASK, 2, 0xc, 0x4, 0x0, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    APPLE_T6050_CIO_RX_DOWN(0),
    APPLE_T6050_CIO_RX_DOWN(1),
    { T6050_DELAY, 0, 0x0, 0x0, 0xc8, 0, 0 },
    { T6050_FIRMWARE, 0, 0x0, 0x0, 0x0, 0, 0 },
    { T6050_POWER, 0, 0x0, 0x0, 0x0, 0, 0 },
    { T6050_MASK, 39, 0x2c, 0x1, 0x1, 0, 0 },
    { T6050_POLL, 39, 0x4, 0x100, 0x100, 1000, 0 },
    { T6050_MASK, 39, 0x0, 0x10, 0x0, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_MASK, 39, 0x0, 0x8, 0x0, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_MASK, 39, 0x0, 0x4, 0x4, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_MASK, 39, 0x0, 0x2, 0x0, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_POLL, 39, 0x4, 0x2, 0x0, 1000, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_MASK, 39, 0x0, 0x1, 0x0, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_POLL, 39, 0x4, 0x1, 0x0, 1000, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
    { T6050_MASK, 39, 0x8, 0x1, 0x0, 0, 0 },
    { T6050_DELAY, 0, 0x0, 0x0, 0x1, 0, 0 },
};
#endif
