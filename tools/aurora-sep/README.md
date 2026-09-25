# Aurora SEP userspace integration

These files connect the Apple SEP kernel driver to the shared APFS xART
gigalocker and the desktop fingerprint stack.

Install the driver service:

```sh
install -Dm755 load-driver /usr/local/sbin/aurora-sep-load
install -Dm644 aurora-sep.service /etc/systemd/system/aurora-sep.service
systemctl daemon-reload
systemctl enable aurora-sep.service
```

`xart_writes=0` remains the default. `load-driver` explicitly opts into writes
and must only be enabled on a machine whose APFS `.gl` extent passes the
driver's narrow ownership checks: one physical extent, no snapshots or revert
state, and a single extent reference owned by the `.gl` inode. The kernel
resolves that extent from checksum-validated APFS metadata; unsupported layouts
fail closed. The opt-in has been tested on m2 with identity-keybag provisioning,
trusted-key seal/unseal, and reboot reload. The raw iBoot container hash did not
change in those tests, so an actual xART write was **not** validated. Do not
infer that all SEP or Touch ID operations work with a read-only GigaLocker.
Read `XART-SAFETY.md` before enabling writes on another dual-boot machine.

For read-only structural diagnostics on Linux, run
`sudo python3 apfs-xart-inspect.py /dev/disk/by-partlabel/iBootSystemContainer`.
The inspector checks APFS metadata checksums, resolves the latest complete
checkpoint's xART `.gl` file extent, and compares it with the legacy root-record
heuristic. It prints physical addresses and record metadata, not payloads.
It currently supports only a single-node object map and file-system tree and
cannot authorize raw writes. Apple's APFS reference is the format authority:
https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf.
`python3 -m unittest discover -s tests -v` runs a synthetic regression case:
when the first live root moves to slot 10, the legacy heuristic chooses a
window shifted 90 APFS blocks beyond the actual `.gl` extent. This proves the
locator can misidentify the extent; it does not identify the historical cause
of any particular damaged container.

For fingerprint support, apply
`patches/libfprint-1.94.100-apple-sep.patch` to libfprint 1.94.100, build and
install libfprint, then install the fprintd device policy:

```sh
git clone --depth 1 --branch v1.94.100 \
    https://gitlab.freedesktop.org/libfprint/libfprint.git libfprint-aurora
cd libfprint-aurora
git apply ../linux/tools/aurora-sep/patches/libfprint-1.94.100-apple-sep.patch
meson setup build -Dprefix=/usr -Ddrivers=default -Dintrospection=false \
    -Dgtk-examples=false -Ddoc=false -Dinstalled-tests=false
ninja -C build
meson test -C build --print-errorlogs
```

Adjust the patch path for your checkout. The patch includes a fix for an
upstream Meson test loop when introspection is disabled. In the Aurora
`linux-build` guest, the additional packages needed for this build are
`libgudev libgusb pixman cairo glib2-devel`; `meson` and `ninja` are already
in the release image. The default driver set retains all normal libfprint
drivers and adds Aurora. This build was checked on aarch64: the shared library
and all default drivers compiled, with 0 test failures (most device tests
skip without their optional mocks/hardware). That does **not** prove Touch ID
works on m2. The patched library is installed privately under
`/opt/aurora-sep` there, with fprintd loading it via a service drop-in while
the distro libfprint remains untouched. fprintd discovers one Apple SEP sensor.
The Aurora driver now reports stage progress only when the kernel's stage
number increases; guidance and stage-zero events are not captured samples.
fprintd 1.94.5 itself emits an initial `enroll-stage-passed` after its
duplicate check, **before** starting enrollment. That first line does not
prove a finger was captured.
The device-specific `apple/mesa_calibration.bin` on m2 was recovered using
the bundled [`extract-mesa-calibration.py`](extract-mesa-calibration.py), copied
from [Gist revision `eb079a8007985d04e75182f20994f1a1c496f12e`](https://gist.github.com/DjDeveloperr/867a1961b861c570442724f48f770158/eb079a8007985d04e75182f20994f1a1c496f12e),
which scans the local iBoot System Container read-only for an FSCl/CALB record
with an IM4M manifest. It checks the container structure and markers, **not**
the manifest's cryptographic signature. Keep this blob private to the machine;
it is not a generic firmware package. On m2, a premature master
`SAVE_CATACOMB` returned SEP status `0x6` while its component state was `0x3`
(no save-pending bit). The
`m2fix10` fresh-context gate skips that unavailable save, and bounded
enrollment attempts reached the physical-finger wait without an immediate
enclave error. With the later UUID-forwarding stage2, a physical enrollment
completed on m2fix15 and master/user Catacombs were saved, but on each tested
reboot SEP listed zero live identities despite the host index still listing
the finger. m2fix16–m2fix20 narrowed this to the user Catacomb load: it
returns `0x8002` and becomes active, but reveals zero identities even with
the SCRD credential and sensor/device view prepared first. The saved files
remain backed up root-only on m2. A bounded repeated load still listed zero
identities. An opt-in owner export then produced a durable owner file, but
loading it did not recover the old user identity. `m2fix23` is a separate,
boot-tested candidate that saves owner in the fresh context and user before
master at completion; it has not been finger- or reboot-persistence-tested.
Login must stay password-based until a live post-reboot identity and
successful match are independently verified.

On each Linux machine, extract its own calibration from its local iBoot System
Container (the input partition is discovered automatically):

```sh
sudo install -d -m 0755 /usr/lib/firmware/apple
sudo python3 tools/aurora-sep/extract-mesa-calibration.py \
  -o /usr/lib/firmware/apple/mesa_calibration.bin
```

The extractor reads the iBoot partition but never writes to it. It creates the
output mode `0600`, rejects input/output aliases, and refuses ambiguous or
malformed calibration candidates. Do not commit or copy the resulting blob to
another machine.

Once the hardware path is verified, install libfprint and then the fprintd
device policy:

```sh
install -Dm644 fprintd-aurora.conf \
    /etc/systemd/system/fprintd.service.d/aurora.conf
systemctl daemon-reload
systemctl restart fprintd.service
```

On Omarchy, run `omarchy-apply-lock` once after enrollment. The Quickshell
lock screen then selects `omarchy-lock-fingerprint` automatically and accepts
Touch ID through fprintd. Hyprlock's separate fingerprint switch is not used
by the Quickshell lock screen.
