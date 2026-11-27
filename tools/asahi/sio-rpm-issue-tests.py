#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual apple-sio.c runtime PM and issue lifecycle code in a host model.

Verifies that a configured channel issuing after autosuspend or mid-suspend
defers descriptor publication to a resume worker rather than sending MSG_ISSUE
to stopped firmware, holds a runtime PM reference until the channel is quiet,
and releases it cleanly across terminate, synchronize, report, and idle paths.
"""
import argparse
import json
import os
import re
import resource
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SIO = "drivers/dma/apple-sio.c"

FUNCTIONS = [
    "sio_issued_slot",
    "sio_tx_quiet_locked",
    "sio_scrub_locked",
    "sio_source_locked",
    "sio_ring_enter_locked",
    "sio_ring_leave_locked",
    "sio_settle_locked",
    "sio_old_issue_ack_locked",
    "sio_ring_issue_ack_locked",
    "sio_firmware_active",
    "sio_chan_tx_busy_locked",
    "sio_try_get_tx_pm_locked",
    "sio_take_tx_pm_put_locked",
    "sio_put_tx_pm",
    "sio_handle_issue_ack",
    "sio_fill_in_old_locked",
    "sio_fill_in_ring_locked",
    "sio_fill_in_locked",
    "sio_record_error_locked",
    "sio_clear_error_locked",
    "sio_lookup_error_locked",
    "sio_update_current_tx_locked",
    "sio_tx_status",
    "sio_abort_issued_locked",
    "sio_issue_work",
    "sio_issue_pending",
    "sio_terminate_all",
    "sio_terminate_work",
    "sio_drained",
    "sio_clear_idle",
    "sio_set_idle",
    "sio_synchronize",
    "sio_old_report_locked",
    "sio_ring_report_locked",
    "sio_process_report",
    "sio_device_config",
    "sio_can_runtime_suspend",
    "sio_remove",
    "sio_chan_busy",
    "sio_halt_cpu",
    "sio_suspend",
    "sio_resume",
]

MUTATIONS = {
    "publish-while-suspended": (
        "return !READ_ONCE(sio->rpm_stopped) && !READ_ONCE(sio->asleep);",
        "(void)sio;\n\treturn true;",
    ),
    "skip-rpm-get": (
        "ret = pm_runtime_get_if_active(sio->dev);",
        "ret = 1;",
    ),
    "drop-tx-pm-early": (
        "if (!siochan->tx_rpm_held || sio_chan_tx_busy_locked(siochan))",
        "if (!siochan->tx_rpm_held)",
    ),
    "skip-issue-flush": (
        "\tflush_work(&siochan->issue_wq);\n",
        "",
    ),
    "skip-remove-cancel": (
        "\t\tcancel_work_sync(&siochan->issue_wq);\n",
        "",
    ),
    "strand-on-resume-fail": (
        "\t\tdev_err(sio->dev, \"runtime resume before issue on chan %d failed: %d\\n\",\n"
        "\t\t\tsiochan->no, ret);\n"
        "\t\tspin_lock_irqsave(&siochan->vc.lock, flags);\n"
        "\t\tif (!siochan->current_tx)\n"
        "\t\t\tsio_abort_issued_locked(siochan);",
        "\t\tdev_err(sio->dev, \"runtime resume before issue on chan %d failed: %d\\n\",\n"
        "\t\t\tsiochan->no, ret);\n"
        "\t\tspin_lock_irqsave(&siochan->vc.lock, flags);",
    ),
    "late-issue-rearms-tasklet": (
        "\tif (READ_ONCE(siochan->host->removing)) {\n"
        "\t\tspin_unlock_irqrestore(&siochan->vc.lock, flags);\n"
        "\t\treturn;\n"
        "\t}\n"
        "\tvchan_issue_pending(&siochan->vc);",
        "\tvchan_issue_pending(&siochan->vc);\n"
        "\tif (READ_ONCE(siochan->host->removing)) {\n"
        "\t\tif (!siochan->current_tx)\n"
        "\t\t\tsio_abort_issued_locked(siochan);\n"
        "\t\tspin_unlock_irqrestore(&siochan->vc.lock, flags);\n"
        "\t\treturn;\n"
        "\t}",
    ),
    "clear-errors-on-start": (
        "\t\t/* Retire any stale wrapped-around entry for this reused cookie. */\n"
        "\t\tsio_clear_error_locked(siochan, vd->tx.cookie);",
        "\t\tmemset(siochan->error_cookies, 0, sizeof(siochan->error_cookies));",
    ),
    "skip-remove-mailbox-off": (
        "\t\tapple_rtkit_mailbox_off(sio->rtk);\n",
        "",
    ),
    "skip-remove-shutdown": (
        "\t\tif (sio->arrays) {\n"
        "\t\t\tint err = apple_rtkit_shutdown(sio->rtk);",
        "\t\tif (0 && sio->arrays) {\n"
        "\t\t\tint err = apple_rtkit_shutdown(sio->rtk);",
    ),
}


def block(text, start):
    begin = text.index("{", start)
    depth, end = 1, begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return end


def extract_function(text, name):
    pat = re.compile(
        r"^(?:static [^\n;]*?\b)" + re.escape(name) + r"\([^;]*?\)\n\{",
        re.M | re.S,
    )
    match = pat.search(text)
    if not match:
        return None
    return text[match.start():block(text, match.start())]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--ref", help="read apple-sio.c from this revision")
    parser.add_argument("--mutation", choices=sorted(MUTATIONS))
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    if args.ref:
        sio_src = subprocess.check_output(
            ["git", "show", f"{args.ref}:{SIO}"], cwd=ROOT, text=True
        )
    else:
        sio_src = (ROOT / SIO).read_text()

    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        if sio_src.count(old) != 1:
            raise RuntimeError(f"mutation {args.mutation} did not match once")
        sio_src = sio_src.replace(old, new)

    extracted = []
    has_issue_work = False
    for fn in FUNCTIONS:
        body = extract_function(sio_src, fn)
        if body:
            extracted.append(body)
            if fn == "sio_issue_work":
                has_issue_work = True

    extra_fields = ""
    if has_issue_work:
        extra_fields = (
            "\tstruct work_struct issue_wq;\n"
            "\tbool tx_rpm_held;\n"
            "\tstruct sio_error_cookie error_cookies[SIO_MAX_ERROR_COOKIES];\n"
            "\tunsigned int error_head;"
        )

    fwd = "static bool sio_fill_in_locked(struct sio_chan *siochan);\n"
    code = (
        Path(__file__).with_suffix(".c")
        .read_text()
        .replace("/* EXTRA_CHAN_FIELDS */", extra_fields)
        .replace("/* ACTUAL_SIO_FUNCTIONS */", fwd + "\n".join(extracted))
    )

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        source = out / "sio-rpm-issue.c"
        binary = out / "sio-rpm-issue"
        source.write_text(code)
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-function",
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                "-D__packed=__attribute__((packed))",
                "-DIS_ENABLED(option)=1",
            ]
            + (["-DHAVE_ISSUE_WORK"] if has_issue_work else [])
            + [str(source), "-o", str(binary)],
            check=True,
        )
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / "results.json").write_text(
            json.dumps(
                {
                    "exit": result.returncode,
                    "stdout": result.stdout,
                    "stderr": result.stderr,
                    "ref": args.ref,
                    "mutation": args.mutation,
                    "limits": "Host model of pm_runtime, virt-dma, and RTKit; no real SIO hardware.",
                },
                indent=2,
            )
            + "\n"
        )
        print(result.stdout + result.stderr, end="")
        return result.returncode != 0

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if args.out:
        return run(args.out.resolve())
    with tempfile.TemporaryDirectory(prefix="sio-rpm-issue-") as out:
        return run(Path(out))


if __name__ == "__main__":
    raise SystemExit(main())
