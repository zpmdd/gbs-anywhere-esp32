#include "hub_ws_reassembly.h"

#include <string.h>

void hub_ws_reassembly_reset(hub_ws_reassembly_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

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
)
{
    if (completed_data != NULL) {
        *completed_data = NULL;
    }
    if (completed_len != NULL) {
        *completed_len = 0;
    }
    if (state == NULL || slots == NULL || capacity == 0
        || completed_data == NULL || completed_len == NULL
        || (opcode != 0x00 && opcode != 0x01)
        || payload_offset > payload_len
        || data_len > payload_len - payload_offset
        || (data == NULL && data_len > 0)) {
        return HUB_WS_REASSEMBLY_INVALID;
    }

    bool starts_frame = payload_offset == 0;
    if (starts_frame) {
        if (state->current_frame_active) {
            return HUB_WS_REASSEMBLY_INVALID;
        }
        if (opcode == 0x01) {
            if (state->fragmented_message_active) {
                return HUB_WS_REASSEMBLY_UNEXPECTED_TEXT;
            }
            state->active_slot = (state->active_slot + 1U) % HUB_WS_REASSEMBLY_SLOT_COUNT;
            state->accumulated_bytes = 0;
            state->frame_base_offset = 0;
            if (slots[state->active_slot] == NULL) {
                return HUB_WS_REASSEMBLY_INVALID;
            }
            memset(slots[state->active_slot], 0, capacity + 1);
        } else {
            if (!state->fragmented_message_active) {
                return HUB_WS_REASSEMBLY_UNEXPECTED_CONTINUATION;
            }
            state->frame_base_offset = state->accumulated_bytes;
        }
        state->current_frame_active = true;
        state->current_frame_opcode = opcode;
    } else if (!state->current_frame_active || state->current_frame_opcode != opcode) {
        return opcode == 0x00
            ? HUB_WS_REASSEMBLY_UNEXPECTED_CONTINUATION
            : HUB_WS_REASSEMBLY_INVALID;
    }

    if (state->frame_base_offset > capacity
        || payload_len > capacity - state->frame_base_offset) {
        return HUB_WS_REASSEMBLY_OVERSIZED;
    }
    size_t offset = state->frame_base_offset + payload_offset;
    if (slots[state->active_slot] == NULL
        || offset > capacity
        || data_len > capacity - offset) {
        return HUB_WS_REASSEMBLY_INVALID;
    }
    if (data_len > 0) {
        memcpy(slots[state->active_slot] + offset, data, data_len);
    }
    if (offset + data_len > state->accumulated_bytes) {
        state->accumulated_bytes = offset + data_len;
    }

    if (payload_offset + data_len < payload_len) {
        return HUB_WS_REASSEMBLY_MORE;
    }
    state->current_frame_active = false;
    state->current_frame_opcode = 0;
    if (!fin) {
        state->fragmented_message_active = true;
        return HUB_WS_REASSEMBLY_MORE;
    }

    state->fragmented_message_active = false;
    state->frame_base_offset = 0;
    slots[state->active_slot][state->accumulated_bytes] = '\0';
    *completed_data = slots[state->active_slot];
    *completed_len = state->accumulated_bytes;
    return HUB_WS_REASSEMBLY_COMPLETE;
}
