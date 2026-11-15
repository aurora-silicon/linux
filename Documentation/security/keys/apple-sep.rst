======================
Apple SEP Trusted Keys
======================

The ``apple_sep`` driver registers ``applesep`` as a source for the
:doc:`trusted-encrypted` key type. Select it with ``trusted.source=applesep``
when another trusted-key source would otherwise be selected. Registration fails
if another source is already active. The driver must have an available SEP
profile, a persistent Linux identity keybag, and an admitted machine reference
key before sealing or loading keys can succeed.

Commands and plaintext boundary
===============================

The trusted-key core accepts ``new <bytes>``, ``import <hex-key>``, and
``load <hex-blob>``. Key sizes are 32 through 128 bytes. ``new`` obtains random
key material through the SEP source by default; ``trusted.rng=kernel`` selects
the kernel RNG instead. ``import`` accepts plaintext already held by the caller.
For example, in a session keyring with the source active::

    umask 077
    key_id=$(keyctl add trusted sep-example "new 32" @s)
    keyctl pipe "$key_id" > sep-example.blob
    keyctl revoke "$key_id"
    loaded_id=$({ printf 'load '; cat sep-example.blob; } |
        keyctl padd trusted sep-example-loaded @s)

To import an existing key, supply its hex encoding on standard input. Here
``hex_key`` represents an existing 32-to-128-byte key, encoded as 64-to-256 hex
characters::

    imported_id=$(printf 'import %s' "$hex_key" |
        keyctl padd trusted sep-import @s)
    unset hex_key

``keyctl pipe`` and ordinary key reads return the sealed blob in hex, not the
plaintext key. Loading a blob places recovered plaintext in the kernel trusted
key payload for kernel consumers. Plaintext therefore exists in kernel memory;
this source does not keep all key use inside the SEP. There is no current
userspace plaintext-unseal or login-keyring unlock API. Adding one requires an
explicit authorization and userspace integration policy.

The SEP source ignores trailing trusted-key policy options: they do not add PCR
binding, a password requirement, or user-presence enforcement. Unsealing uses the
persisted identity-bag secret, without requiring a fingerprint or fresh login
authentication. Normal Linux key permissions still apply. The source marks keys
non-migratable, so ``keyctl update`` is refused with ``EPERM``.

Persistent identity and reference key
=====================================

The driver uses these files under the system root filesystem:

* ``/var/lib/aurora-sep-keybag.bin`` holds the Linux identity-keybag record,
  including its wrapped bag, identity, and bag secret.
* ``/var/lib/aurora-sep-refkey.bin`` holds the default machine reference-key
  record. Its private key remains in the SEP.
* ``/var/lib/aurora-sep-refkey-v2.bin`` is selected instead when the
  ``apple_sep`` module parameter ``refkey_v2`` is nonzero. The default is zero.

Store creation runs with kernel root credentials and mode ``0600``. Preserve
root ownership and restricted permissions on existing files. Treat the
identity-keybag record as secret state, not merely a copy of an encrypted key.
The reference-key record and identity bag must continue to admit the same SEP
private key to recover existing blobs. Do not delete or regenerate these files
as routine error recovery. The alternate slot preserves the default reference
key file; selecting it does not migrate existing blobs to a new key.

The host identity-keybag record has its own version and integrity checks; these
are separate from the trusted-key sealed blob. Neither firmware-independent
blob compatibility nor automatic rotation or migration is provided by this
interface. Moving a blob to another machine, identity bag, or reference key is
not a supported recovery procedure.

Admission and persistence failures
==================================

Before publishing a new or imported blob, sealing performs a SEP private-key
unseal round trip and compares the recovered key with the input. This rejects
new blobs when a cached public key can encrypt but the identity bag can no
longer admit the matching private key. It proves recovery at that operation,
not after a later bag change, reboot, or firmware update. SEP recovery failures
in the unseal callback return ``EBADMSG`` without distinguishing a corrupt blob
from a foreign or unavailable private-key context. Command parsing and an absent
source can return other errors.

Content-changing identity-bag saves followed by reboot remain an unresolved
`persistence issue <https://github.com/iconidentify/aurora-linux/issues/31>`_.
The admission check does not repair that lifecycle or recover already stranded
blobs. Keep the existing identity and reference-key state intact when reporting
such failures. Userspace login-keyring integration remains a separate open
decision in `issue 18 <https://github.com/iconidentify/aurora-linux/issues/18>`_.

Implementation
==============

The command parser and hex-blob reads are in
:file:`security/keys/trusted-keys/trusted_core.c`. Source registration and
payload handling are in :file:`drivers/soc/apple/trusted_shim.c` and
:file:`drivers/soc/apple/trusted.rs`. The private-key admission check, slot
selection, and identity-bag recovery are in
:file:`drivers/soc/apple/refkey.rs`; host identity persistence is in
:file:`drivers/soc/apple/keybag.rs` and :file:`drivers/soc/apple/store_shim.c`.
