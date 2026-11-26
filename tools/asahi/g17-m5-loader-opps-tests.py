#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for the M5 HW data's loader-filled GPU voltages.

Extracts production `M5PerfRows`, `LoaderOpp`, `LoaderOppError` and their helpers from
`drivers/gpu/drm/asahi/g17/fw/m5/initdata.rs`, with the J714 tables, and checks:
1. The DT's zero placeholders are `NotFilled`: the board's tables stay.
2. The J714 table handed over as operating points gives the J714 rows unchanged.
3. The K116s 27.2 ADT values (perf-states, perf-states1, perf-states-sram, -sram1,
   afr-perf-states, min SRAM 775 mV) give exactly the K116s rows the driver used to carry
   as a board table: 13 main and 5 CS/AFR states differ from the J714's.
4. A frequency other than the board's, a voltage far from it or not whole mV, a wrong
   state or cell count, missing CS/AFR points on a board with those domains, and an SRAM
   floor that puts a CS/AFR SRAM voltage far from the board's (zero, or too high) are refused.
5. An AFR core voltage under the SRAM floor gets the floor as its SRAM voltage.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'drivers/gpu/drm/asahi/g17/fw/m5/initdata.rs'

MUTATIONS = ('no-state-check', 'no-slack', 'no-whole-mv', 'no-floor', 'no-floor-check',
             'aux-optional', 'placeholder-taken')


def item(source, signature):
    """The item at `signature`, through its closing brace."""
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def decl(source, signature):
    """The declaration at `signature`, through its `;` outside brackets."""
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    depth, end = 0, start
    while source[end] != ';' or depth:
        depth += (source[end] in '([{') - (source[end] in ')]}')
        end += 1
    return source[start:end + 1]


def build_harness(mutation=None):
    src = SRC.read_text()
    parts = [
        decl(src, 'pub(crate) type M5PerfState '),
        decl(src, 'pub(crate) const M5_PERF_LANES'),
        decl(src, 'pub(crate) type M5AuxPerfState '),
        item(src, 'const fn two_tables('),
        item(src, 'const fn one_lane('),
        decl(src, 'pub(crate) const J714_GPU_NUM_PERF_STATES'),
        decl(src, 'pub(crate) const J714_GPU_PERF_STATES'),
        decl(src, 'const J714_AUX_PERF_STATES'),
        item(src, 'pub(crate) struct M5PerfRows'),
        item(src, 'pub(crate) struct LoaderOpp '),
        item(src, 'pub(crate) enum LoaderOppError'),
        decl(src, 'const LOADER_VOLTAGE_SLACK_MV'),
        item(src, 'fn loader_mv('),
        item(src, 'fn loader_opps_unfilled('),
        item(src, 'impl M5PerfRows'),
    ]
    code = '\n'.join(parts).replace('pub(crate) ', '')
    # item() starts at the signature: the derives above each item are given here.
    for name, derives in (('struct M5PerfRows', 'Clone, Copy, Debug, PartialEq'),
                          ('struct LoaderOpp ', 'Clone, Copy, Default'),
                          ('enum LoaderOppError', 'Clone, Copy, Debug, PartialEq')):
        code = code.replace(name, f'#[derive({derives})]\n{name}', 1)

    def mutate(old, new):
        nonlocal code
        assert code.count(old) == 1, old
        code = code.replace(old, new)

    if mutation == 'no-state-check':
        mutate('if opp.hz != u64::from(row.0) * 1_000_000 {\n                return Err(LoaderOppError::State(state));\n            }\n            for table',
               'for table')
    elif mutation == 'no-slack':
        mutate('mv.abs_diff(board_mv) <= LOADER_VOLTAGE_SLACK_MV', 'board_mv == board_mv')
    elif mutation == 'no-whole-mv':
        mutate('(uv % 1000 == 0 && mv != 0', '(mv != 0')
    elif mutation == 'no-floor':
        mutate('let sram = core.max(floor);', 'let sram = core;')
    elif mutation == 'no-floor-check':
        mutate('if sram.abs_diff(row.2[lane]) > LOADER_VOLTAGE_SLACK_MV {', 'if false {')
    elif mutation == 'aux-optional':
        mutate('let (opps, floor_uv) = aux.ok_or(LoaderOppError::Shape)?;',
               'let Some((opps, floor_uv)) = aux else { return Ok(rows); };')
    elif mutation == 'placeholder-taken':
        mutate('if loader_opps_unfilled(main) {', 'if false {')

    return '''
#![allow(dead_code)]
struct Board {
    perf_state_count: u32,
    perf_states: &'static [M5PerfState; 14],
    perf_tables: usize,
    aux_perf_states: Option<&'static [M5AuxPerfState; 14]>,
}
''' + code + '''

const J714: Board = Board {
    perf_state_count: J714_GPU_NUM_PERF_STATES,
    perf_states: &J714_GPU_PERF_STATES,
    perf_tables: 2,
    aux_perf_states: Some(&J714_AUX_PERF_STATES),
};

// The K116s 27.2 ADT: perf-states / perf-states1 and perf-states-sram / -sram1 (mV).
const K116_MAIN: [(u32, u32, u32, u32, u32); 14] = [
    (0, 125, 125, 775, 775), (338, 595, 600, 775, 775),
    (486, 625, 640, 775, 775), (636, 660, 660, 775, 775),
    (796, 710, 710, 775, 775), (888, 720, 720, 775, 775),
    (988, 750, 750, 775, 775), (1084, 775, 775, 775, 775),
    (1182, 805, 805, 805, 805), (1278, 845, 845, 845, 845),
    (1374, 865, 870, 865, 870), (1470, 910, 915, 910, 915),
    (1578, 985, 985, 985, 985), (1620, 1025, 1025, 1025, 1025),
];
// Its afr-perf-states (cs-perf-states is the same), mV; min SRAM 775 mV.
const K116_AFR: [(u32, u32); 14] = [
    (24, 125), (338, 580), (486, 630), (636, 650), (796, 690), (888, 705), (988, 745),
    (1084, 770), (1182, 795), (1278, 835), (1374, 860), (1470, 900), (1578, 960), (1620, 990),
];
// The K116s rows the driver carried as a board table (MHz, core mV, SRAM mV).
const K116_GPU_PERF_STATES: [M5PerfState; 14] = two_tables(K116_MAIN);
const K116_AUX_PERF_STATES: [M5AuxPerfState; 14] = one_lane([
    (24, 125, 775), (338, 580, 775), (486, 630, 775), (636, 650, 775),
    (796, 690, 775), (888, 705, 775), (988, 745, 775), (1084, 770, 775),
    (1182, 795, 795), (1278, 835, 835), (1374, 860, 860),
    (1470, 900, 900), (1578, 960, 960), (1620, 990, 990),
]);

fn main_opps(rows: &[(u32, u32, u32, u32, u32); 14]) -> Vec<LoaderOpp> {
    rows.iter().map(|&(mhz, c0, c1, s0, s1)| {
        let mut opp = LoaderOpp { hz: mhz as u64 * 1_000_000, cells: 2, sram_cells: 2,
                                  ..Default::default() };
        opp.core_uv[..2].copy_from_slice(&[c0 * 1000, c1 * 1000]);
        opp.sram_uv[..2].copy_from_slice(&[s0 * 1000, s1 * 1000]);
        opp
    }).collect()
}

fn aux_opps(rows: &[(u32, u32); 14]) -> Vec<LoaderOpp> {
    rows.iter().map(|&(mhz, core)| {
        let mut opp = LoaderOpp { hz: mhz as u64 * 1_000_000, cells: 1, ..Default::default() };
        opp.core_uv[0] = core * 1000;
        opp
    }).collect()
}

fn j714_main() -> [(u32, u32, u32, u32, u32); 14] {
    let mut out = [(0, 0, 0, 0, 0); 14];
    for (i, s) in J714_GPU_PERF_STATES.iter().enumerate() {
        out[i] = (s.0, s.1[0], s.1[1], s.2[0], s.2[1]);
    }
    out
}

fn j714_afr() -> [(u32, u32); 14] {
    let mut out = [(0, 0); 14];
    for (i, s) in J714_AUX_PERF_STATES.iter().enumerate() {
        out[i] = (s.0, s.1[0]);
    }
    out
}

fn take(main: &[LoaderOpp], afr: Option<&[LoaderOpp]>, floor_uv: u32)
    -> Result<M5PerfRows, LoaderOppError> {
    M5PerfRows::with_loader_voltages(&J714, main, afr.map(|a| (a, floor_uv)))
}

fn main() {
    let mut fails = 0;
    let mut check = |name: &str, ok: bool| {
        println!("{}: {}", name, if ok { "PASS" } else { "FAIL" });
        if !ok { fails += 1; }
    };

    // 1. Zero placeholders.
    let zero_main: Vec<LoaderOpp> = (0..14).map(|_| LoaderOpp { cells: 2, sram_cells: 2,
        ..Default::default() }).collect();
    let zero_aux: Vec<LoaderOpp> = (0..14).map(|_| LoaderOpp { cells: 1,
        ..Default::default() }).collect();
    check("placeholders are not filled",
          take(&zero_main, Some(&zero_aux), 0) == Err(LoaderOppError::NotFilled));

    // 2. The J714 table itself.
    let board = M5PerfRows::of_board(&J714);
    let j714 = take(&main_opps(&j714_main()), Some(&aux_opps(&j714_afr())), 775_000);
    check("J714 values give the J714 rows", j714 == Ok(board));
    check("J714 values change no state", j714.map(|r| r.changed_states(&J714)) == Ok(0));

    // 3. The K116s ADT.
    let k116 = take(&main_opps(&K116_MAIN), Some(&aux_opps(&K116_AFR)), 775_000);
    let carried = M5PerfRows { states: K116_GPU_PERF_STATES, aux: Some(K116_AUX_PERF_STATES) };
    check("K116s ADT gives the carried K116s rows", k116 == Ok(carried));
    check("K116s differs from J714 in 13 + 5 states",
          k116.map(|r| r.changed_states(&J714)) == Ok(13 + 5));

    // 4. Refusals.
    let mut wrong_freq = main_opps(&K116_MAIN);
    wrong_freq[5].hz += 1_000_000;
    check("other frequency refused",
          take(&wrong_freq, Some(&aux_opps(&K116_AFR)), 775_000) == Err(LoaderOppError::State(5)));
    let mut far = main_opps(&K116_MAIN);
    far[9].core_uv[1] += 60_000;
    check("voltage 60 mV off refused",
          take(&far, Some(&aux_opps(&K116_AFR)), 775_000) == Err(LoaderOppError::Voltage(9)));
    let mut odd = main_opps(&K116_MAIN);
    odd[3].sram_uv[0] += 500;
    check("voltage not whole mV refused",
          take(&odd, Some(&aux_opps(&K116_AFR)), 775_000) == Err(LoaderOppError::Voltage(3)));
    let mut one_cell = main_opps(&K116_MAIN);
    one_cell[0].cells = 1;
    check("one voltage cell refused",
          take(&one_cell, Some(&aux_opps(&K116_AFR)), 775_000) == Err(LoaderOppError::Shape));
    let short = &main_opps(&K116_MAIN)[..13];
    check("13 states refused",
          take(short, Some(&aux_opps(&K116_AFR)), 775_000) == Err(LoaderOppError::Shape));
    check("missing CS/AFR points refused",
          take(&main_opps(&K116_MAIN), None, 0) == Err(LoaderOppError::Shape));
    let mut aux_freq = aux_opps(&K116_AFR);
    aux_freq[0].hz = 0;
    check("other AFR frequency refused",
          take(&main_opps(&K116_MAIN), Some(&aux_freq), 775_000) == Err(LoaderOppError::State(0)));
    check("zero SRAM floor refused",
          take(&main_opps(&K116_MAIN), Some(&aux_opps(&K116_AFR)), 0) == Err(LoaderOppError::Voltage(0)));
    check("SRAM floor far above the board's refused",
          take(&main_opps(&K116_MAIN), Some(&aux_opps(&K116_AFR)), 3_000_000)
              == Err(LoaderOppError::Voltage(0)));

    // 5. SRAM floor.
    let floored = take(&main_opps(&K116_MAIN), Some(&aux_opps(&K116_AFR)), 800_000).unwrap();
    let aux = floored.aux.unwrap();
    check("AFR SRAM takes the floor under it", aux[6].2[0] == 800 && aux[6].1[0] == 745);
    check("AFR SRAM follows the core above it", aux[13].2[0] == 990);

    assert!(fails == 0, "{} checks failed", fails);
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=MUTATIONS)
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-m5-opps-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '--edition=2021', '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
