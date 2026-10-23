# Hardware and feature compatibility

[Overview](../README.md) · [中文](../README.zh-CN.md) · [Deutsch](../README.de.md)

Updated 2026-10-23. **Implemented**, **build-checked** and **field-tested** are
separate claims. Field results apply to a named board and binary, not every
variant of a product family. No ready-to-flash general release is implied.

## Boards and source locations

| Board | Build entry | Flash / PSRAM | Display | Source / evidence |
| --- | --- | --- | --- | --- |
| Waveshare ESP32-S3-Touch-LCD-1.69, integrated | `firmware/integrated`: `sh tools/build-integrated.sh touchscreen` | 16 MB / 8 MB octal | 240 × 280, ST7789 / CST816 | `main`. Physical touch, two recipe-follow cups and a separate OTA/scale-state cycle. |
| Waveshare ESP32-S3-Touch-AMOLED-2.16, integrated | `firmware/integrated`: `sh tools/build-integrated.sh amoled216` | 16 MB / 8 MB octal | 480 × 480, CO5300 / CST9220 | [AMOLED branch](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/amoled-216-local-adaptation). Physical rotation/touch, battery power cycles, retained scale battery and cup feedback. Latest display fix needs its own cross-cup check. |
| ESP32-S3, lightweight companion | Root: `idf.py build` | 8 MB / 8 MB octal | None | `main`. Historical feedback/boot evidence has [build-specific limits](UPSTREAM.md#validation). |
| ESP32-S3, integrated headless | `firmware/integrated`: `sh tools/build-integrated.sh headless` | 8 MB / 8 MB octal | None | `main`. Local build checks; dedicated hardware acceptance pending. |
| Waveshare 1.69-inch, external-Hub preview | Root: `GBS_TOUCHSCREEN=ON` | 16 MB / 8 MB octal | 240 × 280 | `main`. Synthetic UI/client checks; separate Hub required. See [guide](TOUCHSCREEN.md). |

Default display orientation is USB-C on the right. AMOLED automatic rotation
also maps touch to the chosen orientation. This is a board-specific adaptation;
matching an ESP32-S3 chip name alone does not establish pin or display compatibility.
Other ESP families, pinouts and memory layouts need separate work.

### Toolchains

- Integrated builds: **ESP-IDF 5.5.4**, as pinned in CI. A generic 5.5.0 install
  lacks an API needed by the pinned WebSocket component.
- Root companion: ESP-IDF 5.5, native USB Serial/JTAG, host C compiler, Python
  environment and Node.js 18+ for offline tests.
- Native simulators: SDL2, CMake and cJSON from the IDF environment. They use
  synthetic data and do not establish physical display acceptance.

## Grinders

| Model / protocol | Implemented path | Field evidence |
| --- | --- | --- |
| **Mahlkönig E65W GbS** | E65/E80 Generic WiFi client | Actual feedback and integrated recipe-follow cups on the recorded V01.36 setup. |
| **E65T / E80W / E80T GbS** | Same Generic WiFi family | Not physically verified by this port. Confirm the matching menu, firmware and pairing flow before testing. |
| **E64 WS** | Different Xenia-style server protocol | Not implemented in this ESP32 port. The upstream desktop has a separate E64 WS path. |
| Other grinders | None claimed | No compatibility claim. |

The family distinction comes from the
[upstream setup documentation](https://github.com/Skamba/gbs-anywhere#readme).
Upstream model support is not transferred as field acceptance of this C port.
Existing pairing is imported; this is not an on-device new-pairing wizard.

## Espresso machines and scales

| Device / feature | Scope |
| --- | --- |
| **La Marzocco Linea Mini R** | The field-tested coffee-machine setup for controls, Cloud results and Brew-by-Weight. |
| Other La Marzocco models | Not independently accepted. Controls depend on the selected machine's capabilities and available Cloud fields. |
| Other brands / manual shot entry | No implementation claimed here. The upstream desktop has a broader integration surface. |
| Machine-paired Brew-by-Weight scale | The machine/Cloud provides measured time and beverage weight. The ESP does not pair directly with the scale. |
| Scale status and battery | Existing dashboard/accessory observations; not continuous power detection. On AMOLED, a gray percentage is history, not evidence that the scale is currently connected. |
| Scale off | Does not gate quick-weight commands. The normal measured-cup workflow still needs actual time and beverage weight; recipe targets are never substituted as measurements. |

Internet, working Cloud authentication, valid time and a selected machine are
required. The local screen and LAN diagnostics do not make the workflow
Cloud-independent.

## Feature matrix

| Feature | Lightweight | Integrated LCD | Integrated AMOLED | Integrated headless |
| --- | --- | --- | --- | --- |
| Generic WiFi feedback | Yes | Yes | Yes | Yes |
| Persistent task journal / startup baseline | Yes | Yes | Yes | Yes |
| Browser status in 8 languages | Yes | Not this UI | Not this UI | Not this UI |
| Coffee-machine controls | No | Yes | Yes | API; hardware pending |
| Recipe target following | No | Yes | Yes | Code path; hardware pending |
| Native LVGL controls, history and existing-plan editing | No | Yes | Yes | No display |
| Authenticated application OTA | No | Yes | Yes | Yes |
| Framebuffer preview | No | 240 × 280 | 480 × 480 | No display |
| Scale battery retained across ESP shutdown | No | No | Yes | No display history |
| Device battery, automatic rotation, physical keys and node cues | No | No | Yes | No board-specific UI |

AMOLED entries refer to its development branch. The earlier external-Hub
preview is a separate root application and is intentionally not grouped with
integrated LCD support; see its [scope](TOUCHSCREEN.md).

## Field evidence and open checks

- LCD recipe following: [one cup in each direction](RECIPE_FOLLOW_ACCEPTANCE.md)
  on `c88791b2`; the reverse cup's physical adjustment was not separately confirmed.
- LCD scale-state update: [off/on/off cycle](SCALE_STATE_ACCEPTANCE.md) on
  `b923fa91`, without a new cup on that binary.
- AMOLED: [power, battery and cup evidence](AMOLED216.md#validation-status-2026-10-23),
  including accepted node cues on `f8b54b1a`; subsequent `211d2963` has local
  UI and startup checks, with the physical cross-cup check still open.

Long-duration use, real outages, battery runtime/charging transitions, further
models and production security gates remain open. A completed control, a
measured cup and an acknowledged grinder delivery are different observations.
