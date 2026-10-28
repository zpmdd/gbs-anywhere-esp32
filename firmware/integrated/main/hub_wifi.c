#include "hub_wifi.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "hub_config.h"
#include "hub_event_log.h"
#include "hub_https.h"
#include "hub_identity.h"
#include "hub_version.h"
#include "mdns.h"

static const char *TAG = "hub_wifi";
static const char *SETUP_AP_PREFIX = "LamaBuddy-Hub-";
static char s_ap_ssid[33] = "LamaBuddy-Hub";
static char s_ip_address[16] = "0.0.0.0";
static const char *s_mode = "not-started";
static bool s_legacy_mdns_service_added = false;
static bool s_v2_mdns_service_added = false;
static atomic_bool s_station_has_ip = ATOMIC_VAR_INIT(false);

static esp_err_t publish_mdns_service(void)
{
    mdns_txt_item_t service_txt[] = {
        {"model", LAMABUDDY_DEVICE_MODEL},
        {"version", LAMABUDDY_FIRMWARE_VERSION},
        {"host", LAMABUDDY_MDNS_HOSTNAME ".local"},
    };

    ESP_RETURN_ON_ERROR(mdns_hostname_set(LAMABUDDY_MDNS_HOSTNAME), TAG, "mDNS hostname failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(LAMABUDDY_MDNS_INSTANCE), TAG, "mDNS instance failed");

    if (!s_legacy_mdns_service_added) {
        ESP_RETURN_ON_ERROR(
            mdns_service_add(
                LAMABUDDY_MDNS_INSTANCE,
                LAMABUDDY_LEGACY_MDNS_SERVICE_TYPE,
                "_tcp",
                LAMABUDDY_HTTP_PORT,
                service_txt,
                sizeof(service_txt) / sizeof(service_txt[0])
            ),
            TAG,
            "mDNS HTTP service failed"
        );
        s_legacy_mdns_service_added = true;
    }

    if (hub_identity_ready() && hub_https_ready() && !s_v2_mdns_service_added) {
        mdns_txt_item_t v2_service_txt[] = {
            {"apiVersion", "2"},
            {"hid", hub_identity_hid_summary()},
            {"mode", "legacy"},
            {"host", LAMABUDDY_MDNS_HOSTNAME ".local"},
        };
        ESP_RETURN_ON_ERROR(
            mdns_service_add(
                LAMABUDDY_MDNS_INSTANCE " v2",
                LAMABUDDY_V2_MDNS_SERVICE_TYPE,
                "_tcp",
                LAMABUDDY_HTTPS_PORT,
                v2_service_txt,
                sizeof(v2_service_txt) / sizeof(v2_service_txt[0])
            ),
            TAG,
            "mDNS HTTPS v2 service failed"
        );
        s_v2_mdns_service_added = true;
    }
    return ESP_OK;
}

esp_err_t hub_wifi_refresh_mdns_services(void)
{
    return publish_mdns_service();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Station started; connecting");
        hub_event_log_record("wifi", "stationStart", true, "station mode started");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&s_station_has_ip, false);
        s_ip_address[0] = '\0';
        ESP_LOGW(TAG, "Station disconnected; reconnecting");
        hub_event_log_record("wifi", "stationDisconnected", false, "station disconnected");
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR, IP2STR(&event->ip_info.ip));
        atomic_store(&s_station_has_ip, true);
        ESP_LOGI(TAG, "Station got IPv4 %s", s_ip_address);
        hub_event_log_record("wifi", "gotIp", true, hub_wifi_ip_address());
        if (publish_mdns_service() == ESP_OK) {
            ESP_LOGI(TAG, "mDNS hostname active: %s.local", LAMABUDDY_MDNS_HOSTNAME);
        }
    }
}

static esp_err_t ensure_network_stack(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

static void build_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(
        s_ap_ssid,
        sizeof(s_ap_ssid),
        "%s%02X%02X",
        SETUP_AP_PREFIX,
        mac[4],
        mac[5]
    );
}

static esp_err_t start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return publish_mdns_service();
}

static esp_err_t start_softap(void)
{
    atomic_store(&s_station_has_ip, false);
    esp_netif_create_default_wifi_ap();
    build_ap_ssid();

    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_mode = "softap";
    ESP_LOGI(TAG, "SoftAP setup mode started as %s", s_ap_ssid);
    hub_event_log_record("wifi", "softApStart", true, hub_wifi_ap_ssid());
    return ESP_OK;
}

static esp_err_t start_station(void)
{
    char ssid[HUB_CONFIG_MAX_SSID_LEN + 1] = {0};
    char password[HUB_CONFIG_MAX_PASSWORD_LEN + 1] = {0};
    ESP_RETURN_ON_ERROR(
        hub_config_get_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password)),
        TAG,
        "Wi-Fi credentials unavailable"
    );

    atomic_store(&s_station_has_ip, false);
    esp_netif_create_default_wifi_sta();
    wifi_config_t station_config = {0};
    strlcpy((char *)station_config.sta.ssid, ssid, sizeof(station_config.sta.ssid));
    strlcpy((char *)station_config.sta.password, password, sizeof(station_config.sta.password));
    station_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    s_mode = "station";
    ESP_LOGI(TAG, "Station mode started for configured Wi-Fi");
    return ESP_OK;
}

esp_err_t hub_wifi_init(void)
{
    ESP_RETURN_ON_ERROR(ensure_network_stack(), TAG, "network stack init failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Wi-Fi init failed");
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler, NULL, NULL),
        TAG,
        "Wi-Fi station start handler failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL, NULL),
        TAG,
        "Wi-Fi station disconnect handler failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL),
        TAG,
        "IP event handler failed"
    );
    build_ap_ssid();
    ESP_RETURN_ON_ERROR(start_mdns(), TAG, "mDNS init failed");

    if (hub_config_has_wifi_credentials()) {
        return start_station();
    }
    return start_softap();
}

const char *hub_wifi_mode(void)
{
    return s_mode;
}

const char *hub_wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *hub_wifi_ip_address(void)
{
    return s_ip_address;
}

bool hub_wifi_station_has_ip(void)
{
    return atomic_load(&s_station_has_ip);
}
