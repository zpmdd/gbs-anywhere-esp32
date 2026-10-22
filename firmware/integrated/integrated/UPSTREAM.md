# Integrated source provenance

The display, workflow, protocol parser, fonts, icons, simulator and offline tests
are vendored from https://github.com/zpmdd/gbs-anywhere-esp32 at
`423e0866756e0372d0d98520455d6ac09466578b`. See NOTICE and LICENSE for
AGPL-3.0-or-later attribution and individual asset licenses.

The standalone upstream repository remains independently buildable. This port
uses the LamaBuddy Hub for Wi-Fi, time, cloud authentication, machine selection,
command admission, App endpoints and authenticated local OTA. It creates no
second Hub client or separate cloud identity.

Port-specific files: main/network.c, display/gbs_hub_client.c. Integration edits:
main/main.c startup, display/gbs_display.c hardware probing, UI setup text.
Compare these files deliberately when updating the pinned source; run both the
workflow tests and Hub integration tests before advancing the pin.

## 2026-10-19 upstream review

Fetched `Skamba/gbs-anywhere` main at
`40bb37230e097427d1758a2f790330298cc75e92`, advancing the previous reviewed
`b8f7dc2` reference. Compared the Cloud authentication/coffee parser and machine
job/result paths. Authentication paths/algorithms are unchanged. New Rust/web
history and settings/log/update UI, CSRF hardening and abort-cause events are
not imported as a second server. The integrated code keeps measured Cloud
extraction duration, stable outcomes and redacted service logs. This is a
compatibility review of the new upstream range, not a claim that every upstream
commit has been merged into this C/ESP-IDF port.

## 2026-10-22 upstream review and field result

Fetched and rechecked `Skamba/gbs-anywhere` main at
`ebaac65901fee0570ac26f9fda202c61f74ba055` before publishing this integration.
The vendored companion baseline remains `423e086`; advancing the reviewed Rust
reference does not replace that source pin or imply a whole-tree merge.

| Upstream change | Integrated-port disposition |
| --- | --- |
| `40fbf3b`: null/missing `LAST_COFFEE.lastCoffees` | Ported to the C parser with regression tests; invalid outer output and wrong list types still fail. |
| `164dfe3`: duration targets below 1 ms are absent | Ported and tested in the job parser. |
| `b67e6fc` / `164dfe3`: target-relative adjustment-window hints | Reviewed; deferred UI metadata. The port reports measured results and leaves physical adjustment eligibility to the grinder, without claiming every acknowledged result adjusts it. |
| Rust control API renames, Web history/settings, stop signals and pairing-file takeover | Reviewed; not imported into the Hub's existing API, single worker or NVS identity lifecycle. Preserve the single active client and existing no-replay behavior. |

The C implementation now also supports one Owner-authenticated ordinary log
export per accepted fresh grind, exact `grind_uuid` matching, bounded ZIP/CSV
parsing in PSRAM, and guarded Hub preset selection before extraction. This is
an integrated-port extension, not an upstream Generic WiFi API addition.
One 20 g -> 30 g real cup passed on build `c88791b2`; see
[the acceptance report](../../../docs/RECIPE_FOLLOW_ACCEPTANCE.md)
for timing, exact-build evidence and remaining hardware checks.
