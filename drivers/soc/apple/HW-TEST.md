# T6041 MCC GPU parameter-buffer residency test

Run on J616s with the fleet46 baseline and a candidate differing only by this
change. Use the existing dead-man procedure for both boots.

1. Confirm the candidate logs `released SLC residency for stream 14 DSID D`
   once, with `D` matching stream 14 in `apple,tunable-mcache-stream`.
   If the existing counter tool is available, confirm that DSID's occupancy
   reaches zero on all 16 planes, stays zero after GPU work, and that the CPU
   group gains about 3.3 MiB.
2. Run the same P-cluster-0 pointer chase on both boots. Compare 24, 28, and
   32 MiB: 24 MiB must be unchanged or better; expect about 40.4 to 35.6 ns at
   28 MiB and 51.4 to 38.0 ns at 32 MiB (P1: 54.5 to 40.6 ns).
3. Run the complete gpubench suite, including all `rt.*` and `render.*` cases.
   Require no fault, timeout, or performance regression.
4. Compare five minutes of idle power and require no change. Require zero new
   AVC messages.

M1, M2, and M1 Pro must show no behavior change because they do not carry the
`apple,t6041-mcc` compatible.
