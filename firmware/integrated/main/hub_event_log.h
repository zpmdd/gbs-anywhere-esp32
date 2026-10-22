#pragma once

#include <stdbool.h>

#define HUB_EVENT_LOG_CAPACITY 48

void hub_event_log_record(const char *source, const char *event, bool ok, const char *message);
const char *hub_event_log_json(void);
