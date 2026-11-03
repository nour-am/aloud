#ifndef ALOUD_STORE_H
#define ALOUD_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STORE_KEY_MAX 512
#define STORE_MAX_ENTRIES 1024

typedef struct {
    char key[STORE_KEY_MAX];
    int file_index;
    int64_t position_ms;
    int speed_x100; /* e.g., 100 = 1.00x, 125 = 1.25x */
    int64_t updated_epoch;
    bool finished;
} StoreEntry;

typedef struct {
    StoreEntry entries[STORE_MAX_ENTRIES];
    size_t count;
    bool recovered_from_bak;
} Store;

void store_init(Store *s);

/* Load positions.tsv from state_dir. If missing or corrupt, falls back to
 * positions.tsv.bak (D8). Never fails on missing directory/files (starts empty). */
int store_load(Store *s, const char *state_dir);

/* Atomically save positions.tsv via .tmp -> fsync -> rename -> fsync(dir),
 * keeping the previous valid positions.tsv as positions.tsv.bak (D8). */
int store_save(const Store *s, const char *state_dir);

const StoreEntry *store_get(const Store *s, const char *key);

int store_set(Store *s, const char *key, int file_index, int64_t position_ms,
              int speed_x100, int64_t updated_epoch, bool finished);

#endif /* ALOUD_STORE_H */
