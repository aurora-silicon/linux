/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/* Copyright 2026 Dj */
/*
 * Userspace interface for the SEP biometric device.
 *
 * The enclave does the matching. Nothing biometric crosses this interface: only
 * an operation, a stage, a status, an opaque identity UUID, and an opaque host
 * label userspace chooses.
 */

#ifndef _UAPI_LINUX_APPLE_SEP_H
#define _UAPI_LINUX_APPLE_SEP_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SEP_BIO_IFACE_VERSION  5

#define SEP_BIO_UUID_LEN       16
#define SEP_BIO_LABEL_LEN      128
#define SEP_BIO_NONCE_LEN      32
#define SEP_BIO_TOKEN_LEN      32
#define SEP_BIO_MAX_IDENTITIES 32
#define SEP_BIO_CHALLENGE_LEN  32
#define SEP_BIO_ATTEST_PUB_LEN 65
#define SEP_BIO_ATTEST_SIG_MAX 72

enum {
	SEP_BIO_STATE_IDLE     = 0,
	SEP_BIO_STATE_PENDING  = 1,
	SEP_BIO_STATE_PROGRESS = 2,
	SEP_BIO_STATE_DONE     = 3,
	SEP_BIO_STATE_FAILED   = 4,
};

enum {
	SEP_BIO_NO_MATCH = 0,
	SEP_BIO_MATCH    = 1,
	SEP_BIO_NOT_COMPARED = 2,
};

struct sep_bio_identity {
	__u8 uuid[SEP_BIO_UUID_LEN];
	__u8 label[SEP_BIO_LABEL_LEN];
};

struct sep_bio_info {
	__u32 version;
	__u32 sensor_present;
	__u32 enrolled;
	__u32 capacity; /* usable slots in this SEP user context, not ABI list size */
	__u32 enroll_stages;
	__u32 reserved[3];
};

/* reserved[0] flags; reserved[1] is a live SEP identity count only when valid. */
#define SEP_BIO_INFO_LIVE_COUNT_VALID 1u

struct sep_bio_list {
	__u32 count;
	__u32 reserved;
	struct sep_bio_identity id[SEP_BIO_MAX_IDENTITIES];
};

struct sep_bio_enrol_start {
	__u32 flags;
	__u32 account_uid; /* resolved passwd uid; UINT32_MAX is invalid */
	__u32 finger;      /* FP_FINGER_LEFT_THUMB..FP_FINGER_RIGHT_LITTLE (1..10) */
	__u32 reserved;
	__u8  label[SEP_BIO_LABEL_LEN];
};

/* Linux's built-in SEP user context only. This does not enumerate other SEP
 * users or OS lanes. DELETE_ORPHAN uses an exact UUID; the kernel index may
 * still hold it when fprintd has already lost its own record.
 */
struct sep_bio_enclave_list {
	__u32 count;
	__u32 reserved;
	__u8 uuid[SEP_BIO_MAX_IDENTITIES][SEP_BIO_UUID_LEN];
};

#define SEP_BIO_GUIDANCE_NONE          0
#define SEP_BIO_GUIDANCE_PLACE         1
#define SEP_BIO_GUIDANCE_LIFT_AND_MOVE 2
#define SEP_BIO_GUIDANCE_HOLD_STILL    3
/*
 * The finger of the last touch is still on the sensor; the next capture is
 * armed when it is lifted. Older readers treat it as SEP_BIO_GUIDANCE_NONE.
 */
#define SEP_BIO_GUIDANCE_LIFT          4
/*
 * Why a capture did not count. A verify that ends SEP_BIO_STATE_FAILED with
 * status 5 (retry), and a running enrolment whose poll carries status 5
 * (reported once per unusable capture), name the cause
 * in 'guidance': PLACE when no finger came in time, LIFT_AND_MOVE for a frame
 * that could not be read, or one of these. Older kernels leave the guidance of
 * the capture there.
 */
#define SEP_BIO_GUIDANCE_CENTER        5 /* partial contact */
#define SEP_BIO_GUIDANCE_TOO_FAST      6 /* the finger moved */
#define SEP_BIO_GUIDANCE_CLEAN         7 /* dirt or moisture */

struct sep_bio_enrol_poll {
	__u32 state;
	__u32 stage;
	__u32 stages_total;
	__u32 status;
	__u8  uuid[SEP_BIO_UUID_LEN];
	__u32 guidance;
	__u32 progress_percent;
};

struct sep_bio_verify_start {
	__u32 flags;
	__u32 reserved;
	__u8  nonce[SEP_BIO_NONCE_LEN];
};

struct sep_bio_verify_poll {
	__u32 state;
	__u32 result;
	__u32 status;
	__u32 guidance; /* SEP_BIO_GUIDANCE_*; status only, no capture side effect */
	__u8  uuid[SEP_BIO_UUID_LEN];
	__u8  token[SEP_BIO_TOKEN_LEN];  /* single use, bound to the nonce */
	__u64 deadline_ns;      /* CLOCK_MONOTONIC; past this the token is void */
};

struct sep_bio_delete {
	__u8 uuid[SEP_BIO_UUID_LEN];
};

/*
 * Device attestation of key possession: the enclave signs 'challenge' with the
 * machine ref-key (ECDSA-P256 over the challenge as the pre-computed digest) and
 * returns the DER signature and public point. The private key never leaves the
 * enclave.
 */
struct sep_bio_attest {
	__u32 sig_len;                              /* out: DER signature length */
	__u8  challenge[SEP_BIO_CHALLENGE_LEN];  /* in */
	__u8  public_key[SEP_BIO_ATTEST_PUB_LEN]; /* out: P-256 point, 04||X||Y */
	__u8  signature[SEP_BIO_ATTEST_SIG_MAX]; /* out: DER SEQUENCE{r,s} */
	__u8  reserved[3];
};

#define SEP_BIO_IOC_MAGIC 0xB1

#define SEP_BIO_GET_INFO     _IOR(SEP_BIO_IOC_MAGIC, 0x01, struct sep_bio_info)
#define SEP_BIO_LIST         _IOR(SEP_BIO_IOC_MAGIC, 0x02, struct sep_bio_list)
#define SEP_BIO_ENROL_START  _IOW(SEP_BIO_IOC_MAGIC, 0x03, struct sep_bio_enrol_start)
#define SEP_BIO_ENROL_POLL   _IOR(SEP_BIO_IOC_MAGIC, 0x04, struct sep_bio_enrol_poll)
#define SEP_BIO_VERIFY_START _IOW(SEP_BIO_IOC_MAGIC, 0x05, struct sep_bio_verify_start)
#define SEP_BIO_VERIFY_POLL  _IOR(SEP_BIO_IOC_MAGIC, 0x06, struct sep_bio_verify_poll)
#define SEP_BIO_CANCEL       _IO(SEP_BIO_IOC_MAGIC, 0x07)
#define SEP_BIO_DELETE       _IOW(SEP_BIO_IOC_MAGIC, 0x08, struct sep_bio_delete)
#define SEP_BIO_DELETE_ALL   _IO(SEP_BIO_IOC_MAGIC, 0x09)
#define SEP_BIO_ATTEST       _IOWR(SEP_BIO_IOC_MAGIC, 0x0a, struct sep_bio_attest)
#define SEP_BIO_LIST_ENCLAVE _IOR(SEP_BIO_IOC_MAGIC, 0x0b, struct sep_bio_enclave_list)
#define SEP_BIO_DELETE_ORPHAN _IOW(SEP_BIO_IOC_MAGIC, 0x0c, struct sep_bio_delete)
/* Creates the empty Linux owner context only when SEP lists zero Linux
 * identities. ENROL_START itself never repairs a missing persistence binding.
 */
#define SEP_BIO_PREPARE_ENROL _IOW(SEP_BIO_IOC_MAGIC, 0x0d, struct sep_bio_enrol_start)

/*
 * A verify poll's 'status' when the enclave has stopped matching after too
 * many failed attempts. Every verify then fails at once, without a capture,
 * until a password authentication re-arms Touch ID through SEP_BIO_REARM.
 * The lockout outlasts a restart.
 */
#define SEP_BIO_STATUS_LOCKOUT 8

/*
 * SEP_BIO_LOCKOUT_STATE works on /dev/sep-bio-rearm, which any number of root
 * callers may open, and on /dev/sep-bio, so that the verifying daemon can say
 * why a verify ended with SEP_BIO_STATUS_LOCKOUT. Older kernels answer it on
 * /dev/sep-bio with -ENOTTY.
 */
struct sep_bio_lockout {
	__u32 locked;    /* 1 while every verify is refused */
	__u32 failures;  /* failed matches since the last match or re-arm */
	__u32 reserved;
	__u32 flags;     /* SEP_BIO_LOCKOUT_FLAG_*; zero from older kernels */
};

/*
 * SEP_BIO_REARM never waits for a capture while Touch ID is not locked, and
 * then clears pending failed attempts: call it after every verified password.
 * Without this flag, call it only while 'locked'.
 */
#define SEP_BIO_LOCKOUT_FLAG_EVERY_PASSWORD 1
/*
 * Set while Touch ID waits for the first password since this boot
 * (SEP_BIO_REQUIRE_PASSWORD); 'locked' is 1 then too.
 */
#define SEP_BIO_LOCKOUT_FLAG_AFTER_BOOT 2
/*
 * Set while 'locked' follows too many failed matches: five since the last
 * match or password, or the enclave's own lockout after failed matches. A
 * lockout without it (and without SEP_BIO_LOCKOUT_FLAG_AFTER_BOOT) needs the
 * password for another reason, such as a lapsed match credential.
 */
#define SEP_BIO_LOCKOUT_FLAG_FAILED_ATTEMPTS 4

#define SEP_BIO_LOCKOUT_STATE _IOR(SEP_BIO_IOC_MAGIC, 0x0e, struct sep_bio_lockout)
/*
 * Ends a lockout. Call it only once the user's password has been verified:
 * the password is used once to unwrap the Linux-owned FV re-arm record and is
 * wiped after the request. Needs CAP_SYS_ADMIN. Without a lockout it clears
 * the failed attempts (see SEP_BIO_LOCKOUT_FLAG_EVERY_PASSWORD).
 */
#define SEP_BIO_REARM_SECRET_MAX 1024
struct sep_bio_rearm {
	__u32 secret_len;
	__u32 reserved;
	__u8  secret[SEP_BIO_REARM_SECRET_MAX];
};

#define SEP_BIO_REARM _IOW(SEP_BIO_IOC_MAGIC, 0x0f, struct sep_bio_rearm)

/*
 * Touch ID refuses every verify, without a capture and with
 * SEP_BIO_STATUS_LOCKOUT, until the next SEP_BIO_REARM: macOS's "Touch ID
 * requires your password when the Mac restarts". A boot service calls it,
 * before any login, when the administrator asks for that; without it Touch ID
 * works from the first login. Nothing is recorded across a restart. Needs
 * CAP_SYS_ADMIN; on /dev/sep-bio-rearm.
 */
#define SEP_BIO_REQUIRE_PASSWORD _IO(SEP_BIO_IOC_MAGIC, 0x10)

/*
 * /dev/sep-key: P-256 keys created inside the SEP.
 *
 * Each key is made by the enclave's key store and bound to this SEP and to
 * the Linux identity key bag. The private key never leaves the enclave.
 * Userspace holds only an opaque handle (at most SEP_KEY_HANDLE_MAX bytes)
 * that works on the Mac that made it and nowhere else; keep it like a key
 * file. Signing takes a 32-byte digest (for ES256/ecdsa-sha2-nistp256, the
 * SHA-256 of the message) and returns a DER ECDSA-Sig-Value.
 *
 * A key made without flags has no access control: whoever can open the node
 * and holds its handle can sign with it. A key made with
 * SEP_KEY_CREATE_BIOMETRY_CURRENT_SET (version 2) carries an access-control
 * list the enclave enforces: every signature needs a Touch ID match of a
 * finger enrolled when the key was made. SEP_KEY_SIGN then waits (about a minute)
 * for a touch; the kernel opens a credential context, runs the match in it,
 * signs and deletes the context inside that one call. Enrolling or removing a
 * finger makes the key unusable (macOS .biometryCurrentSet).
 * SEP_KEY_CREATE_BIOMETRY_ANY is macOS .biometryAny: the same, but any finger
 * enrolled on this install matches, including ones enrolled later. At most one
 * policy bit may be set; flags_supported lists the ones this kernel accepts.
 *
 * SEP_KEY_CREATE_USER_PRESENCE (.userPresence: Touch ID or the password) and
 * SEP_KEY_CREATE_DEVICE_PASSCODE (.devicePasscode) are reserved. They need
 * the enclave to check the user's own password; on Linux today the enclave's
 * only passcode is a key bag secret root holds, so CREATE refuses them with
 * EOPNOTSUPP rather than check a password in software.
 *
 * Version 3 adds SEP_KEY_CANCEL, SEP_KEY_SEAL and SEP_KEY_UNSEAL (INFO
 * 'features'). CANCEL ends a touch-gated SIGN or UNSEAL that is waiting for
 * a finger; that call fails with ECANCELED. SEAL encrypts up to
 * SEP_KEY_SECRET_MAX bytes to a biometric key's public point (ECIES, in the
 * kernel, no touch). UNSEAL has the enclave decrypt such a blob with the key
 * under the same touch-satisfied credential context as a SIGN, so a sealed
 * secret is released only after a Touch ID match the enclave saw. SEAL and
 * UNSEAL refuse keys made without a biometric policy (EINVAL).
 *
 * Keep a second key (a backup) wherever a biometric key is trusted, as with
 * hardware security keys: no key can be exported or recovered.
 *
 * The node is created 0600; distributions may grant a group. Unknown 'flags'
 * bits and 'reserved' fields must be zero.
 *
 * Errors: EINVAL for a malformed request or handle, EKEYREJECTED when the
 * enclave refuses the key (a handle from another Mac or an earlier Linux
 * install, or an access-control list not satisfied), EACCES when the touch
 * did not match (or Touch ID is locked out), ETIMEDOUT when no finger came,
 * ENODEV while the key store or Touch ID is not up, EOPNOTSUPP for a
 * reserved policy, ECANCELED after SEP_KEY_CANCEL, EIO for any other
 * enclave failure.
 */
#define SEP_KEY_IFACE_VERSION 3
#define SEP_KEY_FEATURE_CANCEL 0x1
#define SEP_KEY_FEATURE_SEAL   0x2
#define SEP_KEY_SECRET_MAX 64
#define SEP_KEY_SEALED_MAX 256
#define SEP_KEY_CREATE_BIOMETRY_CURRENT_SET 0x1
#define SEP_KEY_CREATE_BIOMETRY_ANY         0x2
#define SEP_KEY_CREATE_USER_PRESENCE        0x4  /* reserved: EOPNOTSUPP */
#define SEP_KEY_CREATE_DEVICE_PASSCODE      0x8  /* reserved: EOPNOTSUPP */
#define SEP_KEY_HANDLE_MAX 2048
#define SEP_KEY_PUB_LEN    65  /* uncompressed point, 04 || X || Y */
#define SEP_KEY_DIGEST_LEN 32
#define SEP_KEY_SIG_MAX    72  /* DER SEQUENCE { INTEGER r, INTEGER s } */

struct sep_key_info {
	__u32 version;          /* SEP_KEY_IFACE_VERSION */
	__u32 flags_supported;  /* SEP_KEY_CREATE_* bits this kernel accepts */
	__u32 handle_max;       /* SEP_KEY_HANDLE_MAX */
	__u32 features;         /* SEP_KEY_FEATURE_* (version 3; zero before) */
};

struct sep_key_create {
	__u32 flags;                            /* in: SEP_KEY_CREATE_* */
	__u32 handle_len;                       /* out */
	__u8  public_key[SEP_KEY_PUB_LEN];      /* out */
	__u8  reserved[3];                      /* in: 0 */
	__u8  handle[SEP_KEY_HANDLE_MAX];       /* out */
};

struct sep_key_sign {
	__u32 flags;                            /* in: 0 */
	__u32 handle_len;                       /* in */
	__u8  digest[SEP_KEY_DIGEST_LEN];       /* in */
	__u32 sig_len;                          /* out */
	__u8  signature[SEP_KEY_SIG_MAX];       /* out, zero-padded */
	__u8  handle[SEP_KEY_HANDLE_MAX];       /* in */
};

struct sep_key_public {
	__u32 flags;                            /* in: 0 */
	__u32 handle_len;                       /* in */
	__u8  public_key[SEP_KEY_PUB_LEN];      /* out */
	__u8  reserved[3];                      /* in: 0 */
	__u8  handle[SEP_KEY_HANDLE_MAX];       /* in */
};

#define SEP_KEY_GET_INFO _IOR(SEP_BIO_IOC_MAGIC, 0x20, struct sep_key_info)
#define SEP_KEY_CREATE   _IOWR(SEP_BIO_IOC_MAGIC, 0x21, struct sep_key_create)
#define SEP_KEY_SIGN     _IOWR(SEP_BIO_IOC_MAGIC, 0x22, struct sep_key_sign)
#define SEP_KEY_PUBLIC   _IOWR(SEP_BIO_IOC_MAGIC, 0x23, struct sep_key_public)

/* SEAL: secret in, sealed out. UNSEAL: sealed in, secret out (waits for a touch). */
struct sep_key_seal {
	__u32 flags;                            /* in: 0 */
	__u32 handle_len;                       /* in */
	__u32 secret_len;                       /* SEAL in, UNSEAL out */
	__u32 sealed_len;                       /* SEAL out, UNSEAL in */
	__u8  secret[SEP_KEY_SECRET_MAX];
	__u8  sealed[SEP_KEY_SEALED_MAX];
	__u8  handle[SEP_KEY_HANDLE_MAX];       /* in */
};

#define SEP_KEY_CANCEL   _IO(SEP_BIO_IOC_MAGIC, 0x25)
#define SEP_KEY_SEAL     _IOWR(SEP_BIO_IOC_MAGIC, 0x26, struct sep_key_seal)
#define SEP_KEY_UNSEAL   _IOWR(SEP_BIO_IOC_MAGIC, 0x27, struct sep_key_seal)

/*
 * Test only, refused unless apple_sep.key_selftest=1, root: sign with a
 * biometric key under a deliberately unsuitable credential context, to show
 * the enclave refuses. Modes: 1 no context, 2 a context with no match,
 * 3 a context holding only the key bag secret, 4 a deleted context, 5 one
 * touch: sign in the matched context, delete it, sign again with it.
 * 6 (handle ignored): make a throwaway key with the .userPresence list and
 * sign with a key-bag-secret context ('result') and with a context with no
 * match ('result2').
 * 7 (research): the same throwaway key signed with a context whose passcode
 * credential comes from another user's key bag. In: 'enclave_status' = that
 * uid, 'handle'/'handle_len' = that bag's password. Out: 'result2' = the
 * verify-secret status, 'result'/'enclave_status' = the signature.
 * 8 (research): a throwaway .userPresence key under the unlocked bag of the
 * uid in 'enclave_status'; 'result' = signed with that bag's password
 * credential (password in 'handle'), 'result2' = with no credential.
 * 9 (research): as 8, 'result' = with the identity bag's secret as the
 * credential, 'result2' = with the password given in 'handle'.
 * 10 (research): as 8, but the kernel also locks the bag for good and signs
 * with a stale, an empty and a fresh credential, then unlocks and signs
 * again; the results come back in 'digest' (see key.rs).
 * 11 (research): make such a key under that uid's unlocked bag and return its
 * record in 'handle'. 12: sign that record with a password credential from
 * the bag; the password is the NUL-padded 'digest'.
 * 20 one touch: sign, then sign again in the same open context ('result2').
 * 21 one touch: sign, then unseal a secret sealed to the key in the same
 * context ('result2' 0 when it came back unchanged). 22 (no finger): unseal
 * with no context ('result') and with an unsatisfied one ('result2').
 * 24 (no finger, handle ignored): five throwaway keys (no list, "oecd" free,
 * no decrypt entry, "ock" free, the plain ock/odel/osgn/oa/okd list), each
 * signed, decrypted, key-exchanged and unsealed with no, an unsatisfied and
 * a key-bag-secret context; bit 8*key+row of 'result' (keys 0-3) or bit row
 * of 'result2' (key 4) is set when that row worked (rows in key.rs).
 * 'result' is 0 when the enclave signed, else a negative errno.
 */
struct sep_key_selftest {
	__u32 mode;
	__u32 handle_len;
	__u8  digest[SEP_KEY_DIGEST_LEN];
	__s32 result;
	__s32 enclave_status;
	__s32 result2;          /* mode 5: the signature after the delete */
	__s32 enclave_status2;
	__u8  handle[SEP_KEY_HANDLE_MAX];
};

#define SEP_KEY_SELFTEST _IOWR(SEP_BIO_IOC_MAGIC, 0x24, struct sep_key_selftest)

/*
 * DESIGN DRAFT, not a stable ABI: the per-user key bag whose secret is the
 * login password. Root only, on the re-arm node, never on /dev/sep-bio. The
 * password passes through once per call and the kernel keeps no copy.
 * 0x0e/0x0f are the lockout re-arm and 0x10 SEP_BIO_REQUIRE_PASSWORD; these
 * use 0x30-0x36.
 */
#define SEP_KEYBAG_SECRET_MAX 256

struct sep_keybag_unlock {      /* UNLOCK and CREATE */
	__u32 uid;
	__u32 secret_len;          /* UNLOCK permits zero to cancel pending lock */
	__u8  secret[SEP_KEYBAG_SECRET_MAX];
};

#define SEP_KEYBAG_LOCK_SCREEN  1   /* after grace_s */
#define SEP_KEYBAG_LOCK_SUSPEND 2   /* at once */
#define SEP_KEYBAG_LOCK_LOGOUT  3   /* at once; the password is needed again */

struct sep_keybag_lock {
	__u32 uid;
	__u32 reason;
	__u32 grace_s;
	__u32 reserved;
};

#define SEP_KEYBAG_STATE_LOCKED_SINCE_BOOT 0
#define SEP_KEYBAG_STATE_UNLOCKED          1
#define SEP_KEYBAG_STATE_LOCKING           2
#define SEP_KEYBAG_STATE_LOCKED            3

#define SEP_KEYBAG_BIO_ALLOWED             0
#define SEP_KEYBAG_BIO_PASSWORD_SINCE_BOOT 1
#define SEP_KEYBAG_BIO_NO_UNLOCK_48H       2
#define SEP_KEYBAG_BIO_NO_PASSWORD_156H    3
#define SEP_KEYBAG_BIO_FAILED_MATCHES      4

struct sep_keybag_state {
	__u32 uid;                /* in */
	__u32 bag_state;          /* SEP_KEYBAG_STATE_* */
	__u32 biometry;           /* SEP_KEYBAG_BIO_* */
	__u32 failed_matches;
	__u64 since_password_s;   /* ~0 when none since boot */
	__u64 since_unlock_s;
	__u32 enclave_lock_state; /* raw enclave state word */
	__u32 lock_phase;         /* enclave lock phase */
};

struct sep_keybag_change {
	__u32 uid;
	__u32 old_len;
	__u32 new_len;
	__u32 reserved;
	__u8  old_secret[SEP_KEYBAG_SECRET_MAX];
	__u8  new_secret[SEP_KEYBAG_SECRET_MAX];
};

#define SEP_BIO_KEYBAG_STATE  _IOWR(SEP_BIO_IOC_MAGIC, 0x30, struct sep_keybag_state)
#define SEP_BIO_KEYBAG_UNLOCK _IOW(SEP_BIO_IOC_MAGIC, 0x31, struct sep_keybag_unlock)
#define SEP_BIO_KEYBAG_LOCK   _IOW(SEP_BIO_IOC_MAGIC, 0x32, struct sep_keybag_lock)
#define SEP_BIO_KEYBAG_CHANGE _IOW(SEP_BIO_IOC_MAGIC, 0x33, struct sep_keybag_change)
#define SEP_BIO_KEYBAG_CREATE _IOW(SEP_BIO_IOC_MAGIC, 0x34, struct sep_keybag_unlock)

/*
 * A user's wrapped bag, for userspace to keep across reboots (as macOS
 * saves it). It holds no secret: it opens only with the user's password,
 * on this Mac. EXPORT copies the designated bag; IMPORT loads a saved one
 * and designates it again, locked until SEP_BIO_KEYBAG_UNLOCK.
 */
#define SEP_KEYBAG_BLOB_MAX 4096

struct sep_keybag_blob {
	__u32 uid;
	__u32 len;
	__u8  data[SEP_KEYBAG_BLOB_MAX];
};

#define SEP_BIO_KEYBAG_EXPORT _IOWR(SEP_BIO_IOC_MAGIC, 0x35, struct sep_keybag_blob)
#define SEP_BIO_KEYBAG_IMPORT _IOW(SEP_BIO_IOC_MAGIC, 0x36, struct sep_keybag_blob)

/*
 * DESIGN DRAFT, not a stable ABI: sealing a disk-encryption secret (a LUKS2
 * key-slot secret) to a user's key bag, as FileVault ties the volume key to
 * the user's password. Root only, on the re-arm node.
 *
 * FDE_CREATE makes a P-256 sealing key inside the enclave under the
 * designated, unlocked bag of 'uid' and returns its record. The record holds
 * no secret and works only with this Mac's enclave. FDE_SEAL encrypts
 * 'secret' to that key and has the enclave open the result once before
 * returning 'sealed'. FDE_UNSEAL returns the secret; the enclave opens it
 * only while the bag of 'uid' is unlocked (SEP_BIO_KEYBAG_UNLOCK with the
 * user's password, which the enclave checks and counts).
 *
 * The secret is bound to the OS install that sealed it (the apple_sep
 * os_uuid_hi/lo identity, else /chosen/apfs-preboot-uuid): the identity is
 * sealed with the secret and FDE_UNSEAL returns the secret only under the
 * same identity, as FileVault binds a volume key to its volume.
 *
 * Errors: EINVAL for a malformed request, EKEYREJECTED when the enclave
 * refuses (bag locked, a record or blob from another Mac or another bag),
 * EXDEV on FDE_UNSEAL of a secret sealed under another OS install, ENOKEY
 * when the kernel has no OS identity (FDE_SEAL and FDE_UNSEAL),
 * EOPNOTSUPP on a key store without user bags.
 */
#define SEP_FDE_RECORD_MAX 2048
#define SEP_FDE_SECRET_MAX 64
#define SEP_FDE_SEALED_MAX 256

struct sep_fde_key {
	__u32 uid;                         /* in */
	__u32 record_len;                  /* out */
	__u8  record[SEP_FDE_RECORD_MAX];  /* out */
};

struct sep_fde_seal {
	__u32 uid;                         /* in */
	__u32 record_len;                  /* in */
	__u32 secret_len;                  /* SEAL: in; UNSEAL: out (in 0) */
	__u32 sealed_len;                  /* SEAL: out (in 0); UNSEAL: in */
	__u8  secret[SEP_FDE_SECRET_MAX];
	__u8  sealed[SEP_FDE_SEALED_MAX];
	__u8  record[SEP_FDE_RECORD_MAX];  /* in */
};

#define SEP_BIO_FDE_CREATE _IOWR(SEP_BIO_IOC_MAGIC, 0x37, struct sep_fde_key)
#define SEP_BIO_FDE_SEAL   _IOWR(SEP_BIO_IOC_MAGIC, 0x38, struct sep_fde_seal)
#define SEP_BIO_FDE_UNSEAL _IOWR(SEP_BIO_IOC_MAGIC, 0x39, struct sep_fde_seal)

#endif /* _UAPI_LINUX_APPLE_SEP_H */
