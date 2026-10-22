#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define HUB_IDENTITY_HUB_ID_CAPACITY 37
#define HUB_IDENTITY_SPKI_SHA256_HEX_CAPACITY 65
#define HUB_IDENTITY_HID_SUMMARY_CAPACITY 13
#define HUB_IDENTITY_CREDENTIAL_VERSION 1

typedef struct {
    const uint8_t *certificate_pem;
    size_t certificate_pem_len;
    const uint8_t *private_key_pem;
    size_t private_key_pem_len;
} hub_identity_tls_material_t;

/*
 * Initializes the persistent Hub identity only when Secure Boot, Flash
 * Encryption, and encrypted NVS are all active. ESP_ERR_NOT_SUPPORTED is the
 * expected fail-closed result on development hardware that has not been fully
 * security provisioned.
 */
esp_err_t hub_identity_init(void);
bool hub_identity_ready(void);
const char *hub_identity_hub_id(void);
const char *hub_identity_spki_sha256(void);
const char *hub_identity_hid_summary(void);
esp_err_t hub_identity_tls_material(hub_identity_tls_material_t *material);
void hub_identity_forget_private_key(void);
