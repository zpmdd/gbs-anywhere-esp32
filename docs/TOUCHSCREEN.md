# Experimental native touchscreen

This guide describes the earlier root-level display client that connects to an
external Hub. For the complete Hub/grinder/display on one ESP32-S3, use the
[integrated firmware guide](INTEGRATED.md) and its board-specific build entry.

The optional LVGL renderer targets the Waveshare ESP32-S3-Touch-LCD-1.69
(240 x 280, ST7789, CST816 touch, 16 MB flash and octal PSRAM). The normal
8 MB headless profile stays the default. This display binary has not been
flashed or validated on a physical screen.

## Current scope

The home screen has two temperature dials, four weight presets (20/30/35/40 g)
and a direct link-status card. There is no permanent bottom navigation.
The card opens the task/result view; its Records action opens the last
confirmed result. Settings contains machine, connection, display and system
routes. The shared renderer includes weight/boiler editors, steam levels,
pending/confirmed/unknown control states, connection failures, result delivery
failures and restart recovery guidance.

The firmware reads the existing grinder/cloud worker state and supports
monitor/active mode, local brightness and idle dimming. An optional authenticated
HTTPS v2 client now reads coffee/steam current and target temperatures from an
existing LamaBuddy Hub, and submits smart weight presets, coffee temperature,
steam, BBW dose/mode, prebrew, power and smart-standby controls. This is a client
of the separate Hub; the screen does not embed its BLE/Cloud implementation. Without an imported Hub
trust record, machine values remain unavailable and writes stay disabled.
Missing measured temperatures remain dashes, even when targets are known.
Power and smart-standby changes require an explicit confirmation screen.
Schedules and multi-cup history now use the Hub's bounded slow-read cache.
Schedule editing additionally requires the matching Hub extension described below:
LamaBuddy branch `codex/hub-schedule-edit`, commit `7b00f00a`. It includes the
local binding query and v2 command-status metadata required by this client.
Independent idle timers remain unavailable.
The Mac simulator supplies synthetic data and acknowledgments; it never contacts
devices or cloud services. There is no OTA or new pairing workflow.

Grinder/cloud feedback stays in the existing worker. The display reads a
mutex-protected copy of its status JSON and submits mode through the existing
atomic request. A separate task owns Hub HTTPS operations and copies snapshots
and command results under a mutex. Deadlines use monotonic timestamps. A recovered connection
cannot turn an unknown delivery into success, and no result is automatically
replayed. An unconfirmed measurement is shown only when the current boot still
holds that exact answer; a previous successful cup is never substituted.

Controls retain drafts, lock duplicate submissions and wait for a confirmed
snapshot before changing the selected preset. New jobs never navigate away
from an editor. Idle dimming starts after 60 seconds and blanking after 120;
the first touch only wakes the display. Active tasks and confirmations keep it
awake. The two idle rows currently toggle the same fixed-time idle policy.

## Optional Hub connection

For a new touchscreen board, add `--hub-config /path/to/private-hub.json` to the
normal `tools/provision.py` invocation. The JSON must contain these fields from
the existing Hub's trusted provisioning record:

| Field | Required value |
| --- | --- |
| `url` | HTTPS origin, DNS name or IPv4 address, optional port; no path, user information, query or fragment. |
| `hub_id` | Existing Hub UUID. |
| `token` | Existing bearer token, 16–128 printable ASCII characters without spaces. |
| `serial` | Selected coffee machine serial; must match the bridge's imported serial. An unspecified bridge serial is set to this explicit value. |
| `certificate` | One public PEM certificate for that Hub, with newlines represented as JSON escapes. No private key. |

The offline importer validates the self-signed EC P-256 certificate, its signature
and `LamaBuddy Hub <hub_id>` common name. It writes a separate `gbs_hub/config`
NVS entry and private files with mode 0600. It makes no network requests and
preserves the grinder pairing and cloud installation identity. The initial NVS
image still starts in monitor mode; it is not an in-place configuration update
for an existing deployment because writing it replaces the task journal.

The runtime trusts only the imported certificate, verifies the Hub common name,
checks the expected Hub UUID and machine binding, and sends the v2 and machine
serial headers. HTTPS redirects and authorization retries are disabled. SNTP must
have set the clock before TLS starts. There is no automatic discovery, certificate
replacement or unverified HTTP fallback. Tokens and certificates are never part
of the display or public status response.

Each refresh reads binding, capabilities and state, then rechecks the binding
generation. Polling runs every five seconds; a local snapshot older than fifteen
seconds is stale. Current temperatures use only the Hub's measured boiler fields;
the Hub remains responsible for the freshness of its fused BLE/Cloud cache.
Controls are enabled per capability. BBW and prebrew additionally require Cloud
command readiness; smart weights also require Hub-side smart-preset validation
support. Power and standby retain the Hub's transport/capability decisions.
Missing or unknown modes do not become writable defaults.

| Control | Existing Hub route and behavior |
| --- | --- |
| 20/30/35/40 g presets and weight editor | Match Dose 1 or Dose 2: `/controls/bbw/mode` with `presetGrams`. No match: `/controls/quick-weight` with `requireNoStoredMatch: true`. The Hub owns preservation/restoration of other settings; the client verifies the untouched dose on readback. |
| Coffee target | `/controls/coffee-temperature`, 80–100 °C in 0.5 °C steps. |
| Steam level | `/controls/steam-level`, levels 1/2/3 map to 126/128/131 °C targets. |
| Steam boiler switch | `/controls/steam/enabled`; the switch retains the confirmed state while pending or rejected. |
| BBW Dose 1 / Dose 2 | `/controls/bbw/dose`, 0–200 g in whole grams. Editing a dose preserves the other dose and mode; it does not select that dose. |
| BBW mode | `/controls/bbw/mode`, `Continuous`, `Dose1`, `Dose2` or `PulsesType`; both stored doses are preserved. |
| Prebrew | `/controls/prebrew`, one `field` at a time: `mode` (`Disabled`, `PreBrewing`, `PreInfusion`), `onTime` or `offTime` (0–10 s in 0.1 s steps). Other prebrew fields are preserved. |
| Machine power | `/controls/power`, `enabled` plus `confirm: true`. Standby and heating are explicit choices; `EcoMode` is displayed as a separate readback state. |
| Smart standby | `/controls/smart-standby`, `enabled`, 1–240 whole `minutes`, `mode` (`PowerOn` or `LastBrewing`) and `confirm: true`. Each editor changes one value and preserves the other two. |

Each command gets a UUID and one POST. Before submission the client refreshes
state and rejects a changed relevant setting. The UI's edit snapshot is carried
through the queued request; BBW, prebrew and standby compare the whole related
group, including fields the user did not edit. It follows the original UUID via
`/diagnostics/commands?id=...`; a lost POST response is never retried. A completed
successful result must also match a fresh state read before the UI says confirmed.
Partial application and unknown outcomes remain distinct. A stale callback cannot
complete a newer UI request. The UI wait ends after 30 seconds with an unknown
outcome; this is not a promise that remote execution was canceled. HTTP operations
use at most a three-second socket timeout and never start after the wait deadline.
Only smart weight selection carries a twelve-second `clientTtlMs` for network variation;
explicit dose and mode editors use the ordinary Hub commands without that field.
The existing v2 surface enum has no touchscreen value, so requests use `diagnostic`.

## Schedules and multi-cup history

Settings → Machine → Schedule opens plans; Settings → Coffee history opens
recent Cloud cups. Lists show three entries per page, an explicit refresh action,
and loading, stale, empty, unsupported or unavailable states. Opening a page starts
one coalesced refresh request; the client polls cached reads for at most 30 seconds,
without repeated refresh POSTs. Existing telemetry and pending controls keep
priority on the shared Hub task. Collection buffers live in PSRAM.

The integrated local adapter preserves the same `202` loading response as LAN
reads, so a cold cache keeps polling until the background read completes.
Cloud IO contention uses bounded read retries and still yields to controls.
When counters and coffee records succeed but the trend fails or is incomplete,
the records remain available with `trend: null`; missing records are not
replaced with a successful empty list.

| Data | Hub contract |
| --- | --- |
| Schedule read | `GET /machine/schedule`, capability `schedule`; up to 64 plans. |
| Recent cups | `GET /machine/statistics?days=7`, capability `statistics`; keep the latest 32 of at most 512 returned records. |
| Refresh | `POST /diagnostics/slow-cloud-reads/refresh?kind=schedule` or `kind=statistics&days=7`; cached GETs may return 202/503 or `X-LamaBuddy-Stale: 1`. |
| Existing-plan edit | `POST /controls/schedule`, capability `scheduleEdit`; the matching separate Hub extension is required. |

Coffee timestamps are normalized UTC (`YYYY-MM-DDTHH:MM:SSZ`); the UI labels UTC
rather than guessing the machine's timezone. Unknown numeric fields stay dashes.
`doseValue` is a stored dose setting, with no measured-grams claim. These Cloud
records do not assert that the grinder received a result; that remains the
separate linkage/result view.

The schedule editor changes existing start/end minutes, weekdays, enabled state
and steam. Times follow the machine's schedule clock, including overnight ranges;
this client does not change the timezone. Enabling requires at least one weekday
and different start/end minutes. Creation/deletion remain in the machine app.
Old Hub versions retain read-only plans. Missing plan IDs and stale data cannot
be edited.

A schedule write includes `requestId`, `clientSurface: "diagnostic"`,
`confirm: true`, `expected` and `schedule`. Both plan objects contain the same
existing `id`, `enabled`, `onTimeMinutes`, `offTimeMinutes`, `steamBoiler` and
English weekday names in `days`. The UI baseline and a fresh Hub cache read must
agree before the single POST. The Hub then checks a fresh vendor plan, updates
only that entry with `CoffeeMachineSetWakeUpSchedule`, and reads it again.
Only `stateConfirmed: true`, the matching command UUID/control and unchanged
machine binding complete the UI as confirmed. A changed plan is rejected;
an uncertain reply is never automatically replayed. The Hub invalidates its
schedule cache and discards an older in-flight read after a write attempt.

The 30-second display wait is not a server cancellation. The vendor does not
provide an atomic compare-and-set in the inspected client protocol; another app
can still race between preflight and write. Fresh before/after checks reduce that
window but cannot eliminate it. Live schedule timing, vendor behavior and delayed
readback remain explicit hardware acceptance work.

## Local native preview

Install SDL2 and CMake, and source ESP-IDF to provide its existing cJSON source:

```sh
. "$HOME/esp/esp-idf/export.sh"
cmake -S simulator -B build-simulator -DCMAKE_BUILD_TYPE=Debug
cmake --build build-simulator --target gbs_touch_simulator
ctest --test-dir build-simulator --output-on-failure
./build-simulator/gbs_touch_simulator
```

The simulator uses the same `display/gbs_ui.c`, fonts, icons and RGB565 rendering
as the firmware. Its window enlarges the 240 x 280 framebuffer. Keys 0 through 8
select unavailable, ready, waiting, near-timeout, unknown delivery, offline,
success, sending and canceling states. Escape returns home.

Export native pixels for comparison without opening a window:

```sh
mkdir -p .private/screens
SDL_VIDEODRIVER=dummy ./build-simulator/gbs_touch_simulator --self-test .private/screens
```

This writes BMP captures of every route and critical state. The self-test also
checks coordinate-based navigation, fast taps, duplicate-submit locking,
confirmed-only selection, draft conflicts, disabled unavailable controls, bounded
confirmation waits, late result isolation, steam-switch rollback and cloud-error
colors. The 38 routes include BBW, prebrew, power, standby, schedule and coffee-history
screens. Every rendered label checked by the regression suite must resolve its
glyphs; numeric font subsets include spaces, slashes and the complete °C unit. Their tests
cover explicit confirmations, related-field conflicts, preserved values, limits,
stale/unknown modes and swiping to offscreen machine settings. `tools/check.sh`
additionally runs the production Hub contract against an in-memory fake, including
lost responses, identity/binding changes, missing capabilities, partial results
and readback mismatches, without sockets or device access.

On macOS, a local AppKit event monitor supplies the simulator window's own
content coordinates because SDL2 can coalesce a drag and reuse a stale release
position. It does not monitor other apps or synthesize input. LVGL sees the final
movement while still pressed, before release, so a swipe cannot become a row tap.
Focus loss resets the active press. Other platforms retain the SDL event path.

## Firmware builds

Use separate build directories so flash sizes and dependencies cannot leak
between configurations:

```sh
sh tools/check.sh
idf.py -B build-headless -DGBS_TOUCHSCREEN=OFF build
idf.py -B build-touchscreen -DGBS_TOUCHSCREEN=ON \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.touchscreen' build
```

The touchscreen profile is for a dedicated 16 MB board. Its factory partition
is 4 MB; it does not add OTA slots. Do not use it as an in-place upgrade for an
existing 8 MB deployment. The driver follows the vendor pinout and panel setup;
orientation, touch coordinates, colors, wake behavior, sustained heap usage and
network coexistence still require exact-build physical validation.

Two DMA buffers use 23,040 bytes of internal RAM. The 6,144-byte status buffer
and LVGL heap use PSRAM; the display task has an 8 KiB stack. Startup requires
at least 256 KiB free PSRAM. A failed display initialization is logged without
replacing the worker or erasing configuration.
When configured, the Hub client adds a 16 KiB task stack and a bounded 32 KiB
response buffer plus three bounded collection snapshots in PSRAM. The display
keeps two snapshots and LVGL keeps two more (about 61 KiB total collection
storage). Its initialization failure leaves the existing worker
running. The headless build does not link the Hub client or LVGL.

## Fonts and icons

Noto Sans CJK SC (regular/bold) supplies subsetted Chinese UI text; Inter Display
at weights 700/800 supplies small/large numerals. Both use SIL OFL 1.1; license copies are in
`display/fonts/`. Tabler Icons 3.35.0 use MIT, with originals and license in
`display/icons/`. These are standard interface symbols, rendered as small
RGB565 images with alpha. No mockup screenshot is used as a runtime page.

Generated assets are checked in, so firmware and simulator builds do not need
font tooling. To update the Chinese character subset after changing UI copy,
prepare an ignored source cache containing `NotoSansCJKsc-Regular.otf`,
`NotoSansCJKsc-Bold.otf`, `InterVariable.ttf` and the unpacked Tabler package at
`tabler/package/icons/`. Obtain them from the official
[Noto CJK](https://github.com/notofonts/noto-cjk),
[Inter](https://github.com/rsms/inter) and
[Tabler Icons](https://github.com/tabler/tabler-icons) projects.

Run `python tools/generate_display_assets.py /path/to/source-cache` in a tooling
environment with Pillow 11.3.0, fontTools 4.60.1 and CairoSVG 2.8.2. It also uses
Node/npm and the pinned `lv_font_conv@1.5.3`. Cairo must be available to CairoSVG.
Review the regenerated assets, run `idf.py -B build-touchscreen reconfigure`
when adding a font size, rebuild, and repeat native screenshot comparison.

## Validation boundary

Host tests, native interaction checks and both ESP-IDF profiles were exercised
locally. These establish local rendering/build behavior. They do not establish
physical LCD appearance, a real Hub TLS session, live machine control, sustained
network/memory load or a real shot acceptance result. No existing device was
connected, flashed, reconfigured or paired for this integration.
Keep this profile experimental until those gates are separately completed.
