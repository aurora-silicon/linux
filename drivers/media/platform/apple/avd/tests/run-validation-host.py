#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the validation KUnit cases with host shims and extracted core validators.

This does not replace an in-kernel KUnit run. No validator implementation is
copied: the static functions are compiled from the selected kernel source.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source, name):
    match = re.search(r"static int\s+" + name + r"\s*\(", source)
    if not match:
        raise ValueError(f"missing production validator: {name}")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path)
    args = parser.parse_args()
    args.kernel = args.kernel or Path(__file__).resolve().parents[6]
    tests = Path(__file__).resolve().parent
    source = (args.kernel / "drivers/media/v4l2-core/v4l2-ctrls-core.c").read_text()
    names = ["validate_vp9_lf_params", "validate_vp9_quant_params", "validate_vp9_seg_params",
             "validate_vp9_frame", "validate_av1_quantization", "validate_av1_segmentation",
             "validate_av1_loop_filter", "validate_av1_cdef", "validate_av1_loop_restauration",
             "validate_av1_tile_info", "validate_av1_frame"]
    with tempfile.TemporaryDirectory(prefix="avd-validation-") as temp:
        out = Path(temp)
        (out / "kunit").mkdir()
        (out / "media").mkdir()
        (out / "kunit/test.h").write_text('''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/types.h>
#define U32_MAX UINT32_MAX
#define GFP_KERNEL 0
struct kunit { int unused; };
struct kunit_case { void (*run)(struct kunit *); };
struct kunit_suite { const char *name; struct kunit_case *test_cases; };
#define kunit_kzalloc(test, size, flags) calloc(1, size)
#define KUNIT_CASE(fn) { fn }
#define KUNIT_ASSERT_NOT_NULL(test, value) assert((value) != NULL)
#define KUNIT_EXPECT_NOT_NULL(test, value) assert((value) != NULL)
#define KUNIT_EXPECT_NULL(test, value) assert((value) == NULL)
#define KUNIT_EXPECT_EQ(test, a, b) assert((a) == (b))
#define kunit_test_suite(suite) \\
int main(void) { struct kunit test = {0}; unsigned int i; \\
for (i = 0; suite.test_cases[i].run; i++) suite.test_cases[i].run(&test); \\
printf("PASS: %s (%u KUnit cases under host shims)\\n", suite.name, i); return 0; }
''')
        (out / "media/v4l2-ctrls.h").write_text('''
#include <linux/v4l2-controls.h>
#define V4L2_CTRL_TYPE_AV1_FRAME 1
#define V4L2_CTRL_TYPE_VP9_FRAME 2
struct v4l2_ctrl { unsigned int type, new_elems; };
union v4l2_ctrl_ptr { void *p; };
static inline union v4l2_ctrl_ptr v4l2_ctrl_ptr_create(void *p)
{ return (union v4l2_ctrl_ptr){ .p = p }; }
int v4l2_ctrl_type_op_validate(const struct v4l2_ctrl *, union v4l2_ctrl_ptr);
''')
        validators = '''#include <errno.h>
#include <string.h>
#include <linux/types.h>
#include <media/v4l2-ctrls.h>
#define zero_reserved(s) memset(&(s).reserved, 0, sizeof((s).reserved))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
'''
        validators += "\n\n".join(extract(source, name) for name in names)
        validators += '''
int v4l2_ctrl_type_op_validate(const struct v4l2_ctrl *ctrl, union v4l2_ctrl_ptr ptr)
{
    if (ctrl->new_elems != 1) abort();
    return ctrl->type == V4L2_CTRL_TYPE_AV1_FRAME ?
        validate_av1_frame(ptr.p) : validate_vp9_frame(ptr.p);
}
'''
        validators = "#include <stdlib.h>\n" + validators
        (out / "validators.c").write_text(validators)
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-O2",
            "-I" + str(out), "-I" + str(tests / "include"),
            "-I" + str(args.kernel / "include/uapi"),
            str(tests.parent / "avd-validation-test.c"), str(out / "validators.c"),
            "-o", str(out / "validate")]
        subprocess.run(command, check=True)
        subprocess.run([str(out / "validate")], check=True)


if __name__ == "__main__":
    main()
