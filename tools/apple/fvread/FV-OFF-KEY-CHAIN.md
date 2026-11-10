# Apple-silicon APFS FileVault-off key chain

## Finding

Keybag tag 7 is `KB_TAG_VOLUME_DEVICE_KEY`: a class-based device wrapping
key used by hardware-encryption configurations.  On the M1 Pro it is a raw
40-byte wrapped key, keyed by the container UUID.  It is not the 60-byte
versioned media-state record used by tags 5 and 6.

The 40-byte tag-7 payload is the input to the SEP `UNWRAP_MEDIA_KEY`
operation with APFS protection class M (14).  The returned opaque key and IV
decrypt the `nx_keylocker` extent.  The resulting container keybag contains the
Data-volume UUID's tag-2 wrapped VEK.  With FileVault off, that VEK is protected
only by the hardware UID, so `UNWRAP_VEK` receives empty secret and KEK fields
plus the tag-2 VEK.  `LOAD_CLASS_KEYS` receives the same empty fields and VEK
blob for per-file class-key operations.

In compact form:

```text
nx_mkb_locker
  container UUID, tag 7, 40 bytes
    -> SEP UNWRAP_MEDIA_KEY(class M)
    -> opaque media key + IV
    -> AES-XTS read of nx_keylocker (DUN = physical block)
    -> container keybag
       Data-volume UUID, tag 2, wrapped VEK
         -> SEP UNWRAP_VEK(secret = empty, KEK = empty)
         -> opaque VEK for metadata reads
         -> SEP LOAD_CLASS_KEYS(empty, empty, wrapped VEK)
```

The tag-3 volume-keybag extent and its user/recovery KEKs are the FileVault-on
credential path.  A tag-6 volume media key is not part of the observed
Apple-silicon FileVault-off chain.

## Evidence

- Read-only captures of both the installed-macOS container and the stub
  container contain exactly one `nx_mkb_locker` entry: container UUID, tag 7,
  length 40.  Neither has tag 5 or tag 6.  The capture changed zero sectors on
  every APFS partition.
- Apple's APFS reference defines `nx_keylocker` as the container keybag range,
  tag 2 as the wrapped VEK, tag 3 as the volume-keybag location, protection
  class M as 14, and explicitly limits its key-unwrapping procedure to software
  encryption.
- Apple's platform-security documentation says an Apple-silicon Mac with
  FileVault off still encrypts the volume, but protects its VEK only with the
  Secure Enclave hardware UID.
- The in-tree SEP provider's `UNWRAP_MEDIA_KEY` request accepts exactly a
  40-byte wrapped key and a protection class.  Its FV-off `UNWRAP_VEK` and
  `LOAD_CLASS_KEYS` request builders already support empty secret/KEK fields.

## Public sources

- [Apple File System Reference](https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf)
- [Apple Platform Security: Volume encryption with FileVault](https://support.apple.com/guide/security/volume-encryption-with-filevault-sec4c6dc1b6e/web)
- [Public APFS keybag analysis naming tag 7](https://jtsylve.blog/post/2022/12/21/APFS-Keybags)
- [Asahi Linux Apple-silicon platform security overview](https://asahilinux.org/docs/platform/security/)
- [apfs-fuse fixed-key B-tree and omap implementation](https://github.com/sgan81/apfs-fuse/tree/master/ApfsLib)

## Implementation consequences

`fvread.rs` now selects only the container-UUID tag-7 record from
`nx_mkb_locker`, passes it to `apple_sep_fv_unwrap_media_key()` with class M,
decrypts `nx_keylocker`, and sends the selected Data-volume tag-2 VEK directly
through the existing FV-off unwrap/load operations.  It never reads a volume
keybag on this path.

The container omap lookup now follows physical fixed-key index nodes until a
leaf, validating checksums, object identity/type/subtype, root B-tree metadata,
strictly increasing `(oid, xid)` keys, decreasing levels, child bounds, leaf
object size, and deletion flags.  All device access remains bounded and
read-only; the driver neither mounts nor writes an APFS object.
