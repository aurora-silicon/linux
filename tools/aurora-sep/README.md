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
Container. The service loads it with xART writes off. Before enabling writes,
boot once read-only and check the located base in `dmesg`:

```sh
journalctl -k -b | grep 'in-kernel raw-extent owner'
```

Then pin that base and enable writes in `/etc/modprobe.d/aurora-sep.conf`.
`xart_start_sector` is the base divided by 512, and `provision_keybag=1` is
needed only on the boot that creates the keybag:

```
options apple_sep xart_writes=1 xart_start_sector=<base / 512> provision_keybag=1
```

Module options go in `modprobe.d` rather than on a `modprobe` command line, so
they apply however the module is loaded. The SEP attach is one-shot per boot:
a load with the wrong options cannot be redone without a reboot.

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
