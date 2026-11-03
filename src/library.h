#ifndef ALOUD_LIBRARY_H
#define ALOUD_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store.h"

#define LIB_MAX_BOOKS 256
#define LIB_MAX_FILES_PER_BOOK 512
#define LIB_MAX_CHAPTERS_PER_BOOK 512
#define LIB_THUMB_SIZE 256

typedef struct {
    char relpath[512];  /* Path relative to the book folder (or empty for single-file book) */
    char fullpath[1024];
    char title[256];
    int track_number;
    int64_t duration_ms;
    int64_t start_book_ms; /* Cumulative offset within the book */
} BookFile;

typedef struct {
    char title[256];
    int file_index;
    int64_t start_book_ms;
    int64_t duration_ms;
} BookChapter;

typedef struct {
    char key[STORE_KEY_MAX]; /* Path relative to library root */
    char fullpath[1024];
    char title[256];
    char author[256];
    bool is_folder;

    BookFile *files;
    size_t file_count;

    BookChapter *chapters;
    size_t chapter_count;

    int64_t total_duration_ms;

    /* Saved listening state (synced from Store). */
    int saved_file_index;
    int64_t saved_file_pos_ms;
    int64_t saved_book_pos_ms;
    int saved_speed_x100;
    int64_t updated_epoch;
    bool finished;

    /* Pre-dithered square cover buffer (LIB_THUMB_SIZE x LIB_THUMB_SIZE, 8-bit gray), or NULL. */
    uint8_t *cover_gray;
} Book;

typedef struct {
    char root_dir[1024];
    char state_dir[1024];
    Book books[LIB_MAX_BOOKS];
    size_t count;
} Library;

/* Natural sort comparator ("Chapter 2" < "Chapter 10", case-insensitive with ASCII tie-break). */
int lib_natural_cmp(const char *a, const char *b);

/* Check whether two filenames differ in at least one numeric run. */
bool lib_names_differ_in_number(const char *a, const char *b);

bool lib_is_supported_audio_ext(const char *filename);

void lib_init(Library *lib);
void lib_free(Library *lib);

/* Scan library_dir, using state_dir for metadata/thumbnail caching, and sync
 * saved positions from store. Orders books most-recently-played first, then by title. */
int lib_scan(Library *lib, const char *library_dir, const char *state_dir, const Store *store);

/* Re-sync book positions and re-sort after a playback update. */
void lib_sync_store(Library *lib, const Store *store);

/* Convert (file_index, file_pos_ms) <-> book_pos_ms. */
int64_t lib_book_pos_from_file(const Book *b, int file_index, int64_t file_pos_ms);
void lib_file_pos_from_book(const Book *b, int64_t book_pos_ms, int *out_file_index, int64_t *out_file_pos_ms);

/* Find the chapter index containing book_pos_ms. */
int lib_chapter_at_pos(const Book *b, int64_t book_pos_ms);

#endif /* ALOUD_LIBRARY_H */
