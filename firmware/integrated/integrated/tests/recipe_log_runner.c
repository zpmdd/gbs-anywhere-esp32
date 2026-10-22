#include "recipe_log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    assert(argc == 4);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long size = ftell(f); assert(size > 0);
    rewind(f); uint8_t *data = malloc((size_t)size); assert(data);
    assert(fread(data, 1, (size_t)size, f) == (size_t)size); fclose(f);
    gbs_recipe_match_t result;
    bool ok = gbs_recipe_log_match(data, (size_t)size, argv[2], &result);
    unsigned expected = (unsigned)atoi(argv[3]);
    assert(ok == (expected != 0));
    if (ok) { assert(result.grams == expected); printf("matched %ug, %u rows, %u CSV bytes\n", result.grams, result.rows, result.csv_bytes); }
    free(data); return 0;
}
