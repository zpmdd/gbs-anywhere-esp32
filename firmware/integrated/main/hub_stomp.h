#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    HUB_STOMP_UNKNOWN = 0,
    HUB_STOMP_HEARTBEAT,
    HUB_STOMP_CONNECTED,
    HUB_STOMP_MESSAGE,
    HUB_STOMP_RECEIPT,
    HUB_STOMP_ERROR,
} hub_stomp_type_t;

typedef struct {
    hub_stomp_type_t type;
    const char *body;
    size_t body_len;
    char destination[128];
    char receipt_id[64];
    char message_id[64];
} hub_stomp_frame_t;

bool hub_stomp_parse(char *data, size_t data_len, hub_stomp_frame_t *out);
size_t hub_stomp_write_connect(char *destination, size_t capacity, const char *access_token);
size_t hub_stomp_write_subscribe(
    char *destination,
    size_t capacity,
    const char *subscription_id,
    const char *topic
);
