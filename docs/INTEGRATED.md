# One ESP32-S3: Hub, grinder and native touchscreen

The application in [`firmware/integrated`](../firmware/integrated) runs coffee
controls, Generic WiFi feedback, the native display and LAN OTA on one board.
The display calls the local Hub in-process. Wi-Fi, Cloud authentication, machine
selection and command admission have one owner; no external Hub, phone or
always-on computer is needed at runtime. La Marzocco Cloud is still required.

## Board profiles

| Profile | Flash / PSRAM | Build directory | First-install NVS size |
| --- | --- | --- | --- |
| `touchscreen` | Waveshare ESP32-S3-Touch-LCD-1.69, 16 MB / 8 MB octal | `build-integrated` | `0x15000` |
| `headless` | ESP32-S3, 8 MB / 8 MB octal | `build-integrated-headless` | `0x6000` |

The touchscreen is 240 x 280, with **USB-C on the right**. ST7789 pins are
DC4/CS5/SCLK6/MOSI7/RST8/BL15; CST816 pins are SDA11/SCL10/RST13/INT14.
Other boards need a verified wiring/memory profile. Within the touchscreen
profile, a failed touch-controller probe leaves Hub/grinder services running
without starting display tasks; this does not make arbitrary boards compatible.

Home has four weight presets, boiler temperature, a large steam level 1/2/3
with current/target steam temperatures below, and a direct grinder-linkage card.
Settings, BBW, prebrew, power/standby, existing schedule editing and multi-cup
history share the native UI. Persistent confirmed dose selection prevents an
older Cloud snapshot from silently clearing the selected weight.

## Build and local checks

Install ESP-IDF 5.5 with the ESP32-S3 toolchain. Host checks need a C compiler,
Python from the IDF environment and zlib development headers (`zlib1g-dev` on
Debian/Ubuntu). From the repository root:

```sh
. "$HOME/esp/esp-idf/export.sh"
cd firmware/integrated
sh tools/check-integrated.sh
sh tools/build-integrated.sh touchscreen
# Use this instead for the integrated headless board:
sh tools/build-integrated.sh headless
```

`sdkconfig` is scoped to each build directory. The dependency lock is included;
no private checkout, asset cache or pre-generated local build directory is
needed. Generated C fonts/icons are checked in with their licenses. The
optional `integrated/simulator` uses SDL2 and `--self-test` with synthetic data.

The root `idf.py build` still builds the lightweight companion. It does not
select this integrated application.

## First installation and identity handover

Initial pairing remains an upstream desktop task. Follow the root
[pairing/export instructions](../README.md#migrating-from-docker), using an
upstream version with PR15's recovery fixes. Keep a single active client:
stop the previous bridge before the new board takes over its imported identity,
and stop all clients during any pairing recovery quiet period.

The root `tools/provision.py` prepares ignored `.private/config.json` from the
saved desktop integration, pairing and pinned grinder certificate. It does not
flash hardware. The integrated importer consumes that configuration and
requires the selected machine's serial explicitly; it does not guess a machine
when the serial is absent. Cloud login field limits are 96/128 bytes here, and
the importer rejects configurations exceeding those Hub capacities.

Example, from the repository root, for a **new touchscreen installation**:

```sh
python firmware/integrated/tools/prepare-integrated-nvs.py \
  .private/config.json .private/integrated-touch-first-install \
  --profile touchscreen --include-cloud --include-grinder
```

For a new headless installation, use `--profile headless` and a different output
directory. The generator creates a fresh Hub token and imports the retired
Cloud/pairing identity, with grinder mode initially **monitor**. It writes a
0700 directory with 0600 files and never opens a serial port. Do not import the
identity while another client is still using it.

The generated `nvs.bin` belongs at offset `0x9000` in the selected integrated
partition table. After backing up the intended board, install its matching
bootloader/partition/application through USB using `idf.py -B <build-directory>
-p <port> flash`, then write that profile's generated NVS at `0x9000` and reboot.
The lightweight companion's NVS image and partition layout are different.
The touchscreen application slots start at `0x30000`, `0x430000`, `0x830000`;
the headless slots start at `0x20000`, `0x220000`, `0x420000`.

Never use this initial NVS import for an application update: it would replace
configuration and journal state. The public development profiles do not arm
Secure Boot/Flash Encryption provisioning. Release security gates and HTTPS-v2
identity provisioning are separate; this source publication is not a certified
production security configuration. The default operational API is authenticated
LAN HTTP, not a public internet service.

## Confirm setup and enable recipe following

The touchscreen advertises `lamabuddy-touch.local:8787`; headless advertises
`lamabuddy-integrated.local:8787`. Authenticate with the generated token using
the existing `Authorization: Bearer` API. Verify the model and selected machine
before enabling controls. Configuration requests should be made from a private
client that does not print tokens or passwords.

| Endpoint | Purpose |
| --- | --- |
| `GET /health` | Model, firmware/build identity and resource state. |
| `GET /machine/binding` and `GET /machine/state` | Selected machine identity and cached state. |
| `GET /diagnostics/grinder` | Connection, current task, last feedback, recipe-follow stage and target. |
| `POST /controls/grinder-mode` | Set `active` and explicit `confirm: true`; verify the requested mode in diagnostics. |
| `POST /controls/grinder-recipe` | Set `enabled` and, on first enable, `ownerPassword` for the grinder's ordinary web export. Stored in NVS; never returned in diagnostics. |
| `POST /diagnostics/grinder/recipe-probe` | Read-only historical log match by `grindUuid`, while idle; does not set machine weight or prove a new-cup flow. |
| `GET /diagnostics/display` and `GET /diagnostics/display/frame` | Touch/display diagnostics and actual RGB565 framebuffer; no remote touch injection. |

Recipe following is opt-in. After successful `brewAccept` of a fresh grind,
the worker performs one Owner-authenticated ordinary log export. It validates
ZIP sizes/CRC, streams the CSV and uniquely matches the task's `grind_uuid`,
successful grind and GbS-enabled row before using `recipe_brew_weight` (mg).
It does not use the last row, a hardcoded handle map or an external database.

The matched target reuses an existing equal-weight Dose if possible; otherwise
it uses the guarded quick-weight path. A fresh Dashboard read checks preset
identity and machine availability before writing. Manual weight changes cancel
automatic work. The write window is the first 15 seconds of the task, with at
most 8 seconds per command; the 180-second countdown waits for the shot result.
Unknown outcomes are not automatically replayed.

There is not yet a reliable live extraction-start signal. **Wait for the target
weight to be highlighted and recipe synchronization confirmed before pulling
the lever.** A failed or unconfirmed sync needs the operator to check the weight.
Measured shot time/weight then follow the existing Cloud-to-`brewDone` path.

## LAN updates and observation

Use the integrated application's own OTA tool and the target's current token:

```sh
cd firmware/integrated
python tools/run_ota_update.py \
  --base-url http://lamabuddy-touch.local:8787 \
  --token-file /private/path/hub-token.txt \
  --image build-integrated/lamabuddy_esp32_s3_hub.bin \
  --restart --wait-health
```

Choose the headless URL/image only for that board. OTA admission rejects an
active job and blocks new acceptance while an image is staged. Check running
versus boot partition and the build ID after a restart. A lost upload response
must be reconciled through status, rather than blindly uploading again.

`tools/display_preview.py` reads actual screen pixels and serves a local preview;
`tools/record_grinder_recipe.py` captures bounded, redacted cached-state samples.
Use `--help` for their local arguments. Captures and credentials stay outside
Git. No test or build step invokes these tools against a real device.

## Acceptance and remaining work

One source-build cup passed automatic 20 g -> 30 g following before the lever,
30.2 g / 27.696 s feedback and observed grinder adjustment. Read the
[exact-build field report](RECIPE_FOLLOW_ACCEPTANCE.md) before treating a new
binary as accepted. Reverse 20 g following, headless integrated hardware, E80,
network faults, endurance and reliable extraction-start protection remain open.
