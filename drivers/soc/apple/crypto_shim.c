/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Dj */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/scatterlist.h>
#include <linux/crypto.h>
#include <linux/random.h>
#include <crypto/hash.h>
#include <crypto/sha2.h>
#include <crypto/aead.h>
#include <crypto/aes.h>

#include "shim.h"

#define SEP_GCM_TAG_LEN 16

int sep_random_bytes(void *buf, size_t len)
{
	int ret = wait_for_random_bytes();

	if (ret)
		return ret;
	get_random_bytes(buf, len);
	return 0;
}

/*
 * AES-GCM with a 16-byte (non-96-bit) IV, over the raw AES block cipher. The
 * kernel's gcm(aes) only takes a 12-byte IV; the SEP's ECIES uses a 16-byte IV,
 * whose J0 must be derived as GHASH_H(IV ‖ len-block), not IV‖0x00000001.
 */

/* GF(2^128) multiply, GCM bit convention (big-endian block, R = 0xe1‖0^120). */
static void sep_gf_mult(const u8 *X, const u8 *Y, u8 *out)
{
	u8 Z[16] = {0};
	u8 V[16];
	int i, j;

	memcpy(V, Y, 16);
	for (i = 0; i < 128; i++) {
		if ((X[i >> 3] >> (7 - (i & 7))) & 1)
			for (j = 0; j < 16; j++)
				Z[j] ^= V[j];
		{
			int lsb = V[15] & 1;

			for (j = 15; j > 0; j--)
				V[j] = (V[j] >> 1) | ((V[j - 1] & 1) << 7);
			V[0] >>= 1;
			if (lsb)
				V[0] ^= 0xe1;
		}
	}
	memcpy(out, Z, 16);
}

/* GHASH: fold `len` bytes of `data` (zero-padded to 16) into accumulator Y. */
static void sep_ghash(const u8 H[16], u8 Y[16], const u8 *data, size_t len)
{
	u8 blk[16], t[16];
	size_t i, n, j;

	for (i = 0; i < len; i += 16) {
		n = (len - i >= 16) ? 16 : (len - i);
		memset(blk, 0, 16);
		memcpy(blk, data + i, n);
		for (j = 0; j < 16; j++)
			Y[j] ^= blk[j];
		sep_gf_mult(Y, H, t);
		memcpy(Y, t, 16);
	}
}

/* Increment the low 32 bits (big-endian) of a counter block, in place. */
static void sep_inc32(u8 cb[16])
{
	u32 c = ((u32)cb[12] << 24) | ((u32)cb[13] << 16) |
		((u32)cb[14] << 8) | (u32)cb[15];
	c++;
	cb[12] = c >> 24;
	cb[13] = c >> 16;
	cb[14] = c >> 8;
	cb[15] = c;
}

/*
 * AES-GCM in place over `buf` = [aadlen AAD][datalen payload][16 tag], 16-byte
 * IV, keylen 16 or 32. Returns 0, or -EBADMSG on a decrypt tag mismatch.
 */
static int sep_gcm16(int encrypt, const void *key, size_t keylen,
			    const u8 *iv, size_t aadlen, u8 *buf, size_t datalen)
{
	struct aes_enckey aes;
	u8 H[16], J0[16], EJ0[16], S[16], cb[16], ks[16], lb[16], zero[16];
	u8 *ct;
	u64 aadbits, ctbits;
	size_t i, n, j;
	int rc;

	rc = aes_prepareenckey(&aes, key, keylen);
	if (rc)
		return rc;

	/* H = AES_K(0^128) */
	memset(zero, 0, 16);
	aes_encrypt(&aes, H, zero);

	/* J0 = GHASH_H(IV ‖ [0^64 ‖ len(IV)_64]) for a non-96-bit IV. */
	memset(J0, 0, 16);
	sep_ghash(H, J0, iv, 16);
	memset(lb, 0, 16);
	lb[15] = 0x80; /* 128 bits, big-endian in the low 64 bits */
	for (j = 0; j < 16; j++)
		J0[j] ^= lb[j];
	{
		u8 t[16];

		sep_gf_mult(J0, H, t);
		memcpy(J0, t, 16);
	}
	aes_encrypt(&aes, EJ0, J0);

	ct = buf + aadlen;

	/* GHASH over AAD, then ciphertext (before CTR on decrypt). */
	memset(S, 0, 16);
	sep_ghash(H, S, buf, aadlen);
	if (!encrypt)
		sep_ghash(H, S, ct, datalen);

	/* CTR keystream from inc32(J0). */
	memcpy(cb, J0, 16);
	sep_inc32(cb);
	for (i = 0; i < datalen; i += 16) {
		aes_encrypt(&aes, ks, cb);
		n = (datalen - i >= 16) ? 16 : (datalen - i);
		for (j = 0; j < n; j++)
			ct[i + j] ^= ks[j];
		sep_inc32(cb);
	}

	if (encrypt)
		sep_ghash(H, S, ct, datalen);

	/* lengths block: (aadbits)_64 ‖ (ctbits)_64, big-endian. */
	aadbits = (u64)aadlen * 8;
	ctbits = (u64)datalen * 8;
	memset(lb, 0, 16);
	for (j = 0; j < 8; j++)
		lb[7 - j] = (u8)(aadbits >> (8 * j));
	for (j = 0; j < 8; j++)
		lb[15 - j] = (u8)(ctbits >> (8 * j));
	for (j = 0; j < 16; j++)
		S[j] ^= lb[j];
	{
		u8 t[16];

		sep_gf_mult(S, H, t);
		memcpy(S, t, 16);
	}
	for (j = 0; j < 16; j++)
		S[j] ^= EJ0[j]; /* S is now the computed tag */

	if (encrypt) {
		memcpy(ct + datalen, S, 16);
		rc = 0;
	} else {
		u8 diff = 0;

		for (j = 0; j < 16; j++)
			diff |= S[j] ^ ct[datalen + j];
		rc = diff ? -EBADMSG : 0;
	}

	memzero_explicit(ks, sizeof(ks));
	memzero_explicit(EJ0, sizeof(EJ0));
	memzero_explicit(H, sizeof(H));
	memzero_explicit(&aes, sizeof(aes));
	return rc;
}

/* HMAC-SHA256 over one message; writes 32 bytes to `out`, untouched on error. */
int sep_hmac_sha256(const void *key, size_t keylen,
			   const void *data, size_t datalen, u8 *out)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	int rc;

	tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	rc = crypto_shash_setkey(tfm, key, keylen);
	if (rc)
		goto out_tfm;

	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		rc = -ENOMEM;
		goto out_tfm;
	}
	desc->tfm = tfm;

	rc = crypto_shash_digest(desc, data, datalen, out);

	kfree_sensitive(desc);
out_tfm:
	crypto_free_shash(tfm);
	return rc;
}

/*
 * AES-256-GCM in place over `buf` = [aadlen AAD][payload][tag]. Encrypt appends
 * the tag; decrypt expects it and leaves plaintext in the first `datalen` bytes
 * after the AAD. keylen 16 or 32, ivlen 12 or 16 (a 16-byte IV is routed to the
 * GHASH J0 derivation, not truncated).
 */
int sep_gcm(int encrypt, const void *key, size_t keylen,
		   const void *iv, size_t ivlen, size_t aadlen,
		   void *buf, size_t buflen, size_t datalen)
{
	struct crypto_aead *tfm;
	struct aead_request *req;
	struct scatterlist sg;
	DECLARE_CRYPTO_WAIT(wait);
	size_t cryptlen;
	u8 ivcopy[16];
	int rc;

	if ((keylen != 16 && keylen != 32) ||
	    (ivlen != 12 && ivlen != 16) || ivlen > sizeof(ivcopy))
		return -EINVAL;

	if (aadlen + datalen + SEP_GCM_TAG_LEN < aadlen ||
	    aadlen + datalen + SEP_GCM_TAG_LEN > buflen)
		return -EINVAL;

	/* A 16-byte IV needs the GHASH J0 derivation; gcm(aes) would silently
	 * use only its first 12 bytes. */
	if (ivlen == 16)
		return sep_gcm16(encrypt, key, keylen, iv, aadlen, buf,
					datalen);

	tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	rc = crypto_aead_setkey(tfm, key, keylen);
	if (rc)
		goto out_tfm;
	rc = crypto_aead_setauthsize(tfm, SEP_GCM_TAG_LEN);
	if (rc)
		goto out_tfm;

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		rc = -ENOMEM;
		goto out_tfm;
	}

	/* The API may modify the IV buffer, so it never sees the caller's. */
	memcpy(ivcopy, iv, ivlen);

	sg_init_one(&sg, buf, aadlen + datalen + SEP_GCM_TAG_LEN);

	/* AEAD API quirk: cryptlen includes the tag on decrypt but not on encrypt. */
	cryptlen = encrypt ? datalen : datalen + SEP_GCM_TAG_LEN;

	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				  crypto_req_done, &wait);
	aead_request_set_ad(req, aadlen);
	aead_request_set_crypt(req, &sg, &sg, cryptlen, ivcopy);

	rc = crypto_wait_req(encrypt ? crypto_aead_encrypt(req)
				     : crypto_aead_decrypt(req), &wait);

	aead_request_free(req);
	memzero_explicit(ivcopy, sizeof(ivcopy));
out_tfm:
	crypto_free_aead(tfm);
	return rc;
}
