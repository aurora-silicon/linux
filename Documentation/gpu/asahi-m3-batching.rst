M3 completion-worker batching
============================

The M3 scheduler queues ready packets and returns their fences immediately.
The completion worker publishes and retires the GPU work. Adjacent render
packets with the same VM and priority can share a firmware round trip; compute
and VM changes wait for full retirement. Equal-priority packets stay FIFO.
More urgent work can run before a partially submitted packet's next batch.

The first draw initializes the buffer manager and runs alone. On the explicitly
enabled experimental T8122 runtime, subsequent work defaults to at most two
render batches in flight and a 4000 microsecond measured GPU-time budget.
T6030 defaults to one batch in flight and no time budget. M1/M2 scheduling is
unchanged. These controls do not change firmware admission or board calibration.

Overlap requires the same bound VM, consumed TA/fragment notifications, enough
free slots in the 16-pass arena, and no storage growth. Already running slots
cannot be reset or reused. A VM without a valid timing measurement runs one
pass before coalescing. The budget counts active and newly prepared passes;
a single heavy pass can exceed it, but additional passes cannot extend that
client's burst. An urgent waiting client prevents extending lower-priority work.

Root can tune ``asahi.m3_pipeline_depth`` (0 selects the SoC default, 1..16
sets a maximum), ``asahi.m3_render_batch_override`` (0 retains the boot batch
size), and ``asahi.m3_render_batch_budget_us`` (0 disables the budget,
18446744073709551615 restores the SoC default). Changes affect future
publications; lowering a limit does not free in-flight storage.

Cancellation removes unsubmitted commands and signals the fence once. Published
commands keep their VM and buffer guards until retirement, even when cancellation
won the signal race. Partial publication and failed retirement mark the runtime
failed and retain uncertain DMA owners. Failed coprocessor stop retains the
runtime allocation and pins the module. Host bookkeeping is reserved before
shared producer state changes; preparation failure restores producer state.

The standalone host policy tests run with::

    rustc --edition=2021 --test tools/asahi/m3-batching-tests.rs -o /tmp/m3-batching-tests
    /tmp/m3-batching-tests

These tests cover admission and ownership decisions. Actual desktop latency,
firmware scheduling and fault recovery require the matched GPU stack on hardware.
