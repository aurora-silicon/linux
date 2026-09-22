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
that script requests `xart_writes=1`, which the kernel now rejects while the
physical `.gl` extent is selected by scanning record signatures rather than
validated against APFS file metadata. The scan is ambiguous when live root
records have moved away from the start of the file. Read-only inspection does
not establish write safety. Re-enable provisioning only after the APFS mapping
and macOS-compatible record update protocol are verified on hardware.

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
