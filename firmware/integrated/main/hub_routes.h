#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t hub_routes_send_json(httpd_req_t *req, const char *json);
esp_err_t hub_routes_send_unavailable(httpd_req_t *req, const char *feature);
esp_err_t hub_routes_send_unauthorized(httpd_req_t *req);
esp_err_t hub_routes_send_forbidden(httpd_req_t *req, const char *message);
esp_err_t hub_routes_send_bad_request(httpd_req_t *req, const char *message);
esp_err_t hub_routes_send_upgrade_required(httpd_req_t *req);
esp_err_t hub_routes_send_internal_error(httpd_req_t *req, const char *message);
esp_err_t hub_routes_send_service_unavailable(httpd_req_t *req, const char *message);
