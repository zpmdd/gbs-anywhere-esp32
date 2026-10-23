# Changelog

## Unreleased - 2026-10-23

- Add the experimental 2.16-inch AMOLED profile with a 480 x 480 interface,
  USB-C-right orientation, optional automatic rotation, matched touch mapping,
  physical buttons, battery sessions and authenticated LAN OTA.
- Prioritize grinder linkage and machine status, followed by quick weights and
  compact boiler controls. Add independent boiler warm-up estimates using the
  existing Cloud snapshot.
- Show scale state and battery in one icon, retain the last valid percentage
  across disconnection and ESP power cycles, and keep the device's own battery
  separate. Scale observations never gate quick weights or grinder feedback.
- Improve display responsiveness and foreground command scheduling; add bounded
  HTTP connection reuse and per-stage timing without replaying ambiguous writes.
- Add clear success, failure and neutral cues at cup milestones, with bounded
  audio queues, duplicate suppression and no startup replay. Preserve valid PMU
  readings through brief failures without extending their freshness.
- Fix manual weight feedback after a completed cup so old measurements cannot
  mask a new setting or appear as a requested preset outline.
- Record user-accepted cup and audio validation on `f8b54b1a`; the subsequent
  display fix `211d2963` passed local regression and OTA/startup checks. Physical
  cross-cup manual selection on that build and cold-start boiler countdowns
  remain pending. See [AMOLED validation status](docs/AMOLED216.md#validation-status-2026-10-23).

- Publish the complete integrated ESP32-S3 Hub, grinder and optional touchscreen
  application in `firmware/integrated`. A clone of this public repository now
  contains the runtime source needed for the integrated feature set; no private
  Hub checkout or second ESP is required for that profile.
- Add ordinary grinder ZIP/CSV recipe lookup after fresh job acceptance, exact
  UUID matching and guarded automatic target-weight selection during puck
  preparation. Keep manual cancellation, short deadlines and no ambiguous retry.
- Include PSRAM allocation fixes, fresh preset/online preflight, persistent dose
  selection, USB-C-right touch mapping, steam-level home display, shared Cloud
  history, authenticated LAN OTA and real framebuffer diagnostics.
- Preserve the lightweight companion and external-Hub touchscreen profiles.
  Port null/missing coffee-list handling and sub-millisecond target rejection
  to both C parsers; review the Rust upstream through `ebaac65`.
- Record one successful 20 g -> 30 g automatic-follow cup on source build
  `c88791b2`: 30.2 g / 27.696 s, acknowledged feedback and user-confirmed physical
  adjustment. Publishing or rebuilding this source is not another hardware test.
- Add integrated CI and board-specific first-install NVS sizing. Production
  security certification, reliable live extraction-start protection, reverse
  20 g following, E80, outages and endurance remain open.

## 0.2.1 - 2026-10-17

- Check the C port against merged gbs-anywhere main `3adf778`, including PR15's
  final `13e4a05` fixes. Point initial setup at the published nightly while the
  latest desktop stable tag still predates E65 support.
- Require 60 seconds of consecutive pairing refusals before showing an action
  message; timeouts interrupt that period. Preserve the imported identity.
- Keep a pending result after lost acceptance replies and an empty task list.
  Absence expires it only after an explicit missing-ongoing-job response;
  reacceptance clears that old absence. Ambiguous result delivery is still not
  automatically replayed.
- Explain that an export with only `refused_pairing_id` needs desktop recovery
  before import. Add worker and provisioning regression tests for these paths.

## 0.2.0 - 2026-10-16

- Review the bridge against PR15 `8f398c5` and PR16 `ba00c92`, retaining immediate
  task acceptance and reacceptance while waiting. Reconcile a lost accept reply
  from the next job list and stop a result when its task is no longer available.
- Track grinder and cloud failures independently, clear current errors after
  recovery and retain the last failure/recovery details for the current boot.
- Bound ordinary reconnect waits to five seconds for grinders switched off daily;
  show an explicit pairing problem after three refusals without erasing identity.
- Add accessible colored state badges, localized explanations, delivered vs cloud
  measurements, timestamps, timeout countdown and connection diagnostics.
- Stop showing cached connections as live when the page cannot reach the S3.
- Test the real worker with in-memory services, alongside protocol, provision,
  serial and eight-language UI tests. Preserve the NVS layout and task ledger.

## 0.1.0 - 2026-10-16

- Split the experimental ESP32-S3 firmware from gbs-anywhere PR18 into a
  separately maintained companion repository, preserving source attribution
  and the AGPL-3.0-or-later license.
- Check local ports 80 and 8089 before activation; a custom desktop port adds
  another check. Refuse activation on an open port or an unknown check result
  before opening USB.
- Document private Docker settings/pairing exports and the need to save the
  La Marzocco integration in the desktop app before migrating.
- Add port-ownership and saved-integration regression tests plus ESP-IDF CI.
- Clarify precursor real-cup results and the portable build's pending hardware
  validation. No hardware-validated firmware release is published yet.
