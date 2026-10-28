#pragma once
#include "gbs_ui_model.h"
#include <stddef.h>

typedef struct {
    char url[160], token[129], hub_id[37], serial[65], certificate[4096];
} gbs_hub_config_t;

// Exchange returns an HTTP status, or -1 for an ambiguous transport failure.
// Implementations enforce TLS identity, response bounds and request timeouts;
// no request starts after the absolute deadline, which does not cancel execution.
typedef struct {
    int (*exchange)(void *context, const char *path, const char *body, char *reply,
                    size_t size, int64_t deadline_ms);
    int64_t (*now)(void *context);
    void (*wait)(void *context, unsigned milliseconds);
    void *context;
    char *reply;
    size_t reply_size;
    const bool *response_stale;
} gbs_hub_io_t;

bool gbs_hub_config_parse(const char *json, gbs_hub_config_t *out);
// Reads the existing machine and BLE diagnostic snapshots; never starts a probe.
bool gbs_hub_refresh(const gbs_hub_config_t *config, const gbs_hub_io_t *io,
                     int64_t deadline_ms, gbs_ui_machine_t *machine);
// A bounded read step. Only the first step requests Hub cache refresh.
void gbs_hub_fetch_data(const gbs_hub_config_t *config, const gbs_hub_io_t *io,
                       gbs_ui_data_kind_t kind, bool refresh, int64_t deadline_ms,
                       gbs_ui_data_t *data);
bool gbs_hub_prepare(gbs_ui_action_t action, double value, const gbs_ui_machine_t *machine,
                     const char *request_id, char *path, size_t path_size,
                     char *body, size_t body_size);
gbs_ui_control_t gbs_hub_result(const char *json, const char *request_id);
gbs_ui_control_t gbs_hub_execute(const gbs_hub_config_t *config, const gbs_hub_io_t *io,
                                gbs_ui_action_t action, double value, const char *request_id,
                                int64_t deadline_ms, gbs_ui_machine_t *machine);

gbs_ui_control_t gbs_hub_execute_schedule(const gbs_hub_config_t *config,const gbs_hub_io_t *io,
    const gbs_ui_schedule_t *expected,const gbs_ui_schedule_t *requested,const char *request_id,
    int64_t deadline_ms,gbs_ui_data_t *data);
