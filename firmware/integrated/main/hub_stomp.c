#include "hub_stomp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool header_value_safe(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (*cursor == '\r' || *cursor == '\n' || *cursor < 0x20 || *cursor == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool copy_span(char *destination, size_t capacity, const char *start, size_t length)
{
    if (destination == NULL || capacity == 0 || start == NULL || length >= capacity) {
        return false;
    }
    memcpy(destination, start, length);
    destination[length] = '\0';
    return true;
}

static size_t line_length_without_cr(const char *line, size_t length)
{
    return length > 0 && line[length - 1] == '\r' ? length - 1 : length;
}

static bool next_line(
    char *data,
    size_t frame_len,
    size_t *offset,
    char **line,
    size_t *line_len
)
{
    if (data == NULL || offset == NULL || line == NULL || line_len == NULL || *offset > frame_len) {
        return false;
    }
    size_t start = *offset;
    size_t cursor = start;
    while (cursor < frame_len && data[cursor] != '\n') {
        cursor++;
    }
    *line = data + start;
    *line_len = line_length_without_cr(data + start, cursor - start);
    *offset = cursor < frame_len ? cursor + 1 : cursor;
    return true;
}

static hub_stomp_type_t command_type(const char *command, size_t length)
{
    if (length == 9 && memcmp(command, "CONNECTED", 9) == 0) {
        return HUB_STOMP_CONNECTED;
    }
    if (length == 7 && memcmp(command, "MESSAGE", 7) == 0) {
        return HUB_STOMP_MESSAGE;
    }
    if (length == 7 && memcmp(command, "RECEIPT", 7) == 0) {
        return HUB_STOMP_RECEIPT;
    }
    if (length == 5 && memcmp(command, "ERROR", 5) == 0) {
        return HUB_STOMP_ERROR;
    }
    return HUB_STOMP_UNKNOWN;
}

static bool parse_size(const char *value, size_t length, size_t *out)
{
    if (value == NULL || length == 0 || out == NULL || length >= 24) {
        return false;
    }
    char buffer[24] = {0};
    memcpy(buffer, value, length);
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(buffer, &end, 10);
    if (errno != 0 || end == buffer || *end != '\0') {
        return false;
    }
    *out = (size_t)parsed;
    return true;
}

bool hub_stomp_parse(char *data, size_t data_len, hub_stomp_frame_t *out)
{
    if (data == NULL || data_len == 0 || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    size_t frame_len = 0;
    while (frame_len < data_len && data[frame_len] != '\0') {
        frame_len++;
    }
    bool heartbeat = true;
    for (size_t index = 0; index < frame_len; index++) {
        if (data[index] != '\r' && data[index] != '\n') {
            heartbeat = false;
            break;
        }
    }
    if (heartbeat) {
        out->type = HUB_STOMP_HEARTBEAT;
        return true;
    }

    size_t offset = 0;
    char *line = NULL;
    size_t line_len = 0;
    if (!next_line(data, frame_len, &offset, &line, &line_len) || line_len == 0) {
        return false;
    }
    out->type = command_type(line, line_len);
    if (out->type == HUB_STOMP_UNKNOWN) {
        return false;
    }

    bool content_length_set = false;
    size_t content_length = 0;
    bool header_terminator = false;
    while (offset <= frame_len) {
        if (!next_line(data, frame_len, &offset, &line, &line_len)) {
            return false;
        }
        if (line_len == 0) {
            header_terminator = true;
            break;
        }
        const char *separator = memchr(line, ':', line_len);
        if (separator == NULL || separator == line) {
            return false;
        }
        size_t name_len = (size_t)(separator - line);
        const char *value = separator + 1;
        size_t value_len = line_len - name_len - 1;
        if (name_len == 11 && memcmp(line, "destination", 11) == 0) {
            if (!copy_span(out->destination, sizeof(out->destination), value, value_len)) {
                return false;
            }
        } else if (name_len == 10 && memcmp(line, "receipt-id", 10) == 0) {
            if (!copy_span(out->receipt_id, sizeof(out->receipt_id), value, value_len)) {
                return false;
            }
        } else if (name_len == 10 && memcmp(line, "message-id", 10) == 0) {
            if (!copy_span(out->message_id, sizeof(out->message_id), value, value_len)) {
                return false;
            }
        } else if (name_len == 14 && memcmp(line, "content-length", 14) == 0) {
            if (!parse_size(value, value_len, &content_length)) {
                return false;
            }
            content_length_set = true;
        }
    }
    if (!header_terminator || offset > frame_len) {
        return false;
    }

    size_t available = frame_len - offset;
    if (content_length_set && available < content_length) {
        return false;
    }
    out->body = data + offset;
    out->body_len = content_length_set ? content_length : available;
    return true;
}

static size_t finish_frame(char *destination, size_t capacity, int written)
{
    if (destination == NULL || capacity == 0 || written < 0 || (size_t)written + 1 > capacity) {
        return 0;
    }
    destination[written] = '\0';
    return (size_t)written + 1;
}

size_t hub_stomp_write_connect(char *destination, size_t capacity, const char *access_token)
{
    if (!header_value_safe(access_token)) {
        return 0;
    }
    int written = snprintf(
        destination,
        capacity,
        "CONNECT\n"
        "host:lion.lamarzocco.io\n"
        "accept-version:1.2,1.1,1.0\n"
        "heart-beat:0,0\n"
        "Authorization:Bearer %s\n\n",
        access_token
    );
    return finish_frame(destination, capacity, written);
}

size_t hub_stomp_write_subscribe(
    char *destination,
    size_t capacity,
    const char *subscription_id,
    const char *topic
)
{
    if (!header_value_safe(subscription_id) || !header_value_safe(topic)) {
        return 0;
    }
    int written = snprintf(
        destination,
        capacity,
        "SUBSCRIBE\n"
        "destination:%s\n"
        "ack:auto\n"
        "id:%s\n"
        "content-length:0\n\n",
        topic,
        subscription_id
    );
    return finish_frame(destination, capacity, written);
}
