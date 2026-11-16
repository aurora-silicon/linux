// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! TEST ONLY, not for merge: the uapi items of the VM fence-error count (include/uapi/drm/asahi_drm.h) for an
//! external module build against a kernel whose generated `uapi` crate predates them (the Neo's default kit).
//! Same names, types and values as the header's bindings.

/// `DRM_ASAHI_FEATURE_VM_FENCE_ERROR_COUNT`: `(1ULL) << 39`.
pub(crate) const DRM_ASAHI_FEATURE_VM_FENCE_ERROR_COUNT: u64 = 1 << 39;
/// `DRM_ASAHI_PARAM_GROUP_VM_FENCE_ERROR_MIRROR`.
pub(crate) const DRM_ASAHI_PARAM_GROUP_VM_FENCE_ERROR_MIRROR: u32 = 4;

/// `struct drm_asahi_vm_fence_error_mirror`.
#[repr(C)]
#[allow(non_camel_case_types, dead_code)]
pub(crate) struct drm_asahi_vm_fence_error_mirror {
    pub(crate) vm_id: u32,
    pub(crate) handle: u32,
    pub(crate) offset: u64,
    pub(crate) flags: u64,
}
