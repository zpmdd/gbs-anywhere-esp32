# Experimental 2.16-inch AMOLED profile

This profile targets the Waveshare ESP32-S3-Touch-AMOLED-2.16. It shares the
integrated Hub, grinder workflow, command admission and data collections with
the existing LCD and headless profiles. It does not identify a portafilter or
replace the grinder's recipe selection. Installation and on-device debugging
have been authorized for the current board. This remains experimental:
exact-build evidence and outstanding checks are tracked in the private field
report; partial hardware results do not establish complete acceptance.

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
Percentages are strictly 0..100: 1 means 1%, not 100%. Current connection
observations expire after 60 seconds; disconnected or unavailable does not
prove the scale is physically powered off. The icon is green only with fresh
online evidence. A valid last-known battery percentage remains inside the gray
scale icon after disconnection, expiry or an ESP power cycle. With no valid
history it shows a dash. The detail distinguishes the last reading from current
connection state; an unavailable wall clock does not produce an invented age.

Battery history is local to this AMOLED display and scoped to the selected
coffee machine. It does not change the shared API's fresh-value/null contract,
control admission, success results or grinder workflow. The current upstream
snapshot has no reliable independent scale identifier: after changing the
physical scale on the same machine, the old percentage remains historical until
a new valid observation arrives. Restore never claims the scale is online.
A versioned NVS record saves the first valid observation while idle, then
coalesces changed percentages with at least 60 seconds between saves. Repeated
polls or unchanged percentages do not write flash. A sudden power loss before
a deferred save can restore the previous committed value.

Scale fields primarily update from existing successful Dashboard reads,
including the fresh preset safety read before a control. If none provides a
valid battery observation, the AMOLED keeper may make at most two additional
accessory requests per boot and selected-machine binding, first after about
90 seconds and then at least 90 seconds apart. Normal Cloud/token refreshes
retain priority; interactive work, its completion grace period, grinder jobs,
maintenance and resource pressure defer the optional read. Seeing a valid
battery observation stops these extra reads. A single in-flight HTTPS request
cannot be immediately preempted by a later control. Reading the UI cache adds
no Cloud request; this is not real-time power detection. The scale detail's
manual refresh continues to use the existing Cloud worker.

Each compact home-screen boiler control shows its own warm-up estimate with
an amber clock and rounded-up remaining minutes while the corresponding Cloud
ready time is in the future. It reuses `coffeeBoilerReadyTime`,
`steamBoilerReadyTime` and the source `lastUpdated`; UI ticks never issue a
Cloud request or renew the snapshot. Estimates require a valid wall clock,
a connected machine and a snapshot less than five minutes old. Standby,
disabled steam, malformed timestamps, estimates over a day away and stale
snapshots hide the countdown. When it ends, the normal temperature display
returns; elapsed time alone never declares a measured boiler ready.

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

A manual weight request after a completed cup takes over the home status and
preset feedback. Its confirmed target is filled; the previous cup's measured
weight never becomes a target outline. A new grinder job restores recipe
following, and a new confirmed cup receipt replaces the manual status. Link
details use the same precedence and only report canceled automatic following
when a job is actually active.

The A cue uses unity codec gain and a 20000/32767 PCM peak, with 12 ms
attack/release and actual silent DMA gaps. Success is a rising 650 ms double
tone; failure is a descending 760 ms double tone. A neutral 240 ms note marks
job acceptance and receipt of a valid cup measurement, and distinguishes an
unconfirmed/expired result from a definite failure. Recipe completion and the
final grinder acknowledgement each sound success; recipe failure sounds
failure. Stable job identity and per-node latches prevent repeated cache
polls or reopening a page from replaying notifications. Startup snapshots and
stages observed while muted are silent. Adjacent cues use bounded, nonblocking
four-entry queues, preserving the newest feedback when overloaded. Audio
continues on its own worker; no cue issues a business command or Cloud read.

## Local build

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

The shared component manifest also resolves and builds the pinned AMOLED
dependencies for the other integrated profiles. Their board selection remains
unchanged, and the new board driver is enabled only for `amoled216`; a clean
build may therefore need additional downloads even for an existing profile.

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
