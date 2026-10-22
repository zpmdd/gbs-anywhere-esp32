#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void hub_brew_timer_reset(void);
void hub_brew_timer_update_mode(const char *machine_mode, int64_t now_us);
void hub_brew_timer_record_cloud_seconds(double extraction_seconds, int64_t event_us);
void hub_brew_timer_update_source_stale(int64_t now_us);
bool hub_brew_timer_available(void);
bool hub_brew_timer_is_brewing(void);
bool hub_brew_timer_live_state_json(char *dest, size_t dest_len, int64_t now_us);
