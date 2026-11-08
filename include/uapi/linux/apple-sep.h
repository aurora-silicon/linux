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

/* /dev/sep-bio-rearm, which any number of root callers may open. */
struct sep_bio_lockout {
	__u32 locked;    /* 1 while every verify is refused */
	__u32 failures;  /* failed matches since the last match or re-arm */
	__u32 reserved[2];
};

#define SEP_BIO_LOCKOUT_STATE _IOR(SEP_BIO_IOC_MAGIC, 0x0e, struct sep_bio_lockout)
/*
 * Ends a lockout. Call it only once the user's password has been verified:
 * the password is used once to unwrap the Linux-owned FV re-arm record and is
 * wiped after the request. Needs CAP_SYS_ADMIN. Without a lockout it does
 * nothing.
 */
#define SEP_BIO_REARM_SECRET_MAX 1024
struct sep_bio_rearm {
	__u32 secret_len;
	__u32 reserved;
	__u8  secret[SEP_BIO_REARM_SECRET_MAX];
};

#define SEP_BIO_REARM _IOW(SEP_BIO_IOC_MAGIC, 0x0f, struct sep_bio_rearm)

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
 * Interface version 1 has no access control: whoever can open the node and
 * holds a handle can sign with it. The node is created 0600; distributions
 * may grant a group. Every 'flags' and 'reserved' field must be zero; later
 * versions report what they accept in SEP_KEY_GET_INFO.
 *
 * Errors: EINVAL for a malformed request or handle, EKEYREJECTED when the
 * enclave refuses the key (for example a handle from another Mac or an
 * earlier Linux install), ENODEV/ETIMEDOUT while the key store is not up,
 * EIO for any other enclave failure.
 */
#define SEP_KEY_IFACE_VERSION 1
#define SEP_KEY_HANDLE_MAX 2048
#define SEP_KEY_PUB_LEN    65  /* uncompressed point, 04 || X || Y */
#define SEP_KEY_DIGEST_LEN 32
#define SEP_KEY_SIG_MAX    72  /* DER SEQUENCE { INTEGER r, INTEGER s } */

struct sep_key_info {
	__u32 version;          /* SEP_KEY_IFACE_VERSION */
	__u32 flags_supported;  /* 0 in version 1 */
	__u32 handle_max;       /* SEP_KEY_HANDLE_MAX */
	__u32 reserved;
};

struct sep_key_create {
	__u32 flags;                            /* in: 0 */
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

#endif /* _UAPI_LINUX_APPLE_SEP_H */
