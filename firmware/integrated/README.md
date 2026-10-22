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
```

The original Hub-only build remains available with `idf.py build`. All three
profiles use separate build directories. These commands only compile and test;
they do not discover, contact or change any device.
