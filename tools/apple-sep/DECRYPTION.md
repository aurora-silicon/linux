# Biometric item decryption: application integration

This extends the biometric signing interface. The driver accepts a wrapped
application P-256 key
and an ECIES ciphertext, performs a new fingerprint match in a fresh kernel-owned
context, decrypts in SEP, deletes the context, and delivers plaintext to the host.
No raw SEP protocol, authentication handle or machine artifact is needed by the
application. Do not use trusted-key machine sealing or ATTEST as a substitute.

The recipient private scalar remains inside SEP. The requested item's plaintext
does reach the application. A synced passkey encrypted as an item is therefore
used for **software signing after biometric decryption**. This is different from
a new SEP-generated passkey, for which `sign_digest` returns only a signature.
There is no demonstrated import-existing-private-key-and-sign-in-SEP capability.

## Files and supported entry points

- `sep-bio.h`: structures, constants and ioctl definitions.
- `sep-key.py`: `SepClient.create(decrypt=True)` and
  `SepClient.decrypt_into(blob, sealed, output, timeout_ms=60000)`.
- `sep_envelope.py`: `seal_item(public, context, plaintext)` and
  `item_view(output, length, expected_context)`.

Python 3.11+ and the `cryptography` package are needed for the envelope example.
Signing verification additionally uses OpenSSL. The examples are for integration
and disposable-data validation; Python and its crypto backend cannot guarantee
zeroization of every temporary. Production secret handling belongs in the
application's existing bounded, zeroizing memory implementation.

The device is root-only and exclusively opened. Coordinate with fprintd through
a trusted helper/broker that checks caller identity, key ownership and purpose.
Neither the sample nor driver implements application authorization, WebAuthn
RP/origin checks or multi-user isolation. Do not grant applications unrestricted
device access or trust a caller-supplied `user_verified` boolean.

## Key creation and persistence

Use CREATE flag `SEP_BIO_KEY_CREATE_DECRYPT` (1) for a new biometric item key.
Flag 0 retains the existing signing policy; do not assume signing-only blobs
support decryption. The decrypt-enabled policy also permits biometric signing;
the broker must restrict allowed application operations itself.

```sh
sudo python3 sep-key.py create --decrypt disposable-item-key.json
```

Creation needs working biometric restoration and current enrollments but does
not scan a finger. The blob, 65-byte public point, policy and expected key ID must
be stored atomically with integrity protection in the application's private
store. The example JSON is not a kernel format. Do not use a maintainer's test
key, provision/replace the system keybag, alter enrollments or load old xART.

Wrapped blobs are bound to this device and secure-state lineage, not portable
backups. Their number is not a measured hardware capacity. Serialize requests
and bound live resources rather than assuming unlimited contexts or slots.

`GET_INFO.reserved[0]` advertises `SEP_BIO_INFO_BIOMETRIC_KEYS` (2) for signing
and `SEP_BIO_INFO_BIOMETRIC_DECRYPT` (4) for this decrypt interface. The ABI version
remains 4. The client checks the appropriate capability before key creation or
decryption. Older signing-only drivers need not support decrypt. Handle ENOTTY,
EINVAL and EOPNOTSUPP explicitly; do not downgrade to an unrestricted key or
software root. Unsupported key-store protocol profiles fail closed.

## Reference use

The following uses only disposable application data. Resolve example paths
relative to the directory containing these files. The application supplies its
own canonical context encoding; this illustrative context is not a new storage
standard or a WebAuthn authorization scheme.

```python
import base64
import ctypes
import importlib.util
import json
from pathlib import Path
from sep_envelope import seal_item, item_view

spec = importlib.util.spec_from_file_location('sep_key', Path('sep-key.py'))
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
saved = json.loads(Path('disposable-item-key.json').read_text())
blob = base64.b64decode(saved['blob'], validate=True)
public = base64.b64decode(saved['public'], validate=True)

# Derive this from trusted selected-record state, including owner and purpose.
context = b'app=example;owner=local;record=disposable;purpose=password;v=1'
sealed = seal_item(public, context, b'disposable example secret')
# Store sealed, plus integrity-protected key/record metadata. No scan to encrypt.

output = bytearray(4096)
try:
    print('Touch an enrolled finger now', flush=True)
    with api.SepClient() as client:
        size = client.decrypt_into(blob, sealed, output)
    item = item_view(output, size, context)
    # Use this borrowed view in the authorized operation; do not log it.
    # Imported passkey scalars must still be signed with host software.
finally:
    ctypes.memset(ctypes.addressof((ctypes.c_ubyte * len(output)).from_buffer(output)),
                  0, len(output))
```

`decrypt_into` writes into a caller-owned 4096-byte bytearray and returns the
actual length. It clears that buffer on errors; the caller must clear successful
output too. `item_view` checks the decrypted binding and returns a borrowed view,
not a private-key import or a secret-free signing capability. Do not copy it to
immutable strings/bytes, logs or a plaintext temporary file.

Encryption uses transient ECDH/AES material on the host. The persistent recipient
private key stays inside SEP; a claim that all encryption keys stay in SEP would
be incorrect. Decrypting a vault-wide root is technically possible within the
size limit, but releases that root to the host. Prefer per-item envelopes when
the desired authorization scope is one selected item.

## Exact wire and ciphertext formats

All ioctl fields use native integer byte order (little-endian on the target).
Pointers are aligned 64-bit userspace addresses, not IOVAs or SEP handles.

| Request | Number | Layout in field order | Size |
| --- | --- | --- | --- |
| CREATE | `0xc058b10b` | flags u32, capacity/length u32, blob pointer u64, public[65], reserved[7] | 88 |
| DECRYPT_START | `0x4020b10e` | flags u32, timeout_ms u32, blob_len u32, sealed_len u32, blob pointer u64, ciphertext pointer u64 | 32 |
| DECRYPT_POLL | `0xc018b10f` | state u32, status i32, guidance u32, capacity/length u32, plaintext pointer u64 | 24 |

CREATE capacity must be 4096; reserved bytes zero. DECRYPT_START flags must be
zero; timeout is 1..120000 ms; blob length 1..4096; ciphertext length 81..4096
including overhead. Both inputs are copied before START returns. Rebuild the
POLL descriptor with output capacity before every call; never reuse a previous
pending response's zero length as the next capacity.

States: pending 1, progress 2, done 3, failed 4. Status is negative errno on
failure. Use the same `poll(2)` and CANCEL (`0xb107`) as signing. Allocate 4096
output bytes to avoid ENOSPC; it reports required length but does not consume
the successful result. Successful delivery consumes it; another immediate POLL
returns ENODATA. After expiry it can report ETIMEDOUT instead.

The result is eligible for delivery only before the original monotonic request
deadline and for five seconds after completion, whichever is sooner. An existing
worker waits for delivery, cancellation or the deadline and expires abandoned
output independently of userspace polling, subject to timer granularity and
normal scheduling. Close/cancel drops retained output. These kernel
bounds do not erase plaintext already copied into application memory.

The ciphertext format tested with this firmware is:

```text
ephemeral uncompressed P-256 point[65] || ciphertext[N] || GCM tag[16]
Z = ECDH(sender ephemeral private, recipient public), 32-byte big-endian X
KDF block i = SHA256(Z || BE32(i) || ephemeral point[65]), starting at i=1
material = block1 || block2
AES key = material[0:32]; IV = material[32:48]; AAD = empty
```

Use AES-256 with the derived **16-byte IV**, not AES-128 or a random 12-byte IV.
The sample authenticated plaintext is `SEPITEM 0x01` (8 bytes), a BE32 context
length, context, then item bytes. The 81-byte ECIES overhead leaves at most 4015
plaintext bytes, including that envelope. The firmware authenticates ciphertext;
the application checks the decrypted context before using the item.

Expected context must come from trusted selected-record state, not merely an
unencrypted field stored beside the blob. Include application/owner, record ID,
purpose and algorithm with canonical unambiguous encoding. Envelope binding does
not prevent replay of an older valid version; rollback protection needs separate
trusted state. Encryption is public-key encryption: anyone with the public point
can construct a new valid ciphertext, so this is not proof of who created an
item. Preserve the application's authenticated storage and record ownership.

## Errors and evidence limits

Fail closed on EACCES (match/authorization refusal), ETIMEDOUT, ECANCELED,
EIO (transport/protocol/cleanup or rejected decrypt), EROFS and unsupported
operations. EBUSY means another operation/cleanup owns the device. Do not
automatically retry ambiguous completion or request another scan in a hidden
loop. A UI may offer an explicit new operation.

Biometric-gated decryption, saved-key warm-reboot use, modified-tag rejection
without plaintext, fresh-request refusal without a new scan, single-use delivery
and signing regression passed on J293/M1. Autonomous expiry of an undelivered
result and cancellation before delivery also passed on hardware, in addition
to the focused source tests. Cold power-cycle, suspend, other hardware,
biometric enrollment-set invalidation,
native private-key import and production broker isolation are not established.
No production vault migration is implied by these primitive tests.
