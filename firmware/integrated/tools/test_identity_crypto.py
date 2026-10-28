#!/usr/bin/env python3
"""Exercise identity crypto with a host Mbed TLS source/build pair (3.x or 4.x)."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    source, build = map(Path, sys.argv[1:3])
    identity = (ROOT / 'main/hub_identity.c').read_text().split('static esp_err_t read_nvs_string(')[0]
    identity = identity.replace('#include "hub_security.h"', '').replace('#include "nvs.h"', '')
    check = r'''
#include <assert.h>
int main(void) {
#if MBEDTLS_VERSION_MAJOR >= 4
    assert(psa_crypto_init() == PSA_SUCCESS);
#endif
    assert(generate_material() == ESP_OK);
    assert(hub_id_valid(s_hub_id));
    unsigned char key[sizeof(s_private_key_pem)], cert[sizeof(s_certificate_pem)];
    char fingerprint[sizeof(s_spki_sha256)];
    memcpy(key, s_private_key_pem, sizeof(key));
    memcpy(cert, s_certificate_pem, sizeof(cert));
    memcpy(fingerprint, s_spki_sha256, sizeof(fingerprint));
    assert(validate_loaded_material() == ESP_OK);
    assert(strcmp(fingerprint, s_spki_sha256) == 0);
    FILE *file = fopen("certificate.pem", "wb"); assert(file);
    assert(fwrite(cert, 1, strlen((char *)cert), file) == strlen((char *)cert)); fclose(file);
    file = fopen("private.pem", "wb"); assert(file);
    assert(fwrite(key, 1, strlen((char *)key), file) == strlen((char *)key)); fclose(file);
    file = fopen("fingerprint.txt", "w"); assert(file); fputs(fingerprint, file); fclose(file);
    for (unsigned i = 0; i < 20; ++i) {
        assert(generate_material() == ESP_OK);
        memcpy(s_certificate_pem, cert, sizeof(cert));
        assert(validate_loaded_material() != ESP_OK);
    }
    memcpy(s_private_key_pem, key, sizeof(key));
    assert(validate_loaded_material() == ESP_OK);
    assert(strcmp(fingerprint, s_spki_sha256) == 0);
    strcpy((char *)s_certificate_pem, "invalid certificate");
    assert(validate_loaded_material() != ESP_OK);
    memcpy(s_certificate_pem, cert, sizeof(cert));
    strcpy((char *)s_private_key_pem, "invalid key");
    assert(validate_loaded_material() != ESP_OK);
    memcpy(s_private_key_pem, key, sizeof(key));
    s_hub_id[0] = 'z';
    assert(validate_loaded_material() != ESP_OK);
    assert(compute_spki_sha256(NULL, fingerprint, sizeof(fingerprint)) == ESP_ERR_INVALID_ARG);
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(s_private_key_pem, sizeof(s_private_key_pem));
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='hub-identity-crypto-') as temporary:
        directory = Path(temporary)
        (directory / 'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG -2\n#define ESP_ERR_INVALID_STATE -3\n')
        (directory / 'esp_random.h').write_text('''#include <stdio.h>
#include <stdlib.h>
static void esp_fill_random(void *dest, size_t size) {
    FILE *random = fopen("/dev/urandom", "rb");
    if (random == NULL || fread(dest, 1, size, random) != size) abort();
    if (fclose(random) != 0) abort();
}
''')
        harness = directory / 'identity.c'
        harness.write_text(identity + check)
        includes = [directory, ROOT / 'main', source / 'include']
        crypto = 'libmbedcrypto.a'
        if (source / 'tf-psa-crypto').is_dir():
            includes.extend([source / 'tf-psa-crypto/include',
                             source / 'tf-psa-crypto/drivers/builtin/include'])
            crypto = 'libtfpsacrypto.a'
        libraries = [next(build.rglob(name)) for name in ('libmbedx509.a', crypto)]
        executable = directory / 'identity'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                        *[f'-I{path}' for path in includes], str(harness),
                        *map(str, libraries), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], cwd=directory, check=True)
        subprocess.run(['openssl', 'verify', '-CAfile', 'certificate.pem', 'certificate.pem'],
                       cwd=directory, check=True, capture_output=True)
        public_key = subprocess.run(['openssl', 'pkey', '-in', 'private.pem', '-pubout', '-outform', 'DER'],
                                    cwd=directory, check=True, capture_output=True).stdout
        assert hashlib.sha256(public_key).hexdigest() == (directory / 'fingerprint.txt').read_text()
    print('Identity crypto PASS: P-256 certificate, stable SPKI, invalid/mismatched material rejected')


if __name__ == '__main__':
    main()
