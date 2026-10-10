// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#include <drm/drm_modes.h>
#include <drm/drm_rect.h>

#include "dcp-internal.h"

struct neo_apple_dcp;

typedef void (*neo_dcp_callback_t)(struct neo_apple_dcp *, void *, void *);


#define DCP_THUNK_VOID(func, handle)                                         \
	static void func(struct neo_apple_dcp *neo_dcp, bool oob, neo_dcp_callback_t cb, \
			 void *cookie)                                       \
	{                                                                    \
		neo_dcp_push(neo_dcp, oob, &neo_dcp_methods[handle], 0, 0, NULL, cb, cookie);          \
	}

#define DCP_THUNK_OUT(func, handle, T)                                       \
	static void func(struct neo_apple_dcp *neo_dcp, bool oob, neo_dcp_callback_t cb, \
			 void *cookie)                                       \
	{                                                                    \
		neo_dcp_push(neo_dcp, oob, &neo_dcp_methods[handle], 0, sizeof(T), NULL, cb, cookie);  \
	}

#define DCP_THUNK_IN(func, handle, T)                                       \
	static void func(struct neo_apple_dcp *neo_dcp, bool oob, T *data,          \
			 neo_dcp_callback_t cb, void *cookie)                   \
	{                                                                   \
		neo_dcp_push(neo_dcp, oob, &neo_dcp_methods[handle], sizeof(T), 0, data, cb, cookie); \
	}

#define DCP_THUNK_INOUT(func, handle, T_in, T_out)                            \
	static void func(struct neo_apple_dcp *neo_dcp, bool oob, T_in *data,         \
			 neo_dcp_callback_t cb, void *cookie)                     \
	{                                                                     \
		neo_dcp_push(neo_dcp, oob, &neo_dcp_methods[handle], sizeof(T_in), sizeof(T_out), data, \
			 cb, cookie);                                         \
	}

#define IOMFB_THUNK_INOUT(name)                                     \
	static void neo_iomfb_ ## name(struct neo_apple_dcp *neo_dcp, bool oob, \
			struct neo_iomfb_ ## name ## _req *data,        \
			neo_dcp_callback_t cb, void *cookie)            \
	{                                                           \
		neo_dcp_push(neo_dcp, oob, &neo_dcp_methods[iomfbep_ ## name],                \
			 sizeof(struct neo_iomfb_ ## name ## _req),     \
			 sizeof(struct neo_iomfb_ ## name ## _resp),    \
			 data,  cb, cookie);                        \
	}

/*
 * Define type-safe trampolines. Define typedefs to enforce type-safety on the
 * input data (so if the types don't match, gcc errors out).
 */

#define TRAMPOLINE_VOID(func, handler)                                        \
	static bool __maybe_unused func(struct neo_apple_dcp *neo_dcp, int tag, void *out, void *in) \
	{                                                                     \
		trace_neo_iomfb_callback(neo_dcp, tag, #handler);                     \
		handler(neo_dcp);                                                 \
		return true;                                                  \
	}

#define TRAMPOLINE_IN(func, handler, T_in)                                    \
	typedef void (*callback_##handler)(struct neo_apple_dcp *, T_in *);       \
                                                                              \
	static bool __maybe_unused func(struct neo_apple_dcp *neo_dcp, int tag, void *out, void *in) \
	{                                                                     \
		callback_##handler cb = handler;                              \
                                                                              \
		trace_neo_iomfb_callback(neo_dcp, tag, #handler);                     \
		cb(neo_dcp, in);                                                  \
		return true;                                                  \
	}

#define TRAMPOLINE_INOUT(func, handler, T_in, T_out)                          \
	typedef T_out (*callback_##handler)(struct neo_apple_dcp *, T_in *);      \
                                                                              \
	static bool __maybe_unused func(struct neo_apple_dcp *neo_dcp, int tag, void *out, void *in) \
	{                                                                     \
		T_out *typed_out = out;                                       \
		callback_##handler cb = handler;                              \
                                                                              \
		trace_neo_iomfb_callback(neo_dcp, tag, #handler);                     \
		*typed_out = cb(neo_dcp, in);                                     \
		return true;                                                  \
	}

#define TRAMPOLINE_OUT(func, handler, T_out)                                  \
	static bool __maybe_unused func(struct neo_apple_dcp *neo_dcp, int tag, void *out, void *in) \
	{                                                                     \
		T_out *typed_out = out;                                       \
                                                                              \
		trace_neo_iomfb_callback(neo_dcp, tag, #handler);                     \
		*typed_out = handler(neo_dcp);                                    \
		return true;                                                  \
	}

/* Call a DCP function given by a tag */
void neo_dcp_push(struct neo_apple_dcp *neo_dcp, bool oob, const struct neo_dcp_method_entry *call,
		     u32 in_len, u32 out_len, void *data, neo_dcp_callback_t cb,
		     void *cookie);

/* Parse a callback tag "D123" into the ID 123. Returns -EINVAL on failure. */
int neo_dcp_parse_tag(char tag[4]);

void neo_dcp_ack(struct neo_apple_dcp *neo_dcp, enum neo_dcp_context_id context);

/* The user may own drm_display_mode, so we need to search for our copy */
struct neo_dcp_display_mode *neo_lookup_mode(struct neo_apple_dcp *neo_dcp,
					    const struct drm_display_mode *mode);
