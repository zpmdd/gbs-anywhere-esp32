#pragma once

#include <stdbool.h>

typedef enum {
    HUB_STATE_SLOW_READ_HIT,
    HUB_STATE_SLOW_READ_REFRESHING,
    HUB_STATE_SLOW_READ_UNAVAILABLE,
} hub_state_slow_read_state_t;

typedef struct {
    const char *body;
    hub_state_slow_read_state_t state;
    bool stale;
} hub_state_slow_read_result_t;

void hub_state_record_command(
    const char *command,
    const char *transport,
    bool ok,
    int duration_ms,
    const char *message
);
const char *hub_state_machine_state_json(void);
const char *hub_state_capabilities_json(void);
const char *hub_state_transports_json(void);
const char *hub_state_recent_commands_json(void);
hub_state_slow_read_result_t hub_state_statistics_read(int days);
hub_state_slow_read_result_t hub_state_schedule_read(void);
hub_state_slow_read_result_t hub_state_firmware_read(void);
const char *hub_state_slow_cloud_reads_json(void);
const char *hub_state_start_slow_cloud_read_json(const char *kind_name, int days);
bool hub_state_process_pending_slow_cloud_read(void);
const char *hub_state_live_state_json(void);
const char *hub_state_ble_state_json(void);
const char *hub_state_ble_diagnostics_json(void);
const char *hub_state_cloud_diagnostics_json(void);
const char *hub_state_cloud_machines_json(void);

void hub_state_invalidate_schedule(void);
const char *hub_state_machine_binding_json(void);
