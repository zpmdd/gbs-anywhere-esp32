# Integrated ESP32 application

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

The [ESP-Mosaico profile guide](../../docs/MOSAICO.md) covers the separate
ESP32-S31 build, first USB installation, identity handover and build-specific
field evidence. Mosaico uses a separate ESP-IDF 6.2 checkout. The validated
toolchain revision is `4d80c1b29aeabc8e735ffc6e1be9bb23e95d1d5d`; S31 is still an
IDF preview target. After exporting that SDK, run:

```sh
sh tools/build-integrated.sh mosaico
sh tools/check-integrated.sh
```

Build first: IDF 6 host checks require the managed cJSON sources downloaded by
the build. S3 and S31 application images are not interchangeable, including when
they have the same output filename.

This profile reuses the AMOLED interface and workflow with Mosaico-specific
display/touch orientation, BMI270, read-only BQ27220 battery monitoring, ES8311
audio and USB CDC. It has 16 MB PSRAM and three 5 MiB application partitions in
16 MB NOR flash. Its initial installation requires its own bootloader,
`partitions.mosaico.csv`, application and private NVS; never flash an S3 image
or change this layout through application-only OTA. Back up the original flash
first. The BSP detects CoreBoard revision from eFuse and preserves battery
calibration. The NAND flash is not modified or required.

The new device uses model `lamabuddy-esp32s31-mosaico` and hostname
`lamabuddy-mosaico`. A retired grinder identity must have only one running
owner. First-install imports start in monitor mode and establish a fresh
task baseline before activation. Final build `8d4984d3` has OTA/restart and
100-second, four-update BLE temperature evidence. Four-direction rotation and
touch were confirmed on `0436954d`, and sound on an earlier installation;
these were not repeated manual checks of the final build. S31 real cups, BLE
writes, prolonged stability and battery endurance remain open. More PSRAM is
capacity, not a measured speedup. See the guide for the exact artifact digest
and remaining hardware gates.

The original Hub-only build remains available with `idf.py build`. All five
profiles use separate build directories, but `dependencies.lock` and
`managed_components` are shared inside this source checkout. Use separate
checkouts or worktrees for S3 and S31; never build concurrently with both SDKs
in the same source checkout. A target/SDK switch may normally resolve the
shared dependency state again. These commands only compile and test; they do
not discover, contact or change any device.

The S3 2.16-inch AMOLED board has field evidence for rotation/touch, battery power cycles,
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
