# AVD request ownership tests

From the kernel source root:

```sh
python3 tools/asahi/avd-request-tests.py --mutations --output /tmp/avd-requests
python3 tools/asahi/avd-codec-tail-tests.py --mutations --output /tmp/avd-codecs
```

Each output directory must be new. The runners compile with the host C compiler,
ASan and UBSan, retain build/test logs and write a JSON receipt. `CC` selects a
different compiler. No decoder, firmware or root access is needed.

The request runner executes the same cases as the `apple-avd-request` KUnit
suite, compiles the actual OF match table and variants, then extracts production
submission, IRQ, watchdog, retirement and buffer-completion functions. The C
fixture supplies deterministic MMIO, m2m, PM and workqueue mocks. It drives two
contexts through CPU preparation and serialized hardware ownership, submission
IRQs, duplicate/wrong-slot VP events, timeout recovery, PM/validation failures,
held capture, streamoff drain failed-reset admission and failed-table cleanup. Five mutations must
compile and fail at runtime; a failed control or mutant build fails the runner.

The capability runner executes the production firmware-tail parser, including
short/absent tails, variant intersection, and T6040's requirement for explicit
firmware HEVC support. Missing legacy metadata retains the other codecs.

These tests do not emulate the kernel scheduler or hardware memory ordering.
They establish host-side ownership behavior, not decoded pixel correctness,
firmware concurrency, runtime performance or hardware recovery. The pipeline
allows one CPU builder alongside one hardware request across independent
contexts. Each context retains one request, and H.264 retains the m2m core's
held-capture scheduling path. No concurrent VP/PP ownership or single-stream
throughput gain is claimed.

For an arm64 compile with the KUnit cases included:

```sh
make O=out ARCH=arm64 LLVM=1 defconfig
scripts/config --file out/.config --enable MEDIA_SUPPORT --enable MEDIA_CONTROLLER \
  --enable VIDEO_DEV --enable MEDIA_PLATFORM_SUPPORT --enable V4L_PLATFORM_DRIVERS \
  --enable VIDEO_APPLE_AVD --enable KUNIT --enable VIDEO_APPLE_AVD_KUNIT_TEST
make O=out ARCH=arm64 LLVM=1 olddefconfig
make O=out ARCH=arm64 LLVM=1 W=1 -j8 drivers/media/platform/apple/avd/
```
