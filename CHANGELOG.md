# Changelog

## Maintenance verification - 2026-11-09

- Consolidate the pending S31 smart-steam, confirmation-recovery and official-app
  reminder changes with their fonts, diagnostics, tests and field documentation.
- Re-run integrated/root offline checks, crypto checks, the three native AMOLED
  CTest checks and the S31 build successfully before publication. No new flashing
  or real-cup test was performed; earlier field results remain build-specific.

## Explicit Cloud policy reminder - 2026-11-06 (unreleased)

- Recognize the documented `USER_MUST_ACCEPT_POLICY` code from a complete,
  authenticated Cloud rejection and show **Confirm the user agreement** on
  AMOLED. A single explicit code is enough; unrelated 403/412 responses retain
  the existing suggested official-app check without claiming a policy update.
- Keep the code observation bounded and temporary, clear it on another complete
  authenticated response or authentication result, and preserve active-cup
  outcomes. Do not store response bodies, accept agreements or add requests.
- Expose `policyAcceptanceRequired` in Cloud diagnostics and
  `cloud_policy_required` in grinder/display state. Integrated/root host checks,
  native AMOLED rendering/tests and the S31 build pass. This source change has
  not been installed or verified against a fresh real policy rejection.

## Official app recovery hint - 2026-11-03 (unreleased)

- Show an actionable AMOLED home hint to open the official app, check updates
  and confirm its agreement after repeated paired Cloud access rejections.
  Require Wi-Fi, synchronized time and a successful login; a single 412,
  ordinary network errors and coffee-machine power-off do not qualify.
- Clear the hint on normal readback, a new authentication result or expired
  evidence. Preserve active-cup and delivery outcomes. This suggests a check;
  it does not claim that a terms update is the only possible cause.
- Install `a8cbc2db`: normal Cloud/grinder readiness and an absent hint were
  verified on hardware, with no new control command. Rejection detection,
  clearing and the rendered warning were tested with synthetic evidence.

## S31 steam confirmation recovery - 2026-11-03 (unreleased)

- Clear an unknown smart-steam outcome when a later fresh, online observation
  matches the current time window, including a journal restored after restart.
  Preserve manual priority and never replay the uncertain steam command.
- Reject observations preceding the current boot's command when reconciling
  its result. Stale, missing and offline observations remain unconfirmed.
- Let dashboard refresh wait briefly for shared grinder/statistics IO, while
  preserving foreground command priority and bounded contention backoff.
- Install build `a8ec00d5` and verify that a restored unknown result becomes
  confirmed from fresh Cloud state, with no new steam command. The actual home
  screen shows linkage ready and steam disabled. This boot separately waited
  about 80 seconds for network time; no new cup was tested on this revision.

## S31 smart steam - 2026-11-02 (unreleased)

- Add opt-in Beijing-time steam selection at startup/wake: off during
  07:00-12:00 and on during 14:00-20:00, preserving the selected steam level.
- Show automatic progress and confirmation in the home steam area and add a
  native settings switch. Other hours keep the current setting.
- Require fresh clock and boiler evidence, share the command coordinator,
  preserve manual choices for the current window and journal before sending so
  sleep/reconnect cannot replay an uncertain write.
- Support the same automation outside Bluetooth range with bounded Cloud reads,
  explicit steam/machine-state parsing and fresh confirmation after the command.
- Cover the policy, runtime adapter, manual priority, readback and UI offline.
- Install build `e1fbb5b5` and validate one morning transition with Bluetooth
  disconnected: one Cloud command, steam on -> off, level 3 preserved and fresh
  readback confirmed on the actual home screen. Afternoon hardware acceptance
  and repeated-cycle validation remain open.
- Validate two consecutive 30 g cups on the same build: 30.1 g / 26.476 s and
  30.2 g / 32.762 s, both with recipe synchronization and grinder-acknowledged
  returns; success counter 11 -> 12 -> 13, with no manual replay.
- Check HTTP route capacity at compile time so added board-specific endpoints
  cannot produce a firmware image that aborts during route registration.

## S31 sleep, recovery and feedback - 2026-11-01

- Make the AI-button hold enter deep sleep at the hold threshold, with a failure
  cue; a separate short press wakes with a success cue and restores saved mode.
  Active cups and queued controls prevent sleep.
- Recover the Mosaico battery gauge from its unconfigured ROM profile, verify
  the BSP profile with bounded retries and expose battery diagnostics.
- Require a fresh SNTP completion after every boot and repair inherited future
  coffee watermarks when accepting a fresh job, preserving the no-replay journal.
- Show clock synchronization, linkage recovery and readiness on the native
  home screen; retain previous cup outcomes in details and prioritize new cups.
- Validate application build `96c43cd7` on S31 through a physical sleep/wake
  cycle and one 20 g recipe-follow cup: 19.4 g / 38.148 s, acknowledged return,
  success counter 10 -> 11 and user confirmation. No manual return was needed.
- Publish synthetic native-renderer recovery previews and exact-build evidence.
  Repeated cups, physical burr adjustment and battery endurance remain open.

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
