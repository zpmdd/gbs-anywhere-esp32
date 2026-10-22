#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define HUB_HTTP_RESPONSE_DEFAULT_MAX_CAPACITY 65536U

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
    size_t max_capacity;
    esp_err_t append_error;
} hub_http_response_t;

esp_err_t hub_http_response_append(hub_http_response_t *response, const void *data, size_t len);
esp_err_t hub_http_response_finish(
    const hub_http_response_t *response,
    int64_t expected_bytes,
    bool chunked
);
void hub_http_response_reset(hub_http_response_t *response);
