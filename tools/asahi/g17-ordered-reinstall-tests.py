#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Guard the 25G83 configuration-before-work transaction."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMPUTE = ROOT / 'drivers/gpu/drm/asahi/g17/queue/compute.rs'
EVENTS = ROOT / 'drivers/gpu/drm/asahi/g17/runtime/events.rs'
RECOVERY = ROOT / 'drivers/gpu/drm/asahi/g17/recovery.rs'
CORE = ROOT / 'drivers/gpu/drm/asahi/asahi_core.rs'


def item(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def ordered(*needles):
    positions = [INSTALL.index(needle) for needle in needles]
    assert positions == sorted(positions), (needles, positions)


SOURCE = COMPUTE.read_text()
INSTALL = item(SOURCE, 'fn install_ordered(')
BIND = item(SOURCE, 'pub(crate) fn bind_owner(')
ROLLBACK = item(SOURCE, 'pub(crate) fn rollback_unpublished(')
QUARANTINE = item(SOURCE, 'pub(crate) fn quarantine(')
RECYCLE = item(SOURCE, 'fn recycle_settled(')
CHANNEL_ERROR = item(SOURCE, 'pub(crate) fn set_channel_error(')
RESTART = item(SOURCE, 'pub(crate) fn prepare_recovery_restart(')
PUBLISH = item(SOURCE, 'pub(in crate::g17) fn publish_with_native_waits(')

# The arm is opt-in and exactly qualified to the public 25G83 queue shape.
core = CORE.read_text()
assert 'g17_ordered_compute_reinstall: u32' in core
assert 'g17_ordered_compute_reinstall.value() != 0' in SOURCE
assert '!self.graph.m5' in item(SOURCE, 'fn ordered_reinstall_enabled(')
assert 'self.graph.pointers.recovery_target.is_none()' in SOURCE

# A refused hand-over dirties the QID before its first fallible operation.
assert BIND.index('self.require_ordered_reinstall();') < BIND.index('reset_for_owner')
for method in (ROLLBACK, QUARANTINE, RECYCLE, CHANNEL_ERROR):
    assert 'self.require_ordered_reinstall();' in method
assert 'queue.prepare_recovery_restart()?;' in EVENTS.read_text()
recovery_service = item(RECOVERY.read_text(), 'pub(super) fn service<')
assert recovery_service.index('host.prepare_compute_restart()?;') \
    < recovery_service.index('host.transition(3, 0)?;')
assert RESTART.index('if self.active.len != 0') < RESTART.index('self.graph.reset_items()?')

# No client descriptor is built until the empty configuration transaction has
# either completed or returned EAGAIN to the common retry loop.
assert PUBLISH.index('self.install_ordered(&packet, host)?;') < PUBLISH.index('work_state()?')
ordered('if self.active.len != 0', 'self.retire_configuration(epoch)?',
        'QueueConfig::new', 'self.ordered_reinstall_end.is_some()',
        'self.ordered_reinstall = false', 'self.graph.cursors()?')
ordered('configuration::Configuration::new', '.append(cursors.producer',
        'self.ordered_reinstall_end = Some(producer)', 'host.publish_outer(',
        'host.notify(')
assert INSTALL.rindex('Err(EAGAIN)') > INSTALL.index('host.notify(')
assert 'install: true' in INSTALL
assert 'qos_update: true' in INSTALL and 'context_update: true' in INSTALL

# The lockless fast path must fall back while the transaction is outstanding.
assert '|| queue.ordered_reinstall_pending()' in (ROOT / 'drivers/gpu/drm/asahi/g17/runtime.rs').read_text()

print('PASS ordered reinstall: default-off 25G83 qualification; bind/rollback/recovery dirtiness; empty configuration consumption precedes every successor descriptor')
