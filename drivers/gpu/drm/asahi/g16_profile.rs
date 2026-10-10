// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Exact J613 firmware selection and calibrated performance-table construction.

pub(crate) const COMPAT: [u32; 3] = [26, 6, 2];

pub(crate) fn admit(j613: bool, t8122: bool, compat: &[u32], marker: u32, intent: u32) -> bool {
    j613 && t8122 && compat == COMPAT && marker == 1 && intent == 1
}

/// A /chosen switch from m1n1.conf: m1n1 writes `chosen.<name>=1` as the
/// string "1" (two bytes); a big-endian u32 1 is accepted as well.
pub(crate) fn chosen_switch_on(value: Option<&[u8]>) -> bool {
    matches!(value, Some(b"1\0") | Some([0, 0, 0, 1]))
}

#[cfg(not(test))]
pub(crate) fn selected(pdev: &kernel::platform::Device<kernel::device::Core>) -> kernel::error::Result<bool> {
    use kernel::{bindings, c_str, prelude::*};
    let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
    let compat = node.get_property::<KVec<u32>>(c_str!("apple,firmware-compat"))?;
    if compat.as_slice() != COMPAT { return Ok(false); }
    let root = kernel::of::root().ok_or(ENODEV)?;
    let names = root.get_property::<KVec<u8>>(c_str!("compatible"))?;
    let j613 = names.split(|b| *b == 0).any(|s| s == b"apple,j613");
    let t8122 = names.split(|b| *b == 0).any(|s| s == b"apple,t8122");
    // SAFETY: the static path is NUL terminated; a successful lookup owns one reference.
    let chosen = unsafe { bindings::of_find_node_opts_by_path(c_str!("/chosen").as_char_ptr(), core::ptr::null_mut()) };
    if chosen.is_null() { return Err(ENODEV); }
    let mut length = 0;
    // SAFETY: the owned reference keeps the property alive through the read below.
    let raw = unsafe { bindings::of_get_property(chosen, c_str!("asahi,t8122-gpu").as_char_ptr(), &mut length) };
    // m1n1 sets chosen.asahi,t8122-gpu=1 from m1n1.conf as the string "1".
    let value = if raw.is_null() || !(0..=4).contains(&length) { None } else {
        // SAFETY: the property has `length` (at most four) bytes, checked above.
        Some(unsafe { core::slice::from_raw_parts(raw.cast::<u8>(), length as usize) })
    };
    let intent = u32::from(chosen_switch_on(value));
    // SAFETY: releases the reference returned by the lookup.
    unsafe { bindings::of_node_put(chosen) };
    let marker = node.get_property::<u32>(c_str!("apple,j613-25g83-gpu-handoff")).unwrap_or(0);

    if !admit(j613, t8122, &compat, marker, intent) { return Err(ENODEV); }
    Ok(true)
}

/// Only a stopped ASC with proven GPU retirement may release firmware backing.
pub(crate) fn may_release_runtime(gpu_pending: bool, asc_stopped: bool) -> bool {
    !gpu_pending && asc_stopped
}

/// Positive acknowledgement of the command ABI, in addition to GET_PARAMS coverage.
pub(crate) fn client_admitted(hal: u32, flags: u32, covered: bool) -> bool {
    covered && flags == if hal == 200 { 1 } else { 0 }
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Tables {
    pub(crate) count: usize,
    pub(crate) primary: [u32; 16],
    pub(crate) secondary: [u32; 16],
    pub(crate) core_mv: [u32; 16],
    pub(crate) sram_mv: [u32; 16],
    pub(crate) power_mw: [u32; 16],
    pub(crate) primary_indices: [u32; 16],
    pub(crate) secondary_indices: [u32; 16],
    pub(crate) secondary_sram_mv: [u32; 16],
}

/// One entry per voltage, ascending; frequencies are the exact high/low ADT values.
/// The off state is first. SRAM is the voltage required by every point in its group.
pub(crate) fn performance_tables(points: &[(u64, u32, u32, u32)]) -> Option<Tables> {
    if points.len() < 2 || points.len() > 255 || points[0].0 != 0 { return None; }
    let mut t = Tables { count: 1, primary: [0; 16], secondary: [0; 16],
        core_mv: [0; 16], sram_mv: [0; 16], power_mw: [0; 16], primary_indices: [0; 16], secondary_indices: [0; 16], secondary_sram_mv: [0; 16] };
    t.core_mv[0] = points[0].1.div_ceil(1000);
    t.sram_mv[0] = points[0].2.div_ceil(1000);
    t.secondary_sram_mv[0] = t.sram_mv[0];
    let mut previous_mv = 0;
    loop {
        let next = points[1..].iter().map(|p| p.1.div_ceil(1000))
            .filter(|v| *v > previous_mv).min();
        let Some(mv) = next else { break; };
        if t.count == 16 { return None; }
        let i = t.count;
        let mut low = u64::MAX;
        let mut high = 0;
        for (point, &(hz, core, sram, power)) in points.iter().enumerate().skip(1) {
            if hz < 1_000_000 || hz > u64::from(u32::MAX) || core == 0 || sram == 0 || power == 0 {
                return None;
            }
            if core.div_ceil(1000) != mv { continue; }
            if hz > high { high = hz; t.primary_indices[i] = point as u32; t.sram_mv[i] = sram.div_ceil(1000); }
            if hz < low { low = hz; t.secondary_indices[i] = point as u32; t.secondary_sram_mv[i] = sram.div_ceil(1000); }
            t.power_mw[i] = t.power_mw[i].max(power.div_ceil(1000));
        }
        if high / 1_000_000 <= u64::from(t.primary[i-1])
            || low / 1_000_000 <= u64::from(t.secondary[i-1]) { return None; }
        t.primary[i] = (high / 1_000_000) as u32;
        t.secondary[i] = (low / 1_000_000) as u32;
        t.core_mv[i] = mv;
        t.count += 1; previous_mv = mv;
    }
    (t.count > 1).then_some(t)
}

/// Publish every calibrated Main table, including the secondary voltage rows and index maps.
pub(crate) fn calibration_words<E>(tables: &Tables, sram_k: u32, base: usize,
    mut write: impl FnMut(usize, u32) -> Result<(), E>) -> Result<(), E> {
    let top = tables.count - 1;
    let max_power = tables.power_mw[..tables.count].iter().copied().max().unwrap_or(1).max(1);
    let base_hz = tables.primary[base];
    let span = tables.primary[top].saturating_sub(base_hz);
    write(0x1cd8, top as u32)?;
    for state in 0..tables.count {
        for (offset, value) in [
            (0xfc8, tables.primary[state]), (0x1808, tables.secondary[state]),
            (0x1848, sram_k), (0x19c8, tables.primary_indices[state]),
            (0x1a08, tables.secondary_indices[state]), (0x1cdc, tables.secondary[state]),
            (0x18c8, (u64::from(tables.power_mw[state]) * 100 / u64::from(max_power)) as u32),
            (0x1908, if span == 0 { 0 } else { tables.primary[state].saturating_sub(base_hz) * 100 / span }),
        ] { write(offset + state * 4, value)?; }
        for core in 0..16 {
            for (offset, value) in [(0x1008, tables.core_mv[state]), (0x1408, tables.sram_mv[state]),
                (0x1d1c, tables.core_mv[state]), (0x211c, tables.secondary_sram_mv[state])] {
                write(offset + state * 64 + core * 4, value)?;
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn failed_stop_or_unproven_retirement_retains_firmware_ownership() {
        assert!(may_release_runtime(false,true));
        assert!(!may_release_runtime(false,false));
        assert!(!may_release_runtime(true,true));
        assert!(!may_release_runtime(true,false));
    }
    #[test]
    fn client_acknowledgement_is_profile_specific() {
        assert!(client_admitted(200,1,true));
        assert!(!client_admitted(200,0,true));
        assert!(!client_admitted(200,1,false));
        assert!(!client_admitted(200,3,true));
        assert!(!client_admitted(200,2,true));
        assert!(client_admitted(0,0,true));
        assert!(!client_admitted(0,1,true));
    }
    #[test]
    fn exact_profile_requires_each_independent_input() {
        assert!(admit(true,true,&COMPAT,1,1));
        for c in [&[14,8,3][..], &[26,6,1], &[26,6,3], &[26,6,2,0]] {
            assert!(!admit(true,true,c,1,1));
        }
        assert!(!admit(false,true,&COMPAT,1,1));
        assert!(!admit(true,false,&COMPAT,1,1));
        assert!(!admit(true,true,&COMPAT,0,1));
        assert!(!admit(true,true,&COMPAT,1,0));
        assert!(!admit(true,true,&COMPAT,2,1));
    }
    #[test]
    fn chosen_switch_accepts_m1n1_string_and_u32() {
        assert!(chosen_switch_on(Some(b"1\0")));
        assert!(chosen_switch_on(Some(&[0,0,0,1])));
        for v in [&b""[..], b"1", b"0\0", b"11\0", &[0,0,0,2], &[1,0,0,0]] {
            assert!(!chosen_switch_on(Some(v)), "{v:?}");
        }
        assert!(!chosen_switch_on(None));
    }
    #[test]
    fn uses_this_macs_voltage_sram_frequency_and_power() {
        let a = performance_tables(&[(0,125000,700000,0),
            (600000000,700001,810000,9000000), (300000000,650000,750001,5000000),
            (500000000,700001,820000,8000000)]).unwrap();
        assert_eq!(a.count,3);
        assert_eq!(&a.primary[..3], &[0,300,600]);
        assert_eq!(&a.secondary[..3], &[0,300,500]);
        assert_eq!(&a.core_mv[..3], &[125,650,701]);
        assert_eq!(&a.sram_mv[..3], &[700,751,810]);
        assert_eq!(&a.power_mw[..3], &[0,5000,9000]);
        let b = performance_tables(&[(0,125000,720000,0), (300000000,660000,760000,6000000)]).unwrap();
        assert_ne!(a.core_mv[1],b.core_mv[1]);
        assert_ne!(a.power_mw[1],b.power_mw[1]);
    }
    #[test]
    fn two_macs_publish_distinct_calibration_bytes_in_every_table() {
        let a = performance_tables(&[(0,125000,700000,0),(300000000,650000,750000,4000000),
            (500000000,700000,800000,6000000),(600000000,700000,810000,9000000)]).unwrap();
        let b = performance_tables(&[(0,125000,720000,0),(310000000,660000,760000,5000000),
            (510000000,710000,820000,7000000),(620000000,710000,830000,12000000)]).unwrap();
        let encode = |t: &Tables| {
            let mut bytes = [0u8; 0x2540];
            calibration_words::<()>(t, 0x3f828f5c, 1, |offset, value| {
                bytes[offset..offset+4].copy_from_slice(&value.to_le_bytes()); Ok(())
            }).unwrap(); bytes
        };
        let aa = encode(&a); let bb = encode(&b);
        for offset in [0xfcc,0x1048,0x1448,0x180c,0x18cc,0x1ce0,0x1d5c,0x215c] {
            assert_ne!(&aa[offset..offset+4], &bb[offset..offset+4], "offset {offset:x}");
        }
        assert_eq!(u32::from_le_bytes(aa[0x1910..0x1914].try_into().unwrap()),100);
        assert_eq!(u32::from_le_bytes(aa[0x19d0..0x19d4].try_into().unwrap()),3);
        assert_eq!(u32::from_le_bytes(aa[0x1a10..0x1a14].try_into().unwrap()),2);
        assert_eq!(u32::from_le_bytes(aa[0x1cd8..0x1cdc].try_into().unwrap()),2);
        assert_eq!(u32::from_le_bytes(aa[0x229c..0x22a0].try_into().unwrap()),0);
    }
    #[test]
    fn rejects_unusable_calibration_without_fallback() {
        assert!(performance_tables(&[(0,125000,700000,0),(300000000,650000,750000,0)]).is_none());
        assert!(performance_tables(&[(0,125000,700000,0),(300000000,650000,0,5000000)]).is_none());
        assert!(performance_tables(&[(0,125000,700000,0),(300000000,650000,750000,5000000),
            (200000000,700000,800000,6000000)]).is_none());
        let mut p = [(0,125000,700000,0); 17];
        for i in 1..17 { p[i]=(i as u64*100000000,600000+i as u32*10000,800000,1000000); }
        assert!(performance_tables(&p).is_none());
    }
}
