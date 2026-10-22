#include "hub_http_response.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#define HUB_HTTP_RESPONSE_INITIAL_CAPACITY 2048U

static esp_err_t remember_error(hub_http_response_t *response, esp_err_t error)
{
    if (response->append_error == ESP_OK) {
        response->append_error = error;
    }
    return response->append_error;
}

esp_err_t hub_http_response_append(hub_http_response_t *response, const void *data, size_t len)
{
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (response->append_error != ESP_OK) {
        return response->append_error;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return remember_error(response, ESP_ERR_INVALID_ARG);
    }

    size_t max_capacity = response->max_capacity;
    if (max_capacity == 0) {
        max_capacity = HUB_HTTP_RESPONSE_DEFAULT_MAX_CAPACITY;
        response->max_capacity = max_capacity;
    }
    if (response->len >= max_capacity || len > max_capacity - response->len - 1) {
        return remember_error(response, ESP_ERR_INVALID_SIZE);
    }

    size_t needed = response->len + len + 1;
    if (needed > response->capacity) {
        size_t new_capacity = response->capacity;
        if (new_capacity == 0) {
            new_capacity = max_capacity < HUB_HTTP_RESPONSE_INITIAL_CAPACITY
                ? max_capacity
                : HUB_HTTP_RESPONSE_INITIAL_CAPACITY;
        }
        while (new_capacity < needed) {
            if (new_capacity > max_capacity / 2) {
                new_capacity = max_capacity;
                break;
            }
            new_capacity *= 2;
        }
        if (new_capacity < needed || new_capacity > max_capacity) {
            return remember_error(response, ESP_ERR_INVALID_SIZE);
        }

        char *new_data = heap_caps_realloc(
            response->data,
            new_capacity,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (new_data == NULL) {
            new_data = heap_caps_realloc(response->data, new_capacity, MALLOC_CAP_8BIT);
        }
        if (new_data == NULL) {
            return remember_error(response, ESP_ERR_NO_MEM);
        }
        response->data = new_data;
        response->capacity = new_capacity;
    }

    memcpy(response->data + response->len, data, len);
    response->len += len;
    response->data[response->len] = '\0';
    return ESP_OK;
}

esp_err_t hub_http_response_finish(
    const hub_http_response_t *response,
    int64_t expected_bytes,
    bool chunked
)
{
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (response->append_error != ESP_OK) {
        return response->append_error;
    }
    if (!chunked && expected_bytes >= 0 && expected_bytes != (int64_t)response->len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

void hub_http_response_reset(hub_http_response_t *response)
{
    if (response == NULL) {
        return;
    }
    free(response->data);
    memset(response, 0, sizeof(*response));
}
