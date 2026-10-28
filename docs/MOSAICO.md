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
experimental; the build-specific evidence below does not establish complete
cup or endurance acceptance.

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
The AI button provides the shared home/blank-screen behavior, and BOOT toggles
rotation when the application is running. POWER remains the board's physical
power control.

Fresh display preferences are 60% brightness, automatic dimming off, automatic
rotation off, sound muted and a 40 g variable preset alongside 20 / 30 / 36 g.
Saved preferences take precedence. The shared AMOLED workflow describes the
coffee and scale-state UI, but its AXP2101/QMI8658 hardware details belong to
the S3 board.

Mosaico reads BQ27220 presence, current, operation status and state of charge
without invoking the BSP battery initializer or replacing the factory capacity
calibration. Unknown or unready readings remain unknown. A charging indication
requires positive measured current and consistent gauge status; USB power alone
does not establish charging. Failed reads retain a valid observation for at
most five seconds. The coffee scale's battery state is a separate Cloud-derived
observation.

ES8311 audio runs in its own worker with the existing edge-triggered milestone
policy. A 0–100% setting controls PCM amplitude; mute suppresses queued cues.
After a cue, the driver drains playback, mutes and closes the codec, and turns
off the PA while retaining the BSP-owned handles for reuse. Microphone capture
is not enabled. A success cue reports the existing command/receipt outcome; it
does not independently prove physical machine or grinder action.

## Field evidence as of 2026-10-28

The final installed application was:

| Artifact field | Value |
| --- | --- |
| Build ID | `8d4984d3` |
| Application size | 4,412,704 bytes |
| SHA-256 | `d1a78f1dd657f0e9797ebf5a18f5c2348d2c5d4f9431c0bc6f4fc69d7ba62c32` |

On **`8d4984d3`**, LAN OTA, restart and health recovery succeeded; Cloud
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

Still open on S31: a complete new-cup workflow through grinder feedback,
end-to-end BLE writes, prolonged stability, battery runtime and full power-loss
recovery. More PSRAM increases capacity; it does not establish twice the speed.
The current display still uses a single eight-row DMA buffer and 40 MHz QSPI.
Comparable control latency, frame rate, sustained load and endurance have not
been measured. Build/host checks and earlier S3 cup evidence do not close these
S31 hardware gates.
