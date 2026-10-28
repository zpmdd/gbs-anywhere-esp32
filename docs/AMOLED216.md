# Experimental 2.16-inch AMOLED profile

[Project overview](../README.md) · [Compatibility](COMPATIBILITY.md)

**Source:** current maintenance is on
[`codex/s31-mosaico-port`](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/s31-mosaico-port),
which retains this S3 profile and adds a separate Mosaico profile. The earlier
S3-only adaptation remains on
[`codex/amoled-216-local-adaptation`](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/amoled-216-local-adaptation).
Neither board adaptation is on `main`. For the current source:

```sh
git fetch origin
git switch --track origin/codex/s31-mosaico-port
```

This profile targets the Waveshare ESP32-S3-Touch-AMOLED-2.16. It shares the
integrated Hub, grinder workflow, command admission and data collections with
the existing LCD and headless profiles. It does not identify a portafilter or
replace the grinder's recipe selection. Installation and on-device debugging
have been authorized for the current board. This remains experimental:
exact-build evidence and outstanding checks are tracked in the private field
report; partial hardware results do not establish complete acceptance.

For the ESP-Mosaico ESP32-S31 board, use [MOSAICO.md](MOSAICO.md). It shares
the UI and workflow, but uses different board peripherals, 16 MB PSRAM,
ESP-IDF 6.2 preview and its own flash layout. The S3 pinout, power-control
details and hardware results below apply only to the Waveshare 2.16-inch board.

## Interface and device state

The 480 x 480 home screen prioritizes grinder linkage and machine status,
followed by four weight presets and compact boiler/steam controls. A scale
silhouette contains the scale percentage. A separate battery silhouette is
reserved for this device's own battery. The scale percentage comes from Cloud
dashboard data; the device percentage comes from its local AXP2101. Neither
source substitutes for the other, and they have independent freshness and
connection flags. Percentages in design previews are synthetic examples.

Scale data uses the successful selected-machine Cloud dashboard observation.
The BBW `scaleConnected` boolean takes precedence, with `thingScaleConnected`
used only when it is absent or not a boolean. The two fields retain their
distinct upstream meanings; the UI follows the shared cache's connection rule.
Percentages are strictly 0..100: 1 means 1%, not 100%. AMOLED connection
observations expire after five minutes; legacy boards retain 60 seconds.
Disconnected or unavailable does not prove the scale is physically powered off.
The scale body is green only with recent online evidence. A valid last-known
battery percentage retains its separate one-hour freshness color inside the gray
scale icon after disconnection, expiry or an ESP power cycle. With no valid
history it shows a dash. The detail distinguishes the last reading from current
connection state; an unavailable wall clock does not produce an invented age.

Battery history is local to this AMOLED display and scoped to the selected
coffee machine. Apart from the AMOLED freshness window, it preserves the API's value/null
semantics, control admission, success results or grinder workflow. The current upstream
snapshot has no reliable independent scale identifier: after changing the
physical scale on the same machine, the old percentage remains historical until
a new valid observation arrives. Restore never claims the scale is online.
A versioned NVS record saves the first valid observation while idle, then
coalesces changed percentages with at least 60 seconds between saves. Repeated
polls or unchanged percentages do not write flash. A sudden power loss before
a deferred save can restore the previous committed value.

Scale fields reuse successful Dashboard reads, including the preset safety read
before a control. The AMOLED keeper checks for another accessory observation
about three minutes after the last one, or initially after 90 seconds when none
exists. Failed network attempts use the same three-minute backoff; reads rejected
before IO retry after 30 seconds. Ordinary observations postpone the next read.
Cloud/token refreshes, interactive work and its completion grace period, grinder
jobs, maintenance and resource pressure retain priority. A single in-flight TLS
read cannot be preempted by a later control. UI cache reads add no Cloud request.
A green scale icon means recently observed online, not real-time power detection.
Explicit offline or expired observations turn the scale body gray. The percentage
is colored independently: a battery observation less than one hour old is green
(or amber at 20% or below), while older or unknown-age values are gray. This also
applies to retained readings after reboot; a colored percentage never claims the
scale is online. No extra caption is added to the small home icon.

During warm-up, each home-screen boiler control replaces its primary temperature
or steam-level value with large amber remaining minutes, a clock and a warm-up
label. Normal one- and two-digit estimates use 45 px numerals; longer estimates
fit within the same control. The two boilers return to their normal displays
independently. Each clock, numeral and minute-unit group is horizontally centered
within its boiler control. The unit follows the numeral width with an 8 px gap
and shares its baseline. Minutes round up while the corresponding Cloud ready time is in
the future. This reuses `coffeeBoilerReadyTime`,
`steamBoilerReadyTime` and the source `lastUpdated`; UI ticks never issue a
Cloud request or renew the snapshot. Estimates require a valid wall clock,
a connected machine and a snapshot less than five minutes old. Standby,
disabled steam, malformed timestamps, estimates over a day away and stale
snapshots hide the countdown. When it ends, the normal temperature display
returns; elapsed time alone never declares a measured boiler ready.

On AMOLED, a cached offline or incomplete snapshot and an unfinished boiler
estimate cap the existing background refresh interval at two minutes. Initial
5/15/30/60-second cold-start backoff is preserved. The warm-up cadence remains
until a successful snapshot also covers the estimated finish, then returns to
the normal interval. These extra reads yield to controls, grinder work, pending
refreshes, unavailable Wi-Fi and low internal memory; token renewal retains its
own deadline. UI ticks and diagnostic cache reads still issue no Cloud request.
This allows cold-start observation without nearby BLE, but cannot guarantee
the Cloud service's own reporting delay or a refresh during sustained work.
An observation that cannot start because another background client owns the
shared connection retries after 30 seconds instead of consuming the full
observation interval. Requests that actually started retain their normal cadence.

The AXP2101 battery state is local. Missing battery, failed reads and unknown
percentage are distinct from an empty battery. USB presence alone does not
show a charging indication: the lightning indicator requires a valid PMU
charging state. Measurement is enabled, while charger current limits, target
voltage and temperature protection remain unchanged. Battery polling does
not write display preferences to NVS. A failed PMU status read retains the
last valid sample within the existing five-second freshness window, without
advancing its observation time. Sustained failures still show unknown; a
valid battery-removed sample takes effect immediately.

## Orientation and sound

USB-C on the right is the default reading direction. Automatic four-direction
rotation is optional. QMI8658 acceleration must settle before switching;
flat placement, motion and held touches keep the current direction. Display
and input mapping move together. Rotation does not reconstruct the current
page, submit a command or discard an edit. Brightness, rotation and sound
preferences are local settings. New AMOLED devices default to 60% brightness
and an always-on display (automatic dimming off); a previously saved preference
takes precedence. Manual screen blanking remains available. The existing LCD
profile keeps its 75% brightness and automatic dimming defaults.

Cue pattern A is optional and defaults to muted: the ascending success cue
lasts 650 ms and the descending failure cue lasts 760 ms, including their
silent gaps. Cues must be edge-triggered: no repetition during polling, no
startup announcement of an old result and no success sound for an ambiguous
result. A newly confirmed
grinder receipt can indicate success; it does not mean the grinder physically
adjusted. Completed Cloud commands use the existing operation-success semantics:
manual settings show a green check and `已设定`; `recipe_follow.applied` shows
`已同步`. These report completed settings, without requiring the scale to be
online or adding an operator verification step. They do not assert independent
live hardware telemetry or change `stateConfirmed`. Pending and unknown outcomes
remain separate and never receive success feedback. Audio output uses a separate worker and releases
its codec and I2S DMA allocations after each cue; microphone capture is not
enabled.

The sound setting uses a 0–100% slider with an explicit Save action. 0% mutes
all cues; 80% preserves the previous enabled volume, and 100% raises the PCM
peak to 25000/32767 without clipping. Saving a nonzero level plays one success
cue at that level. Editing the slider does not write settings or replay cues.
The level is stored locally and restored after power-off. An old enabled
sound preference maps to 80%; an old muted preference stays at 0%.

A manual weight request after a completed cup takes over the home status and
preset feedback. Its confirmed target is filled; the previous cup's measured
weight never becomes a target outline. A new grinder job restores recipe
following, and a new confirmed cup receipt replaces the manual status. Link
details use the same precedence and only report canceled automatic following
when a job is actually active.

The four AMOLED quick-weight keys are **20 / 30 / 36 / variable**, with **40 g**
as the initial variable value. Tap the fourth key to apply it; hold it to edit
an integer target from 1 to 200 g. A confirmed manual setting or current recipe
application remembers a non-fixed target in the fourth slot. Fixed targets
never replace it. Pending, failed, partial, unconfirmed, unrelated job records
and measured coffee results do not change the saved shortcut. Repeated recipe
snapshots cannot overwrite a later manual choice or cause duplicate NVS writes.

The shortcut is stored in `gbs_display/weight_preset` only after success. It
returns after power-off without sending a command or implying that the machine
has selected it. Missing or invalid storage falls back to 40 g; failed saves
keep the previous value and show a local warning. A successful selection uses
a filled gold key; a pending target uses only a gold outline. Outside an active
setting outcome, a valid Cloud-selected Dose1/Dose2 weight restores the matching
key's highlight after ESP restart without requiring another write. The latest
known selection remains visible while the machine is off or disconnected.
Complete Cloud dose values and confirmed
dose writes are also saved locally for the selected machine, independently of
the fourth shortcut. After restart, missing Cloud display fields fall back to
this history; live fields take precedence. Unchanged observations do not rewrite
Flash. Manual quick-weight keys and the variable-weight editor remain usable
while the coffee machine is off if the ESP has a current, ready Cloud connection.
After a successful dashboard read explicitly reports the machine offline,
manual weight commands may fill missing dose fields from the same machine's
saved history. Fresh fields take precedence; missing history, invalid values,
an unknown connection state, read errors or a changed machine identity prevent
the write. The success caption states that Cloud accepted the setting; the
machine and scale apply it after they reconnect. This does not bypass the
online guard for automatic grinder recipes or unlock other offline controls.
Expired local observations and network/authentication failures disable controls.
The saved fourth shortcut alone never selects a key, and absent Cloud data plus
absent history never invents a selection. An unfamiliar
pending target temporarily occupies the fourth key, which is disabled until
the outcome arrives. Failure restores its saved label with no success highlight.
This local shortcut does not add a machine dose slot: existing Dose1/Dose2
selection and preservation rules remain in force, and recipe following still
sends the target for every cup. Display diagnostics expose the saved shortcut
as `variableWeightGrams` without extra machine polling.

The A cue uses unity codec gain and a 20000/32767 PCM peak at 80%, with 12 ms
attack/release and actual silent DMA gaps. Success is a rising 650 ms double
tone; failure is a descending 760 ms double tone. A normal cup uses two success
cues: confirmed recipe/weight setting, then acknowledged result delivery to the
grinder. Job acceptance and measurement receipt update the display silently,
including when result receipt and acknowledgement arrive in the same UI poll.
Manual weight confirmation still plays one success cue. Failed, unconfirmed,
expired and interrupted operations use the
selected failure cue without changing the displayed outcome semantics.
Stable job identity and per-node latches prevent repeated cache
polls or reopening a page from replaying notifications. Startup snapshots and
stages observed while muted are silent. Adjacent cues use bounded, nonblocking
four-entry queues, preserving the newest feedback when overloaded. Audio
continues on its own worker; no cue issues a business command or Cloud read.

A local AMOLED build can set `GBS_CUE_PCM_HEADER` to an existing private header
containing `static const int16_t gbs_success_pcm[]` and `gbs_failure_pcm[]`:
16 kHz mono PCM, with peaks at or below 25000. The player scales each block by
the saved volume and duplicates mono samples into the existing stereo I2S
output. The same key-milestone policy applies to custom cues. Mute cancellation
and resource cleanup are unchanged.
The selected local pair uses Complete (success, 0.952 s) and Pulse (failure,
0.627 s); those source recordings are not included in this repository.
Without the header, builds retain the synthesized A cues. CMake caches this
path: pass `-DGBS_CUE_PCM_HEADER=` explicitly to restore the default build.

## Local build

Use **ESP-IDF 5.5.4** and the ESP32-S3 toolchain for `amoled216`, including
when building from the S31 development branch. The S31 SDK is only for the
separate `mosaico` profile.

```sh
. "$HOME/esp/esp-idf/export.sh"
cd firmware/integrated
sh tools/check-integrated.sh
sh tools/build-integrated.sh amoled216
```

The new build directory is `build-integrated-amoled216`, and its device model
is `lamabuddy-esp32s3-amoled216`. Existing `touchscreen` and `headless` profiles
retain their board configuration. The NVS preparation tool accepts
`--profile amoled216` with the touchscreen partition layout; preparation is
not required for local builds. Keep only one active client for a populated
device identity during an authorized hardware handover.

The shared component manifest resolves target-specific dependencies. S3
profiles retain their IDF 5.5.4 path, and the Waveshare board driver is selected
only by `amoled216`; `mosaico` selects its own BSP and driver. Build directories
separate generated configuration, while managed components and the dependency
lock remain shared by the checkout. Use separate source copies when building
S3 and S31 concurrently.

The read-only display preview accepts 240 x 280 and 480 x 480 frames and
checks board dimensions. Device observation is a separate, authorized step;
native simulator and host tests use synthetic data only.

## Application updates

AMOLED uses the same protected OTA routes as the integrated Hub:
`GET /diagnostics/ota`, `POST /diagnostics/ota/update` and
`POST /diagnostics/restart`, authenticated with the target's current Bearer
token. Use `firmware/integrated/tools/run_ota_update.py` with the
`build-integrated-amoled216/lamabuddy_esp32_s3_hub.bin` image; see the
[LAN update procedure](INTEGRATED.md#lan-updates-and-observation).
Keep the previous application image and private configuration/NVS backup
before updating. OTA writes an application partition and selects it for the
next boot; it does not rewrite NVS or perform a full-flash restore. Do not
repeat first-install NVS import for an application update. Check the running
build and partition after restart; preserve the single active client rule.

The AMOLED profile uses one eight-row DMA draw buffer, totaling 7,680 bytes
at 480 pixels and RGB565, to reserve internal RAM for the network and grinder
workers while halving full-frame submissions from 120 to 60. Its display and
LVGL components use `-O2`; CPU frequency and the Hub build profile are unchanged.
The LCD profile retains two twelve-row buffers. These choices do not establish
runtime responsiveness or sustained performance.

## S31 porting regression (2026-10-28)

The S31 porting round included a full `amoled216` build in an isolated source
copy using ESP-IDF 5.5.4. It passed without replacing the active S31 managed
components or flashing the S3 board. This is source compatibility evidence;
the S3 field results below and the [Mosaico field results](MOSAICO.md) remain
specific to their respective boards and binaries.

## Validation status (2026-10-23)

The board has been installed and exercised with USB-C on the right, matched
touch coordinates and automatic rotation in all four directions. Physical PWR
off/on cycles restored the UI and connections without replaying old jobs.
A valid scale battery observation was retained as gray history after both the
scale and ESP were powered off. These are field observations, not endurance
or calibrated battery-capacity measurements.

User-made cups completed recipe following, measured-result collection and
acknowledged grinder feedback on `77d15a61` and `f8b54b1a`. The user accepted
the louder four-node cues on `f8b54b1a`; 73 audio/PMU observations had no output
errors or invalid PMU samples. One later cached machine-state read timed out
and recovered; it was not a failed coffee control. Raw logs, identities and
screenshots remain private.

The subsequent manual-weight display fix `211d2963` passed the consecutive-cup
native UI regression, AMOLED 2/2 and LCD 1/1 native suites, firmware build,
OTA and startup recovery checks. The complete integrated host suite and all
three integrated board builds passed during adaptation. Neither a local
build nor precursor field acceptance proves a later binary's entire workflow.

Remaining physical checks include manual weight selection after a completed
cup on `211d2963`, the next natural cold-start boiler countdowns, failure-cue
audibility, battery runtime/charging transitions and sustained operation.
Keep exact-build results and remaining gates in the private field report.
Independent live machine telemetry remains separate from command completion.
The green synchronization state follows `recipe_follow.applied`, which requires
a completed command with `ESP_OK` and `result.ok`; queue admission alone is not
success. It does not constitute hardware acceptance of a complete cup.

An afternoon cup on `7c7e1708` exposed a separate command failure: the recipe
target was read correctly, but automatic weight setting timed out. The user
then retried from the ESP and phone before extracting; the successful setting
cannot be attributed to the automatic command. Measurement collection and
grinder acknowledgement completed. This run is a failed automatic-setting
gate, even though the final cup receipt succeeded.

Quick weight now shares one fresh dashboard read, Cloud I/O lock and HTTP
session across its mode and Dose1 writes. It preserves Dose2 from that read,
the eight-second deadline, per-write machine/recipe guards and uncertain-write
handling. A validated read also repairs cached preset values so a rejected
stale preset decision does not persist across later clicks. It does not change
the earlier command's outcome or refresh the age of unrelated dashboard data.
The local lock cannot serialize writes made independently by a phone.

The fix was installed as `abda9da4`. Four user-operated ESP selections
(`30 -> 35 -> 40 -> 20 g`) completed in 2.292, 3.051, 2.944 and 3.201 seconds,
with the original eight-second TTL. Request-correlated timing confirmed one
dashboard GET and reuse of its connection for both quick-weight POSTs.
The user confirmed success and shorter waits. This is a four-command manual
check; automatic recipe setting and a complete cup on this binary remain
pending the next morning's test. The gray stale-scale indication is unchanged.

## Approved workflow UI implementation (2026-10-23)

The `codex/amoled-approved-flow-ui` working branch implements the approved
480 x 480 workflow composition in native LVGL. Build `9947aa33`, including
the panel legibility calibration, was installed on the AMOLED 2.16 board
by application-only OTA on 2026-10-23. Startup, the exact running build and
the device's 480 x 480 framebuffer were verified. Existing configuration and
successful-result history were retained. Physical touch, panel appearance
and a complete coffee workflow still require acceptance with this build.

The panel review prompted a second typography pass: AMOLED body text uses
weight 650, flow labels and headings 750, and primary numerals 800. Small
numeric values and units also use stronger real font instances. Secondary
text is brighter, and disabled buttons retain 70 percent opacity while
remaining disabled. Small-font line metrics preserve the stronger glyph
edges; a wider coffee warmup field keeps the maximum estimate on one line.
The legacy LCD fonts and disabled styling remain unchanged. Native layout,
glyph coverage and control-state regressions passed before this installation.

The home page uses six evenly spaced status entries, a five-step evidence
trail, a short current-step headline, and the existing cup/delivery deadlines.
Completed steps have a solid mint circle. A new job clears prior milestones;
delivery acknowledgement does not infer that automatic recipe setting worked.
Cancellation is not shown as result delivery, and an early cancellation is
not described as a three-minute timeout. Missing measurements remain unknown.

The header colors the entire icon: green for connected/ready, gray for
disconnected, unknown or stale, amber for pending/recoverable attention, and
red for a confirmed issue requiring intervention. The coffee-machine BLE
summary comes from local worker diagnostics; cached telemetry alone is not a
live connection. The scale keeps its independent cloud freshness and battery
history rules. Device battery and charging still use PMU observations.

The approved A scale, simplified coffee-machine outline, and other flow icons
are native alpha assets. Geometry and text baselines follow the approved SVG
coordinates. Chinese uses licensed Noto and numbers use Inter; their glyph
shapes differ from the macOS design preview. RGB565 quantization and LVGL
antialiasing also prevent a literal browser-pixel match. Source/license and
regeneration details are in `integrated/display/icons/FLOW-SOURCES.md` and
`integrated/display/fonts/FLOW-SOURCES.md` under `firmware/integrated`.

The display updates its monotonic UI clock after applying fresh or cached
snapshots and before drawing. A newer healthy snapshot therefore does not
briefly turn connection icons gray because it arrived after the previous UI
tick. Genuine disconnections and the existing freshness limits still apply.

The unified numeric editor retains the existing submission, conflict,
confirmation and unknown-result behavior. Header navigation now invokes
the unsaved-edit confirmation. Presets, temperature/steam controls, detail
pages, battery history, sound, rotation and hardware-button handlers remain.

For offline acceptance, build `integrated/simulator` with
`-DHUB_AMOLED_216=ON` and run CTest. The `approved_flow_snapshots` test exports
thirteen 480 x 480 BMPs from the actual LVGL framebuffer, with synthetic values
and assertions for visible text, countdowns and milestone states. The
`--approved-review <directory>` command exports the same fixtures manually.
The legacy LCD simulator remains a separate regression target. No simulator
result proves physical touch alignment, brightness, charging or a real cup.

## Battery sessions and physical buttons

The device need not run continuously. Local display preferences are stored
only when changed. After a power cycle it reacquires Wi-Fi, valid time, Cloud
state and a grinder job baseline. The first job listing is a baseline: jobs
already listed are not accepted as new cups. Start a new cup after connection
and baseline readiness, rather than expecting a powered-off bridge to resume
an interrupted cup.

The persistent ledger and fresh job baseline prevent automatic replay of old
jobs or results. Invalid journal data disables result delivery. A
saved successful result remains historical; it is not displayed as a new
current-cup success after reboot. Interrupted and ambiguous outcomes retain
their warning. Turning the bridge off does not undo a command already sent to
the coffee machine or grinder. A previous ambiguous warning may remain even
after connections and the new-job baseline are ready.

| Button | Runtime behavior |
| --- | --- |
| Middle power key, PWR | Retains the AXP2101 hardware power behavior, independent of the display loop. The firmware does not rewrite power-key timing or shutdown/restart configuration. The installed PMU hold threshold has not been read or verified. |
| User key, GPIO18 | Short press wakes a dark display or returns home using the existing unsaved-edit confirmation; when already awake, hold for approximately 1.5 seconds to blank only the display. |
| BOOT, GPIO0 | Short press wakes the display or toggles automatic rotation / lock current direction through the same saved setting as the screen. Its hardware bootloader role is retained. |

When dark, the first GPIO18 or BOOT press only wakes the screen, even if held.
A long press does not produce another short press on release. GPIO18 and BOOT
keys already held during startup are ignored until released. Neither key
sends a coffee-machine command. Either programmable key cancels a concurrent screen
gesture; touch input resumes after the keys and touch are released. The first
touch that wakes a dark display is also consumed without activating a control.

Screen blanking preserves the page, draft and background job. Busy work does
not override manual blanking. Only display brightness is set to zero: the CPU,
wireless connections and sensor polling continue. This is neither deep sleep
nor complete shutdown, and idle current and battery runtime remain unmeasured.
Complete power off stops Hub/grinder services. The PMU power key retains its
hardware behavior even when the display task is stalled; no specific hold
duration is promised by this firmware.

Audio output is wired to the board's P10 speaker connector. An attached
speaker is required; a codec on the board alone does not prove audible output.
Speaker presence, audible level and distortion require hardware acceptance.

Board references: [official ESP-IDF examples](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/ESP-IDF)
and [board schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.16/ESP32-S3-Touch-AMOLED-2.16-Schematic.pdf).
The panel initialization sequence and pins are adapted from the
[Waveshare BSP](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/bfeb6e6d5737178cdde78b630c8118074da0a657/bsp/esp32_s3_touch_amoled_2_16)
under Apache-2.0; its license is retained in
`firmware/integrated/integrated/display/gbs_board_amoled216.LICENSE`.
The pinned CO5300, CST9217/CST9220 and codec components retain their Apache-2.0
licenses in the component packages. LVGL remains pinned to 8.4.0.
This project is independent and is not affiliated with Waveshare, La Marzocco
or the grinder vendor.
