#pragma once
#include <stddef.h>
typedef int nvs_handle_t;
int64_t esp_timer_get_time(void);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
void test_log(const char *tag, const char *format, ...);
#define ESP_LOGI test_log
#define ESP_LOGW test_log
#define ESP_LOGE test_log
