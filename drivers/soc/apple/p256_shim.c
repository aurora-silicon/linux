// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Dj */
/*
 * Apple SEP driver — P-256 ECDH sender shim for the ref-key ECIES seal.
 *
 * The ref-key (op 0x22) SE-seal is an ECIES scheme: the AP encrypts to the
 * ref-key's public point and the enclave decrypts (op "od"). The enclave holds
 * the recipient private key and never exposes it; the encrypt half touches only
 * the public key, so it runs here in the kernel with no secret crossing into
 * the SEP request but the (public) ephemeral point. This shim does the sender's
 * key agreement: generate an ephemeral P-256 keypair, return its public point
 * for the envelope, and compute the ECDH shared secret against the recipient's.
 *
 * Byte order: the kernel's `ecdh-nist-p256` kpp is big-endian throughout
 * (ecdh_set_secret decodes the key via ecc_digits_from_bytes; ecc_swap_digits
 * reads/writes coordinates as __be64). So the private key, both public points
 * (X||Y), and the shared secret are plain big-endian byte strings, matching the
 * ECIES/SEP convention: this shim does no byte swapping. It only marshals
 * through kmalloc'd buffers, since a scatterlist entry must live in the linear
 * map and the caller's may not.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/scatterlist.h>
#include <crypto/kpp.h>
#include <crypto/ecdh.h>
#include <crypto/internal/ecc.h>

#include "shim.h"

#define P256_COORD_LEN 32
#define P256_POINT_LEN 64 /* X || Y, big-endian, no 0x04 prefix */

/*
 * Core key agreement. `priv_be` is a 32-byte big-endian private scalar, or NULL
 * to generate a fresh ephemeral key. `pub_be_out` receives the 64-byte
 * big-endian public point (X||Y), `shared_be_out` the 32-byte big-endian shared
 * secret (X coordinate of priv*peer). Scatterlist buffers are kmalloc'd since
 * sg_init_one requires the linear map.
 */
static int p256_agree(const u8 *priv_be, const u8 *peer_pub_be,
		      u8 *pub_be_out, u8 *shared_be_out)
{
	struct crypto_kpp *tfm;
	struct kpp_request *req;
	struct ecdh params;
	struct scatterlist src, dst;
	DECLARE_CRYPTO_WAIT(wait);
	unsigned int enc_len;
	char *enc = NULL;
	u8 *pub_buf = NULL;
	u8 *peer_buf = NULL;
	u8 *shared_buf = NULL;
	int rc;

	tfm = crypto_alloc_kpp("ecdh-nist-p256", 0, 0);
	if (IS_ERR(tfm)) {
		rc = PTR_ERR(tfm);
		pr_err("apple_sep p256: alloc_kpp rc=%d\n", rc);
		return rc;
	}

	memset(&params, 0, sizeof(params));
	if (priv_be) {
		params.key = (void *)priv_be; /* big-endian, as the kpp wants */
		params.key_size = P256_COORD_LEN;
	} else {
		params.key = NULL;
		params.key_size = 0; /* kpp generates a valid random scalar */
	}

	enc_len = crypto_ecdh_key_len(&params);
	enc = kmalloc(enc_len, GFP_KERNEL);
	if (!enc) {
		rc = -ENOMEM;
		goto out_tfm;
	}
	rc = crypto_ecdh_encode_key(enc, enc_len, &params);
	if (rc) {
		pr_err("apple_sep p256: encode_key rc=%d\n", rc);
		goto out_enc;
	}
	rc = crypto_kpp_set_secret(tfm, enc, enc_len);
	if (rc) {
		pr_err("apple_sep p256: set_secret rc=%d\n", rc);
		goto out_enc;
	}

	req = kpp_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		rc = -ENOMEM;
		goto out_enc;
	}

	pub_buf = kmalloc(P256_POINT_LEN, GFP_KERNEL);
	if (!pub_buf) {
		rc = -ENOMEM;
		goto out_req;
	}
	sg_init_one(&dst, pub_buf, P256_POINT_LEN);
	kpp_request_set_input(req, NULL, 0);
	kpp_request_set_output(req, &dst, P256_POINT_LEN);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 crypto_req_done, &wait);
	rc = crypto_wait_req(crypto_kpp_generate_public_key(req), &wait);
	if (rc) {
		pr_err("apple_sep p256: generate_public_key rc=%d\n", rc);
		goto out_pub;
	}
	memcpy(pub_be_out, pub_buf, P256_POINT_LEN);

	peer_buf = kmalloc(P256_POINT_LEN, GFP_KERNEL);
	shared_buf = kmalloc(P256_COORD_LEN, GFP_KERNEL);
	if (!peer_buf || !shared_buf) {
		rc = -ENOMEM;
		goto out_pub;
	}
	memcpy(peer_buf, peer_pub_be, P256_POINT_LEN);
	sg_init_one(&src, peer_buf, P256_POINT_LEN);
	sg_init_one(&dst, shared_buf, P256_COORD_LEN);
	kpp_request_set_input(req, &src, P256_POINT_LEN);
	kpp_request_set_output(req, &dst, P256_COORD_LEN);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 crypto_req_done, &wait);
	rc = crypto_wait_req(crypto_kpp_compute_shared_secret(req), &wait);
	if (rc) {
		pr_err("apple_sep p256: compute_shared_secret rc=%d\n", rc);
		goto out_pub;
	}
	memcpy(shared_be_out, shared_buf, P256_COORD_LEN);
	rc = 0;

out_pub:
	if (shared_buf) {
		memzero_explicit(shared_buf, P256_COORD_LEN);
		kfree(shared_buf);
	}
	kfree(peer_buf);
	kfree(pub_buf);
out_req:
	kpp_request_free(req);
out_enc:
	kfree_sensitive(enc);
out_tfm:
	crypto_free_kpp(tfm);
	return rc;
}

/*
 * ECIES sender key agreement with a fresh ephemeral key. `peer_pub_be` is the
 * recipient's 64-byte big-endian public point (0x04 prefix already stripped by
 * the caller). `eph_pub_be_out` receives the 64-byte big-endian ephemeral
 * public point (caller prepends 0x04 for the envelope), `shared_be_out` the
 * 32-byte big-endian shared secret for the KDF.
 */
int sep_p256_sender(const void *peer_pub_be, void *eph_pub_be_out,
			   void *shared_be_out)
{
	return p256_agree(NULL, peer_pub_be, eph_pub_be_out, shared_be_out);
}

