#include "hub_security.h"

#include <stdio.h>
#include <string.h>

#include "esp_flash_encrypt.h"
#include "esp_random.h"
#include "esp_secure_boot.h"
#include "hub_config.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"
#include "sdkconfig.h"

static void generate_token(char *token, size_t token_len)
{
    uint32_t random_parts[4] = {
        esp_random(),
        esp_random(),
        esp_random(),
        esp_random(),
    };
    snprintf(
        token,
        token_len,
        "lb-%08lx%08lx%08lx%08lx",
        (unsigned long)random_parts[0],
        (unsigned long)random_parts[1],
        (unsigned long)random_parts[2],
        (unsigned long)random_parts[3]
    );
}

esp_err_t hub_security_init(void)
{
    char token[HUB_CONFIG_TOKEN_LEN] = {0};
    esp_err_t err = hub_config_get_local_token(token, sizeof(token));
    if (err == ESP_OK) {
        mbedtls_platform_zeroize(token, sizeof(token));
        return ESP_OK;
    }

    generate_token(token, sizeof(token));
    err = hub_config_save_local_token(token);
    mbedtls_platform_zeroize(token, sizeof(token));
    return err;
}

esp_err_t hub_security_get_local_token(char *token, size_t token_len)
{
    return hub_config_get_local_token(token, token_len);
}

esp_err_t hub_security_get_setup_token(char *token, size_t token_len)
{
    return hub_security_get_local_token(token, token_len);
}

bool hub_security_is_authorized(httpd_req_t *req)
{
    char token[HUB_CONFIG_TOKEN_LEN] = {0};
    if (hub_security_get_local_token(token, sizeof(token)) != ESP_OK) {
        return false;
    }

    size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_len == 0 || header_len >= 96) {
        mbedtls_platform_zeroize(token, sizeof(token));
        return false;
    }

    char authorization[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", authorization, sizeof(authorization)) != ESP_OK) {
        mbedtls_platform_zeroize(token, sizeof(token));
        return false;
    }

    char expected[96] = {0};
    int written = snprintf(expected, sizeof(expected), "Bearer %s", token);
    size_t expected_len = written > 0 ? (size_t)written : 0;
    bool authorized = expected_len < sizeof(expected)
        && header_len == expected_len
        && mbedtls_ct_memcmp(authorization, expected, expected_len) == 0;
    mbedtls_platform_zeroize(token, sizeof(token));
    mbedtls_platform_zeroize(expected, sizeof(expected));
    mbedtls_platform_zeroize(authorization, sizeof(authorization));
    return authorized;
}

bool hub_security_storage_protected(void)
{
#if defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
    return esp_flash_encryption_enabled();
#else
    return false;
#endif
}

bool hub_security_release_security_ready(void)
{
    return hub_security_storage_protected() && esp_secure_boot_enabled();
}
