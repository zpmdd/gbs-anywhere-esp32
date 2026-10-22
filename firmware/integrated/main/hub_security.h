#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t hub_security_init(void);
esp_err_t hub_security_get_local_token(char *token, size_t token_len);
esp_err_t hub_security_get_setup_token(char *token, size_t token_len);
bool hub_security_is_authorized(httpd_req_t *req);
bool hub_security_storage_protected(void);
bool hub_security_release_security_ready(void);
