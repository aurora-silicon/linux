.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Apple DCP sleep transition logging
=================================

The H17P external display pipe can report routine run-mode transitions on the
RTKit syslog endpoint. In particular, ``set_run_mode_safe: deferring`` and
``ready_for_run_mode_change: initiating deferred run mode change`` describe a
transition being queued and subsequently started. They are firmware text,
not a Linux error return. The word ``deferred`` alone says nothing about the
success of the IOMFB command or the time taken to restore a picture.

A log scanner using ``apple-dcp.*(err|fail|timeout)`` counts ``err`` inside
``deferring`` and ``deferred``. A blank/restore sequence can produce four such
lines (the two messages for each direction). Further display reprogramming
can repeat the sequence. A fixed count must not be treated as a known-error
allowance, and excluding every line containing either function name can hide
future errors.

Only the complete known records, including their context and transition, are
logged at debug on the external H17P transport. Other firmware records retain
info logging, including unknown transitions and diagnostic suffixes. RTKit
still validates, decodes and acknowledges every record normally. Other RTKit
clients keep their existing logging policy. The policy does not change the
DCP power, link, mailbox or retained-memory ownership paths.

With dynamic debug enabled in the kernel, the records can be collected by
enabling the syslog callsite in ``drivers/soc/apple/rtkit.c``. Save its previous
state and restore that state afterwards::

  echo 'func apple_rtkit_syslog_rx_log +p' > /sys/kernel/debug/dynamic_debug/control

Changing the console log level does not remove info records from the kernel
ring buffer and therefore does not repair a substring-based scanner.

Qualification
-------------

Host-only checks, from a kernel checkout::

  python3 tools/testing/selftests/drivers/gpu/apple-dcp/run.py --group boot
  python3 tools/testing/selftests/drivers/gpu/apple-dcp/run.py

The boot group executes the production RTKit decoder and DCP policy with
synthetic records. It checks debug/info selection, foreign contexts, appended
failures, absent policy, inherited sessions, validation failures and unchanged
acknowledgments. ``CONFIG_DRM_APPLE_KUNIT_TEST`` includes the
``apple-dcp-syslog`` suite for the complete-record policy.

A device qualification should compare the same sleep/display workload before
and after this change with debug disabled, and separately with the syslog
callsite enabled. Preserve the full kernel delta and compare suspend errors,
DART faults, IOMFB command timeouts, residency and independently measured
picture restoration. Debug-enabled transition records are expected; command
failures remain failures. CPU sleep residency alone does not establish display
power consumption or successful scanout.
