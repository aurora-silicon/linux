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

The raw xART owner is currently read-only. Do not enable the installed
`aurora-sep.service` or run `load-driver` on a shared macOS/Linux installation:
that script requests `xart_writes=1`, which the kernel rejects. The kernel now
resolves the physical `.gl` extent from checksum-validated APFS metadata for
read-only operation; unsupported layouts fail closed. This removes the raw
record-signature locator, which was ambiguous when roots moved away from the
start of the file. APFS lookup alone does not establish write safety. Re-enable
provisioning only after macOS-compatible record updates and APFS ownership
invariants are verified on hardware.

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
install -Dm644 fprintd-aurora.conf \
    /etc/systemd/system/fprintd.service.d/aurora.conf
systemctl daemon-reload
systemctl restart fprintd.service
```

On Omarchy, run `omarchy-apply-lock` once after enrollment. The Quickshell
lock screen then selects `omarchy-lock-fingerprint` automatically and accepts
Touch ID through fprintd. Hyprlock's separate fingerprint switch is not used
by the Quickshell lock screen.
