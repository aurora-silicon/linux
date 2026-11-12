/* SPDX-License-Identifier: GPL-2.0 */
/* A defined GNU C empty record still has alignment, despite having no storage. */
struct rust_bindgen_aligned_empty {} __attribute__((aligned(64)));
