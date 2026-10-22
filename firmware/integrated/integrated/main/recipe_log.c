#include "recipe_log.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "miniz.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define allocate(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#include <zlib.h>
#define allocate(n) calloc(1, (n))
#endif

enum { UUID, DATE, WEIGHT, SUCCESS, ENABLED, RECIPE, FIELDS };
static const char *const names[FIELDS] = {
    "grind_uuid", "grind_date_time", "recipe_brew_weight", "grind_successfull",
    "recipe_gbs_enabled", "recipe_index"
};
typedef struct {
    const char *uuid;
    unsigned row, column, field_len, row_len, matches, columns;
    int indexes[FIELDS];
    bool quoted, closing, started, cr;
    char field[81], values[FIELDS][81];
    gbs_recipe_match_t match;
} csv_t;

static bool integer(const char *s, unsigned max, unsigned *out)
{
    if (!*s) return false;
    unsigned n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > max / 10) return false;
        n = n * 10 + (unsigned)(*s - '0');
        if (n > max) return false;
    }
    *out = n; return true;
}

static bool field_end(csv_t *c)
{
    c->field[c->field_len] = 0;
    for (unsigned i = 0; i < FIELDS; ++i) {
        if (!c->row && !strcmp(c->field, names[i])) {
            if (c->indexes[i] >= 0) return false;
            c->indexes[i] = (int)c->column;
        } else if (c->row && c->indexes[i] == (int)c->column) strcpy(c->values[i], c->field);
    }
    c->field_len = 0; c->started = c->closing = false;
    return ++c->column <= 48;
}

static bool row_end(csv_t *c)
{
    if (!field_end(c)) return false;
    if (!c->row) {
        for (unsigned i = 0; i < FIELDS; ++i) if (c->indexes[i] < 0) return false;
        c->columns = c->column;
    } else {
        if (c->column != c->columns) return false;
        if (!strcmp(c->uuid, c->values[UUID])) {
            unsigned mg, recipe;
            if (++c->matches != 1 || strcmp(c->values[SUCCESS], "1") || strcmp(c->values[ENABLED], "1") ||
                !integer(c->values[WEIGHT], 200000, &mg) || mg < 1000 || mg % 1000 ||
                !integer(c->values[RECIPE], 255, &recipe) || strlen(c->values[DATE]) != 20) return false;
            c->match.grams = mg / 1000; c->match.recipe_index = recipe;
            memcpy(c->match.date, c->values[DATE], 21);
        }
    }
    c->column = c->row_len = 0;
    memset(c->values, 0, sizeof(c->values));
    return ++c->row <= 5001;
}

static bool append(csv_t *c, char b)
{
    bool wanted = !c->row;
    for (unsigned i = 0; i < FIELDS; ++i) wanted |= c->indexes[i] == (int)c->column;
    if (wanted) {
        if (c->field_len >= sizeof(c->field) - 1) return false;
        c->field[c->field_len++] = b;
    }
    c->started = true; return true;
}

static bool feed(csv_t *c, const uint8_t *data, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        char b = (char)data[i];
        if (!b || ++c->row_len > 4096) return false;
        if (c->cr) { c->cr = false; if (b == '\n') { c->row_len = 0; continue; } }
        if (c->quoted) {
            if (b == '"') { c->quoted = false; c->closing = true; }
            else if (!append(c, b)) return false;
        } else if (b == '"') {
            if (c->closing) { c->quoted = true; c->closing = false; if (!append(c, b)) return false; }
            else if (!c->started) { c->quoted = c->started = true; }
            else return false;
        } else if (b == ',') { if (!field_end(c)) return false; }
        else if (b == '\n' || b == '\r') {
            if (!row_end(c)) return false;
            c->cr = b == '\r';
        } else if (c->closing || !append(c, b)) return false;
    }
    return true;
}

static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p + 2) << 16); }
static bool span(size_t at, size_t len, size_t end) { return at <= end && len <= end - at; }

static bool csv_member(const uint8_t *z, size_t size, size_t *offset, size_t *compressed,
                       size_t *raw, unsigned *method, uint32_t *crc)
{
    if (size < 22 || size > GBS_LOG_ZIP_LIMIT) return false;
    size_t end = size - 22, low = size > 65557 ? size - 65557 : 0;
    while (u32(z + end) != 0x06054b50 || end + 22u + u16(z + end + 20) != size) {
        if (end == low) return false;
        --end;
    }
    unsigned entries = u16(z + end + 10);
    size_t at = u32(z + end + 16), directory = at;
    if (u16(z + end + 4) || u16(z + end + 6) || u16(z + end + 8) != entries ||
        entries > 16 || !span(at, u32(z + end + 12), end) || at + u32(z + end + 12) != end) return false;
    bool found = false;
    for (unsigned i = 0; i < entries; ++i) {
        if (!span(at, 46, end) || u32(z + at) != 0x02014b50) return false;
        size_t name = u16(z + at + 28), extra = u16(z + at + 30), comment = u16(z + at + 32);
        if (!span(at + 46, name + extra + comment, end)) return false;
        char filename[160];
        if (name >= sizeof(filename)) return false;
        memcpy(filename, z + at + 46, name); filename[name] = 0;
        if (strstr(filename, "GrindLogs") && name >= 4 && !strcmp(filename + name - 4, ".csv")) {
            if (found || (u16(z + at + 8) & ~(8u | 2048u)) || u16(z + at + 34)) return false;
            found = true; *compressed = u32(z + at + 20); *raw = u32(z + at + 24);
            *method = u16(z + at + 10); *crc = u32(z + at + 16);
            size_t local = u32(z + at + 42);
            if (!*raw || *raw > GBS_LOG_CSV_LIMIT || (*method != 0 && *method != 8) ||
                !span(local, 30, directory) || u32(z + local) != 0x04034b50 ||
                u16(z + local + 6) != u16(z + at + 8) || u16(z + local + 8) != *method ||
                u16(z + local + 26) != name || !span(local + 30, name + u16(z + local + 28), directory) ||
                memcmp(z + local + 30, z + at + 46, name)) return false;
            *offset = local + 30 + name + u16(z + local + 28);
            if (!span(*offset, *compressed, directory)) return false;
        }
        at += 46 + name + extra + comment;
    }
    return found && at == end;
}

bool gbs_recipe_log_match(const uint8_t *zip, size_t size, const char *uuid, gbs_recipe_match_t *result)
{
    if (!zip || !uuid || strlen(uuid) != 36 || !result) return false;
    memset(result, 0, sizeof(*result));
    size_t offset = 0, compressed = 0, raw = 0; unsigned method = 0; uint32_t expected_crc = 0;
    if (!csv_member(zip, size, &offset, &compressed, &raw, &method, &expected_crc)) return false;
    csv_t *csv = allocate(sizeof(*csv)); uint8_t *window = allocate(32768);
#ifdef ESP_PLATFORM
    tinfl_decompressor *decoder = allocate(sizeof(*decoder));
    if (decoder) tinfl_init(decoder);
#else
    z_stream *decoder = allocate(sizeof(*decoder));
    bool initialized = decoder && inflateInit2(decoder, -15) == Z_OK;
#endif
    bool ok = csv && window && decoder;
#ifndef ESP_PLATFORM
    ok = ok && initialized;
#endif
    if (!ok) goto done;
    csv->uuid = uuid;
    for (unsigned i = 0; i < FIELDS; ++i) csv->indexes[i] = -1;
    size_t used = 0, output = 0; uint32_t crc = 0; bool finished = false;
    while (ok && !finished) {
        size_t in = compressed - used, out = 32768;
        if (method == 0) {
            out = in < out ? in : out; memcpy(window, zip + offset + used, out); in = out;
            finished = used + in == compressed;
        } else {
#ifdef ESP_PLATFORM
            tinfl_status status = tinfl_decompress(decoder, zip + offset + used, &in, window, window, &out, 0);
            ok = status == TINFL_STATUS_DONE || status == TINFL_STATUS_HAS_MORE_OUTPUT;
            finished = status == TINFL_STATUS_DONE;
#else
            decoder->next_in = (Bytef *)(zip + offset + used); decoder->avail_in = (uInt)in;
            decoder->next_out = window; decoder->avail_out = (uInt)out;
            int status = inflate(decoder, Z_NO_FLUSH);
            in -= decoder->avail_in; out -= decoder->avail_out;
            ok = status == Z_OK || status == Z_STREAM_END; finished = status == Z_STREAM_END;
#endif
        }
        used += in; output += out;
        ok = ok && output <= raw && (in || out || finished) && feed(csv, window, out);
#ifdef ESP_PLATFORM
        crc = (uint32_t)mz_crc32(crc, window, out);
        vTaskDelay(1);
#else
        crc = (uint32_t)crc32(crc, window, (uInt)out);
#endif
    }
    ok = ok && used == compressed && output == raw && crc == expected_crc && !csv->quoted;
    if (ok && (csv->started || csv->column)) ok = row_end(csv);
    ok = ok && csv->matches == 1;
    if (ok) { *result = csv->match; result->rows = csv->row - 1; result->csv_bytes = (unsigned)output; }
done:
#ifndef ESP_PLATFORM
    if (initialized) inflateEnd(decoder);
#endif
    free(decoder); free(csv); free(window); return ok;
}
