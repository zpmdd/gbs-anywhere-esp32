#!/bin/sh
# Build the exported SDK's real crypto libraries natively, then test production code.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
: "${IDF_PATH:?Source ESP-IDF export.sh first}"
crypto_source="$IDF_PATH/components/mbedtls/mbedtls"
crypto_work=$(mktemp -d "${TMPDIR:-/tmp}/lamabuddy-crypto-check.XXXXXX")
trap 'rm -rf "$crypto_work"' EXIT HUP INT TERM
crypto_build="$crypto_work/build"
crypto_flags=""
if [ -d "$crypto_source/tf-psa-crypto" ]; then
    # The ESP fork retains two legacy includes inside TF-PSA's software drivers.
    # Supply host-only forwarding headers; leave the SDK checkout untouched.
    mkdir -p "$crypto_work/include/mbedtls"
    printf '%s\n' '#include "mbedtls/private/bignum.h"' > "$crypto_work/include/mbedtls/bignum.h"
    printf '%s\n' '#include "mbedtls/private/ecp.h"' > "$crypto_work/include/mbedtls/ecp.h"
    crypto_flags="-I\"$crypto_work/include\""
fi
cmake -S "$crypto_source" -B "$crypto_build" \
    -DCMAKE_C_COMPILER="${CC:-cc}" -DCMAKE_C_FLAGS="$crypto_flags" \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DGEN_FILES=OFF
cmake --build "$crypto_build" --parallel 2
python "$root/tools/test_identity_crypto.py" "$crypto_source" "$crypto_build"
if [ -d "$crypto_source/tf-psa-crypto" ]; then
    python "$root/tools/test_cloud_crypto.py" --idf-major 6 \
        --crypto-source "$crypto_source/tf-psa-crypto" \
        --crypto-build "$crypto_build/tf-psa-crypto"
else
    python "$root/tools/test_cloud_crypto.py" --idf-major 5 \
        --crypto-source "$crypto_source" --crypto-build "$crypto_build"
fi
