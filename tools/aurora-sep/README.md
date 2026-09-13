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

The kernel driver locates the xART gigalocker itself inside the iBoot System
Container (`modprobe apple_sep xart_writes=1 provision_keybag=1`, with no
`xart_start_sector`): it opens the container directly and serves only the
gigalocker extent, needing no external helper and no hand-supplied sector.

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
