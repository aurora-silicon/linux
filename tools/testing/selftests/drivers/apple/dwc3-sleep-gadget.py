#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check how dwc3-apple takes a device role off its host for suspend-to-idle.

Builds the production sleep works, the gadget helpers, the role switch and
dwc3_apple_exit() against host mocks. The reconnect after the wake must run
while the sleep epoch still defers role changes and refuses a new sleep, so a
role change can't free the gadget under usb_gadget_connect() and a back-to-back
sleep can't miss the disconnect. Controller start/stop and the PHY are mocked.
"""
from pathlib import Path
import re
import resource
import subprocess
import sys
import tempfile


def function(source, name):
    # The definition, not a forward declaration.
    match = re.search(r"^static [^;(\n]*\b" + name + r"\([^;{]*\)\n\{", source, re.M)
    assert match, name
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def block(source, start):
    text = source[source.index(start):]
    return text[:text.index("};") + 2] + "\n"


PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define IS_ENABLED(x) 1
#define lockdep_assert_held(l) assert(*(l))
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
static unsigned warns;
#define dev_warn(...) ((void)warns++)
#define READ_ONCE(x) (x)
#define container_of(p, type, member) ((type *)((char *)(p) - __builtin_offsetof(type, member)))
#define spin_lock_irqsave(l, f) ((void)(f = 0), (void)(l))
#define spin_unlock_irqrestore(l, f) ((void)(f), (void)(l))
/* The glue mutex: held is 1, and taking it twice or dropping it unheld asserts. */
static int *mutex_take(int *l) { assert(!*l); *l = 1; return l; }
static void mutex_drop(int **l) { assert(**l); **l = 0; }
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define guard(t) guard_##t
#define guard_mutex(l) int *__attribute__((cleanup(mutex_drop))) CAT(_g, __LINE__) = mutex_take(l)
#define scoped_guard(t, l) \
    for (int *__attribute__((cleanup(mutex_drop))) _sg = mutex_take(l), *_once = _sg; _once; _once = NULL)
enum usb_role { USB_ROLE_NONE, USB_ROLE_HOST, USB_ROLE_DEVICE };
struct device { int locked; };
struct usb_gadget { struct device dev; bool connected; bool freed; };
struct work_struct { int pending; };
struct delayed_work { struct work_struct work; };
struct usb_role_switch { void *data; };
struct dwc3 { struct usb_gadget *gadget; void *gadget_driver; bool dis_u3_susphy_quirk; };
'''

MOCKS = r'''
struct dwc3_apple {
    struct dwc3 dwc;
    struct device *dev;
    void *usb2_phy;
    enum dwc3_apple_state state, sleep_role, sleep_state;
    enum dwc3_apple_sleep_phase sleep_phase;
    enum usb_role sleep_tunnel_role;
    int lock, sleep_pm_lock;
    bool sleep_deep, core_active, core_exit_pending, reset_pending, shutting_down, sleep_dead;
    bool sleep_role_pending, sleep_stopped, sleep_tunnel, sleep_in_place, fixed_hub;
    bool host_only, dt_dis_u3_susphy, sleep_ddr, sleep_phy_pending, sleep_phy_wait, ss_retry;
    bool sleep_gadget_off;
    unsigned sleep_guid;
    struct work_struct sleep_tunnel_work, sleep_stop_work;
    struct delayed_work sleep_start_work, hub_check;
};
static struct dwc3_apple A;
static int dwc3_apple_exit(struct dwc3_apple *appledwc);
static struct usb_gadget G;
static struct device dev;
static unsigned disconnects, connects, gadget_exits, stops, starts, inits, idles_seen;
static enum usb_role role_in_connect = (enum usb_role)-1;
static int dwc3_usb_role_switch_set(struct usb_role_switch *sw, enum usb_role role);
static struct usb_role_switch sw = { &A };
static void *usb_role_switch_get_drvdata(struct usb_role_switch *s) { return s->data; }
static void flush_work(struct work_struct *w) { (void)w; }
static void cancel_delayed_work_sync(struct delayed_work *w) { (void)w; }
static struct delayed_work *to_delayed_work(struct work_struct *w)
{ return container_of(w, struct delayed_work, work); }
static void device_lock(struct device *d) { assert(!A.lock && !d->locked); d->locked = 1; }
static void device_unlock(struct device *d) { assert(d->locked); d->locked = 0; }
static int usb_gadget_disconnect(struct usb_gadget *g)
{
    assert(g == A.dwc.gadget && !g->freed && g->dev.locked && !A.lock);
    /* Admitted and not yet prepared: role changes are deferred. */
    assert(A.sleep_phase == DWC3_APPLE_SLEEP_PREPARING);
    disconnects++; g->connected = false; return 0;
}
static int usb_gadget_connect(struct usb_gadget *g)
{
    assert(!g->freed && !A.lock);
    /* No new sleep is admitted and role changes are still deferred. */
    assert(A.sleep_phase == DWC3_APPLE_SLEEP_FINISHING);
    if (role_in_connect != (enum usb_role)-1) {
        assert(!dwc3_usb_role_switch_set(&sw, role_in_connect));
        assert(!g->freed && A.state == DWC3_APPLE_DEVICE && A.sleep_role_pending);
    }
    connects++; g->connected = true; return 0;
}
static void dwc3_gadget_exit(struct dwc3 *d) { assert(A.lock); d->gadget->freed = true; gadget_exits++; }
static void dwc3_host_exit(struct dwc3 *d) { (void)d; }
static void apple_sleep_usb_slp_ddr(bool on) { (void)on; }
static int dwc3_apple_core_stop(struct dwc3_apple *a) { a->core_active = false; return 0; }
static int dwc3_apple_init(struct dwc3_apple *a, enum dwc3_apple_state s)
{ inits++; a->state = s; a->core_active = true; return 0; }
static void dwc3_apple_check_hub_later(struct dwc3_apple *a) { (void)a; }
static void dwc3_apple_syscore(struct dwc3_apple *a, bool on) { (void)a; (void)on; }
static void apple_atcphy_sleep_finish_async(void *p, bool kept) { (void)p; (void)kept; }
static int dwc3_apple_sleep_start(struct dwc3_apple *a) { (void)a; starts++; return 0; }
static int dwc3_apple_sleep_stop(struct dwc3_apple *a)
{ stops++; a->sleep_stopped = true; return dwc3_apple_exit(a); }
static void dwc3_apple_sleep_take_role(struct dwc3_apple *a)
{
    a->sleep_role_pending = a->sleep_stopped = false;
    a->state = a->sleep_role; a->core_active = a->state != DWC3_APPLE_NO_CABLE;
}
static int dwc3_apple_recover(struct dwc3_apple *a) { (void)a; assert(0); return 0; }
static void dwc3_apple_tunnel_done(struct dwc3_apple *a) { (void)a; assert(0); }
static int dwc3_apple_sleep_kept(struct dwc3_apple *a, bool *k) { (void)a; (void)k; assert(0); return 0; }
static bool dwc3_apple_tunnel_keep(struct dwc3_apple *a) { (void)a; return false; }
static int apple_atcphy_sleep_prepare(void *p) { (void)p; return -EOPNOTSUPP; }
static unsigned long dwc3_apple_hub_wake_ports(struct dwc3_apple *a) { (void)a; return 0; }
static void apple_sleep_usb_set_wake_ports(unsigned long m) { (void)m; }
static unsigned dwc3_readl(struct dwc3 *d, int r) { (void)d; (void)r; return 0; }
static void dwc3_writel(struct dwc3 *d, int r, unsigned v) { (void)d; (void)r; (void)v; }
#define DWC3_GUID 0
#define DWC3_APPLE_SLEEP_GUID 0
'''

TESTS = r'''
static void device_role(bool connected)
{
    memset(&A, 0, sizeof(A)); memset(&G, 0, sizeof(G));
    A.dev = &dev; A.dwc.gadget = &G; A.dwc.gadget_driver = &G;
    A.state = DWC3_APPLE_DEVICE; A.core_active = true;
    G.connected = connected;
    disconnects = connects = gadget_exits = stops = starts = inits = warns = 0;
    role_in_connect = (enum usb_role)-1;
}
static int prepare(bool deep)
{
    A.sleep_phase = DWC3_APPLE_SLEEP_PREPARING; A.sleep_deep = deep;
    return dwc3_apple_sleep_prepare_work(&A);
}
static void finish(void)
{
    A.sleep_phase = DWC3_APPLE_SLEEP_FINISHING;
    dwc3_apple_sleep_start_work(&A.sleep_start_work.work);
    assert(A.sleep_phase == DWC3_APPLE_SLEEP_IDLE && !A.lock && !G.dev.locked);
}
int main(void)
{
    printf("TAP version 13\n1..4\n");
    /* Off the bus for suspend-to-idle, back on before the epoch ends. */
    for (int round = 0; round < 3; round++) {
        if (!round) device_role(true);
        assert(!prepare(false) && disconnects == (unsigned)round + 1 && A.sleep_gadget_off);
        finish();
        assert(connects == (unsigned)round + 1 && !A.sleep_gadget_off && G.connected && !gadget_exits);
    }
    puts("ok 1 - back-to-back suspend-to-idle disconnects and reconnects before IDLE");
    /* A role change during the reconnect is deferred, then applied before IDLE. */
    device_role(true); role_in_connect = USB_ROLE_NONE;
    assert(!prepare(false) && disconnects == 1);
    finish();
    assert(connects == 1 && gadget_exits == 1 && stops == 1 && !A.sleep_role_pending);
    assert(A.state == DWC3_APPLE_NO_CABLE);
    /* One deferred before the wake takes the gadget down: nothing to connect. */
    device_role(true);
    assert(!prepare(false) && A.sleep_gadget_off);
    assert(!dwc3_usb_role_switch_set(&sw, USB_ROLE_HOST) && A.sleep_role_pending && !gadget_exits);
    finish();
    assert(!connects && gadget_exits == 1 && !A.sleep_gadget_off && A.state == DWC3_APPLE_HOST);
    puts("ok 2 - role changes around the reconnect never free the gadget under it");
    /* Deep sleeps, hosts, unbound or soft-disconnected gadgets are left alone. */
    for (int c = 0; c < 3; c++) {
        device_role(c != 2);
        if (c == 0) A.state = DWC3_APPLE_HOST;
        if (c == 1) A.dwc.gadget_driver = NULL;
        assert(!prepare(false) && !A.sleep_gadget_off); finish();
        assert(!disconnects && !connects && G.connected == (c != 2));
    }
    device_role(true);
    assert(!prepare(true) && !disconnects && stops == 1);
    puts("ok 3 - only a connected gadget leaves its host, and only for suspend-to-idle");
    /* Teardown before the preparation: no disconnect. */
    device_role(true); A.shutting_down = true;
    assert(prepare(false) == -ESHUTDOWN && !disconnects && !A.sleep_gadget_off);
    /* Teardown after it: no reconnect, and the flag goes with the role. */
    device_role(true);
    assert(!prepare(false) && A.sleep_gadget_off);
    A.lock = 1; assert(!dwc3_apple_exit(&A)); A.lock = 0;
    assert(!A.sleep_gadget_off && gadget_exits == 1);
    finish();
    assert(!connects);
    puts("ok 4 - shutdown and a role exit drop the pending reconnect");
    return 0;
}
'''


def run(directory, name, program, expect_failure=False):
    path = directory / (name + ".c")
    binary = directory / name
    path.write_text(program)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-Wno-unused-variable", str(path), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    if expect_failure:
        assert result.returncode != 0, name + " negative control passed"
        print("# " + name + ": rejected as expected")
        return
    sys.stdout.write(result.stdout)
    assert result.returncode == 0, result.stderr


def main():
    root = Path(__file__).resolve().parents[5]
    path = root / "drivers/usb/dwc3/dwc3-apple.c"
    if not path.exists():
        print("# SKIP: requires matching kernel source tree")
        return 4
    apple = path.read_text()
    states = (root / "drivers/usb/dwc3/dwc3-apple-state.h").read_text()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    program = PREAMBLE + block(states, "enum dwc3_apple_state {")
    program += block(apple, "enum dwc3_apple_sleep_phase {")
    program += function(states, "dwc3_apple_state_for_role") + MOCKS
    for name in ["dwc3_apple_exit", "dwc3_usb_role_switch_set", "dwc3_apple_sleep_gadget",
                 "dwc3_apple_sleep_gadget_off", "dwc3_apple_sleep_gadget_back",
                 "dwc3_apple_sleep_prepare_work", "dwc3_apple_sleep_role",
                 "dwc3_apple_sleep_idle", "dwc3_apple_sleep_start_work"]:
        program += function(apple, name)
    program += TESTS
    mutants = {
        # The reconnect after IDLE (the use-after-free and back-to-back race).
        "idle-before-connect": ("\t\tif (!gadget)\n\t\t\tdwc3_apple_sleep_idle(appledwc, finish, kept);",
                                "\t\tdwc3_apple_sleep_idle(appledwc, finish, kept);"),
        "no-role-after-connect": ("\t\t/* A role change requested during the connect. */\n"
                                  "\t\tif (!ret)\n\t\t\tret = dwc3_apple_sleep_role(appledwc);",
                                  "\t\t(void)0;"),
        "exit-keeps-flag": ("\t\tappledwc->sleep_gadget_off = false;\n\t\tdwc3_gadget_exit",
                            "\t\tdwc3_gadget_exit"),
        "disconnect-unconnected": ("\tif (gadget->connected)\n\t\toff = ", "\toff = "),
    }
    with tempfile.TemporaryDirectory(prefix="dwc3-sleep-gadget-") as directory:
        directory = Path(directory)
        run(directory, "dwc3-sleep-gadget", program)
        for name, (old, new) in mutants.items():
            assert program.count(old) == 1, name
            run(directory, "negative-" + name, program.replace(old, new), expect_failure=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
