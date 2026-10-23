# Documentation image sources

All published images use synthetic data. No private framebuffer, real device
identifier, network address, pairing identity or raw device log is included.
The gallery is visual documentation, not an additional hardware test.

## Current showcase (2026-10-23)

| Files | Source |
| --- | --- |
| `amoled-home`, `amoled-result`, `amoled-link`, `amoled-history`, `amoled-settings` | PNG exports of the compiled 480 × 480 LVGL simulator's `--review` frames: `ready`, `received`, `link-accepted`, `history`, `settings`. Source: `cb52fe2` on the AMOLED adaptation branch. |
| `amoled-warmup` | The same renderer's `--self-test` frame `boiler-warming-both`; synthetic future ready times, not a cold machine observation. |
| `lcd-home`, `lcd-steam` | 240 × 280 integrated simulator `--self-test` frames `home-telemetry` and `steam`, from the same source tree. |
| `web-status-en`, `web-status-de` | Chrome captures of the real root `main/index.html` with a local, read-only `/api/state` fixture. Firmware text is `demo`; all cup, time, connection and memory values are synthetic. No grinder or vendor Cloud was contacted. |
| `project-cover`, `project-cover-mobile` | Original presentation layout using the unedited `amoled-home` and `amoled-result` images, resized proportionally. The surrounding frame is decorative, not a board photo. |

The simulator can be rebuilt from the public AMOLED branch with ESP-IDF and
SDL2 available:

```sh
. "$HOME/esp/esp-idf/export.sh"
cmake -S firmware/integrated/integrated/simulator -B build-simulator-amoled \
  -DHUB_AMOLED_216=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-simulator-amoled --target gbs_touch_simulator
mkdir -p build-screenshots/review build-screenshots/tests
SDL_VIDEODRIVER=dummy build-simulator-amoled/gbs_touch_simulator \
  --review build-screenshots/review
SDL_VIDEODRIVER=dummy build-simulator-amoled/gbs_touch_simulator \
  --self-test build-screenshots/tests
```

The renderer exports BMP files; the documentation stores lossless RGB PNG
conversions. Browser captures and the cover are documentation compositions,
not firmware features. Keep their synthetic-data captions when reusing them.

## Earlier previews

`touchscreen-preview.png`, `daily-controls-preview.png` and
`schedule-history-preview.png` are earlier compiled 240 × 280 native UI
compositions. Their values are synthetic, and the external-Hub preview's
acceptance boundaries are recorded in [its guide](../TOUCHSCREEN.md).

## Attribution

The screenshots retain the project's UI and its third-party attribution:
LVGL, Noto Sans CJK, Inter and Tabler Icons. Font/icon licenses are retained in
`display/fonts/` and `display/icons/` at repository root and in the integrated
source notices. The cover typography also uses Inter. See the repository's
[NOTICE](../../NOTICE) and [LICENSE](../../LICENSE); image reuse does not remove
third-party license obligations.
