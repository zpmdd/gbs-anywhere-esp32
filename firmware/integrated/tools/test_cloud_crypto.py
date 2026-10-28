#!/usr/bin/env python3
"""Check production Cloud proofs/signatures against a native crypto build and OpenSSL."""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

from validate_stability_foundation import function_body

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#if ESP_IDF_VERSION_MAJOR >= 6
#include "psa/crypto.h"
#else
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#endif
typedef int esp_err_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=1, ESP_ERR_NO_MEM=2, ESP_ERR_INVALID_SIZE=3 };
typedef struct { char installation_id[40],private_key_b64[256]; unsigned char secret[32]; } installation_key_t;
static bool hash_fails;
static int checked_md(const mbedtls_md_info_t *info,const unsigned char *data,size_t size,unsigned char *out) {
    return hash_fails ? -1 : mbedtls_md(info,data,size,out);
}
#define mbedtls_md checked_md
/* FUNCTIONS */
int main(int argc,char **argv) {
    assert(argc==2);
#if ESP_IDF_VERSION_MAJOR >= 6
    assert(psa_crypto_init()==PSA_SUCCESS);
#endif
    installation_key_t key={0};
    assert(strlen(argv[1])<sizeof(key.private_key_b64));
    strcpy(key.private_key_b64,argv[1]);
    for(unsigned i=0;i<sizeof(key.secret);++i) key.secret[i]=(unsigned char)i;
    char proof[64]={0},signature[160]={0};
    assert(generate_request_proof("synthetic.request",key.secret,proof,sizeof(proof))==ESP_OK);
    puts(proof);
    assert(sign_with_installation_key(&key,"synthetic.signature",signature,sizeof(signature))==ESP_OK);
    puts(signature);
    assert(sign_with_installation_key(&key,"synthetic.signature",signature,1)==ESP_ERR_INVALID_SIZE);
    hash_fails=true;
    strcpy(proof,"unchanged");
    assert(generate_request_proof("synthetic.request",key.secret,proof,sizeof(proof))==ESP_FAIL);
    assert(!strcmp(proof,"unchanged"));
    assert(sign_with_installation_key(&key,"synthetic.signature",signature,sizeof(signature))==ESP_FAIL);
    hash_fails=false;
    strcpy(key.private_key_b64,"eA==");
    assert(sign_with_installation_key(&key,"synthetic.signature",signature,sizeof(signature))==ESP_FAIL);
    strcpy(key.private_key_b64,"!invalid!");
    assert(sign_with_installation_key(&key,"synthetic.signature",signature,sizeof(signature))==ESP_FAIL);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--crypto-source', type=Path, required=True,
                        help='Mbed TLS 3 root or TF-PSA-Crypto root')
    parser.add_argument('--crypto-build', type=Path, required=True, help='Matching native CMake build')
    parser.add_argument('--idf-major', type=int, choices=(5, 6), required=True)
    args = parser.parse_args()
    code = (ROOT / 'main/hub_cloud.c').read_text()
    pieces = []
    for name in ('base64_decode_alloc', 'base64_encode_alloc', 'generate_request_proof', 'sign_with_installation_key'):
        signature = re.search(r'static esp_err_t ' + name + r'\([^;{]*\)\s*\{', code)
        assert signature
        pieces.append(signature.group() + function_body(code, name) + '}')
    with tempfile.TemporaryDirectory(prefix='hub-cloud-crypto-') as temporary:
        directory = Path(temporary)
        private = directory / 'synthetic.pem'
        public = directory / 'public.pem'
        subprocess.run(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', str(private)], check=True, capture_output=True)
        der = subprocess.run(['openssl', 'pkey', '-in', str(private), '-outform', 'DER'], check=True, capture_output=True).stdout
        subprocess.run(['openssl', 'pkey', '-in', str(private), '-pubout', '-out', str(public)], check=True, capture_output=True)
        path = directory / 'crypto.c'
        path.write_text(HARNESS.replace('/* FUNCTIONS */', '\n'.join(pieces)))
        includes = [args.crypto_build / 'include', args.crypto_source / 'include']
        if args.idf_major == 6:
            includes.append(args.crypto_source / 'drivers/builtin/include')
            libraries = [args.crypto_build / 'core/libtfpsacrypto.a']
        else:
            libraries = [args.crypto_build / 'library/libmbedcrypto.a',
                         args.crypto_build / '3rdparty/everest/libeverest.a',
                         args.crypto_build / '3rdparty/p256-m/libp256m.a']
        executable = directory / 'crypto'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-g', '-DESP_IDF_VERSION_MAJOR=' + str(args.idf_major),
                        *['-I' + str(p.resolve()) for p in includes], str(path),
                        *[str(p.resolve()) for p in libraries], '-o', str(executable)], check=True)
        result = subprocess.run([str(executable), base64.b64encode(der).decode()], check=True, capture_output=True, text=True)
        proof, signature = result.stdout.splitlines()
        work = bytearray(range(32))
        for value in b'synthetic.request':
            index = value % 32
            shift = work[(index + 1) % 32] & 7
            mixed = value ^ work[index]
            work[index] = ((mixed << shift) | (mixed >> (8 - shift))) & 255
        assert proof == base64.b64encode(hashlib.sha256(work).digest()).decode()
        signature_path = directory / 'signature.der'
        signature_path.write_bytes(base64.b64decode(signature, validate=True))
        data = directory / 'data'
        data.write_bytes(b'synthetic.signature')
        subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(public),
                        '-signature', str(signature_path), str(data)], check=True, capture_output=True)
    print(f'Cloud crypto PASS: IDF {args.idf_major} API, SHA-256 proof, OpenSSL-verified DER signature, invalid keys, hash failure and bounded output')


if __name__ == '__main__':
    main()
