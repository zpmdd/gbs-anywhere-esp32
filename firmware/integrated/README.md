# Integrated ESP32-S3 application

This directory is a self-contained ESP-IDF application combining the LamaBuddy
Hub, Generic WiFi grinder worker and optional native touchscreen. It builds
from this public checkout without another Hub repository or a second ESP.

Start with [build, provisioning and LAN update instructions](../../docs/INTEGRATED.md).
See [field acceptance](../../docs/RECIPE_FOLLOW_ACCEPTANCE.md) for the tested
binary and [source provenance](UPSTREAM.md) for import boundaries.

From this directory, after sourcing ESP-IDF 5.5.4:

```sh
sh tools/check-integrated.sh
sh tools/build-integrated.sh touchscreen
# Or, for an 8 MB board with 8 MB PSRAM:
sh tools/build-integrated.sh headless
# Experimental 2.16-inch AMOLED board:
sh tools/build-integrated.sh amoled216
```

The original Hub-only build remains available with `idf.py build`. All four
profiles use separate build directories. These commands only compile and test;
they do not discover, contact or change any device.

The AMOLED board has field evidence for rotation/touch, battery power cycles,
scale battery retention, audible node cues and real cups. Exact-build evidence
remains in the private field report; the subsequent manual-weight display fix
still needs its own physical cross-cup check. See the
[AMOLED profile guide](../../docs/AMOLED216.md) for its default always-on
display, USB-C-right orientation with optional automatic rotation, muted
650/760 ms success/failure cues, physical buttons and remaining validation gates.

AMOLED application updates use the same authenticated OTA routes and tool as
the other integrated profiles. Retain the previous application image and a
private configuration/NVS backup. OTA updates an application partition without
rewriting NVS; first-install NVS import is not an update procedure. The AMOLED
profile uses one eight-row DMA buffer to leave internal RAM for services; this is
not a runtime performance acceptance claim.
