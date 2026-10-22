#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#define HUB_HTTP_MAX_URI_HANDLERS 57
#define HUB_HTTP_LEGACY_MAX_OPEN_SOCKETS 2
#define HUB_HTTP_HTTPS_MAX_OPEN_SOCKETS 2

typedef enum {
    HUB_HTTP_TRANSPORT_LEGACY = 0,
    HUB_HTTP_TRANSPORT_HTTPS_V2,
} hub_http_transport_t;

esp_err_t hub_http_start(void);
esp_err_t hub_http_register_routes(
    httpd_handle_t server,
    hub_http_transport_t transport
);
void *hub_http_server_context(hub_http_transport_t transport);
bool hub_http_request_is_secure(httpd_req_t *req);
size_t hub_http_max_uri_handlers(void);

// Local touchscreen reads share the HTTP/HTTPS snapshot serialization lock.
bool hub_http_local_lock(void);
void hub_http_local_unlock(void);
