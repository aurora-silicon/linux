# Biometric P-256 keys

`sep-key.py` is a small reference client for the key ioctls in
`drivers/soc/apple/sep-bio.h`. It needs Python 3.11 or newer and the OpenSSL
command-line tool for independent signature verification. It does not implement
Apple firmware protocols in userspace.

The driver creates a P-256 private key with a biometric credential-set policy.
Applications store the returned opaque wrapped blob and public point. The private
scalar remains inside SEP. These keys are device bound; the blob is not a backup
that can be restored onto another machine or after erasing secure state.

## Example

The device must already have working biometric restoration and enrolled fingers.
It uses the same exclusive open as fprintd. Stop competing clients for this test;
the sample does not change services, enroll fingers or provision keybags.

```sh
sudo python3 tools/apple-sep/sep-key.py create application-key.json
printf 'example message' > message
sudo python3 tools/apple-sep/sep-key.py sign application-key.json message signature.der
```

Touch the sensor when prompted. The second command hashes the message once with
SHA-256, asks SEP to sign that digest, and independently verifies the returned
DER ECDSA signature against the saved public key before writing it. Running it
again starts a new authentication request. The client never automatically retries
an operation whose completion is unknown.

Applications which already hash their input can use `sign-digest` instead of
`sign`. Its input file must contain exactly 32 raw digest bytes; it does not hash
them again. Python callers can directly use `SepClient.sign_digest(blob, digest)`.

The JSON file is an example application container, not a kernel wire format.
Persist the blob and public point atomically in an application's existing store.
Protect their integrity and associate them with the intended application key ID.
For WebAuthn, hash `authenticatorData || clientDataHash` once. Existing synced
passkeys cannot be replaced with newly generated SEP keys without registering
the new public key with the relying party.

## Interface

Open `/dev/sep-bio` read/write. `SEP_BIO_GET_INFO` advertises
`SEP_BIO_INFO_BIOMETRIC_KEYS` in `reserved[0]`. Unsupported key-store protocol
profiles fail closed. The interface is additive to biometric ABI version 4.

* `SEP_BIO_KEY_CREATE`: flags/reserved zero for signing; use
  `SEP_BIO_KEY_CREATE_DECRYPT` for biometric signing/decryption keys when
  `SEP_BIO_INFO_BIOMETRIC_DECRYPT` is advertised. Supply a 4096-byte output buffer
  through `blob_ptr`, with `blob_len = SEP_BIO_KEY_BLOB_MAX`. Returns actual blob
  length and the 65-byte uncompressed public point. Creation does not scan a
  finger; private-key use carries the biometric policy.
* `SEP_BIO_KEY_SIGN_START`: supply the saved blob, a 32-byte prehashed digest,
  flags/reserved zero and a timeout from 1 to 120000 milliseconds. The driver
  copies the inputs before returning. It creates a fresh authentication context,
  matches into that context and signs the submitted digest after a match.
* `SEP_BIO_KEY_SIGN_POLL`: returns pending/done/failed, existing capture guidance,
  a negative errno on failure, or a DER ECDSA signature. `poll(2)` wakes on guidance
  or completion. Repeated polls report the same operation; they do not sign again.
* `SEP_BIO_CANCEL`: requests cancellation. Closing the descriptor also cancels
  delivery. The worker retains ownership until its hardware operations and
  credential cleanup finish; another start/open may temporarily return `EBUSY`.

A request's credential never crosses the application interface. It is deleted
before successful completion is published. Disposal failure disables application
key operations until reboot. A canceled or expired request cannot deliver a
successful signature. Cancellation cannot undo a signing command already sent
into hardware; callers must not automatically retry an ambiguous failure.

The existing firmware pipeline can finish capture before the requested maximum
timeout. `ETIMEDOUT` requires a fresh explicit request. `EACCES` reports a
nonmatching finger or authorization refusal, `ECANCELED` cancellation, `EROFS`
disabled persistent storage, `EOPNOTSUPP` an unsupported/disabled key API, and
`EIO` a transport, protocol or cleanup failure.

## Trust and scope

The device remains root-only. A privileged application or trusted broker owns the
request; this API does not authenticate browser origins, enforce relying-party
ownership or isolate multiple Linux users sharing one SEP identity. Do not grant
unrestricted device access to applications. fprintd and a key broker must
coordinate the existing exclusive device ownership.

The policy accepts enrolled fingerprints in the configured credential set. It
does not implement invalidation whenever enrollment changes. Ordinary fingerprint
match tokens are not SEP key credentials. Existing machine-key attestation and
trusted-key sealing are separate operations and do not confer biometric key use.

This interface covers new P-256 keys and biometric signing. It also supports
[biometric item decryption](DECRYPTION.md), with a reference envelope
implementation in `sep_envelope.py`. It does not implement private-key
import, P-384, protected-root migration or passkey registration.
