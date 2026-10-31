# AVD conformance

The baseline records the six scores reported in the public AVD v3 cover letter:
77/135, 42/69, 216/305, 1/6, 145/147 and 238/242, in suite order below.
These are upstream reference scores, not results measured with this driver.

`baseline.json` records per-vector limitations separately from scores. Fourteen
AVC vectors have an interlaced SPS verified from the public ITU archives, with
archive checksums and SPS flags recorded. Format marks come from the pinned
Fluster catalogues. The list is incomplete: an unmarked vector is unclassified.
4:4:4, 12-bit, AV1 monochrome and unavailable P210 capture are driver limits;
only the documented H.264 interlace/size limits use `hw-unsupported` marks.

No vector is skipped. A rejected unsupported vector still counts in the raw
Fluster result and denominator. Each suite runs after a preceding failure, with
one job by default and a 30-second per-vector timeout. `run.json` records commands,
return codes and catalogue hashes. CSV summaries, decoder logs and decoded outputs
are retained. Use a new result directory for every run.

Use Fluster revision `5d9d2bc676d97a17900e98fc69216d6d2c8f1ad0` and a GStreamer
build with the four `v4l2sl*dec` elements. Explicit V4L2 stateless decoder names
avoid automatic software decoder selection. Decoder overrides use
`--decoder 'H.264=NAME'`, `H.265`, `VP9` or `AV1`.

Download the public vectors once:

```sh
python3 /opt/fluster/fluster.py --resources /srv/avd-fluster/resources download \
  JVT-AVC_V1 JVT-FR-EXT VP9-TEST-VECTORS VP9-TEST-VECTORS-HIGH \
  JCT-VC-HEVC_V1 AV1-TEST-VECTORS
```

From the kernel checkout, print the exact six commands without opening a device:

```sh
python3 drivers/media/platform/apple/avd/tests/conformance/run-fluster.py \
  --fluster /opt/fluster --resources /srv/avd-fluster/resources \
  --results /srv/avd-fluster/results/conformance-001 --plan
```

Run them on the AVD target:

```sh
python3 drivers/media/platform/apple/avd/tests/conformance/run-fluster.py \
  --fluster /opt/fluster --resources /srv/avd-fluster/resources \
  --results /srv/avd-fluster/results/conformance-001 --probe
```

`--probe` uses ffprobe to annotate the first stream header, retaining probe errors.
It cannot certify that later parameter sets are supported, or identify every
multiple-slice-group vector. Its failure does not prevent decoding. To exercise
several contexts after the serial baseline, repeat with `--jobs 4` and a new result
directory. Check that valid vectors after a timeout still decode and that the
kernel log has no new DART faults. A device without AV1 will report that suite's
failure; do not treat it as an AV1 measurement.

Local checks, without a decoder device:

```sh
sh drivers/media/platform/apple/avd/tests/run-host.sh /tmp/avd-queue-host
python3 drivers/media/platform/apple/avd/tests/run-validation-host.py
python3 drivers/media/platform/apple/avd/tests/conformance/test_fluster.py
```

The validation host runner compiles the production static V4L2 validators and
executes the validation KUnit cases through host shims. It does not exercise the
kernel control framework, MMIO or IOMMU. An in-kernel KUnit run must also enable
`CONFIG_VIDEO_APPLE_AVD_KUNIT_TEST`, `CONFIG_VIDEO_APPLE_AVD` and `CONFIG_KUNIT`.
For an arm64 QEMU KUnit run from a full source tree with the cross compiler:

```sh
python3 tools/testing/kunit/kunit.py run --arch=arm64 \
  --cross_compile=aarch64-linux-gnu- --build_dir=/tmp/avd-kunit \
  --kunitconfig=drivers/media/platform/apple/avd/tests/.kunitconfig 'apple-*'
```

The `apple-avd-validation`, `apple-avd-queue` and `apple-dart-firmware-leaf` suites
cover validation and recovery bookkeeping; live DART switching still needs a
target run.

Public sources:

- [AVD v3 cover letter and reference scores](https://lore.kernel.org/linux-media/20261020-avd-v3-0-85cbaa2125d8@icloud.com/)
- [AV1 tile validation](https://lore.kernel.org/linux-media/20261020-avd-v3-4-85cbaa2125d8@icloud.com/)
- [VP9 tile row validation](https://lore.kernel.org/linux-media/20261020-avd-v3-5-85cbaa2125d8@icloud.com/)
- [Pinned Fluster catalogues](https://github.com/fluendo/fluster/tree/5d9d2bc676d97a17900e98fc69216d6d2c8f1ad0/test_suites)
