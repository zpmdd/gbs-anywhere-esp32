<p align="center">
  <a href="README.md">English</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.de.md">Deutsch</a>
</p>

# gbs-anywhere ESP32

**Your grinder, espresso machine and coffee workflow — on one ESP32-S3 or ESP32-S31.**

[![CI](https://github.com/zpmdd/gbs-anywhere-esp32/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/zpmdd/gbs-anywhere-esp32/actions/workflows/ci.yml)
[![License: AGPL-3.0-or-later](https://img.shields.io/badge/license-AGPL--3.0--or--later-53694d)](LICENSE)
[![Platform: ESP32-S3 / ESP32-S31](https://img.shields.io/badge/platform-ESP32--S3%20%2F%20ESP32--S31-53694d)](docs/COMPATIBILITY.md)
[![Status: experimental](https://img.shields.io/badge/status-experimental-b78c50)](docs/PROGRESS.md)

<picture>
  <source media="(max-width: 640px)" srcset="docs/images/project-cover-mobile.png">
  <img src="docs/images/project-cover.png" alt="Native AMOLED control surface, rendered with synthetic demo data.">
</picture>

Run Mahlkönig Generic WiFi grinder feedback and La Marzocco Cloud integration
without an always-on computer, container or NAS. Choose a compact headless
bridge or an integrated touchscreen with coffee-machine controls, recipe
following and LAN updates. A computer is needed for initial setup and pairing.

**[Get started](#getting-started)** · **[Compatible hardware](docs/COMPATIBILITY.md)** · **[Screen gallery](docs/GALLERY.md)** · **[Validation & roadmap](docs/PROGRESS.md)**

> Experimental, with field-tested workflows on named builds. Internet access
> and La Marzocco Cloud are required. The grinder decides whether to adjust;
> an acknowledged result does not guarantee a physical grind change.

## A cup, from grind to feedback

| Step | What happens |
| --- | --- |
| **1 · Grind** | The ESP accepts a fresh GbS task from the paired grinder. |
| **2 · Sync the recipe** | Integrated firmware matches the current grind in the grinder's ordinary CSV export and applies the target beverage weight during puck preparation. |
| **3 · Brew** | You pull the shot after checking the target. The machine/scale supplies measured time and beverage weight to Cloud. |
| **4 · Send feedback** | The ESP reports that measured result to the grinder and shows whether delivery was acknowledged. |

The lightweight build handles feedback without recipe or machine controls.
[How the integrated application works →](docs/INTEGRATED.md)

## Built for daily coffee

| Feature | What you get |
| --- | --- |
| **Grinder linkage** | Fresh-task detection, measured shot feedback, monitor/active modes, reconnect diagnostics and a persistent no-replay journal. |
| **Coffee controls** | Quick beverage weights, coffee temperature, steam levels, Brew-by-Weight doses/mode, prebrew, power and smart standby on the integrated profile. |
| **Recipe following** | Target selection from the current grind's recipe, manual cancellation and distinct pending, completed and unknown outcomes. |
| **Native interface** | LVGL screens, large numerals, color plus icons, cup details, coffee history and existing-schedule editing. |
| **AMOLED battery sessions** | Scale battery inside one icon, separate device battery, retained history across shutdown, warm-up estimates, optional rotation, physical buttons and audible milestones. |
| **Updates & diagnostics** | Authenticated LAN OTA, cached diagnostics and framebuffer preview on integrated builds. Application OTA preserves NVS configuration. |

Current AMOLED maintenance and the ESP-Mosaico port are on
[`codex/s31-mosaico-port`](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/s31-mosaico-port).
The earlier S3-only adaptation remains on
[`codex/amoled-216-local-adaptation`](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/amoled-216-local-adaptation).
Neither board adaptation is part of `main`; use the matching branch and guide.

## See the interface

<table>
  <tr>
    <td align="center"><img src="docs/images/amoled-link.png" width="400" alt="Native linkage detail showing a recipe synchronized from 20 to 30 grams"><br><strong>Recipe & linkage</strong></td>
    <td align="center"><img src="docs/images/amoled-history.png" width="400" alt="Native coffee history list with measured shot weights and durations"><br><strong>Coffee history</strong></td>
  </tr>
</table>

These are screenshots from the compiled LVGL renderer, with **synthetic demo
data** and Chinese screen labels. They are not hardware photographs or proof
of a new field test. [More screens, browser UI and image provenance →](docs/GALLERY.md)

## Choose your hardware

### ESP32 boards

| Board / build | Display · flash / PSRAM | Source | Evidence |
| --- | --- | --- | --- |
| **Waveshare ESP32-S3-Touch-LCD-1.69** · integrated | 240 × 280 · 16 MB / 8 MB | `main`, `touchscreen` | Physical touch, recipe-follow cups in both directions, OTA and scale-state cycle. |
| **Waveshare ESP32-S3-Touch-AMOLED-2.16** · integrated | 480 × 480 · 16 MB / 8 MB | [Current development branch](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/s31-mosaico-port), `amoled216` | Earlier S3 builds have rotation/touch, PWR, battery and cup evidence; current source passes an isolated S3 build regression. |
| **ESP-Mosaico, ESP32-S31** · integrated | 480 × 480 · 16 MB / 16 MB | [S31 branch](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/s31-mosaico-port), `mosaico` | USB installation, LAN OTA and BLE temperature refresh; earlier S31 builds passed rotation/touch and sound checks. New-cup writes and endurance pending. |
| **ESP32-S3, no display** · lightweight | 8 MB / 8 MB octal | `main`, root application | Feedback-port hardware evidence; exact-build limits apply. |
| **ESP32-S3, no display** · integrated | 8 MB / 8 MB octal | `main`, `headless` | Local build checks; dedicated physical acceptance pending. |
| **1.69-inch external-Hub preview** | 240 × 280 · 16 MB / 8 MB | `main`, root `GBS_TOUCHSCREEN=ON` | Simulator/client validation; needs a separate Hub. |

### Grinder, machine and scale

| Device | Compatibility here |
| --- | --- |
| **Mahlkönig E65W GbS** | Real-cup feedback and integrated recipe following have field evidence. Generic WiFi pairing is required. |
| **E65T / E80W / E80T GbS** | Same Generic WiFi protocol family; not field-validated by this ESP32 project. |
| **La Marzocco Linea Mini R** | Field setup for controls, Cloud cup results and Brew-by-Weight workflow. |
| **Machine-paired Brew-by-Weight scale** | Measured cups come through the machine/Cloud. AMOLED retains observed battery history; this is not direct scale-to-ESP pairing. |
| **E64 WS / other machines** | Not implemented or independently validated here. The upstream desktop supports a different E64 WS path. |

Board images are not interchangeable. See the
[complete compatibility and feature matrix](docs/COMPATIBILITY.md) before choosing a build.

## Getting started

1. **Choose a profile** in the compatibility matrix and install its ESP-IDF toolchain.
2. **Pair once** with the upstream desktop's supported E65/E80 flow, save the
   integration and export the private configuration.
3. **Stop the previous client.** One grinder identity must have one active owner.
4. **Build and provision** using the matching guide. Fresh imports start in monitor mode.
5. **Wait for readiness**, then test a new cup and verify the target before brewing.

| Your goal | Start here |
| --- | --- |
| One device with controls and a 1.69-inch LCD | [Integrated build, provisioning and OTA](docs/INTEGRATED.md) |
| Battery-powered 2.16-inch AMOLED | [AMOLED branch, build and field status](docs/AMOLED216.md) |
| ESP-Mosaico with 16 MB PSRAM | [S31 toolchain, installation and field status](docs/MOSAICO.md) |
| A small feedback bridge with browser status | [Lightweight companion setup](docs/COMPANION.md) |
| A display connected to an existing Hub | [External-Hub preview](docs/TOUCHSCREEN.md) |

For the integrated LCD build, from a clone with ESP-IDF **5.5.4** installed:

```sh
git clone https://github.com/zpmdd/gbs-anywhere-esp32.git
cd gbs-anywhere-esp32
. "$HOME/esp/esp-idf/export.sh"
cd firmware/integrated
sh tools/check-integrated.sh
sh tools/build-integrated.sh touchscreen
```

These commands build and test. Follow the board-specific guide for backup,
initial provisioning and installation. Later application OTA uses the existing
configuration; it is not a first-install NVS import.

The `mosaico` profile uses a separate **ESP-IDF 6.2 preview** checkout; S3
integrated profiles retain **5.5.4**. Follow the [Mosaico guide](docs/MOSAICO.md)
for its pinned SDK revision and board-specific partition layout.

## Languages

| Surface | Available languages |
| --- | --- |
| **Project overview** | [English](README.md), [简体中文](README.zh-CN.md), [Deutsch](README.de.md) |
| **Lightweight browser status UI** | English, Deutsch, Français, Italiano, 日本語, 한국어, 简体中文, 繁體中文 |
| **Native LCD / AMOLED UI** | Chinese labels in the current implementation. The eight browser translations do not apply to the native screens. |
| **Technical guides** | English; contributions and translation corrections are welcome. |

The browser follows your preferred language, remembers manual selection and
ships translations inside the firmware. No translation service is needed.

## Validation, limits and contributing

- [LCD recipe following: one cup in each direction](docs/RECIPE_FOLLOW_ACCEPTANCE.md)
- [LCD scale-state OTA and physical off/on/off cycle](docs/SCALE_STATE_ACCEPTANCE.md)
- [AMOLED build-specific validation and remaining checks](docs/AMOLED216.md#validation-status-2026-10-23)
- [ESP-Mosaico installation and build-specific validation](docs/MOSAICO.md)
- [Progress, open work and source boundaries](docs/PROGRESS.md)
- [Contributing and reporting a hardware result](CONTRIBUTING.md)

This development firmware has no production Secure Boot / Flash Encryption
certification. Protect NVS backups and credentials, use a trusted LAN and keep
only one active bridge for a paired grinder. Endurance, outages and untested
models remain open; a build or CI pass is not hardware acceptance.

## Credits & license

An independent C / ESP-IDF companion to
[Skamba/gbs-anywhere](https://github.com/Skamba/gbs-anywhere), with protocol work
from Skamba, erikp152 and contributors. It is maintained separately from the
Rust desktop application; their feature sets and version numbers differ.

**AGPL-3.0-or-later.** See [LICENSE](LICENSE), [NOTICE](NOTICE) and
[upstream review notes](docs/UPSTREAM.md). LVGL, Noto Sans CJK, Inter, Tabler
Icons and board components retain their own licenses. Not affiliated with
Mahlkönig, Hemro, La Marzocco or Waveshare.
