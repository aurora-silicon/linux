# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright 2026 Ryan Murray
"""Shared public-source extraction and strict host compilation."""
from pathlib import Path
import hashlib, json, os, re, shlex, subprocess

# The Type-C display fabric moved from dcp.c to dcp-fabric.c; extract from
# them as one unit so a function is found whichever file holds it.
_read_text = Path.read_text


def _read_dcp_unit(self, *args, **kwargs):
    text = _read_text(self, *args, **kwargs)
    fabric = self.with_name('dcp-fabric.c')
    if self.name == 'dcp.c' and self.parent.name == 'apple' and fabric.exists():
        text += '\n' + _read_text(fabric, *args, **kwargs)
    return text


Path.read_text = _read_dcp_unit

def block(source, marker):
    start = source.index(marker) if isinstance(marker, str) else marker
    opening = source.index('{', start)
    if isinstance(marker, str) and marker.startswith('static ') and '(' in marker:
        while 0 <= source.find(';', start) < opening:
            start = source.index(marker, source.index(';', start) + 1)
            opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def source_defines(source):
    lines = iter(source.splitlines())
    definitions = []
    for line in lines:
        if not line.startswith('#define '):
            continue
        definition = [line]
        while definition[-1].endswith(chr(92)):
            definition.append(next(lines))
        definitions.append('\n'.join(definition))
    return definitions

def tipd_pm_header(root):
    """CD321x PM state machine for host units, with a host clamp() for minmax.h."""
    header = (root / 'drivers/usb/typec/tipd/cd321x-pm.h').read_text()
    # Host integer types; a repeated identical typedef is valid C11.
    header = header.replace('#include <linux/types.h>',
        '#include <stdint.h>\ntypedef uint32_t u32;')
    # Units that model probe, not DP status updates, leave cd321x_dp_send() unused.
    header = header.replace('static inline enum cd321x_dp_send cd321x_dp_send(',
        'static inline __attribute__((unused)) enum cd321x_dp_send cd321x_dp_send(')
    # The host units need the status bit, not kernel USB declarations.
    header = header.replace('#include <linux/usb/typec_dp.h>',
        '#ifndef DP_STATUS_IRQ_HPD\n#define DP_STATUS_IRQ_HPD (1U << 8)\n#endif')
    # Single line: probe-mux-pm-dispatch filters duplicate definitions per line.
    return header.replace('#include <linux/minmax.h>',
        '#define clamp(val, lo, hi) ({ __auto_type _v = (val); '
        '__typeof__(_v) _lo = (lo), _hi = (hi); _v < _lo ? _lo : _v > _hi ? _hi : _v; })')

def run_unit(name, unit, output, cases=((),), includes=(), extra=()):
    work = output / name
    work.mkdir(parents=True, exist_ok=True)
    source = work / 'host.c'
    source.write_text(unit)
    command = shlex.split(os.environ.get('HOSTCC', 'clang')) + [
        '-std=gnu11', '-Wall', '-Werror', '-O1', '-g',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    ] + list(extra) + ['-I' + str(p) for p in includes] + [str(source), '-o', str(work / 'host')]
    (work / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (work / 'compile.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    for index, case in enumerate(cases):
        result = subprocess.run([str(work / 'host'), *case], capture_output=True, text=True, timeout=120)
        (work / ('case-' + str(index) + '.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
    return hashlib.sha256(unit.encode()).hexdigest()


# Asahi 7.2 moved the generation-specific setup after the tunables into
# per-SoC atcphy_hw callbacks; atcphy_configure() calls them and compares
# against the all-modes one for the RCAL wait.
ATC_POST_TUNABLE = ('atcphy_configure_post_tunable_t8103', 'atcphy_wake_common_t8122',
                    'atcphy_configure_post_tunable_t8122',
                    'atcphy_configure_post_tunable_all_modes')


# Helpers that the fixed-hub SuperSpeed handover split out of extracted functions
ATC_SPLIT = ('atcphy_pipe_live', 'atcphy_usb3_route')


# The retryable T8103-generation USB3 handoff: the bounded live lock and the
# park of a live legacy PIPE, which the extracted PIPE, mux, reset and route
# functions call. Older sources have neither; atc_function() then yields ''.
ATC_T8103_LIVE = ('atcphy_pipehandler_lock_live', 'atcphy_park_live_t8103_pipehandler')


def atc_function(source, name):
    """The actual ATC function @name, or nothing for a source that does not have it.

    The fixed-hub SuperSpeed handover splits helpers out of the functions that the
    fixtures extract; older sources keep that code inline.
    """
    marker = re.search(r'^static [^\n;]+\b' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    return block(source, marker.start()) + '\n' if marker else ''


def atc_mux_without_pm(source):
    """Lane/teardown fixtures omit USB PM, keeping mode mapping and lane work real.

    The PM fixtures replace only this adapter with the actual mux update/callback.
    """
    mapping = block(source, 'static enum atcphy_mode atcphy_typec_target_mode(')
    locked = block(source, 'static int atcphy_mux_set_locked(')
    if 'static int atcphy_mux_switch(' not in source:
        return mapping + '\n' + locked + r'''
static int atcphy_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
    struct apple_atcphy *atcphy = typec_mux_get_drvdata(mux);
    return atcphy_mux_set_locked(atcphy, atcphy_typec_target_mode(atcphy, state));
}
'''
    # The locked body is atcphy_mux_switch(); atcphy_mux_set_locked() takes the lock
    # and tells whether a SuperSpeed port that the USB side took down may come up.
    helpers = ''.join(atc_function(source, name) for name in
                      ('atcphy_usb3_port_ready', 'atcphy_mux_check', 'atcphy_mux_park'))
    return mapping + '\n' + helpers + block(source, 'static int atcphy_mux_switch(') + '\n' + locked + r'''
static int atcphy_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
    struct apple_atcphy *atcphy = typec_mux_get_drvdata(mux);
    bool port_on;
    int ret = atcphy_mux_set_locked(atcphy, atcphy_typec_target_mode(atcphy, state), &port_on);

    /* Without the USB side no SuperSpeed port went down, so none may come up. */
    assert(!port_on);
    return ret;
}
'''


def fabric_core(root):
    """The real Type-C fabric decisions and effect runners as host C.

    dcp-fabric-core.c and dcp-fabric-effects.h are host-compilable by design
    (KUnit runs them against fake hardware); only the kernel includes and
    module boilerplate are dropped.  The unit supplies u32/u64.  Unused
    static inline helpers are marked so, as they would be in a header.
    """
    apple = root / 'drivers/gpu/drm/apple'
    core = apple / 'dcp-fabric-core.c'
    if not core.exists():
        return ''

    def strip(text):
        return '\n'.join(line for line in text.splitlines()
                         if not line.startswith(('#include', 'EXPORT_SYMBOL', 'MODULE_'))) + '\n'
    tunnel = (root / 'include/linux/soc/apple/dp-tunnel.h').read_text()
    return ('\n#include <limits.h>\n#include <errno.h>\n#include <stdbool.h>\n'
            '#ifndef BIT\n#define BIT(n) (1UL << (n))\n#endif\n'
            '#ifndef time_before\n#define time_before(a, b) ((long)((a) - (b)) < 0)\n#endif\n'
            + block(tunnel, 'static inline bool apple_dp_tunnel_wiring_dual(') + '\n'
            + strip((apple / 'dcp-fabric-core.h').read_text())
            + strip(core.read_text())
            + strip((apple / 'dcp-fabric-effects.h').read_text())).replace(
                'static inline ', 'static inline __attribute__((unused)) ')


def fabric_rebalance(dcp):
    """dcp_typec_rebalance_locked() with the adapter callbacks it runs through.

    The fabric core split turned the loop into dcp_fabric_run_rebalance() over
    struct dcp_rebalance_context and dcp_rebalance_ops; older sources keep the
    loop inline.
    """
    marker = 'static struct apple_dcp_typec_route *\ndcp_typec_rebalance_locked('
    end = dcp.index(marker)
    end += len(block(dcp, end))
    start = dcp.find('struct dcp_rebalance_context {')
    return dcp[start if 0 <= start < end else dcp.index(marker):end] + '\n'


def apple_function(root, name):
    """The definition of @name in whichever drivers/gpu/drm/apple source has it.

    Robust to functions moving between files; '' when no source defines it.
    """
    pattern = re.compile(r'^(?:[A-Za-z_][\w \t*]*\n)?(?:[A-Za-z_][\w \t*]*?\b)?' + re.escape(name) +
                         r'\([^;{]*?\)\s*\{', re.M)
    for path in sorted((root / 'drivers/gpu/drm/apple').glob('*.c')):
        if 'test' in path.name:
            continue
        text = _read_text(path)
        match = pattern.search(text)
        if match:
            return block(text, match.start()) + '\n'
    return ''


def internal_inline(root, name):
    """A static inline helper from dcp-internal.h, marked unused; '' if absent.

    dcp_fw_is_14_7() reads fw_compat and hw.fw_14_7_endpoints.
    """
    text = (root / 'drivers/gpu/drm/apple/dcp-internal.h').read_text()
    match = re.search(r'^static inline [^\n;]*\b' + re.escape(name) + r'\(', text, re.M)
    if not match:
        return ''
    return block(text, match.start()).replace('static inline ', 'static inline __attribute__((unused)) ', 1) + '\n'
