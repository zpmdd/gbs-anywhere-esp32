#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct cJSON cJSON;

// Existing plans only: creation/deletion remain with the machine application.
typedef struct {
    char id[129];
    uint16_t on_minutes, off_minutes;
    uint8_t days;
    bool enabled, steam;
} hub_schedule_entry_t;

bool hub_schedule_parse(const cJSON *json, hub_schedule_entry_t *entry);
bool hub_schedule_valid(const hub_schedule_entry_t *entry);
static inline bool hub_schedule_equal(const hub_schedule_entry_t *a,const hub_schedule_entry_t *b)
{
    return a&&b&&memchr(a->id,0,sizeof(a->id))&&memchr(b->id,0,sizeof(b->id))&&!strcmp(a->id,b->id)&&
        a->on_minutes==b->on_minutes&&a->off_minutes==b->off_minutes&&a->days==b->days&&
        a->enabled==b->enabled&&a->steam==b->steam;
}
bool hub_schedule_find(const char *json, const char *serial, const char *id, hub_schedule_entry_t *entry);
cJSON *hub_schedule_json(const hub_schedule_entry_t *entry);
