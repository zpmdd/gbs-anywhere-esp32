#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
case "${1:-touchscreen}" in
 mosaico) idf.py --preview -B build-integrated-mosaico -D IDF_TARGET=esp32s31 -D HUB_MOSAICO=ON -D SDKCONFIG_DEFAULTS='sdkconfig.mosaico' build ;;
 amoled216) idf.py -B build-integrated-amoled216 -D HUB_AMOLED_216=ON -D HUB_TOUCHSCREEN=ON -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.integrated;sdkconfig.amoled216' build ;;
 touchscreen) idf.py -B build-integrated -D HUB_AMOLED_216=OFF -D HUB_TOUCHSCREEN=ON -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.integrated;sdkconfig.touchscreen' build ;;
 headless) idf.py -B build-integrated-headless -D HUB_AMOLED_216=OFF -D HUB_GRINDER=ON -D HUB_TOUCHSCREEN=OFF -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.integrated' build ;;
 *) echo 'Usage: tools/build-integrated.sh [touchscreen|headless|amoled216|mosaico]' >&2; exit 2 ;;
esac
