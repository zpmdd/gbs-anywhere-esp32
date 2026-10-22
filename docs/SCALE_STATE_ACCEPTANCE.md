# Scale-state OTA acceptance, 2026-10-22

The scale-cache correction passed one physical off -> on -> off cycle on the
existing Waveshare ESP32-S3-Touch-LCD-1.69 (16 MB flash / 8 MB PSRAM), designated
device 3. Only that board received the authorized application OTA; device 1 and
the new AMOLED board were not accessed. This report publishes the recorded
results without another device operation.

## Exact deployed image

| Item | Verified value |
| --- | --- |
| Source commit | `975ebc50909526b63c2750da3d6a9daa0d6e74a4`, published through [PR #3](https://github.com/zpmdd/gbs-anywhere-esp32/pull/3) |
| Previous build | `c88791b2`, the build used for both [recipe-follow cups](RECIPE_FOLLOW_ACCEPTANCE.md) |
| New build ID / application version | `b923fa91` / `975ebc5` |
| Application size | 2,338,144 bytes |
| Application SHA-256 | `f535046100b8425bc72b2542e8d6c6c82ad66c3a319fcf4a3049f884db29ee5e` |
| ELF SHA-256 | `b923fa919f41dd7f4ac9c6dbfb69941f04233b87bd3baaf62ca8068f1a8057d0` |
| SDK in this image | `v5.5.4-777-g0f4a3e8c2e-dirty`; public CI separately pins official ESP-IDF 5.5.4 |
| OTA destination | `ota_1`; running and boot partitions both confirmed after restart |

Before uploading, the target model, previous build and 4 MiB destination slot
were checked. The grinder was ready with no active or pending task. The upload
wrote only the application image, followed by the authorized restart; no NVS or
partition-table image was imported. The new running build ID was checked against
the local image, separately from the health endpoint's availability check.

## Physical scale cycle

The user operated the scale. The Mac only read cached Hub diagnostics during
the cycle: it issued no manual Cloud refresh, grinding request or coffee-machine
control. Automatic keeper reads supplied the new accessory observations.
Times below are the **first sampled states**, in Beijing time (UTC+08:00).

| First sample | User operation / observed state | `scaleConnected` | `thingScaleConnected` | `scaleBatteryLevel` |
| --- | --- | --- | --- | --- |
| 18:46:15.868 | Initially powered off | `false` | `false` | `null` |
| 18:47:04.989 | Powered on and connected to the machine | `true` | `true` | `28` |
| 18:48:11.571 | Powered off again | `false` | `false` | `null` |

Before the update, the off scale still had cached 30% battery. After the update,
that old battery was absent; the new 28% reading appeared only while connected
and cleared when disconnection was observed. Subsequent refreshes retained the
correct off state. The final check also confirmed the scale remained off.

## Observation window

- 58 samples containing 174 cached GET reads over 252 seconds; zero read failures.
- Eight distinct `scaleObservedMs` observations, with seven intervals of
  32.025 to 37.088 seconds, including request time. These are refresh intervals,
  not measured power-switch-to-display latency; user reply and sampling times
  do not establish an exact physical switch timestamp.
- Uptime remained monotonic and the build ID stayed `b923fa91` throughout the
  sampled window. No reboot was observed in that window.
- Sampled internal free heap ranged from 16,579 to 20,251 bytes; the smallest
  sampled largest internal block was 7,680 bytes. These samples do not establish
  instantaneous memory peaks, stack headroom or long-duration stability.
- The grinder remained `active` / `ready`, with no active or pending task and
  its success counter unchanged at 12. Selected Dose1 / 20 g and stored Dose2 /
  30 g remained unchanged. No coffee was made on the new build during this test.

Local and CI regressions additionally cover expiry, missing/invalid fields,
binding changes, heap admission, foreground priority and retry backoff. The
physical cycle above does not independently certify those fault cases. See the
[shared scale contract](INTEGRATED.md#scale-observations).

## Remaining acceptance

1. A real cup on `b923fa91`, followed by repeated alternating 20 g / 30 g handles
   with target confirmation before brewing and result/adjustment verification.
2. Scale-off weight changes retained when the scale powers on, and agreement
   between App and device displays. No preset command was issued in this cycle.
3. Extended operation, Cloud/network interruption, on-device stale/unknown
   transitions, and physical touch latency while TLS and recipe lookup are busy.
4. The separately developed AMOLED board, integrated headless and other boards
   require their own hardware acceptance. Shared logic does not transfer this
   board's acceptance to them.

Earlier cups belong to `c88791b2`; this scale cycle belongs to `b923fa91`.
Neither certifies the other's untested behavior. The firmware remains
experimental. Raw logs, credentials, network addresses and device identities
remain private; this document contains the sanitized aggregate record only.
