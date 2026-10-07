M6 CPU bring-up
==============

J873g has six E CPUs and a shared performance domain containing two P CPUs
and four M CPUs. The T8152 ACC request uses a five-bit hardware state, with
state 2 as the first ADT operating point. Do not use the older voltage or PLL
initialization sequences.

Failure cases and qualification
------------------------------

Before testing, account for incorrect MPIDRs, incomplete spin-table release,
broken cross-CPU interrupts, a stalled architectural timer, mismatched OPP
indices, an unknown inherited request, a stuck busy bit, an unacknowledged
transition, and another agent changing the request. A transition failure must
latch until the policy is recreated. A requested frequency is not a measured
frequency.

The initial tables expose only states 2 and 3. Higher states require an SMC
thermal policy; this port does not qualify them. The loader must hand over
within the advertised table. Keep firmware voltage, PLL and throttling
configuration intact.

On physical J873g, capture the source revision, image and DTB hashes, boot log,
online CPU list, per-CPU workload completion, interrupt counters and both
cpufreq policies. Exercise both exposed states in each domain, compare timed
dependent workloads, restore the low states and check the kernel log. Save
the commands and a machine-readable receipt. Native m1n1 SMP results alone
do not qualify Linux scheduling.
