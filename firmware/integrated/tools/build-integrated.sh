#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
case "${1:-touchscreen}" in
 touchscreen) idf.py -B build-integrated -D HUB_TOUCHSCREEN=ON -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.integrated;sdkconfig.touchscreen' build ;;
 headless) idf.py -B build-integrated-headless -D HUB_GRINDER=ON -D HUB_TOUCHSCREEN=OFF -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.integrated' build ;;
 *) echo 'Usage: tools/build-integrated.sh [touchscreen|headless]' >&2; exit 2 ;;
esac
