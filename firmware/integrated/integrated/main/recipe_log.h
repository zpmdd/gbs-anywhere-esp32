#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GBS_LOG_ZIP_LIMIT (1024u * 1024u)
#define GBS_LOG_CSV_LIMIT (2u * 1024u * 1024u)
typedef struct {
    unsigned grams, recipe_index, rows, csv_bytes;
    char date[21];
} gbs_recipe_match_t;

// The caller owns the compressed buffer; only a 32 KiB inflate window is allocated.
bool gbs_recipe_log_match(const uint8_t *zip, size_t size, const char *uuid,
                          gbs_recipe_match_t *result);
