# Contributing

Thanks for helping improve this experimental ESP32 companion.
[Choose a build](docs/COMPATIBILITY.md) before changing board-specific code.
The Rust desktop application is maintained separately at
[Skamba/gbs-anywhere](https://github.com/Skamba/gbs-anywhere).

## A useful issue or hardware report

Include the board's exact model, flash/PSRAM size, build ID, source revision,
ESP-IDF version, grinder model/firmware and coffee-machine model. Describe the
steps, expected behavior and observed result. Distinguish a local test, boot
check, completed control, measured cup and physical grinder adjustment.

Share only a sanitized excerpt. Remove credentials, certificates, NVS contents,
network addresses, machine serials, pairing identities and raw task IDs. Never
upload a flash backup or private configuration to an issue.

## Local development

Use a focused branch and synthetic fixtures. For S3, after activating ESP-IDF 5.5.4:

```sh
sh tools/check.sh
idf.py build
# For the integrated application:
cd firmware/integrated
sh tools/check-integrated.sh
sh tools/build-integrated.sh touchscreen
```

Use the matching [AMOLED branch](docs/AMOLED216.md) and profile for 2.16-inch
changes. Run the native simulator regression when changing UI behavior.
For ESP-Mosaico / S31, use the separate SDK and source checkout described in
[the Mosaico guide](docs/MOSAICO.md). Build `mosaico` before running host checks
so IDF 6 can download managed cJSON. The integrated `tools/check-crypto.sh`
checks the exported SDK's real identity and Cloud signing implementations.
Check `git diff --check` before submitting. Offline tests must not contact
hardware or vendor services.

Keep one owner for pairing, Cloud state and foreground control execution.
Preserve validation, bounded resource use, startup baselines and the handling
of ambiguous results. Do not add automatic replay after an unknown delivery.

## Documentation, translations and screenshots

- English is the technical-documentation baseline. Keep the
  [Chinese](README.zh-CN.md) and [German](README.de.md) overviews consistent with
  the same compatibility and validation facts.
- Browser translations are in `main/index.html`; `node tests/test_ui.cjs`
  checks the eight dictionaries and language behavior. Native-screen language
  support is separate.
- Use screenshots from the real renderer with synthetic data. Keep the
  [image provenance](docs/images/README.md) current and preserve asset licenses.
- A passing compile or an upstream feature is not evidence of physical support.

## Pull requests

Explain the problem, resulting behavior and relevant checks. State remaining
hardware limits plainly. Keep unrelated changes and private artifacts out of
the diff. Preserve the AGPL-3.0-or-later notices and third-party attribution.
