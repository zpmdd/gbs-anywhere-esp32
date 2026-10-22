#include "hub_event_log.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    bool used;
    unsigned long sequence;
    long long uptime_ms;
    char source[24];
    char event[40];
    bool ok;
    char message[96];
} hub_event_log_entry_t;

#define HUB_EVENT_LOG_SOURCE_MAX_JSON_BYTES 23
#define HUB_EVENT_LOG_EVENT_MAX_JSON_BYTES 39
#define HUB_EVENT_LOG_MESSAGE_MAX_JSON_BYTES 95
#define HUB_EVENT_LOG_SEQUENCE_MAX_JSON_BYTES 10
#define HUB_EVENT_LOG_UPTIME_MAX_JSON_BYTES 19
#define HUB_EVENT_LOG_ENTRY_OVERHEAD_BYTES 91
#define HUB_EVENT_LOG_TOP_LEVEL_OVERHEAD_BYTES 32
#define HUB_EVENT_LOG_JSON_BUFFER_SIZE \
    (HUB_EVENT_LOG_TOP_LEVEL_OVERHEAD_BYTES \
        + (HUB_EVENT_LOG_CAPACITY \
            * (HUB_EVENT_LOG_ENTRY_OVERHEAD_BYTES \
                + HUB_EVENT_LOG_SOURCE_MAX_JSON_BYTES \
                + HUB_EVENT_LOG_EVENT_MAX_JSON_BYTES \
                + HUB_EVENT_LOG_MESSAGE_MAX_JSON_BYTES \
                + HUB_EVENT_LOG_SEQUENCE_MAX_JSON_BYTES \
                + HUB_EVENT_LOG_UPTIME_MAX_JSON_BYTES)))

_Static_assert(HUB_EVENT_LOG_JSON_BUFFER_SIZE >= 13000, "setup event log JSON buffer too small");

static EXT_RAM_BSS_ATTR hub_event_log_entry_t s_entries[HUB_EVENT_LOG_CAPACITY];
static size_t s_next_index;
static size_t s_entry_count;
static unsigned long s_next_sequence = 1;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool contains_sensitive_word(const char *value)
{
    if (value == NULL) {
        return false;
    }

    char normalized[160] = {0};
    size_t write = 0;
    for (const char *read = value; *read != '\0' && write + 1 < sizeof(normalized); ++read) {
        unsigned char c = (unsigned char)*read;
        if (isalnum(c)) {
            normalized[write++] = (char)tolower(c);
        }
    }
    normalized[write] = '\0';

    const char *words[] = {
        "password",
        "hubtoken",
        "localhubtoken",
        "cloudcredential",
        "credential",
        "installationkey",
        "refreshtoken",
        "bleauthtoken",
        "secret",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        if (strstr(normalized, words[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static void copy_redacted(char *dest, size_t dest_len, const char *source)
{
    if (dest == NULL || dest_len == 0) {
        return;
    }

    const char *value = source == NULL ? "" : source;
    if (contains_sensitive_word(value)) {
        value = "[redacted]";
    }

    size_t write = 0;
    for (const char *read = value; *read != '\0' && write + 1 < dest_len; ++read) {
        char c = *read;
        if (c == '"' || c == '\\') {
            continue;
        }
        if ((unsigned char)c < 0x20) {
            c = ' ';
        }
        dest[write++] = c;
    }
    dest[write] = '\0';
}

static void append_json(char *body, size_t body_len, size_t *offset, const char *format, ...)
{
    if (body == NULL || body_len == 0 || offset == NULL || *offset >= body_len) {
        return;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(body + *offset, body_len - *offset, format, args);
    va_end(args);

    if (written < 0) {
        body[body_len - 1] = '\0';
        *offset = body_len - 1;
        return;
    }
    if ((size_t)written >= body_len - *offset) {
        *offset = body_len - 1;
        body[*offset] = '\0';
        return;
    }
    *offset += (size_t)written;
}

void hub_event_log_record(const char *source, const char *event, bool ok, const char *message)
{
    hub_event_log_entry_t entry = {0};
    entry.used = true;
    entry.uptime_ms = esp_timer_get_time() / 1000;
    copy_redacted(entry.source, sizeof(entry.source), source);
    copy_redacted(entry.event, sizeof(entry.event), event);
    entry.ok = ok;
    copy_redacted(entry.message, sizeof(entry.message), message);

    portENTER_CRITICAL(&s_lock);
    entry.sequence = s_next_sequence++;
    s_entries[s_next_index] = entry;
    s_next_index = (s_next_index + 1) % HUB_EVENT_LOG_CAPACITY;
    if (s_entry_count < HUB_EVENT_LOG_CAPACITY) {
        s_entry_count++;
    }
    portEXIT_CRITICAL(&s_lock);
}

const char *hub_event_log_json(void)
{
    static EXT_RAM_BSS_ATTR char body[HUB_EVENT_LOG_JSON_BUFFER_SIZE];
    hub_event_log_entry_t snapshot[HUB_EVENT_LOG_CAPACITY] = {0};
    size_t count = 0;
    size_t start = 0;

    portENTER_CRITICAL(&s_lock);
    count = s_entry_count;
    start = (s_next_index + HUB_EVENT_LOG_CAPACITY - count) % HUB_EVENT_LOG_CAPACITY;
    for (size_t i = 0; i < count; ++i) {
        snapshot[i] = s_entries[(start + i) % HUB_EVENT_LOG_CAPACITY];
    }
    portEXIT_CRITICAL(&s_lock);

    size_t offset = 0;
    append_json(body, sizeof(body), &offset, "{\"events\":[");
    for (size_t i = 0; i < count && offset < sizeof(body); ++i) {
        hub_event_log_entry_t *entry = &snapshot[i];
        if (!entry->used) {
            continue;
        }
        append_json(
            body,
            sizeof(body),
            &offset,
            "%s{\"sequence\":%lu,\"uptimeMs\":%lld,\"source\":\"%s\",\"event\":\"%s\",\"ok\":%s,\"message\":\"%s\"}",
            i == 0 ? "" : ",",
            entry->sequence,
            entry->uptime_ms,
            entry->source,
            entry->event,
            entry->ok ? "true" : "false",
            entry->message
        );
    }
    append_json(body, sizeof(body), &offset, "],\"capacity\":%d}", HUB_EVENT_LOG_CAPACITY);
    return body;
}
