# Experimental ESP-Mosaico / ESP32-S31 profile

[Project overview](../README.md) · [Integrated application](INTEGRATED.md) · [AMOLED workflow](AMOLED216.md)

**Source:** `codex/s31-mosaico-port`. Select that branch before following this
guide. The earlier S3 AMOLED work remains on `codex/amoled-216-local-adaptation`;
the LCD and headless profiles are also available on `main`.

The `mosaico` profile runs the integrated Hub, grinder worker and 480 × 480
AMOLED interface on ESP-Mosaico. It shares the existing coffee controls, recipe
following, scale-state history, quick weights, local preferences and LAN OTA.
The board driver handles CO5300 display output, CST9220 touch, BMI270 rotation,
BQ27220 battery readings, ES8311 sound and USB CDC. Hardware support remains
experimental. Build `96c43cd7` completed one sleep/wake and new-cup return;
repeatability and endurance remain open as detailed below.

## Hardware and toolchain

Espressif specifies **16 MB PSRAM**, **16 MB SPI NOR flash** and a separate
**128 MB SPI NAND flash** for ESP-Mosaico. PSRAM is working memory; NOR holds
this firmware and its NVS configuration. The NAND is neither initialized nor
modified by this profile. See the [official ESP-Mosaico hardware guide](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s31/esp-mosaico/user_guide.html).

The BSP reads the CoreBoard revision from eFuse and selects the V1.0 or V1.2
pin mapping. That selection is implemented in source; it is not a claim that
both revisions have completed field acceptance. Matching resolution does not
make the S31 and S3 drivers or firmware images interchangeable.

| Dependency | Version used for this port |
| --- | --- |
| ESP-IDF | 6.2 development preview, revision [`4d80c1b29aeabc8e735ffc6e1be9bb23e95d1d5d`](https://github.com/espressif/esp-idf/commit/4d80c1b29aeabc8e735ffc6e1be9bb23e95d1d5d) |
| ESP-Mosaico BSP | 0.2.0, pinned revision [`7cbfb390638dcb93522fc3cde094b2d43cf907c2`](https://github.com/esp-mosaico/esp-mosaico-bsp/tree/7cbfb390638dcb93522fc3cde094b2d43cf907c2) |
| USB HS console | `esp-mosaico-claw` component, pinned revision [`2654a4a9f76f5d7ea108e4dff7ade7b1df4c1c5c`](https://github.com/esp-mosaico/esp-mosaico-claw/tree/2654a4a9f76f5d7ea108e4dff7ade7b1df4c1c5c/boards/esp_mosaico/components/usb_hs_console) |
| Application LVGL | 8.4.0; the BSP's LVGL integration is disabled |

Keep a separate ESP-IDF checkout and shell environment for S31. Select the
listed IDF revision, initialize its submodules, install its S31 tools and source
that checkout's `export.sh`. Existing S3 profiles continue to use ESP-IDF 5.5.4.
The S31 manifest accepts `>=6.2,<6.3`; the revision above is the validated
toolchain, not a promise that every newer development commit works.

Use separate source checkouts or worktrees for S3 and S31 builds as well.
`firmware/integrated/dependencies.lock` and `managed_components` are shared by
profiles within one source checkout; separate build directories do not isolate
them. Switching SDK/target can normally resolve and rewrite that shared
dependency state. **Do not build with both SDKs concurrently in the same source
checkout.**

## Build before running host checks

From `firmware/integrated`, with the S31 SDK exported:

```sh
sh tools/build-integrated.sh mosaico
sh tools/check-integrated.sh
sh tools/check-crypto.sh
sh ../../tools/check.sh
git diff --check
```

The wrapper passes `--preview`, selects `esp32s31` and `sdkconfig.mosaico`, and
uses `build-integrated-mosaico`. **Run the first build before host checks**:
IDF 6 supplies cJSON through a managed component, and the checks need the
sources downloaded by the build. These commands do not contact hardware.

Host checks need a C compiler, CMake and OpenSSL; the root checks also use
Node.js. `check-crypto.sh` builds the exported SDK's real Mbed TLS libraries in
a temporary directory, verifies the production identity and Cloud signing
paths, and removes that directory afterward. Run it under each SDK to cover
both the S3 and S31 implementations.

The application is generated as
`build-integrated-mosaico/lamabuddy_esp32_s3_hub.bin`. The inherited filename
does not identify the target: always select the Mosaico build directory and
verify the target in its build metadata. **Never flash or OTA an S3 image to
S31, or an S31 image to S3.** Keep their build directories and SDK environments
separate.

Public builds use the synthesized milestone cues. The existing
`GBS_CUE_PCM_HEADER` option also supports a local PCM header; custom sound data
is not required to build and is not included in this repository. The tested
application's digest below identifies that exact artifact, not every rebuild
from the same source.

## First USB installation and partition layout

Before changing the intended board, save a private backup of its complete
16 MiB NOR flash and record the backup's size and SHA-256. Retain the backup,
previous firmware and exported configuration outside Git. Do not assume that
a factory image or another board's NVS is a usable rollback for this device.

[`partitions.mosaico.csv`](../firmware/integrated/partitions.mosaico.csv) reserves
three **5 MiB** application slots. The previous S3 layout cannot accommodate
the validated S31 application.

| Partition | Offset | Size |
| --- | --- | --- |
| `nvs` | `0x9000` | `0x15000` |
| `otadata` | `0x1e000` | `0x2000` |
| `phy_init` | `0x20000` | `0x1000` |
| `factory` | `0x30000` | 5 MiB |
| `ota_0` | `0x530000` | 5 MiB |
| `ota_1` | `0xa30000` | 5 MiB |
| `nvs_keys` | `0xf30000` | `0x1000` |

The first installation requires USB and the matching S31 bootloader, partition
table, OTA metadata, application and first-install NVS. Use the generated
`flash_args` and IDF flash command for this build, rather than reusing S3
offsets. Application-only OTA cannot install or change this partition layout.
The development profile is not a certified Secure Boot / Flash Encryption
deployment.

The onboard Type-C port uses USB HS OTG. This application includes CDC logging
and automatic download support. If no usable CDC console is running, follow
Espressif's BOOT/power procedure to enter ROM download mode. A reset can briefly
disconnect USB. See the [official USB instructions](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s31/esp-mosaico/user_guide.html)
for the board procedure.

## Retiring an existing bridge

One grinder pairing must have **one active owner**. Stop the old bridge before
the new device uses its Cloud installation or grinder identity, and keep the
old bridge off after handover. Do not resume a cup that was in progress during
the move.

The [integrated first-install workflow](INTEGRATED.md#first-installation-and-identity-handover)
applies with `prepare-integrated-nvs.py --profile mosaico`. The importer prepares
private files without flashing. Its Cloud/grinder options preserve the selected
machine, Cloud installation and grinder pairing, create a fresh local API token,
and start the grinder worker in **monitor** mode. The grinder's paired
`brew_device_id` is distinct from the new Hub's identity: leave `hub_id`,
`tls_key`, `tls_cert` and `tls_schema` absent together so the new Hub generates
its own complete identity.

The importer does not automatically migrate every setting. A complete handover
must separately restore or configure the machine's BLE authentication/pairing,
recipe owner password and recipe-follow preference, and desired display
brightness, rotation, volume and variable-weight preference. Keep those values
private. Do not substitute diagnostic counters or cached screen values for a
saved configuration or journal.

Wait for network/authentication readiness and a fresh grinder task baseline
before explicitly enabling active mode. Existing tasks in the initial list are
recorded as seen, not brewed again. A new task obtains a fresh coffee-record
baseline before accepting a result. Pending execution state and ambiguous
outcomes must not be replayed. If no current persistent journal was available
for migration, old success counts/history are not reconstructed and historical
task IDs outside the new baseline cannot be claimed as permanently deduplicated.
Validate the handover with a genuinely new cup.

LamaBuddy App/Watch integration has not been field-validated on S31. During
App setup, select the Mosaico Bonjour entry or configure the correct endpoint
at `lamabuddy-mosaico.local`; do not retain the older `lamabuddy-hub.local`
default when connecting to this board.

## Application OTA

After the correct layout is installed, use the integrated authenticated
[LAN OTA workflow](INTEGRATED.md#lan-updates-and-observation) with the Mosaico
application and the new device's current token. The default model is
`lamabuddy-esp32s31-mosaico`, and the default hostname is `lamabuddy-mosaico`.
Always verify the selected target before uploading.

Application OTA updates an application slot and preserves NVS configuration,
identity and journal. **Do not rerun the first-install NVS import for an
update.** Wait for idle admission; after restart, verify the running partition,
build ID and health. If an upload response is lost, reconcile device status
before retrying. Keep a known previous application and private configuration
backup available.

## Display, input, battery and sound

With USB on the right, the screen reads upright. The S31 driver applies its own
quarter-turn calibration to display and touch while retaining the physical USB
direction for the UI and BMI270 policy. Automatic rotation waits for stable
gravity; a held touch, motion or flat placement retains the current direction.
The AI button provides home on a short press while awake. Holding it for about
1.5 seconds immediately blanks the screen and plays the existing failure cue
as sleep feedback; releasing the button is not required to enter deep sleep.
A separate short press wakes the screen, plays the existing success cue once,
and restores the saved bridge mode. Both cues respect the saved volume/mute.
If the sleep press is still held, deep sleep first arms for release; that
release is consumed silently before display/network/worker startup and rearms
for the next press. BOOT toggles rotation while awake but
cannot wake deep sleep (GPIO61 is not an RTC IO). POWER remains the board's
physical power control.

Manual sleep is rejected during a grinder job, queued/running control or a
known active brew. Unsaved edits retain the existing discard confirmation.
Admission reserves both the grinder and command coordinator, preventing a new
job or control from starting during shutdown. Sensor polling and sound finish
before the panel enters standby, the controllable peripheral rails turn off,
and the MCU enters deep sleep. Wi-Fi, BLE and automatic linking are unavailable
until a short wake restarts the normal boot path. Automatic display
dimming remains display-only; it does not put the bridge to sleep.

Fresh display preferences are 60% brightness, automatic dimming off, automatic
rotation off, sound muted and a 40 g variable preset alongside 20 / 30 / 36 g.
Saved preferences take precedence. The shared AMOLED workflow describes the
coffee and scale-state UI, but its AXP2101/QMI8658 hardware details belong to
the S3 board.

Mosaico reads BQ27220 presence, current, operation status and state of charge.
The gauge's unconfigured ROM profile has a **3000 mAh** design capacity, which
must not be treated as a calibrated Mosaico battery. At boot, that default is
replaced using the pinned BSP's battery initializer. The BSP's 80 mAh profile
is verified again on subsequent boots: matching parameters skip rewriting and
retain the learned full-charge capacity; an interrupted update is repaired.
Other existing board-specific design capacities are preserved. Initialization
failure leaves the displayed percentage unknown.

The pinned BSP describes its profile as an interim characterization of two
80 mAh cells at 25 C; the public hardware guide lists a nominal 65 mAh battery.
This corrects the 3000 mAh mismatch, but it is not a per-device capacity
calibration or a guarantee of percentage accuracy across a full discharge.
Only the gauge's RAM configuration is changed; OTP is not programmed and the
TP4057 charging circuit is not reconfigured. See the
[BQ27220 reference manual](https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf) and
[pinned BSP profile](https://github.com/esp-mosaico/esp-mosaico-bsp/blob/7cbfb390638dcb93522fc3cde094b2d43cf907c2/components/esp-mosaico-bsp/onboard/battery.c).

Unknown or unready readings remain unknown. A charging indication
requires positive measured current and consistent gauge status; USB power alone
does not establish charging. Failed reads retain a valid observation for at
most five seconds. The coffee scale's battery state is a separate Cloud-derived
observation.

The authenticated display diagnostics include voltage, instantaneous/average
current, remaining/full/design capacity, raw operation status and wake causes.
These are cached sensor observations, not external-power detection or a
measurement of current while the MCU is asleep.

ES8311 audio runs in its own worker with the existing edge-triggered milestone
policy. A 0–100% setting controls PCM amplitude; mute suppresses queued cues.
After a cue, the driver drains playback, mutes and closes the codec, and turns
off the PA while retaining the BSP-owned handles for reuse. Microphone capture
is not enabled. A success cue reports the existing command/receipt outcome; it
does not independently prove physical machine or grinder action.

## Wake-clock regression found on 2026-11-01

Read-only capture of `73db8eca` preserved the actual timeout screen and a
`canceled` job outcome before restarting or updating. Grinder and Cloud were
connected, recipe following had reached `applied`, and the measured coffee
(29.6 g / 37.422 s) was already present. The device clock was about 1,606 seconds
ahead of the host. The live state therefore distinguishes a missing result
match from a failed feedback transport; no successful result was acknowledged
for that job. Earlier user-reported failures did not retain separate captures.

The Cloud startup path previously treated any RTC year >= 2025 as synchronized,
skipping SNTP initialization entirely after a retained-clock wake. Job acceptance
then stored the fast local time as a persistent coffee watermark, rejecting a
newly measured cup as older than that boundary until the job timed out. ESP-IDF
documents RTC retention and sleep-clock drift in its
[S31 system-time guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/system/system_time.html).
The exact hardware cause of the accumulated RTC offset was not measured.

The repair requires an actual SNTP completion on each boot before Cloud startup
can succeed. Only a fresh job with a successful current Cloud baseline may
rebase a stored watermark ahead of synchronized time, including offsets below
two minutes. The new boundary is still at least the freshly read Cloud coffee
timestamp and current synchronized time. Seen job
IDs remain intact, baseline cups are excluded, and old failed jobs are not
replayed. The cached grinder diagnostics now include `watermark_ms`. Offline
tests reproduce both the skipped sync and retained-future-watermark failures,
and cover sync timeout/init failure, same-boot reuse and old-job/result dedup.

The clock-only precursor `7fc6f84b` was OTA-installed to `ota_0`. Its authenticated
readback had a host-aligned clock (within the cached snapshot age), while the
preserved watermark was still about twelve minutes in the future. This directly
confirmed the persisted bad boundary without clearing the journal or replaying
the canceled job. New-cup validation remains pending.

### Battery percentage hidden after an initialization failure

The same preserved `73db8eca` screen showed `-` for device battery. Cached gauge
reads were valid: 4,026 mV, +61 mA, 61/80 mAh and SOC 77%, but
`pmuPercentKnown` was false. The one-shot profile check had failed and left the
percentage disabled for the entire boot, even after ordinary reads recovered.
Restarting the clock-only precursor recovered verified/sealed profile state and
85% without changing the battery code. The precise initial I2C/profile-check
failure was not retained in the live serial capture.

Profile verification now makes at most three attempts, with a one-second pause
between failures. Successful full verification still preserves the BSP's
learned-capacity behavior; persistent failure keeps the percentage unknown.
The cached `batteryProfileReady` and `batteryProfileAttempts` fields distinguish
startup verification failure from stale or failed ordinary samples. Offline
tests cover a transient profile failure, a transient register read, bounded
permanent failure and preservation of a pre-existing 65 mAh profile.

The combined application was OTA-installed to `ota_1`:

| Artifact field | Value |
| --- | --- |
| Build ID | `5a4ad188` |
| Application size | 4,453,520 bytes |
| SHA-256 | `023c85bfb1f5e3b4d9d18405d4ef4e13a89c02fe6dcb9e20d30aeaf56b9e00d5` |

The S31 build, full integrated host suite and diff check passed; root host
checks passed during the clock repair and root application code is unchanged.
At 30 seconds after OTA restart, Cloud authentication, grinder readiness and
baseline completion were confirmed. The cached clock was within 0.02 seconds
of the host sample; battery showed 92%, a verified profile on attempt 1 and
sealed operation status 166. This was an ordinary restart; wake/cup acceptance
was checked separately below.

The user then held AI to sleep, released it, waited about ten seconds and
short-pressed AI to wake while keeping USB connected. The capture observed
online -> offline -> `resetReason: deepSleep` with EXT1 wake cause 8. Serial
evidence recorded a new SNTP completion at 6.6 seconds, full battery profile
verification without rewriting learned capacity, and grinder baseline
completion at 16.6 seconds. The ready readback showed 94%, attempt 1, sealed
status 166, one success cue and no audio output errors. This physical cycle
did not force a battery initialization failure; retry recovery remains an
offline fault-injection result.

The new-cup check on `5a4ad188` **failed**: a new grinder job was accepted, its
30 g recipe was applied, and Cloud supplied 30.0 g / 24.664 s. Its persisted
pre-upgrade watermark was still about 87.7 seconds ahead at admission. The
precursor only rebased offsets exceeding two minutes, so the actual cup was
ignored and the job timed out. The success counter stayed at 10. This is not
recorded as successful cup acceptance.

At the user's explicit request, the ESP result writer was moved to monitor
mode before one manual `brewDone` attempt with that job's measured result.
The grinder returned HTTP 400 / `ClientErrorOngoingJobNotExisting`; a subsequent
read-only `brewJobs` query returned no jobs. The manual delivery was refused,
not ambiguous or successful, and did not trigger a confirmed adjustment. The
expired job was not recreated or replayed against another grind.

The follow-up removes the inherited-watermark tolerance and adds regression
cases at 1 ms, 60 s, the observed 87.67 s, 120 s, 120.001 s and 1,606 s. A
future cup present in the fresh Cloud baseline remains excluded, and old job
IDs remain in the no-replay journal. The S31 build and full integrated host
checks passed for the follow-up:

| Artifact field | Value |
| --- | --- |
| Build ID | `400a0f33` |
| Application size | 4,453,520 bytes |
| SHA-256 | `6e75e1be99b98996881af5c4bc3c411b314ae807402612b987b1c14186835017` |

Protected application OTA installed `400a0f33` to `ota_0`; health recovered in
2.197 seconds after the three-second restart settle. At 45 seconds uptime,
readback verified this build, authenticated Cloud, restored `active` mode,
completed grinder baseline, no pending job/result and a clock within 0.09 s of
the host sample. Battery was known at 100%, profile attempt 1 and sealed status
166. The success counter remained 10; neither the failed cup nor the rejected
manual delivery was converted into success. Exact-build new-cup acceptance is
still pending. The user chose to defer it to the next normal cup and will notify
the operator before grinding. Before that cup, verify build identity, synchronized
time, completed Cloud/grinder baselines and battery readiness, then start the
read-only capture. Check the new job, recipe application, measured Cloud result
and explicit grinder acknowledgement separately; do not use either earlier
failed cup as success evidence.

## Wake readiness on the home screen

The native 480 x 480 home now uses its main status area for boot/wake recovery
until the first new cup. It shows clock synchronization, Cloud/grinder recovery
and a green linkage-ready state from actual runtime evidence. A failed SNTP
attempt remains visibly incomplete while the existing refresh path retries.
The diagnostic `clock_status` is `syncing`, `failed` or `synchronized`; a
plausible retained RTC date is not sufficient for the last state.

Readiness requires the current boot's completed clock synchronization, active
mode, Wi-Fi, Cloud and grinder health, successful storage and completed task
baselines. Pairing/storage errors and monitor mode do not become ready. The
old cup's outcome stays in the ledger and detail view; a current-boot job or
completed result takes precedence over the recovery display. No result is
changed into success by reconnecting. Steam preheat and Bluetooth remain
independent indicators, and no additional machine commands are sent by this UI.

The implementation reuses the existing status area, icons and body font. Only
the 28/30 px headline subsets gain the new characters. Offline checks cover
clock failure/retry evidence, readiness prerequisites, preservation of earlier
outcomes, new-cup precedence, glyph coverage and two-line layout above the
weight buttons. The integrated host suite, native AMOLED UI suite and S31
build passed. Hardware wake/cup acceptance is recorded separately below.

| Artifact field | Value |
| --- | --- |
| Build ID | `96c43cd7` |
| Application size | 4,460,352 bytes |
| SHA-256 | `51e784b5f23028b233857b44f46f6e92b3e39a04734af1324fc0e897270a7058` |

Application-only OTA to `ota_1` completed with the exact byte count in 37.047 s.
Health recovered in 4.171 s after the three-second restart settle, and the
running build ID matched. Root host checks and `git diff --check` also passed.
The actual screen showed the green readiness message without replacing the
preserved ledger outcome or incrementing the success counter.

On this exact build, the user then held AI to sleep, released it, waited ten
seconds and short-pressed AI to wake. Read-only capture observed offline ->
`resetReason: deepSleep`, EXT1 wake cause 8, clock `syncing` at four seconds and
`synchronized` by seven seconds. Real frames captured network recovery, linkage
recovery and green readiness; the separate clock-in-progress/error layouts were
checked in the native simulator, not induced as physical network faults. By
27 seconds, Cloud/grinder baselines were complete, active mode had returned,
the cached clock was within 0.2 s of the host, battery was known at 100% with
profile attempt 1, and one wake success cue with zero audio errors was recorded.
The user confirmed the final readiness screen. No cup had started during the
installation/wake check, and the success counter remained 10.

The subsequent real cup on this same build/wake completed the full return
workflow. One new job was observed with a 30-second target; its matching recipe
specified 20 g and reached `applied`, and the machine's scale was connected.
The actual home switched from readiness to the cup's five-step progress view.
Cloud supplied a new measured result of **19.4 g / 38.148 s**, timestamped
89.347 seconds after the job's admission watermark. The explicit grinder `Ok`
receipt path set `last_outcome: success`, cleared the pending job and advanced
the success counter from 10 to 11. The first captured success was about 11.7 s
after the Cloud record's timestamp; this is the observed Cloud/return latency,
not an isolated network round-trip measurement.

The retained result identity matched the new job. A real framebuffer showed
all five steps complete and the returned **19.4 g / 38.1 s** values, and the
user confirmed the success screen. No manual return or replay was performed;
the prior failed cups were not counted. This proves one exact-build sleep/wake
and new-cup return, not multi-day repeatability, physical burr adjustment or
battery endurance. Private captures were stopped after verification.

## Earlier field evidence as of 2026-10-31

The preceding power-management application was:

| Artifact field | Value |
| --- | --- |
| Build ID | `73db8eca` |
| Application size | 4,453,184 bytes |
| SHA-256 | `1651aa2aa16981cf152e2f8dc587eaf468fd79b7bd71233df64144aa9a9cee2a` |

On **`73db8eca`**, application OTA to `ota_1`, restart, the S31 build and the
full integrated host checks passed. The post-boot gauge reported 100%, 4,188 mV,
zero battery current and 80 mAh design/full/remaining capacity. Ordinary OTA
restart remained silent (zero played-cue counters), preserving the saved 53%
volume. Offline tests cover sleep before button release, silent release
rearming, short-press wake, cue selection, mute, playback failure and busy/failed
sleep admission. The user confirmed all three physical checks: screen-off and
failure cue before releasing a held AI press; continued screen-off after
release; short-press screen-on with the success cue. Readback confirmed
`resetReason: deepSleep`, EXT1 wake cause, one success cue and zero audio
errors. Cloud authentication and the saved `active` mode returned, with battery
still at 100%. The grinder baseline was still pending at the final readback;
this does not establish a completed grinder reconnection or new-cup workflow.

Before the repair, read-only instrumentation (`31da9dac`) reproduced **11%**
with 4,171 mV, 322 mAh remaining and 3,000 mAh design/full capacity. After the
repair, `d83287f8` read **100%**, 4,200 mV and 80 mAh design/full/remaining
capacity. The two instantaneous charging-current samples were +61 and +34 mA.
These readings verify the default-profile mismatch and its correction; they do
not measure full-cycle capacity or deep-sleep current.

On **`d83287f8`**, application OTA to `ota_0`, reboot, the S31 build, integrated host checks and
root host checks passed. Offline tests exercise profile read/update failure,
preservation of an existing 65 mAh profile, short/long wake, busy admission,
shutdown ordering and peripheral timeout/failure. The user confirmed long-press
screen-off and subsequent long-press wake. Readback reported `resetReason:
deepSleep`, EXT1 wake cause, restored Cloud authentication and 100% battery.
This predecessor used long-press wake and release-to-sleep; the current build's
revised timing and feedback require separate acceptance. Extended battery
runtime remains unmeasured.

The preceding history-repair application was:

| Artifact field | Value |
| --- | --- |
| Build ID | `2c3f4497` |
| Application size | 4,412,800 bytes |
| SHA-256 | `e329d61c4f094b6587b2b0303b142fb008090b20555ff6b8c8e9e19cfc00dca2` |

On **`2c3f4497`**, LAN OTA and restart succeeded with the expected image running
in `ota_0`. The first history refresh completed in 13.4 seconds and returned
15 Cloud records. The actual framebuffer showed the three-row list and page
`1 / 5`; the user also confirmed the records were visible on the device.
This validates existing-record display, not a new extraction or grinder result.

The repair preserves the local adapter's `202` loading state, retries transient
Cloud IO lock contention within the existing attempt limit, and retains valid
coffee records with `trend: null` when the chart cannot be read completely.
The integrated and root host suites and S31 build passed. Offline production-code
tests cover trend timeout/malformed responses, missing records, bounded lock
retry and foreground priority. All three statistics endpoints succeeded in the
hardware run; forced trend failure was tested offline only.

On the preceding **`8d4984d3`**, LAN OTA, restart and health recovery succeeded; Cloud
authentication and paired-grinder readiness were observed. A 100-second
observation captured **four automatic BLE temperature updates**; coffee and
steam current temperatures remained present with fresh cached data. This
version resumes the normal 30-second poll after a successful BLE worker
recovery; it retains the 45-second cache validity limit.

On the preceding **`0436954d`** application, the user confirmed four physical
screen orientations and touch behavior. Audible sound was confirmed during an
earlier installation stage. Those drivers did not change in the final BLE
recovery revision, but these earlier checks are not reported as repeated manual
acceptance of `8d4984d3`.

Still open on S31: repeated cup workflows, end-to-end BLE writes, prolonged
stability, battery runtime and full power-loss recovery. One real new-cup
workflow through grinder feedback passed on `96c43cd7` as recorded above.
More PSRAM increases capacity; it does not establish twice the speed.
The current display still uses a single eight-row DMA buffer and 40 MHz QSPI.
Comparable control latency, frame rate, sustained load and endurance have not
been measured. Build/host checks and earlier S3 cup evidence do not close these
S31 hardware gates.
