# Fresh Touch ID contacts

The capture path accepts a frame only after observing the sensor waiting for a
finger (`STATE_ARMED`, 17). If its first contact state is already reading (19)
or frame-ready (7), that frame is discarded before enclave image processing.
Once rejected, a frame stays rejected even if a later status says armed.

Verify ends that attempt with `SEP_BIO_STATUS_REMOVE_FINGER` (8), without a
comparison or result token. The companion libfprint patch translates it to
`FP_DEVICE_RETRY_REMOVE_FINGER`; stock fprintd reports
`verify-remove-and-retry` and starts another attempt. Placement uses the existing
finger-needed property. Identify, including the enrolment duplicate check,
uses the same handling.

Enrolment instead drains and wipes the encrypted frame, idles and rearms the
sensor, and keeps its accepted stages. Removal guidance (4) produces one
`enroll-remove-and-retry` per transition; waiting for placement sets
finger-present false and finger-needed true. Discarded frames neither advance
stages nor enter the enclave image pipeline. Repeated discards share the
original 60-second capture timeout. Cancellation, close and suspend retain the
existing worker/epoch checks.

These are additive status/guidance values: ioctl layouts and interface version
5 are unchanged. Older kernels never send them. Old libfprint builds can still
verify ordinary fresh contacts but cannot describe the new retry; install the
companion userspace build with this kernel.

## Attended qualification (lead)

This change has no hardware qualification yet. In particular, the existing
interpretation of state 17 as waiting for a finger must be checked on each
sensor: a transient armed state with a continuously held finger would defeat
this boundary check. A very fast fresh placement whose armed state was never
sampled is conservatively retried. Do not qualify the change solely from the
mocked state sequences.

Use an existing enrolled finger and unchanged Linux SEP identity. Test the M1
Pro power button, M2 key, J616s power button and Neo power button separately.
Keep test logs private; public reports should contain aggregate outcomes only,
without print UUIDs, account details or biometric/sensor frame payloads.

1. Start with the sensor empty. Run verify, then place the enrolled finger.
   Expect one match without a removal retry. Repeat ten times, including quick
   placements, with both IRQ capture and the supported polling configuration.
2. Rest the enrolled finger on the sensor before starting verify. Keep it there
   for several seconds. Expect `verify-remove-and-retry`, never a match or
   non-match while continuously held. Lift fully, wait for finger-needed, then
   place again. Expect a match. Repeat ten times; a continuously held match or
   non-match fails qualification.
3. Repeat with identify and with the wrong finger. A fresh wrong-finger touch
   must remain a real no-match with the clarified no-match log; a stale touch
   must remain a removal retry, irrespective of identity.
4. Begin an explicitly authorized enrolment with a finger already resting on
   the sensor. Expect `enroll-remove-and-retry` without stage progress. Lift and
   place, then complete normally. Hold between later stages too: no repeated
   accepted stages from the same held contact. Check that discarded encrypted
   frames do not desynchronise the sensor/enclave sequence or break completion.
5. Cancel while removal is requested, while waiting for placement and during
   repeated discards. Immediately start another operation. Check no stale
   callback, token, extra stage or worker-busy failure leaks into the new one.
   Repeat across close/reopen and suspend/resume.
6. Hold through the capture timeout and check that enrolment terminates within
   its original timeout. Verify retries are separate fprintd attempts; cancel
   them explicitly after checking the held-finger condition.
7. Repeat ordinary verify/enrolment with the updated userspace and an older
   supported v4/v5 kernel. The new guidance must not be required from those
   kernels. Confirm struct sizes and ioctl numbers match before installation.

Rollback is the previously qualified kernel and userspace package pair using
the existing boot fallback. Preserve enrolments and Linux SEP OS identity;
rollback does not require reset, deletion or re-enrolment. Do not reload the
SEP driver or change its identity to test this patch.

## Host tests

`python3 tools/aurora-sep/tests/test_fresh_contact.py` compiles and runs the real
Rust capture wait and contact gate against scripted status reads and IRQ/poll
waits. It covers early reading/ready frames, a tainted frame followed by armed,
clean placement, repeated enrolment discards, buffer disposal, status/read/arm
faults, cancellation and the shared deadline. It does not emulate physical
finger sensing or the enclave's encrypted sequence handling.
