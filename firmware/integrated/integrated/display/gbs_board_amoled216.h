#pragma once
#include "gbs_device_policy.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

esp_err_t gbs_board_amoled216_init(size_t transfer_bytes,
    esp_lcd_panel_io_color_trans_done_cb_t done,
    esp_lcd_panel_handle_t *panel, esp_lcd_touch_handle_t *touch);
void gbs_board_amoled216_deinit(void);
void gbs_board_amoled216_start_services(void);
/* Called only by the display task; sensor and audio work run in separate tasks. */
bool gbs_board_amoled216_tick(int64_t now_ms, bool touching, bool auto_rotate,
    unsigned volume_percent, gbs_tone_t tone, gbs_device_status_t *status);
esp_err_t gbs_board_amoled216_brightness(unsigned percent);
gbs_orientation_t gbs_board_amoled216_orientation(void);
bool gbs_board_amoled216_touch_active(void);
/* Read-only diagnostics; acceleration uses the policy's board axes. */
bool gbs_board_amoled216_acceleration(float xyz[3]);
typedef struct {
    uint8_t raw00, raw01;
    gbs_device_status_t decoded;
} gbs_pmu_diagnostics_t;
/* Copy the last sensor poll under its mutex; never performs I2C reads.
 * Expired decoded status is invalid, while raw bytes remain diagnostic. */
bool gbs_board_amoled216_pmu_diagnostics(gbs_pmu_diagnostics_t *snapshot);
/* True while either key is raw-down or waiting for a debounced release. */
bool gbs_board_amoled216_buttons(int64_t now_ms, bool blank,
    gbs_button_event_t *user_key, gbs_button_event_t *boot_key);

typedef struct { unsigned success, failure, notice, errors, volume_percent; } gbs_audio_diagnostics_t;
gbs_audio_diagnostics_t gbs_board_amoled216_audio_diagnostics(void);
