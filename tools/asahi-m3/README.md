# M3 GPU progress readers

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

Run the offline regression checks without touching the GPU or watchdog:

```sh
python3 tools/asahi-m3/test_completion_progress.py
python3 tools/asahi-m3/test-retirement-progress.py
```

These files are libraries and offline tests. The source branch's machine-specific
watchdog, clock-limit and firmware-memory experiment launcher is not needed by
the curated kernel and is not installed here. The roadmap GPU smoke helper
continues to validate Vulkan compute and presentation with the existing Mesa.
