# Upstream comparison and validation

Checked 2026-10-17 against:

- [merged main, 3adf778](https://github.com/Skamba/gbs-anywhere/commit/3adf778c7f0a3803994cbcc5fe2af1f8ac8a5be1)
- [PR15 final fix, 13e4a05](https://github.com/Skamba/gbs-anywhere/commit/13e4a053e596609b78402a2b25015a816dfe7de5)
- [PR16, ba00c92](https://github.com/Skamba/gbs-anywhere/pull/16)

PR15 is merged. The published nightly includes merge `26492fb`; current main
adds the companion README link in PR20. Desktop `v0.2.1` predates E65 support.
PR16 remains open. The merged worker accepts a task immediately and reaccepts
it when listed again; the C bridge retains that behavior.

## Changes relevant to this companion

- PR15 leaves a grinder silent for three minutes after an unanswered
  pairing-status request. Initial pairing remains a desktop task; the S3 imports
  that identity and must be stopped throughout any future pairing/recovery.
- The final fix treats an accept response saying the job is no longer listed as
  possibly already taken, so a pending result survives a lost accept reply. The C
  worker also had a window where a result arrived before job-list reconciliation
  and was dropped on an empty list without ever sending `brewDone`. Version 0.2.1
  requires explicit ongoing-job absence before expiring that result; an attempted
  reacceptance clears the old absence. Production-worker tests cover both windows,
  a relisted job, a truly missing job and delivery timeout.
- Pairing refusals must be sustained for 60 seconds, with a timeout or other
  response interrupting the period. The desktop retains a refused ID for recovery.
  This companion keeps its imported identity and shows an action-required state
  instead of clearing it or starting an unattended pairing.
- A desktop export may contain `pairing_id: null` and `refused_pairing_id`. Import
  deliberately requires a confirmed `pairing_id`; its error now explains how to
  finish desktop recovery and export again. Tests cover both file forms.
- Cloud authentication and coffee parsing did not change between the prior
  `3b6ef9a` snapshot, `ba00c92` and merged main. No authentication change was necessary.
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

- The 0.2.1 application (code `1e85800`) was flashed on 2026-10-17 after a full
  flash backup, writing only the application partition. NVS readback was byte-for-byte
  identical before the new application booted. The new binary restored grinder,
  cloud and storage health in monitor mode, then returned to active/ready with no
  current error, old job acceptance or replay. The persisted delivery count was
  unchanged. The live Chinese status page showed 0.2.1 and Ready in Chrome.

Version 0.2.1 has passed the offline and board checks above; a new-build real cup
still needs validation. Precursor results demonstrate feasibility, not full
acceptance of a different binary. E80, prolonged operation and real network/token-
expiry faults remain unverified. No new grinder pairing or synthetic feedback is
part of this upgrade.
