# Upstream comparison and validation

## Current review: 2026-10-22

Fetched `Skamba/gbs-anywhere` main at
`ebaac65901fee0570ac26f9fda202c61f74ba055`. The root lightweight companion and
the newly published [integrated Hub](INTEGRATED.md) both retain the C port's
single-owner identity and no-replay policy.

| Change since the previous review | Disposition |
| --- | --- |
| `40fbf3b`: null/missing `LAST_COFFEE.lastCoffees` | Ported with regression coverage to both C parsers. Wrong outer output/list types still fail. |
| `164dfe3`: target duration below 1 ms is absent | Ported to both job parsers. |
| `b67e6fc` / `164dfe3`: recipe-relative adjustment-window hints | Reviewed, UI metadata deferred. Valid measured results are delivered; the grinder decides whether it adjusts. |
| Desktop API names, saved-file link takeover, stop signals and Web history/settings | Reviewed differences. No second server, filesystem pairing owner or Rust process lifecycle is introduced into the embedded worker. |

One-shot ordinary CSV recipe following is an integrated-port addition, not a
new upstream Generic WiFi field. It uses the authenticated grinder web export,
exact task UUID matching and existing Hub command guards. The root lightweight
firmware does not gain coffee-machine controls through a parser-only update.

The integrated runtime source and its import pin are recorded in
[`firmware/integrated/UPSTREAM.md`](../firmware/integrated/UPSTREAM.md).
Both applications build from this public checkout. Keep reviewing relevant
upstream deltas; an updated review pin does not mean every Rust commit was merged.
The older sections below preserve the history of earlier comparisons and builds.

Checked 2026-10-17 against:

- [merged main, 3adf778](https://github.com/Skamba/gbs-anywhere/commit/3adf778c7f0a3803994cbcc5fe2af1f8ac8a5be1)
- [PR15 final fix, 13e4a05](https://github.com/Skamba/gbs-anywhere/commit/13e4a053e596609b78402a2b25015a816dfe7de5)
- [PR16, ba00c92](https://github.com/Skamba/gbs-anywhere/pull/16)

PR15 is merged. The published nightly includes merge `26492fb`; current main
adds the companion README link in PR20. Desktop `v0.2.1` predates E65 support.
PR16 remains open. The merged worker accepts a task immediately and reaccepts
it when listed again; the C bridge retains that behavior.

A follow-up comparison on the same date checked PR21 merge `c60eade2` against
`3adf778`. It changes documentation, comments and desktop help/log wording;
the grinder worker and La Marzocco protocol behavior are unchanged. No protocol
port is needed for this display work; `3adf778` remains its behavior baseline.
The Hub integration work fetched upstream again on 2026-10-17; main remained
at `c60eade2`. The optional LamaBuddy Hub v2 client is isolated in `display/` and
does not modify the upstream grinder/cloud feedback protocol. The publication
check fetched both remotes again: companion main remained `b4c1e51`, and upstream
main remained `c60eade2`; no additional protocol port was needed. The development
state and remaining acceptance work are tracked in [PROGRESS.md](PROGRESS.md).

The daily-controls publication check fetched both remotes on 2026-10-18.
Companion main remained `b4c1e51`; upstream main remained `c60eade2`, with no new
commits since the previous review. BBW, prebrew, power and standby reuse existing
LamaBuddy Hub v2 capabilities and do not change the grinder/cloud worker or the
`3adf778` protocol baseline. The unrelated repository histories stay separate.

## Schedule/history review, 2026-10-18

Fetched main at `b8f7dc28c0c00585ebe3cd574bcde5f783e412e5` after PR22 (tabs),
PR23 (settings) and PR24 (history) merged. Compared `src/protocol/machine.rs`,
`src/server/wifi_grinder.rs`, `control_api.rs` and the new settings/history code
against `c60eade2`:

| Upstream change | Companion decision |
| --- | --- |
| Editable shot window and grinder address, with worker owner guards | Keep the bounded 180-second shot window and imported endpoint/identity. The embedded worker is single-owner and has no runtime endpoint replacement API. |
| `brew_job` distinguishes E65/E80 jobs from E64/manual brews for history | The companion handles E65/E80 job IDs and persists a 32-task journal. Its new cup list reads Hub Cloud statistics and does not claim grinder delivery. |
| `reached()` moves into `post()` so any HTTP response clears unreachable state | Keep the companion's distinct protocol/authentication errors and existing capped reconnect policy. A transport response alone does not clear its semantic error. |
| History/settings endpoints and event-age/boot identifiers | Desktop API additions are not copied into the native firmware. Existing Hub APIs supply the requested schedule and coffee data. |
| New abort constants and history bookkeeping | Existing pending-result preservation, replacement and refusal regression coverage remains applicable; no new wire-format change was found. |

The protocol baseline remains `3adf778`; this is a reviewed divergence, not a
merge of unrelated repository histories. The native display work and separate
Hub schedule extension do not change the grinder result-delivery worker.
The publication check fetched both remotes again on 2026-10-18: companion main
remains `b4c1e51`, upstream main remains `b8f7dc28`, and there are no additional
upstream commits beyond this review.

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

The optional native display is a companion-only module, documented in
[TOUCHSCREEN.md](TOUCHSCREEN.md). It reuses the C worker through additive status
fields and the existing mode request; it does not fork the protocol worker,
change pairing, or replay ambiguous results. Local UI work remains on a feature
branch. Keep `origin` for this companion and `upstream` for Skamba's Rust
application. Fetch upstream and review changes against the recorded commit;
do not merge the unrelated repository histories:

```sh
git fetch upstream main
git log --oneline 3adf778c7f0a3803994cbcc5fe2af1f8ac8a5be1..upstream/main
git diff 3adf778c7f0a3803994cbcc5fe2af1f8ac8a5be1..upstream/main -- src
```

Port relevant protocol fixes with worker regression tests. A UI-only update
does not advance the protocol baseline or inherit earlier hardware acceptance.

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
- Native UI regressions cover confirmation timeout, late callbacks, rejected/stale
  steam switches and cloud-error colors. Hub contract tests cover measured versus
  target values, guarded presets, preserved doses, binding changes, authentication
  failures, missing telemetry, partial/unknown results and no-repeat POST behavior.
  Daily-control tests also cover BBW/prebrew/standby groups, input bounds, unknown
  modes, explicit power/standby confirmations and preserved-field readback.
  Provisioning tests verify Hub certificate identity/signature and private NVS
  generation without contacting an existing Hub, grinder or vendor cloud.
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
