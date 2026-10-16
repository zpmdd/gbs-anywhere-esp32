# Upstream comparison and validation

Checked 2026-10-16 against:

- [PR15, 8f398c5](https://github.com/Skamba/gbs-anywhere/pull/15)
- [PR16, ba00c92](https://github.com/Skamba/gbs-anywhere/pull/16)

Both PRs are still open. PR16 includes the current PR15 pairing fix. Its actual
`wifi_grinder.rs` code accepts a task immediately and reaccepts it when listed
again; the older PR15 description of waiting until measurement does not describe
that PR16 code. The C bridge retains the immediate acceptance behavior already
used by its working precursor.

## Changes relevant to this companion

- PR15's newer fix leaves a grinder silent for three minutes after an unanswered
  pairing-status request. Initial pairing remains a desktop task; the S3 imports
  that identity and must be stopped throughout any future pairing/recovery.
- PR16's unchanged normal flow keeps a task alive, reconciles a lost acceptance
  from the next task list and expires an unaccepted result whose job disappeared.
  Version 0.2.0 covers these paths in tests of the actual C worker.
- Three pairing refusals are distinguished from ordinary connection loss. Unlike
  the desktop, this companion shows an action-required state instead of clearing
  the identity or starting an unattended pairing.
- Cloud authentication and coffee parsing did not change between the prior
  `3b6ef9a` snapshot and `ba00c92`. No authentication change was necessary.
- PR16's tabs, settings, history and logs are desktop features. This embedded
  companion keeps a read-only status page with current vs previous errors,
  per-link health and colored text badges rather than porting the full desktop UI.

## Intentional differences

The S3 persists a 32-task journal, skips existing tasks after restart and requires
measured cloud weight. An ambiguous `brewDone` response is not automatically
replayed because device-side idempotency has not been established here. It retains
at most one live task and never applies a previous cup to a new grind.

Ordinary reconnect waits are capped at five seconds for daily grinder power-on,
instead of the desktop's minute-long maximum. Network requests and cloud calls
still add latency. Pairing refusals use a separate one-minute interval. This is
not a guarantee that a grind made before the API is ready can be recovered.

## Validation

- Host C tests run with ASan/UBSan. Worker tests use the production job, coffee,
  acceptance and result functions with in-memory services; no device or vendor
  cloud is contacted.
- Coverage includes long offline periods followed by a new task, startup baseline
  skipping, reacceptance, lost acceptance replies, ambiguous result delivery,
  expired tasks, pairing refusals and failed journal writes.
- Provisioning and serial-helper tests cover credential boundaries, saved cloud
  integration requirements, port checks and the non-resetting USB open sequence.
- UI tests cover eight locales, independent error/recovery states, color with text,
  pairing alerts, browser storage restrictions and stale/offline indicators.
- ESP-IDF 5.5 builds the S3 image. Browser checks use synthetic states locally.
- The 0.2.0 application was flashed to an ESP32-S3 on 2026-10-16 after a full
  flash backup. Its existing pairing, cloud identity, mode and task ledger were
  preserved. A normal boot restored active/ready with successful grinder and cloud
  replies, no active error and no old task accepted or replayed. The live translated
  page and diagnostics were checked in Chrome. New-build real-coffee feedback still
  needs the next cup.

Precursor results demonstrate feasibility, not full acceptance of a different
binary. E80, prolonged operation and real network/token-expiry faults remain
unverified. No new grinder pairing or synthetic feedback is part of this upgrade.
