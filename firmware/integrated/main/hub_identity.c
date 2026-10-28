#include "hub_identity.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "hub_security.h"
#include "mbedtls/version.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/x509_crt.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#else
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#endif
#include "nvs.h"

#define HUB_IDENTITY_SCHEMA_VERSION 1
#define HUB_IDENTITY_PRIVATE_KEY_CAPACITY 512
#define HUB_IDENTITY_CERTIFICATE_CAPACITY 1536
#define HUB_IDENTITY_SPKI_DER_CAPACITY 256

static const char *HUB_NAMESPACE = "lamabuddy";
static const char *HUB_ID_KEY = "hub_id";
static const char *TLS_KEY_KEY = "tls_key";
static const char *TLS_CERT_KEY = "tls_cert";
static const char *TLS_SCHEMA_KEY = "tls_schema";

static bool s_ready;
static char s_hub_id[HUB_IDENTITY_HUB_ID_CAPACITY];
static char s_spki_sha256[HUB_IDENTITY_SPKI_SHA256_HEX_CAPACITY];
static char s_hid_summary[HUB_IDENTITY_HID_SUMMARY_CAPACITY];
static unsigned char s_private_key_pem[HUB_IDENTITY_PRIVATE_KEY_CAPACITY];
static unsigned char s_certificate_pem[HUB_IDENTITY_CERTIFICATE_CAPACITY];

#if MBEDTLS_VERSION_MAJOR < 4
static int identity_rng(void *context, unsigned char *output, size_t len)
{
    (void)context;
    esp_fill_random(output, len);
    return 0;
}
#endif

static bool is_hex_character(char value)
{
    return isxdigit((unsigned char)value) != 0;
}

static bool hub_id_valid(const char *hub_id)
{
    if (hub_id == NULL || strlen(hub_id) != HUB_IDENTITY_HUB_ID_CAPACITY - 1) {
        return false;
    }
    for (size_t index = 0; index < HUB_IDENTITY_HUB_ID_CAPACITY - 1; index++) {
        bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if ((separator && hub_id[index] != '-') || (!separator && !is_hex_character(hub_id[index]))) {
            return false;
        }
    }
    return hub_id[14] == '4'
        && (hub_id[19] == '8' || hub_id[19] == '9'
            || hub_id[19] == 'a' || hub_id[19] == 'b'
            || hub_id[19] == 'A' || hub_id[19] == 'B');
}

static void generate_hub_id(char *hub_id, size_t hub_id_len)
{
    uint8_t random_bytes[16];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    random_bytes[6] = (random_bytes[6] & 0x0fU) | 0x40U;
    random_bytes[8] = (random_bytes[8] & 0x3fU) | 0x80U;
    snprintf(
        hub_id,
        hub_id_len,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        random_bytes[0], random_bytes[1], random_bytes[2], random_bytes[3],
        random_bytes[4], random_bytes[5], random_bytes[6], random_bytes[7],
        random_bytes[8], random_bytes[9], random_bytes[10], random_bytes[11],
        random_bytes[12], random_bytes[13], random_bytes[14], random_bytes[15]
    );
    mbedtls_platform_zeroize(random_bytes, sizeof(random_bytes));
}

static esp_err_t compute_spki_sha256(
    const mbedtls_pk_context *key,
    char *digest_hex,
    size_t digest_hex_len
)
{
    if (key == NULL || digest_hex == NULL
        || digest_hex_len < HUB_IDENTITY_SPKI_SHA256_HEX_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned char der[HUB_IDENTITY_SPKI_DER_CAPACITY];
    int der_len = mbedtls_pk_write_pubkey_der(key, der, sizeof(der));
    if (der_len <= 0 || (size_t)der_len > sizeof(der)) {
        mbedtls_platform_zeroize(der, sizeof(der));
        return ESP_FAIL;
    }

    unsigned char digest[32];
#if MBEDTLS_VERSION_MAJOR >= 4
    size_t digest_len = 0;
    int result = psa_hash_compute(PSA_ALG_SHA_256,
        der + sizeof(der) - (size_t)der_len, (size_t)der_len,
        digest, sizeof(digest), &digest_len);
    if (result == PSA_SUCCESS && digest_len != sizeof(digest)) {
        result = PSA_ERROR_GENERIC_ERROR;
    }
#else
    int result = mbedtls_sha256(
        der + sizeof(der) - (size_t)der_len,
        (size_t)der_len,
        digest,
        0
    );
#endif
    mbedtls_platform_zeroize(der, sizeof(der));
    if (result != 0) {
        mbedtls_platform_zeroize(digest, sizeof(digest));
        return ESP_FAIL;
    }

    for (size_t index = 0; index < sizeof(digest); index++) {
        snprintf(digest_hex + (index * 2), digest_hex_len - (index * 2), "%02x", digest[index]);
    }
    digest_hex[HUB_IDENTITY_SPKI_SHA256_HEX_CAPACITY - 1] = '\0';
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return ESP_OK;
}

static esp_err_t validate_loaded_material(void)
{
    if (!hub_id_valid(s_hub_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    mbedtls_pk_context private_key;
    mbedtls_x509_crt certificate;
    mbedtls_pk_init(&private_key);
    mbedtls_x509_crt_init(&certificate);

    int result = mbedtls_pk_parse_key(
        &private_key,
        s_private_key_pem,
        strlen((const char *)s_private_key_pem) + 1,
        NULL,
        0
#if MBEDTLS_VERSION_MAJOR < 4
        , identity_rng, NULL
#endif
    );
    if (result == 0) {
        result = mbedtls_x509_crt_parse(
            &certificate,
            s_certificate_pem,
            strlen((const char *)s_certificate_pem) + 1
        );
    }
    if (result == 0) {
#if MBEDTLS_VERSION_MAJOR >= 4
        result = mbedtls_pk_check_pair(&certificate.pk, &private_key);
#else
        result = mbedtls_pk_check_pair(&certificate.pk, &private_key, identity_rng, NULL);
#endif
    }

    esp_err_t err = result == 0
        ? compute_spki_sha256(&private_key, s_spki_sha256, sizeof(s_spki_sha256))
        : ESP_ERR_INVALID_STATE;
    mbedtls_x509_crt_free(&certificate);
    mbedtls_pk_free(&private_key);
    return err;
}

static esp_err_t generate_material(void)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert certificate;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&certificate);

#if MBEDTLS_VERSION_MAJOR >= 4
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    int result = psa_generate_key(&attributes, &key_id);
    psa_reset_key_attributes(&attributes);
    if (result == PSA_SUCCESS) {
        result = mbedtls_pk_copy_from_psa(key_id, &key);
        psa_status_t destroy_result = psa_destroy_key(key_id);
        if (result == 0) result = destroy_result;
    }
#else
    int result = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (result == 0) {
        result = mbedtls_ecp_gen_key(
            MBEDTLS_ECP_DP_SECP256R1,
            mbedtls_pk_ec(key),
            identity_rng,
            NULL
        );
    }
#endif
    if (result != 0) {
        mbedtls_x509write_crt_free(&certificate);
        mbedtls_pk_free(&key);
        return ESP_FAIL;
    }

    generate_hub_id(s_hub_id, sizeof(s_hub_id));
    unsigned char serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7fU;
    serial[0] |= 0x01U;

    char common_name[80];
    snprintf(common_name, sizeof(common_name), "CN=LamaBuddy Hub %s", s_hub_id);
    mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&certificate, &key);
    mbedtls_x509write_crt_set_issuer_key(&certificate, &key);

    result = mbedtls_x509write_crt_set_serial_raw(&certificate, serial, sizeof(serial));
    if (result == 0) {
        result = mbedtls_x509write_crt_set_subject_name(&certificate, common_name);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_issuer_name(&certificate, common_name);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_validity(
            &certificate,
            "20240101000000",
            "20491231235959"
        );
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_key_usage(
            &certificate,
            MBEDTLS_X509_KU_DIGITAL_SIGNATURE
        );
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_subject_key_identifier(&certificate);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_authority_key_identifier(&certificate);
    }
    if (result == 0) {
        result = mbedtls_pk_write_key_pem(&key, s_private_key_pem, sizeof(s_private_key_pem));
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_pem(
            &certificate,
            s_certificate_pem,
            sizeof(s_certificate_pem)
#if MBEDTLS_VERSION_MAJOR < 4
            , identity_rng, NULL
#endif
        );
    }

    esp_err_t err = result == 0
        ? compute_spki_sha256(&key, s_spki_sha256, sizeof(s_spki_sha256))
        : ESP_FAIL;
    mbedtls_platform_zeroize(serial, sizeof(serial));
    mbedtls_platform_zeroize(common_name, sizeof(common_name));
    mbedtls_x509write_crt_free(&certificate);
    mbedtls_pk_free(&key);
    return err;
}

static esp_err_t read_nvs_string(
    nvs_handle_t handle,
    const char *key,
    unsigned char *value,
    size_t value_capacity,
    bool *found
)
{
    size_t required_len = value_capacity;
    esp_err_t err = nvs_get_str(handle, key, (char *)value, &required_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *found = false;
        value[0] = '\0';
        return ESP_OK;
    }
    *found = err == ESP_OK;
    return err;
}

static esp_err_t load_or_create_identity(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(HUB_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    bool hub_id_found = false;
    bool key_found = false;
    bool certificate_found = false;
    bool schema_found = false;
    uint8_t schema = 0;
    err = read_nvs_string(
        handle,
        HUB_ID_KEY,
        (unsigned char *)s_hub_id,
        sizeof(s_hub_id),
        &hub_id_found
    );
    if (err == ESP_OK) {
        err = read_nvs_string(
            handle,
            TLS_KEY_KEY,
            s_private_key_pem,
            sizeof(s_private_key_pem),
            &key_found
        );
    }
    if (err == ESP_OK) {
        err = read_nvs_string(
            handle,
            TLS_CERT_KEY,
            s_certificate_pem,
            sizeof(s_certificate_pem),
            &certificate_found
        );
    }
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, TLS_SCHEMA_KEY, &schema);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        } else if (err == ESP_OK) {
            schema_found = true;
        }
    }

    if (err == ESP_OK) {
        unsigned int found_count = (hub_id_found ? 1U : 0U)
            + (key_found ? 1U : 0U)
            + (certificate_found ? 1U : 0U)
            + (schema_found ? 1U : 0U);
        if (found_count == 4U && schema == HUB_IDENTITY_SCHEMA_VERSION) {
            err = validate_loaded_material();
        } else if (found_count == 0U) {
            err = generate_material();
            if (err == ESP_OK) {
                err = nvs_set_str(handle, HUB_ID_KEY, s_hub_id);
            }
            if (err == ESP_OK) {
                err = nvs_set_str(handle, TLS_KEY_KEY, (const char *)s_private_key_pem);
            }
            if (err == ESP_OK) {
                err = nvs_set_str(handle, TLS_CERT_KEY, (const char *)s_certificate_pem);
            }
            if (err == ESP_OK) {
                err = nvs_set_u8(handle, TLS_SCHEMA_KEY, HUB_IDENTITY_SCHEMA_VERSION);
            }
            if (err == ESP_OK) {
                err = nvs_commit(handle);
            }
        } else {
            /* Partial/corrupt identity must require explicit factory reset. */
            err = ESP_ERR_INVALID_STATE;
        }
    }
    nvs_close(handle);
    return err;
}

static void derive_hid_summary_from_hub_id(void)
{
    size_t output_index = 0;
    for (size_t input_index = 0;
         s_hub_id[input_index] != '\0'
            && output_index < HUB_IDENTITY_HID_SUMMARY_CAPACITY - 1;
         input_index++) {
        if (s_hub_id[input_index] == '-') {
            continue;
        }
        s_hid_summary[output_index++] =
            (char)tolower((unsigned char)s_hub_id[input_index]);
    }
    s_hid_summary[output_index] = '\0';
}

esp_err_t hub_identity_init(void)
{
    s_ready = false;
    memset(s_hub_id, 0, sizeof(s_hub_id));
    memset(s_spki_sha256, 0, sizeof(s_spki_sha256));
    memset(s_hid_summary, 0, sizeof(s_hid_summary));
    mbedtls_platform_zeroize(s_private_key_pem, sizeof(s_private_key_pem));
    memset(s_certificate_pem, 0, sizeof(s_certificate_pem));

    if (!hub_security_release_security_ready()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#if MBEDTLS_VERSION_MAJOR >= 4
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
#endif

    esp_err_t err = load_or_create_identity();
    if (err != ESP_OK) {
        hub_identity_forget_private_key();
        return err;
    }

    derive_hid_summary_from_hub_id();
    s_ready = true;
    return ESP_OK;
}

bool hub_identity_ready(void)
{
    return s_ready;
}

const char *hub_identity_hub_id(void)
{
    return s_ready ? s_hub_id : "";
}

const char *hub_identity_spki_sha256(void)
{
    return s_ready ? s_spki_sha256 : "";
}

const char *hub_identity_hid_summary(void)
{
    return s_ready ? s_hid_summary : "";
}

esp_err_t hub_identity_tls_material(hub_identity_tls_material_t *material)
{
    if (material == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || s_private_key_pem[0] == '\0' || s_certificate_pem[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    material->certificate_pem = s_certificate_pem;
    material->certificate_pem_len = strlen((const char *)s_certificate_pem) + 1;
    material->private_key_pem = s_private_key_pem;
    material->private_key_pem_len = strlen((const char *)s_private_key_pem) + 1;
    return ESP_OK;
}

void hub_identity_forget_private_key(void)
{
    mbedtls_platform_zeroize(s_private_key_pem, sizeof(s_private_key_pem));
}
