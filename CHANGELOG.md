# Changelog

## Documentation - 2026-10-23

- Reorganize the project overview around the coffee workflow, native screenshots,
  feature summaries and board/model compatibility.
- Add Chinese and German introductions, a screenshot gallery and contributor guide.
- Move detailed lightweight setup into a focused companion guide; preserve the
  existing integrated and hardware-acceptance instructions.
- Identify the AMOLED development branch explicitly and distinguish native-screen
  language support, browser translations and build-specific field evidence.


## Unreleased - 2026-10-22

- Record the authorized 1.69-inch OTA of source `975ebc5`, build `b923fa91`,
  and physical scale off -> on -> off acceptance: false/null -> true/28% ->
  false/null, with 174 cached reads and no failures or reboot during 252 seconds.
  Grinder readiness, its success count and presets remained unchanged. No new
  cup was made; long-duration and other-board acceptance remain open.
- Fix integrated scale-state caching: publish fresh control preflight readings,
  refresh the accessory through one bounded Dashboard GET every 30 seconds while
  the machine is available, and expire observations after 60 seconds. Missing
  fields stay unknown; foreground controls retain priority, failures back off,
  and low internal memory postpones background reads. The later authorized OTA
  acceptance is recorded separately from the source-only fix publication.
- Add the reverse 30 g -> 20 g real cup on `c88791b2`: 20.1 g / 25.053 s,
  acknowledged grinder feedback, 192 successful samples and normal user completion.
  Each direction has one successful cup; repeated alternation remains open.
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
  security certification, reliable live extraction-start protection, E80,
  outages and endurance remain open.

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
