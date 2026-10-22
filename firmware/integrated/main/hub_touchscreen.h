#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hub_command_coordinator.h"

bool hub_touchscreen_parse_command(const char *path, const char *body, const char *serial,
                                  hub_command_request_t *command);
// In-process dispatch only; never opens a socket or reads another Hub's identity.
int hub_touchscreen_exchange(const char *path, const char *body, const char *serial,
                            char *reply, size_t size, int64_t deadline_ms, bool *stale);
