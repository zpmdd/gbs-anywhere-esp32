#include "hub_https.h"

#include <stdatomic.h>

#include "esp_http_server.h"
#include "esp_https_server.h"
#include "hub_http.h"
#include "hub_identity.h"
#include "hub_security.h"
#include "hub_version.h"

static httpd_handle_t s_https_server;
static atomic_bool s_https_ready = ATOMIC_VAR_INIT(false);

esp_err_t hub_https_start(void)
{
    if (atomic_load(&s_https_ready)) {
        return ESP_OK;
    }
    if (s_https_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hub_security_release_security_ready() || !hub_identity_ready()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    hub_identity_tls_material_t material = {0};
    esp_err_t err = hub_identity_tls_material(&material);
    if (err != ESP_OK) {
        return err;
    }

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = LAMABUDDY_HTTPS_PORT;
    config.httpd.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    config.httpd.max_uri_handlers = (unsigned int)hub_http_max_uri_handlers();
    config.httpd.max_open_sockets = HUB_HTTP_HTTPS_MAX_OPEN_SOCKETS;
    config.httpd.max_resp_headers = 12;
    config.httpd.stack_size = 16384;
    config.httpd.lru_purge_enable = true;
    config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    config.httpd.global_user_ctx =
        hub_http_server_context(HUB_HTTP_TRANSPORT_HTTPS_V2);
    config.servercert = material.certificate_pem;
    config.servercert_len = material.certificate_pem_len;
    config.prvtkey_pem = material.private_key_pem;
    config.prvtkey_len = material.private_key_pem_len;
    config.session_tickets = false;
    config.tls_handshake_timeout_ms = 5000;

    err = httpd_ssl_start(&s_https_server, &config);
    /* esp_https_server copies the PEM buffers during start. */
    hub_identity_forget_private_key();
    if (err != ESP_OK) {
        atomic_store(&s_https_ready, false);
        s_https_server = NULL;
        return err;
    }

    err = hub_http_register_routes(
        s_https_server,
        HUB_HTTP_TRANSPORT_HTTPS_V2
    );
    if (err != ESP_OK) {
        (void)httpd_ssl_stop(s_https_server);
        atomic_store(&s_https_ready, false);
        s_https_server = NULL;
        return err;
    }
    atomic_store(&s_https_ready, true);
    return ESP_OK;
}

bool hub_https_ready(void)
{
    return atomic_load(&s_https_ready);
}
