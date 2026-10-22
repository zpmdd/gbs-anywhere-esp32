#include "hub_routes.h"

#include <stdio.h>
#include <string.h>

#define HUB_ROUTES_LARGE_JSON_BYTES 1024
#define HUB_ROUTES_JSON_CHUNK_BYTES 1024

esp_err_t hub_routes_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    size_t length = json != NULL ? strlen(json) : 0;
    if (length <= HUB_ROUTES_LARGE_JSON_BYTES) {
        return httpd_resp_send(req, json, (ssize_t)length);
    }

    /*
     * Cached statistics are larger than the default 5760-byte lwIP send
     * window. A single keep-alive response can therefore monopolize the HTTP
     * server when iOS has reused the socket heavily. Stream large JSON in
     * bounded chunks and retire that connection after the response; the next
     * on-demand statistics read gets a clean local TCP session, while small
     * state/control responses keep their low-latency keep-alive path.
     */
    httpd_resp_set_hdr(req, "Connection", "close");
    for (size_t offset = 0; offset < length; offset += HUB_ROUTES_JSON_CHUNK_BYTES) {
        size_t remaining = length - offset;
        size_t chunk_bytes = remaining < HUB_ROUTES_JSON_CHUNK_BYTES
            ? remaining
            : HUB_ROUTES_JSON_CHUNK_BYTES;
        esp_err_t err = httpd_resp_send_chunk(req, json + offset, (ssize_t)chunk_bytes);
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t send_error(httpd_req_t *req, const char *message)
{
    char body[160] = {0};
    snprintf(
        body,
        sizeof(body),
        "{\"ok\":false,\"error\":\"%s\"}",
        message != NULL ? message : "request failed"
    );
    return hub_routes_send_json(req, body);
}

esp_err_t hub_routes_send_unavailable(httpd_req_t *req, const char *feature)
{
    char body[256] = {0};
    snprintf(
        body,
        sizeof(body),
        "{\"ok\":false,\"error\":\"%s is unavailable in ESP32-S3 phase 1.\"}",
        feature
    );
    httpd_resp_set_status(req, "503 Service Unavailable");
    return hub_routes_send_json(req, body);
}

esp_err_t hub_routes_send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    return hub_routes_send_json(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
}

esp_err_t hub_routes_send_forbidden(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "403 Forbidden");
    return send_error(req, message);
}

esp_err_t hub_routes_send_bad_request(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "400 Bad Request");
    return send_error(req, message);
}

esp_err_t hub_routes_send_upgrade_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "426 Upgrade Required");
    return hub_routes_send_json(
        req,
        "{\"ok\":false,\"errorCode\":\"httpsRequired\","
        "\"message\":\"Use the pinned v2 HTTPS endpoint.\"}"
    );
}

esp_err_t hub_routes_send_internal_error(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "500 Internal Server Error");
    return send_error(req, message);
}

esp_err_t hub_routes_send_service_unavailable(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return send_error(req, message);
}
