#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HUB_WS_REASSEMBLY_SLOT_COUNT 2U

typedef enum {
    HUB_WS_REASSEMBLY_MORE = 0,
    HUB_WS_REASSEMBLY_COMPLETE,
    HUB_WS_REASSEMBLY_UNEXPECTED_CONTINUATION,
    HUB_WS_REASSEMBLY_UNEXPECTED_TEXT,
    HUB_WS_REASSEMBLY_OVERSIZED,
    HUB_WS_REASSEMBLY_INVALID,
} hub_ws_reassembly_result_t;

typedef struct {
    unsigned active_slot;
    size_t accumulated_bytes;
    size_t frame_base_offset;
    bool fragmented_message_active;
    bool current_frame_active;
    uint8_t current_frame_opcode;
} hub_ws_reassembly_t;

void hub_ws_reassembly_reset(hub_ws_reassembly_t *state);

/*
 * Reassembles esp_websocket_client DATA callbacks into one text message.
 * Each slot must have capacity + 1 bytes so the completed message can be
 * NUL-terminated without reducing the accepted payload limit.
 */
hub_ws_reassembly_result_t hub_ws_reassembly_feed(
    hub_ws_reassembly_t *state,
    char *slots[HUB_WS_REASSEMBLY_SLOT_COUNT],
    size_t capacity,
    uint8_t opcode,
    bool fin,
    size_t payload_len,
    size_t payload_offset,
    const char *data,
    size_t data_len,
    char **completed_data,
    size_t *completed_len
);
