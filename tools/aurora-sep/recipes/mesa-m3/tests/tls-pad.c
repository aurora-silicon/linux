/* Copyright 2026 Chris Kearney
 * SPDX-License-Identifier: MIT */
static __thread char pad[128] __attribute__((tls_model("initial-exec")));
int use_pad(void) { return ++pad[0]; }
