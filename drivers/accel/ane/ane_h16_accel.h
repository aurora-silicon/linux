/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_ACCEL_H__
#define __ANE_H16_ACCEL_H__

#include "ane_h16_program.h"

struct device;

/* Register the ABI 3 node over a session whose transport is armed and started. */
int ane_h16_accel_register(struct device *dev, struct ane_h16_session *session);

#endif /* __ANE_H16_ACCEL_H__ */
