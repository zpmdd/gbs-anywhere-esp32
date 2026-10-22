// Grinder protocol from the pinned companion; cloud sessions belong to the Hub.
#include "network.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_http_client.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "hub_command_coordinator.h"
#include "esp_log.h"
#include "esp_random.h"
#include "hub_cloud.h"
#include "hub_config.h"
#include "lwip/inet.h"
#include "nvs.h"
#define MAX_RESPONSE (128 * 1024)
static const char *TAG = "gbs_net";
EXT_RAM_BSS_ATTR gbs_config_t gbs_config;
gbs_request_info_t gbs_grinder_request, gbs_cloud_request;

static bool string_field(const cJSON *j, const char *key, char *out, size_t capacity, bool empty)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsString(v)) return false;
    size_t len = strlen(v->valuestring);
    if ((!empty && !len) || len >= capacity) return false;
    memcpy(out, v->valuestring, len + 1);
    return true;
}


esp_err_t gbs_load_config(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("gbs_cfg", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    size_t length = 0;
    err = nvs_get_str(nvs, "config", NULL, &length);
    if (err != ESP_OK || length < 2 || length > 8192) { nvs_close(nvs); return ESP_ERR_INVALID_SIZE; }
    char *text = calloc(1, length);
    if (!text) { nvs_close(nvs); return ESP_ERR_NO_MEM; }
    err = nvs_get_str(nvs, "config", text, &length);
    nvs_close(nvs);
    cJSON *j = err == ESP_OK ? cJSON_Parse(text) : NULL;
    memset(text, 0, length); free(text);
    if (!j) return ESP_ERR_INVALID_RESPONSE;
    bool ok = string_field(j, "serial", gbs_config.serial, sizeof(gbs_config.serial), false) &&
        string_field(j, "grinder_host", gbs_config.grinder_host, sizeof(gbs_config.grinder_host), false) &&
        string_field(j, "grinder_cert", gbs_config.grinder_cert, sizeof(gbs_config.grinder_cert), false) &&
        string_field(j, "brew_device_id", gbs_config.device_id, sizeof(gbs_config.device_id), false);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "pairing_id");
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(j, "schema");
    ok = ok && cJSON_IsNumber(schema) && schema->valuedouble == 2 &&
        cJSON_IsNumber(id) && id->valuedouble > 0 && id->valuedouble <= 65535 && floor(id->valuedouble) == id->valuedouble;
    if (ok) gbs_config.pairing_id = (uint32_t)id->valuedouble;
    cJSON_Delete(j);
    struct in_addr address;
    ok = ok && inet_pton(AF_INET, gbs_config.grinder_host, &address) == 1 &&
        gbs_uuid_valid(gbs_config.device_id) &&
        strstr(gbs_config.grinder_cert, "-----BEGIN CERTIFICATE-----") == gbs_config.grinder_cert &&
        strstr(gbs_config.grinder_cert, "-----END CERTIFICATE-----");
    if (!ok) memset(&gbs_config, 0, sizeof(gbs_config));
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

int64_t gbs_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void gbs_uuid(char out[37])
{
    unsigned char b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 15) | 64;
    b[8] = (b[8] & 63) | 128;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

typedef struct { char *data; size_t used, capacity; bool failed; } response_t;

static esp_err_t receive(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    response_t *r = event->user_data;
    size_t next = r->used + (size_t)event->data_len + 1;
    if (next > MAX_RESPONSE || r->failed) { r->failed = true; return ESP_ERR_INVALID_SIZE; }
    if (next > r->capacity) {
        size_t capacity = r->capacity ? r->capacity * 2 : 4096;
        if (capacity < next) capacity = next;
        if (capacity > MAX_RESPONSE) capacity = MAX_RESPONSE;
        char *buffer = realloc(r->data, capacity);
        if (!buffer) { r->failed = true; return ESP_ERR_NO_MEM; }
        r->data = buffer; r->capacity = capacity;
    }
    memcpy(r->data + r->used, event->data, event->data_len);
    r->used += event->data_len;
    r->data[r->used] = 0;
    return ESP_OK;
}

static esp_err_t request(const char *url,
                         const cJSON *body, int *http_status, cJSON **json)
{
    *http_status = 0; *json = NULL;
    response_t response = {0};
    esp_http_client_config_t config = {
        .url = url, .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 5000, .buffer_size = 1024, .buffer_size_tx = 1024,
        .event_handler = receive, .user_data = &response, .disable_auto_redirect = true,
        .cert_pem = gbs_config.grinder_cert,
        // This grinder's pinned self-signed certificate has no DNS/IP name.
        .skip_cert_common_name_check = true,
    };
    char *encoded = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body && !encoded) return ESP_ERR_NO_MEM;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { cJSON_free(encoded); return ESP_ERR_NO_MEM; }
    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Connection", "close");
    if (err == ESP_OK && encoded) err = esp_http_client_set_post_field(client, encoded, strlen(encoded));
    if (err == ESP_OK) err = esp_http_client_perform(client);
    *http_status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && (response.failed || !esp_http_client_is_complete_data_received(client)))
        err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && response.used) {
        *json = cJSON_ParseWithLength(response.data, response.used);
        if (!*json) err = ESP_ERR_INVALID_RESPONSE;
    }
    // Never log cloud response bodies, which include credentials/tokens during sign-in.
    esp_http_client_cleanup(client);
    if (encoded) { memset(encoded, 0, strlen(encoded)); cJSON_free(encoded); }
    if (response.data) { memset(response.data, 0, response.used); free(response.data); }
    return err;
}


esp_err_t gbs_cloud_connect(void)
{
    hub_machine_selection_identity_t selected;
    esp_err_t err = hub_config_get_machine_selection_identity(&selected);
    if (err == ESP_OK && (!selected.serial_number[0] || strcmp(selected.serial_number, gbs_config.serial))) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && !hub_cloud_authenticated()) { hub_cloud_request_refresh(); err = ESP_ERR_INVALID_STATE; }
    strcpy(gbs_cloud_request.operation, "hubSession");
    gbs_cloud_request.transport = err;
    gbs_cloud_request.http_status = 0;
    return err;
}

esp_err_t gbs_cloud_coffee(gbs_coffee_t *coffee)
{
    char *text = NULL;
    esp_err_t err = hub_cloud_latest_coffee_json(gbs_config.serial, &text);
    cJSON *reply = err == ESP_OK ? cJSON_Parse(text) : NULL;
    if (err == ESP_OK && !gbs_latest_coffee(reply, coffee)) err = ESP_ERR_INVALID_RESPONSE;
    cJSON_Delete(reply); free(text);
    strcpy(gbs_cloud_request.operation, "coffee");
    gbs_cloud_request.transport = err;
    gbs_cloud_request.http_status = err == ESP_OK ? 200 : 0;
    return err;
}

esp_err_t gbs_grinder(const char *endpoint, const cJSON *body, cJSON **reply, char status[80])
{
    *reply = NULL;
    memset(&gbs_grinder_request, 0, sizeof(gbs_grinder_request));
    snprintf(gbs_grinder_request.operation, sizeof(gbs_grinder_request.operation), "%s", endpoint);
    gbs_grinder_request.transport = ESP_ERR_INVALID_STATE;
    strcpy(status, "MachineSelectionChanged");
    hub_machine_selection_identity_t selected;
    if (hub_config_get_machine_selection_identity(&selected) != ESP_OK ||
        strcmp(selected.serial_number, gbs_config.serial)) return ESP_ERR_INVALID_STATE;
    char url[160];
    snprintf(url, sizeof(url), "https://%s:30001/grinderApi/%s", gbs_config.grinder_host, endpoint);
    int http_status = 0;
    strcpy(status, "TransportError");
    esp_err_t err = hub_cloud_background_io_begin();
    if (err != ESP_OK) {
        strcpy(status, "LocalBusy");
        gbs_grinder_request.transport = err;
        return err;
    }
    if (!hub_config_machine_selection_matches(&selected)) {
        hub_cloud_background_io_end();
        strcpy(status, "MachineSelectionChanged");
        return ESP_ERR_INVALID_STATE;
    }
    err = request(url, body, &http_status, reply);
    hub_cloud_background_io_end();
    if (err == ESP_OK && !string_field(*reply, "status", status, 80, false)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && !gbs_reply_valid(http_status, status)) err = ESP_ERR_INVALID_RESPONSE;
    snprintf(gbs_grinder_request.operation, sizeof(gbs_grinder_request.operation), "%s", endpoint);
    gbs_grinder_request.http_status = http_status;
    gbs_grinder_request.transport = err;
    if (strcmp(endpoint, "brewJobs"))
        ESP_LOGI(TAG, "grinder %s HTTP=%d status=%s transport=%s", endpoint, http_status, status, esp_err_to_name(err));
    return err;
}

typedef struct {
    uint8_t *data;
    size_t size, capacity, limit;
    int64_t deadline_us;
    char cookie[96];
    esp_err_t error;
    bool close_connection;
} log_response_t;

static esp_err_t receive_log(esp_http_client_event_t *event)
{
    log_response_t *r = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "Connection"))
        r->close_connection = !strcasecmp(event->header_value, "close");
    if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "Set-Cookie") &&
        !strncmp(event->header_value, "PHPSESSID=", 10)) {
        size_t len = strcspn(event->header_value, ";\r\n");
        if (len <= 10 || len >= sizeof(r->cookie)) { r->error = ESP_ERR_INVALID_RESPONSE; return ESP_OK; }
        memcpy(r->cookie, event->header_value, len); r->cookie[len] = 0;
    }
    return ESP_OK;
}

static esp_err_t append_log(log_response_t *r, const void *data, size_t size)
{
    size_t next = r->size + size;
    if (next > r->limit) return ESP_ERR_INVALID_SIZE;
    if (next + 1 > r->capacity) {
        size_t capacity = r->capacity ? r->capacity * 2 : 4096;
        if (capacity < next + 1) capacity = next + 1;
        if (capacity > r->limit + 1) capacity = r->limit + 1;
        void *data = heap_caps_realloc(r->data, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!data) return ESP_ERR_NO_MEM;
        r->data = data; r->capacity = capacity;
    }
    memcpy(r->data + r->size, data, size); r->size = next; r->data[next] = 0;
    return ESP_OK;
}

static esp_err_t log_io_budget(esp_http_client_handle_t client, const log_response_t *r)
{
    int remaining = (int)((r->deadline_us - esp_timer_get_time()) / 1000);
    if (remaining <= 0 || hub_command_interactive_pending()) return ESP_ERR_TIMEOUT;
    return esp_http_client_set_timeout_ms(client, remaining);
}

static esp_err_t log_request(esp_http_client_handle_t *session, const char *path, const char *body, log_response_t *r)
{
    int remaining = (int)((r->deadline_us - esp_timer_get_time()) / 1000);
    if (remaining <= 0) return ESP_ERR_TIMEOUT;
    char url[128]; snprintf(url, sizeof(url), "https://%s/rest_api/%s", gbs_config.grinder_host, path);
    esp_http_client_config_t config = {
        .url = url, .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = remaining, .buffer_size = 1024, .buffer_size_tx = 512,
        .event_handler = receive_log, .user_data = r, .disable_auto_redirect = true,
        .cert_pem = gbs_config.grinder_cert, .skip_cert_common_name_check = true,
    };
    if (!*session) *session = esp_http_client_init(&config);
    esp_http_client_handle_t client = *session;
    if (!client) return ESP_ERR_NO_MEM;
    r->close_connection = false;
    esp_err_t err = esp_http_client_set_url(client, url);
    if (err == ESP_OK) err = esp_http_client_set_method(client, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (err == ESP_OK) err = esp_http_client_set_timeout_ms(client, remaining);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Connection", "keep-alive");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (err == ESP_OK && body) err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK && !body) err = esp_http_client_set_header(client, "Cookie", r->cookie);
    if (err == ESP_OK) err = log_io_budget(client, r);
    if (err == ESP_OK) err = esp_http_client_open(client, body ? (int)strlen(body) : 0);
    if (err == ESP_OK) err = log_io_budget(client, r);
    if (err == ESP_OK && body && esp_http_client_write(client, body, strlen(body)) != (int)strlen(body)) err = ESP_FAIL;
    if (err == ESP_OK) err = log_io_budget(client, r);
    if (err == ESP_OK) {
        int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0 || content_length > r->limit) err = ESP_ERR_INVALID_SIZE;
        else if (esp_http_client_get_status_code(client) != 200) err = ESP_ERR_INVALID_RESPONSE;
    }
    char chunk[1024];
    while (err == ESP_OK) {
        if (r->size && (int64_t)r->size == esp_http_client_get_content_length(client) &&
            esp_http_client_is_complete_data_received(client)) break;
        err = log_io_budget(client, r);
        if (err != ESP_OK) break;
        if (r->error != ESP_OK) { err = r->error; break; }
        int count = esp_http_client_read(client, chunk, sizeof(chunk));
        if (count < 0) { err = ESP_FAIL; break; }
        if (count > 0) err = append_log(r, chunk, (size_t)count);
        else {
            if (!esp_http_client_is_complete_data_received(client)) err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }
    if (err == ESP_OK && (esp_http_client_get_status_code(client) != 200 ||
        !esp_http_client_is_complete_data_received(client))) err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK || r->close_connection) esp_http_client_close(client);
    return err;
}

esp_err_t gbs_grinder_recipe(const char *uuid, gbs_recipe_match_t *match, unsigned *bytes, unsigned *elapsed_ms)
{
    *bytes = *elapsed_ms = 0;
    memset(match, 0, sizeof(*match));
    char password[65] = {0}; size_t length = sizeof(password);
    nvs_handle_t settings;
    esp_err_t err = nvs_open("gbs_cfg", NVS_READONLY, &settings);
    if (err == ESP_OK) { err = nvs_get_str(settings, "owner_pass", password, &length); nvs_close(settings); }
    if (err != ESP_OK || !password[0]) { memset(password, 0, sizeof(password)); return ESP_ERR_INVALID_STATE; }
    cJSON *login = cJSON_CreateObject();
    if (login) { cJSON_AddStringToObject(login, "userRole", "Owner"); cJSON_AddStringToObject(login, "password", password); }
    char *body = login ? cJSON_PrintUnformatted(login) : NULL;
    cJSON *secret = cJSON_GetObjectItemCaseSensitive(login, "password");
    if (cJSON_IsString(secret)) memset(secret->valuestring, 0, strlen(secret->valuestring));
    cJSON_Delete(login); memset(password, 0, sizeof(password));
    if (!body) return ESP_ERR_NO_MEM;
    err = hub_cloud_background_io_begin();
    if (err != ESP_OK) {
        memset(body, 0, strlen(body)); free(body);
        return ESP_ERR_NOT_FINISHED; // No HTTP request started; allow the worker to yield and try admission again.
    }
    int64_t start = esp_timer_get_time();
    log_response_t r = {.limit = 4096, .deadline_us = start + 6000000};
    esp_http_client_handle_t session = NULL;
    if (err == ESP_OK) {
        err = log_request(&session, "login", body, &r);
        cJSON *reply = err == ESP_OK ? cJSON_Parse((char *)r.data) : NULL;
        if (err == ESP_OK && (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "success")) || !r.cookie[0]))
            err = ESP_ERR_INVALID_RESPONSE;
        cJSON_Delete(reply);
        if (r.data) memset(r.data, 0, r.size);
        r.size = 0; r.limit = GBS_LOG_ZIP_LIMIT;
        if (err == ESP_OK) err = log_request(&session, "download_machine_logs", NULL, &r);
        if (session) { esp_http_client_close(session); esp_http_client_cleanup(session); }
        hub_cloud_background_io_end();
        *bytes = (unsigned)r.size;
        if (err == ESP_OK && !gbs_recipe_log_match(r.data, r.size, uuid, match)) err = ESP_ERR_INVALID_RESPONSE;
    }
    *elapsed_ms = (unsigned)((esp_timer_get_time() - start) / 1000);
    memset(body, 0, strlen(body)); free(body); free(r.data); memset(r.cookie, 0, sizeof(r.cookie));
    return err;
}
