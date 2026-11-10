# T6041 MCC GPU parameter-buffer lifecycle test

Run on J616s with the fleet46 baseline and a candidate differing only by this
change. Use the existing dead-man procedure for both boots.

Use `kits/test/20261110-hwp6219` as the base. Arm A with the production DTB;
arm B is a RAM-only `aura-try` boot whose only DT changes are the 256 KiB MCC
plane windows, `apple,pmp` on MCC, and `apple,memory-cache-controller` on GPU.

1. Confirm B logs `GPU lifecycle released SLC residency for stream 14 DSID D`
   after the GPU firmware reaches its initialized state, with `D` matching the
   unique stream-14 record in `apple,tunable-mcache-stream`.
2. On P0 and P1, interleave 24, 28, and 32 MiB pointer chases. Both 28 and
   32 MiB medians must improve over A on both clusters; 24 MiB must not regress.
3. Run the complete gpubench suite, including all `rt.*` and `render.*` cases,
   then run `rt.scene` heavy and `render.heavy` at 2160p. Repeat the same chase
   row after the heavy pair. The 28/32 MiB gain must remain; require neutral
   gpubench geomean and no fault, timeout, or recovery.
4. Run s2idle three times and require 3/3 successful resumes. Compare five
   minutes of idle power and require no change. Require zero new AVC messages.

M1, M2, and M1 Pro must show no behavior change because they do not carry the
`apple,t6041-mcc` compatible.
