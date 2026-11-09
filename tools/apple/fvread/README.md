# apple-fvread helper

`fvreadctl` opens an APFS container `O_RDONLY`, asks `/dev/apple-fvread` to
perform the proved FileVault-off unlock, and hands control fd 3 to a key-aware
read-only APFS consumer. The consumer receives `FVREAD_CONTROL_FD=3` and the
selected volume UUID in `FVREAD_VOLUME_UUID`.

The control fd provides two read operations:

- `APPLE_FVREAD_IOC_GET_INFO` returns the container size, 4 KiB data-unit
  size, selected volume UUID, and proved unlock mode.
- `APPLE_FVREAD_IOC_READ` reads metadata with the opaque VEK.
- `APPLE_FVREAD_IOC_READ_FILE` supplies a validated APFS file crypto-state
  record, unwraps its PFK in the SEP, and performs the read with that opaque
  key.

In both cases the APFS consumer supplies the physical block and DUN from the
validated extent. FVREAD does not guess crypto identifiers and has no write
operation. Closing the control fd unloads the volume class keys and wipes the
session records.

`mount.fvread` always requests `ro,norecovery`. It refuses with
`EOPNOTSUPP`-style status 95 when no key-aware APFS reader is installed; a
generic APFS implementation that cannot consume the ioctl interface is not a
safe fallback.
