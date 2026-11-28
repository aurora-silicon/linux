# Reporting Aurora Linux results

Search the [existing issues](https://github.com/omacom/linux-aurora/issues)
before opening a report. Add evidence for a known bug to its existing issue,
including your Mac and build details. Link related issues, pull requests, and
relevant code or error examples instead of opening duplicates.

For a broad test report, keep **one issue per Mac** and append later results as
comments on that issue. Use a title such as `j516s (MacBook Pro M3 Pro): Bluetooth
setup times out`. The [reporting guide (#6)](https://github.com/iconidentify/aurora-linux/issues/6)
and [roadmap (#56)](https://github.com/omacom/linux-aurora/issues/56) track
reporting and fix priorities. The [latest release](https://github.com/omacom/linux-aurora/releases/latest)
shows available release artifacts; report the exact tag you actually installed.

## Machine and build

Include the Mac model, board (`apple,j…`), SoC (`apple,t…`), distribution, release
tag, running kernel, and installed bootloader/package versions. Mention local
patches or an unreleased build's commit. These read-only commands collect the
board, running kernel, and both firmware versions:

```sh
uname -r
tr '\0' '\n' < /proc/device-tree/compatible
for property in asahi,os-fw-version asahi,system-fw-version; do
    printf '%s: ' "$property"
    if [ -r "/proc/device-tree/chosen/$property" ]; then
        tr '\0' '\n' < "/proc/device-tree/chosen/$property"
        printf '\n'
    else
        printf 'not available\n'
    fi
done
```

Report both firmware fields, even when they differ. If the system cannot boot,
give the available information and say which values could not be collected.
For calibration-related failures, say whether provisioning completed and whether
the data came from this Mac; report a missing file's name, never its contents.

For graphics or session failures, include the Mesa package/version and active
renderer, compositor/window manager and version, relevant application and
lock-screen versions, and selected GPU profile/experimental opt-in. If already
available, `glxinfo -B` reports the GL renderer and `hyprctl version` reports the
Hyprland version. Say when these tools are unavailable. A software renderer,
loaded driver, or successful build alone does not establish hardware rendering.

## Reproduction and results

State the expected result, actual result, shortest reproduction, and frequency
(for example, two failures in ten boots). Include the last known-good and first
bad release/build if known. Keep separate results for each action: `pass`,
`fail`, or `not tested`, with a reason such as no dock available.

- **Display/Thunderbolt:** dock/display model, connection type (Thunderbolt or
  USB-C DisplayPort), port, mode/refresh rate, and hotplug versus coldplug.
- **Touch ID:** distinguish enrolment, verification, and verification after a
  reboot. Do not post enrolled data or reset a keybag to prepare a report.
- **Suspend/session:** say whether the session was still locked **before any
  authentication**, whether it resumed or restarted, and whether the compositor
  PID changed if known. Record what happened; a working desktop after unlocking
  does not prove that the lock survived resume.
- **Crash:** include the executable, signal, timestamp, package/version, and
  relevant trimmed journal/backtrace lines. Identify the action that triggered
  it. A raw core dump is unnecessary.

Quote error lines literally in fenced code blocks with nearby relevant context.
Avoid whole journals or `dmesg` dumps. Link an existing crash report or code
example and explain how its behavior matches yours. Explicitly list untested
features; do not turn a module probe or compile result into a runtime pass.

## Keep private data private

Review attachments before posting. Do not include serial numbers, Bluetooth
addresses, account details, passwords, keys, fingerprints, calibration bytes,
raw device trees, copied per-unit firmware images, or raw core dumps. Keep
useful error text while removing identifying values. If a field is unavailable
or sensitive, say so rather than substituting data from another Mac.

For kernel patches, also follow the existing
[kernel submission guidance](Documentation/process/submitting-patches.rst).

## Pull request rules

See [Aurora Silicon's pull request rules](https://github.com/aurora-silicon/agent-guidelines)
and run the [pre-PR checklist](https://github.com/aurora-silicon/agent-guidelines/blob/main/pre-pr-checklist.md)
before opening a pull request.

## Contributing code

`main` is the only branch work lands in. Open every pull request against
`main` from a short-lived branch:

- `feat/<topic>` for new work, `fix/<topic>` for fixes.
- `wip/<topic>` for work you park without a pull request. It is not merged
  from directly; open a `feat/` or `fix/` pull request when it is ready.
- `archive/<name>` holds old branches kept for their history. Do not push to them.
- `feat/<area>-<name>` can also be a topic branch for a large feature that
  takes several pull requests: the parts are reviewed as pull requests into
  the topic branch, the whole is regression-tested there and lands on `main`
  as one pull request, and the topic branch is then deleted.
- `rebase/v<version>` is the single branch for an upstream version upgrade,
  and `hotfix/v<X.Y.Z>` a short-lived branch for a stable hotfix (see
  [RELEASES.md](RELEASES.md)).

There are no long-lived release or per-platform branches. The full rules are in
[branching-and-releases](https://github.com/aurora-silicon/agent-guidelines/blob/main/policy/branching-and-releases.md).

`main` takes changes only through pull requests. The branch rules prevent
direct pushes, force pushes and deletion. Start new work from a fresh branch
off `main`. Retarget any stacked pull requests before their base is merged
and its branch is deleted.

Releases are cut from `main` on a fixed cadence and never wait for unfinished
work. How the edge, nightly and stable channels are built is in
[RELEASES.md](RELEASES.md).
