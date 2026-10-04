# M3 GPU progress readers and reliability checks

These readers come from Eryk Wieliczko's `eryk_m3_gpu` branch at
`c07786baa30b98bc95b1f689eda226cbd112094c`.
`retirement_progress.py` includes the continuous log-draining change from
`45ac5bdec217e1da9df6c48de685fc2c66cb7bc8`.

The curated kernel exposes `/sys/kernel/debug/asahi-m3/progress` with the
runtime generation, verified completed-batch count, last completion timestamp
(CLOCK_MONOTONIC), and health state. Reading it does not touch GPU registers.
This is a diagnostic interface, not a stable graphics userspace ABI.

`CompletionProgress` reads the endpoint and rejects unhealthy, replaced,
regressed, malformed or stale evidence. An unchanged completion count cannot
renew progress. The older `RetirementProgress` reader drains `/dev/kmsg` in a
separate thread and treats log loss as an error; routine retirement printk
records now require the kernel's existing SubmitTiming debug flag, so use the
state reader for normal validation.

The reliability backports through Eryk commit
`e9268a4002d924f9c6385dff4972f2d94dc93c7a` also retain an unhealthy terminal
progress sample before rejecting it. Such a sample never renews progress.
`fault_snapshot.py` can collect bounded, sealed M3 devcoredumps into a local
directory; its tests use generated dummy records. Actual firmware snapshots
must stay on the machine and must not be committed or attached to public PRs.
The collector is a library; installing this source does not enable collection.

Run the offline regression checks without touching the GPU or watchdog:

```sh
python3 tools/asahi-m3/test_completion_progress.py
python3 tools/asahi-m3/test-retirement-progress.py
python3 tools/asahi-m3/test_fault_snapshot.py
python3 tools/asahi-m3/test-m3-packet-completion.py
python3 tools/asahi-m3/test-fault-dump-bounds.py
python3 tools/asahi-m3/test-host-progress.py
python3 tools/asahi-m3/test-gem-object-size.py
python3 tools/asahi-m3/test-memory-accounting.py
```

The Rust host harnesses compile actual driver methods with host stubs. They
check competing completion/cancellation callbacks, immutable fence results,
retained failed-job ownership, reserved snapshot storage and read rollback,
GEM size overflow, concurrent retirement counters, and allocation lifetime
accounting. Snapshot appends are exercised with allocation unavailable after
construction; the test follows the final preallocation change rather than the
older upstream expectation of an allocating append. These checks do not prove
hardware retirement or recovery from a GPU fault; those need a qualified boot.

These files are libraries and offline tests. The source branch's machine-specific
watchdog, clock-limit and firmware-memory experiment launcher is not needed by
the curated kernel and is not installed here. The roadmap GPU smoke helper
continues to validate Vulkan compute and presentation with the existing Mesa.
