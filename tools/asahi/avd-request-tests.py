#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run AVD KUnit cases and extracted production decode paths on the host.

MMIO, PM, workqueues and the m2m scheduler are deterministic mocks. This checks
ownership and failure ordering, not hardware pixels, concurrency or throughput.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
AVD = ROOT / "drivers/media/platform/apple/avd"


def function(source, name):
    match = re.search(r"^(?:static )?(?:void|int|irqreturn_t) " + name +
                      r"\([^;]*?\)\n\{", source, re.M)
    if not match:
        raise ValueError(f"missing production function: {name}")
    start = match.start()
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    temporary = tempfile.TemporaryDirectory() if args.output is None else None
    out = Path(temporary.name) if temporary else args.output
    out.mkdir(parents=True, exist_ok=bool(temporary))
    (out / "linux").mkdir()
    (out / "kunit").mkdir()
    (out / "linux/types.h").write_text(
        "#include <stdint.h>\n#include <stdbool.h>\n"
        "typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;\n")
    (out / "kunit/test.h").write_text(r'''
#include <assert.h>
#include <stdio.h>
struct kunit { int unused; };
struct kunit_case { void (*run)(struct kunit *); const char *name; };
struct kunit_suite { const char *name; struct kunit_case *test_cases; };
#define KUNIT_EXPECT_EQ(t, a, b) do { (void)(t); assert((a) == (b)); } while (0)
#define KUNIT_EXPECT_TRUE(t, a) do { (void)(t); assert(a); } while (0)
#define KUNIT_EXPECT_FALSE(t, a) do { (void)(t); assert(!(a)); } while (0)
#define KUNIT_CASE(f) { f, #f }
#define kunit_test_suite(s) \
int main(void) { struct kunit t = {0}; \
for (struct kunit_case *c = (s).test_cases; c->run; c++) { \
c->run(&t); printf("PASS: %s/%s\n", (s).name, c->name); } return 0; }
''')
    driver = (AVD / "avd-drv.c").read_text()
    v4l2 = (AVD / "avd-v4l2.c").read_text()
    names = ("avd_submit_job", "avd_reset", "avd_finish_work", "avd_drain_context",
             "avd_job_finish", "avd_watchdog_func", "avd_irq_handler",
             "avd_job_ready", "avd_device_run")
    body = function(v4l2, "avd_finish_buffers") + "\n".join(
        function(driver, name) for name in names)
    fixture = (ROOT / "tools/asahi/avd-request/host.c").read_text()
    variants = [("decode-path", body)]
    if args.mutations:
        mutations = {
            "wrong-irq-owner": ("ctx = avd->job_ctx;", "ctx = current;"),
            "reuse-active-context": ("return !smp_load_acquire(&ctx->active);", "(void)ctx; return 1;"),
            "complete-queue-head": ("v4l2_m2m_buf_done(ctx->dst_buf, result);",
                                    "v4l2_m2m_buf_done(v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx), result);"),
            "premature-buffer-return": ("ctx->scheduler_released = true;",
                                        "ctx->scheduler_released = true; v4l2_m2m_buf_done(ctx->dst_buf, VB2_BUF_STATE_DONE);"),
        }
        for name, (old, new) in mutations.items():
            if body.count(old) != 1:
                raise ValueError(f"mutation {name} matched {body.count(old)} times")
            variants.append((name, body.replace(old, new)))
    cc = shlex.split(os.environ.get("CC", "cc"))
    common = cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                   "-Wno-unused-parameter", "-g", "-O1", "-fsanitize=address,undefined",
                   "-fno-omit-frame-pointer", "-I" + str(out), "-I" + str(AVD)]
    results = []

    def run(name, source, expect_failure=False):
        dest = out / name
        dest.mkdir()
        compile_result = subprocess.run(common + [str(source), "-o", str(dest / "test")],
                                        capture_output=True, text=True)
        (dest / "build.log").write_text(compile_result.stdout + compile_result.stderr)
        rc = None
        if compile_result.returncode == 0:
            proc = subprocess.run([str(dest / "test")], capture_output=True, text=True,
                                  timeout=30)
            rc = proc.returncode
            (dest / "test.log").write_text(proc.stdout + proc.stderr)
        passed = compile_result.returncode == 0 and ((rc != 0) if expect_failure else (rc == 0))
        results.append({"name": name, "build_rc": compile_result.returncode,
                        "test_rc": rc, "passed": passed})
        print(f"{'PASS' if passed else 'FAIL'}: {name}")
        if not passed:
            print((dest / "build.log").read_text())
            if (dest / "test.log").exists():
                print((dest / "test.log").read_text())

    run("kunit-cases", AVD / "avd-request-test.c")
    header = (AVD / "avd.h").read_text()
    definitions = "\n".join(re.findall(r"^#define AVD_(?:CAPABILITY|QUIRK)_[^\n]+", header, re.M))
    variant_struct = re.search(r"struct avd_variant \{.*?\n\};", header, re.S).group()
    codec_enum = re.search(r"enum avd_codec \{.*?\n\};", header, re.S).group()
    matches = driver[driver.index("static const struct avd_variant avd_t8103_variant"):
                     driver.index("MODULE_DEVICE_TABLE(of, avd_of_match);")]
    match_source = out / "match-data.c"
    match_source.write_text("""
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <linux/types.h>
#define BIT(n) (1U << (n))
#define t8103_configure_stream NULL
#define t8112_configure_stream NULL
#define t8122_configure_stream NULL
struct avd_dev;
typedef unsigned long long dma_addr_t;
struct of_device_id { const char *compatible; const void *data; };
""" + definitions + "\n" + codec_enum + "\n" + variant_struct + "\n" + matches + """
int main(void)
{
    const struct avd_variant *matched = NULL;
    for (size_t i = 0; avd_of_match[i].compatible; i++)
        if (!strcmp(avd_of_match[i].compatible, "apple,t6041-avd"))
            matched = avd_of_match[i].data;
    assert(matched == &avd_t6040_variant);
    assert(matched->capabilities == 15);
    assert(matched->fw_caps_required == AVD_CAPABILITY_HEVC);
    assert(matched->quirks & AVD_QUIRK_HEVC_V5_HEADER);
    assert(!strcmp(matched->fw_name, "apple/avd-fw-v5-t1.bin"));
    assert(avd_t8132_variant.capabilities == 15);
    assert(avd_t8140_variant.capabilities == 15);
    return 0;
}
""")
    run("match-data", match_source)
    (out / "fixture.c").write_text(fixture)
    for name, actual in variants:
        (out / "actual-driver.h").write_text(actual)
        run(name, out / "fixture.c", name != "decode-path")
    passed = all(x["passed"] for x in results)
    receipt = {"passed": passed, "compiler": subprocess.check_output(cc + ["--version"], text=True).splitlines()[0],
               "extracted_sha256": hashlib.sha256(body.encode()).hexdigest(),
               "results": results}
    (out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
