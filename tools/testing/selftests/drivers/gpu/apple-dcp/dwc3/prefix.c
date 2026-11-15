/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define ENOTSUPP 524
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define FIELD_PREP(m, v) (((u32)(v) << __builtin_ctz(m)) & (m))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctz(m))
#define field_prep FIELD_PREP
#define __iomem
#define udelay(n) ((void)(n))
#define usleep_range(a, b) ((void)(a), (void)(b))
#define fallthrough __attribute__((fallthrough))
static unsigned int warnings, generic_warnings;
static void warning(const char *message)
{
	warnings++;
	if (strstr(message, "phy_power_on was called before phy_init"))
		generic_warnings++;
}
#define dev_warn(d, message, ...) warning(message)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define lockdep_assert_held(p) assert(*(p))
#define WARN_ON_ONCE(v) assert(!(v))
static void mutex_lock(int *m)
{
	assert(!*m);
	*m = 1;
}
static void mutex_unlock(int *m)
{
	assert(*m);
	*m = 0;
}
struct held {
	int *m;
};
static struct held hold(int *m)
{
	mutex_lock(m);
	return (struct held){ m };
}
static void release(struct held *h)
{
	mutex_unlock(h->m);
}
#define CONFIG_PHY_APPLE_ATC_KUNIT_TEST 0
#define IS_ENABLED(option) (option)
#define KUNIT_STATIC_STUB_REDIRECT(...) ((void)0)
#define guard(kind) \
	struct held held_guard __attribute__((cleanup(release))) = hold
enum phy_mode { PHY_MODE_USB_HOST, PHY_MODE_USB_DEVICE };
enum { APPLE_ATCPHY_LANE_0, APPLE_ATCPHY_LANE_1 };
enum usb_role { USB_ROLE_NONE, USB_ROLE_HOST, USB_ROLE_DEVICE };
struct usb_role_switch {
	struct dwc3_apple *data;
};
#define usb_role_switch_get_drvdata(s) ((s)->data)
#define PHY_MODE_INVALID (-1)
enum {
	TYPEC_STATE_SAFE,
	TYPEC_STATE_USB,
	TYPEC_MODE_USB4,
	TYPEC_DP_STATE_C,
	TYPEC_DP_STATE_D,
	TYPEC_DP_STATE_E
};
enum { USB_TYPEC_TBT_SID = 0x8087, USB_TYPEC_DP_SID = 0xff01 };
#define EUDO_USB_MODE_MASK 3U
enum { EUDO_USB_MODE_USB2, EUDO_USB_MODE_USB3, EUDO_USB_MODE_USB4 };
struct enter_usb_data {
	u32 eudo;
};
struct typec_altmode {
	unsigned int svid;
};
struct typec_mux_state {
	unsigned long mode;
	struct typec_altmode *alt;
	void *data;
};
struct typec_mux_dev {
	struct apple_atcphy *data;
};
#define typec_mux_get_drvdata(mux) ((mux)->data)
struct reset_controller_dev {
	int unused;
};
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
