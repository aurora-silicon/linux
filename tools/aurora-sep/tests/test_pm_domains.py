# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Synthetic controls for the production SEP power-domain lifetime code."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


def function(source, name):
    match = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^;]*?\)\n\{",
                      source, re.M | re.S)
    if match is None:
        raise AssertionError(name)
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


# The retained-domain controls originate in the public fd13ba30ec15 host
# harness. Keep only SEP stubs here; no device access or per-machine input.
PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t u32;
#define PD_FLAG_DEV_LINK_ON 1
struct device { bool syscore; void *of_node; };
struct dev_pm_domain_list { unsigned num_pds; struct device **pd_devs; };
struct dev_pm_domain_attach_data { unsigned pd_flags; };
static int domain_count, attach_error, action_error;
static unsigned attach_calls;
static struct device pd0, pd1;
static struct device *pd_devices[] = { &pd0, &pd1 };
static struct dev_pm_domain_list domains = { 2, pd_devices };
static void (*cleanup)(void *);
static void *cleanup_data;
static int of_count_phandle_with_args(void *n, const char *a, const char *b)
{ return domain_count; }
static int devm_pm_domain_attach_list(struct device *d,
        struct dev_pm_domain_attach_data *a, struct dev_pm_domain_list **l)
{
    attach_calls++;
    assert(a->pd_flags == PD_FLAG_DEV_LINK_ON);
    *l = &domains;
    return attach_error;
}
static void dev_pm_syscore_device(struct device *d, bool v) { d->syscore = v; }
static int devm_add_action_or_reset(struct device *d, void (*f)(void *), void *p)
{
    if (action_error) { f(p); return action_error; }
    cleanup = f;
    cleanup_data = p;
    return 0;
}
"""

MAIN = r"""
int main(void)
{
    struct device d = {0};
    domain_count = -ENOENT;
    assert(!sep_pm_keep_domains(&d) && !attach_calls);
    domain_count = -EINVAL;
    assert(sep_pm_keep_domains(&d) == -EINVAL && !attach_calls);
    domain_count = 0;
    assert(!sep_pm_keep_domains(&d) && !attach_calls);
    domain_count = 1;
    assert(!sep_pm_keep_domains(&d) && !attach_calls);
    domain_count = 2;
    attach_error = -EAGAIN;
    assert(sep_pm_keep_domains(&d) == -EAGAIN && !pd0.syscore);
    attach_error = 0;
    assert(!sep_pm_keep_domains(&d) && pd0.syscore && pd1.syscore && cleanup);
    cleanup(cleanup_data);
    assert(!pd0.syscore && !pd1.syscore);
    action_error = -ENOMEM;
    assert(sep_pm_keep_domains(&d) == -ENOMEM && !pd0.syscore && !pd1.syscore);
    return 0;
}
"""


class PowerDomainTest(unittest.TestCase):
    def test_production_domain_admission_and_cleanup(self):
        source = (ROOT / "drivers/soc/apple/pm_shim.c").read_text()
        program = PREFIX + function(source, "sep_pm_release_domains")
        program += function(source, "sep_pm_keep_domains") + MAIN
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "control.c").write_text(program)
            subprocess.run(["cc", "-std=gnu11", str(path / "control.c"),
                            "-o", str(path / "control")], check=True,
                           capture_output=True)
            subprocess.run([str(path / "control")], check=True,
                           capture_output=True)


if __name__ == "__main__":
    unittest.main()
