#define _POSIX_C_SOURCE 200809L
#include "library.h"

#include "canvas.h"
#include "decoder.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int lib_natural_cmp(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;

    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (*pa && *pb) {
        if (isdigit(*pa) && isdigit(*pb)) {
            while (*pa == '0') pa++;
            while (*pb == '0') pb++;

            const unsigned char *da = pa;
            const unsigned char *db = pb;
            while (isdigit(*pa)) pa++;
            while (isdigit(*pb)) pb++;

            size_t lena = (size_t)(pa - da);
            size_t lenb = (size_t)(pb - db);
            if (lena != lenb) {
                return (lena < lenb) ? -1 : 1;
            }
            for (size_t i = 0; i < lena; i++) {
                if (da[i] != db[i]) {
                    return (da[i] < db[i]) ? -1 : 1;
                }
            }
            continue;
        }

        int ca = tolower(*pa);
        int cb = tolower(*pb);
        if (ca != cb) {
            return (ca < cb) ? -1 : 1;
        }
        pa++;
        pb++;
    }

    if (*pa != *pb) {
        return (*pa == '\0') ? -1 : 1;
    }
    return strcmp(a, b);
}

bool lib_names_differ_in_number(const char *a, const char *b) {
    if (!a || !b) return false;
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (*pa && *pb) {
        if (isdigit(*pa) && isdigit(*pb)) {
            unsigned long va = strtoul((const char *)pa, (char **)&pa, 10);
            unsigned long vb = strtoul((const char *)pb, (char **)&pb, 10);
            if (va != vb) {
                return true;
            }
            continue;
        }
        if (isdigit(*pa) != isdigit(*pb)) {
            return false;
        }
        if (tolower(*pa) != tolower(*pb)) {
            return false;
        }
        pa++;
        pb++;
    }
    return false;
}

bool lib_is_supported_audio_ext(const char *filename) {
    if (!filename || filename[0] == '.') return false;
    const char *dot = strrchr(filename, '.');
    if (!dot || dot[1] == '\0') return false;
    const char *ext = dot + 1;
    static const char *const kExts[] = {
        "m4b", "m4a", "mp3", "ogg", "opus", "flac", "wav", "aac", "mka"
    };
    for (size_t i = 0; i < sizeof(kExts) / sizeof(kExts[0]); i++) {
        if (strcasecmp(ext, kExts[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_cover_image_name(const char *filename) {
    if (!filename || filename[0] == '.') return false;
    static const char *const kNames[] = {
        "cover.jpg", "cover.jpeg", "cover.png",
        "folder.jpg", "folder.jpeg", "folder.png"
    };
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (strcasecmp(filename, kNames[i]) == 0) {
            return true;
        }
    }
    return false;
}

static uint64_t fnv1a_64(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    return h;
}

static void strip_extension(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) {
        *dot = '\0';
    }
}

/* Parse "Author - Title" from folder/file name when tags are absent. */
static void split_author_title_fallback(const char *raw_name,
                                        char *title_out, size_t title_cap,
                                        char *author_out, size_t author_cap) {
    const char *sep = strstr(raw_name, " - ");
    if (sep && sep > raw_name && sep[3] != '\0') {
        size_t alen = (size_t)(sep - raw_name);
        if (alen >= author_cap) alen = author_cap - 1;
        memcpy(author_out, raw_name, alen);
        author_out[alen] = '\0';
        snprintf(title_out, title_cap, "%s", sep + 3);
    } else {
        snprintf(title_out, title_cap, "%s", raw_name);
        snprintf(author_out, author_cap, "Unknown author");
    }
}

/* Metadata cache in state_dir/library.tsv so unchanged files skip FFmpeg probing (D8). */
typedef struct {
    char relpath[512];
    int64_t size;
    int64_t mtime;
    int64_t duration_ms;
    int track_number;
    char title[256];
    char album[256];
    char artist[256];
    char album_artist[256];
    int has_chapters;
} CacheRow;

typedef struct {
    CacheRow *rows;
    size_t count;
    size_t cap;
} MetaCache;

static void cache_free(MetaCache *mc) {
    free(mc->rows);
    mc->rows = NULL;
    mc->count = 0;
    mc->cap = 0;
}

static void sanitize_tsv_field(char *s) {
    for (; *s; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
    }
}

static void cache_load(MetaCache *mc, const char *state_dir) {
    memset(mc, 0, sizeof(*mc));
    if (!state_dir || !state_dir[0]) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/library.tsv", state_dir);
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\n') line[l - 1] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *fields[10] = {0};
        char *p = line;
        for (int i = 0; i < 10; i++) {
            fields[i] = p;
            char *tab = strchr(p, '\t');
            if (tab) {
                *tab = '\0';
                p = tab + 1;
            } else {
                p = p + strlen(p);
            }
        }
        if (!fields[0][0] || !fields[3][0]) continue;

        if (mc->count >= mc->cap) {
            size_t ncap = mc->cap ? mc->cap * 2 : 64;
            CacheRow *nrows = realloc(mc->rows, ncap * sizeof(CacheRow));
            if (!nrows) break;
            mc->rows = nrows;
            mc->cap = ncap;
        }
        CacheRow *r = &mc->rows[mc->count++];
        memset(r, 0, sizeof(*r));
        snprintf(r->relpath, sizeof(r->relpath), "%s", fields[0]);
        r->size = strtoll(fields[1], NULL, 10);
        r->mtime = strtoll(fields[2], NULL, 10);
        r->duration_ms = strtoll(fields[3], NULL, 10);
        r->track_number = (int)strtol(fields[4], NULL, 10);
        snprintf(r->title, sizeof(r->title), "%s", fields[5]);
        snprintf(r->album, sizeof(r->album), "%s", fields[6]);
        snprintf(r->artist, sizeof(r->artist), "%s", fields[7]);
        snprintf(r->album_artist, sizeof(r->album_artist), "%s", fields[8]);
        r->has_chapters = (int)strtol(fields[9], NULL, 10);
    }
    fclose(fp);
}

static const CacheRow *cache_find(const MetaCache *mc, const char *relpath, int64_t size, int64_t mtime) {
    for (size_t i = 0; i < mc->count; i++) {
        if (mc->rows[i].size == size &&
            mc->rows[i].mtime == mtime &&
            strcmp(mc->rows[i].relpath, relpath) == 0) {
            return &mc->rows[i];
        }
    }
    return NULL;
}

static void cache_add(MetaCache *mc, const char *relpath, int64_t size, int64_t mtime,
                      const DecoderProbeInfo *info) {
    if (mc->count >= mc->cap) {
        size_t ncap = mc->cap ? mc->cap * 2 : 64;
        CacheRow *nrows = realloc(mc->rows, ncap * sizeof(CacheRow));
        if (!nrows) return;
        mc->rows = nrows;
        mc->cap = ncap;
    }
    CacheRow *r = &mc->rows[mc->count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->relpath, sizeof(r->relpath), "%s", relpath);
    r->size = size;
    r->mtime = mtime;
    r->duration_ms = info->duration_ms;
    r->track_number = info->track_number;
    snprintf(r->title, sizeof(r->title), "%s", info->title);
    snprintf(r->album, sizeof(r->album), "%s", info->album);
    snprintf(r->artist, sizeof(r->artist), "%s", info->artist);
    snprintf(r->album_artist, sizeof(r->album_artist), "%s", info->album_artist);
    r->has_chapters = (info->chapter_count > 0) ? 1 : 0;

    sanitize_tsv_field(r->relpath);
    sanitize_tsv_field(r->title);
    sanitize_tsv_field(r->album);
    sanitize_tsv_field(r->artist);
    sanitize_tsv_field(r->album_artist);
}

static void cache_save(const MetaCache *mc, const char *state_dir) {
    if (!state_dir || !state_dir[0]) return;
    mkdir(state_dir, 0755);
    char tmp[1024];
    char dst[1024];
    snprintf(tmp, sizeof(tmp), "%s/library.tsv.tmp", state_dir);
    snprintf(dst, sizeof(dst), "%s/library.tsv", state_dir);

    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    fputs("# aloud-library-cache-v1\n", fp);
    for (size_t i = 0; i < mc->count; i++) {
        const CacheRow *r = &mc->rows[i];
        fprintf(fp, "%s\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%d\t%s\t%s\t%s\t%s\t%d\n",
                r->relpath, r->size, r->mtime, r->duration_ms, r->track_number,
                r->title, r->album, r->artist, r->album_artist, r->has_chapters);
    }
    fflush(fp);
    fclose(fp);
    rename(tmp, dst);
}

static uint8_t *load_cached_thumb(const char *state_dir, const char *book_key) {
    if (!state_dir || !state_dir[0]) return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/thumbs/%016" PRIx64 ".raw", state_dir, fnv1a_64(book_key));
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t expected = (size_t)LIB_THUMB_SIZE * (size_t)LIB_THUMB_SIZE;
    uint8_t *buf = malloc(expected);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, expected, fp) != expected) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return buf;
}

static void save_cached_thumb(const char *state_dir, const char *book_key, const uint8_t *gray) {
    if (!state_dir || !state_dir[0] || !gray) return;
    mkdir(state_dir, 0755);
    char tdir[1024];
    snprintf(tdir, sizeof(tdir), "%s/thumbs", state_dir);
    mkdir(tdir, 0755);

    char path[1024];
    snprintf(path, sizeof(path), "%s/%016" PRIx64 ".raw", tdir, fnv1a_64(book_key));
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    size_t expected = (size_t)LIB_THUMB_SIZE * (size_t)LIB_THUMB_SIZE;
    (void)fwrite(gray, 1, expected, fp);
    fclose(fp);
}

static uint8_t *read_whole_file(const char *path, size_t *out_len) {
    *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 16 * 1024 * 1024) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_len = (size_t)sz;
    return buf;
}

typedef struct {
    char relpath[512];
    char fullpath[1024];
} DiscoveredFile;

typedef struct {
    DiscoveredFile *items;
    size_t count;
    size_t cap;
    char cover_path[1024];
} FolderScan;

static int cmp_discovered_natural(const void *pa, const void *pb) {
    const DiscoveredFile *a = (const DiscoveredFile *)pa;
    const DiscoveredFile *b = (const DiscoveredFile *)pb;
    return lib_natural_cmp(a->relpath, b->relpath);
}

static void collect_folder_files_rec(const char *book_root, const char *rel_prefix,
                                     int depth, FolderScan *scan) {
    if (depth > 6) return;
    char dirpath[1024];
    if (rel_prefix[0]) {
        snprintf(dirpath, sizeof(dirpath), "%s/%s", book_root, rel_prefix);
    } else {
        snprintf(dirpath, sizeof(dirpath), "%s", book_root);
    }

    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child_rel[512];
        char child_full[1024];
        if (rel_prefix[0]) {
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_prefix, de->d_name);
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s", de->d_name);
        }
        snprintf(child_full, sizeof(child_full), "%s/%s", book_root, child_rel);

        struct stat st;
        if (stat(child_full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            collect_folder_files_rec(book_root, child_rel, depth + 1, scan);
        } else if (S_ISREG(st.st_mode)) {
            if (lib_is_supported_audio_ext(de->d_name)) {
                if (scan->count < LIB_MAX_FILES_PER_BOOK) {
                    if (scan->count >= scan->cap) {
                        size_t ncap = scan->cap ? scan->cap * 2 : 32;
                        DiscoveredFile *nitems = realloc(scan->items, ncap * sizeof(DiscoveredFile));
                        if (!nitems) continue;
                        scan->items = nitems;
                        scan->cap = ncap;
                    }
                    DiscoveredFile *df = &scan->items[scan->count++];
                    snprintf(df->relpath, sizeof(df->relpath), "%s", child_rel);
                    snprintf(df->fullpath, sizeof(df->fullpath), "%s", child_full);
                }
            } else if (!scan->cover_path[0] && is_cover_image_name(de->d_name)) {
                snprintf(scan->cover_path, sizeof(scan->cover_path), "%s", child_full);
            }
        }
    }
    closedir(d);
}

/* Order book files per DESIGN.md: natural sort of filename ("2" < "10"),
 * falling back to track-number tags only when filenames don't differ in a number. */
static int cmp_book_files(const void *pa, const void *pb) {
    const BookFile *a = (const BookFile *)pa;
    const BookFile *b = (const BookFile *)pb;

    const char *base_a = strrchr(a->relpath, '/');
    const char *base_b = strrchr(b->relpath, '/');
    base_a = base_a ? (base_a + 1) : a->relpath;
    base_b = base_b ? (base_b + 1) : b->relpath;

    /* If files live in different subdirectories (e.g. Disc 1 vs Disc 2), sort by
     * subdirectory natural order first. */
    size_t dir_len_a = (size_t)(base_a - a->relpath);
    size_t dir_len_b = (size_t)(base_b - b->relpath);
    if (dir_len_a != dir_len_b || strncasecmp(a->relpath, b->relpath, dir_len_a) != 0) {
        char dira[512] = {0};
        char dirb[512] = {0};
        if (dir_len_a > 0 && dir_len_a < sizeof(dira)) memcpy(dira, a->relpath, dir_len_a);
        if (dir_len_b > 0 && dir_len_b < sizeof(dirb)) memcpy(dirb, b->relpath, dir_len_b);
        int dcmp = lib_natural_cmp(dira, dirb);
        if (dcmp != 0) return dcmp;
    }

    if (!lib_names_differ_in_number(base_a, base_b) &&
        a->track_number > 0 && b->track_number > 0 &&
        a->track_number != b->track_number) {
        return (a->track_number < b->track_number) ? -1 : 1;
    }
    return lib_natural_cmp(a->relpath, b->relpath);
}

void lib_init(Library *lib) {
    if (!lib) return;
    memset(lib, 0, sizeof(*lib));
}

void lib_free(Library *lib) {
    if (!lib) return;
    for (size_t i = 0; i < lib->count; i++) {
        free(lib->books[i].files);
        free(lib->books[i].chapters);
        free(lib->books[i].cover_gray);
    }
    memset(lib, 0, sizeof(*lib));
}

static int probe_or_cached(const char *rel_key, const char *fullpath,
                           const MetaCache *old_cache, MetaCache *new_cache,
                           DecoderProbeInfo *out_info,
                           bool want_cover, uint8_t **cover_bytes, size_t *cover_len) {
    struct stat st;
    if (stat(fullpath, &st) != 0) {
        return -ENOENT;
    }
    int64_t fsize = (int64_t)st.st_size;
    int64_t fmtime = (int64_t)st.st_mtime;

    const CacheRow *cached = cache_find(old_cache, rel_key, fsize, fmtime);
    /* Use cached row when we don't need embedded chapters or raw cover bytes. */
    if (cached && !cached->has_chapters && !want_cover) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->duration_ms = cached->duration_ms;
        out_info->track_number = cached->track_number;
        snprintf(out_info->title, sizeof(out_info->title), "%s", cached->title);
        snprintf(out_info->album, sizeof(out_info->album), "%s", cached->album);
        snprintf(out_info->artist, sizeof(out_info->artist), "%s", cached->artist);
        snprintf(out_info->album_artist, sizeof(out_info->album_artist), "%s", cached->album_artist);
        cache_add(new_cache, rel_key, fsize, fmtime, out_info);
        return 0;
    }

    int rc = decoder_probe(fullpath, out_info, cover_bytes, cover_len);
    if (rc == 0) {
        cache_add(new_cache, rel_key, fsize, fmtime, out_info);
    }
    return rc;
}

static int build_book_from_entry(Book *b, const char *lib_root, const char *entry_name,
                                 bool is_dir, const char *state_dir,
                                 const MetaCache *old_cache, MetaCache *new_cache) {
    memset(b, 0, sizeof(*b));
    snprintf(b->key, sizeof(b->key), "%s", entry_name);
    snprintf(b->fullpath, sizeof(b->fullpath), "%s/%s", lib_root, entry_name);
    b->is_folder = is_dir;
    b->saved_speed_x100 = 100;

    b->cover_gray = load_cached_thumb(state_dir, b->key);
    bool need_cover = (b->cover_gray == NULL);
    uint8_t *raw_cover = NULL;
    size_t raw_cover_len = 0;

    char album_tag[256] = {0};
    char title_tag[256] = {0};
    char album_artist_tag[256] = {0};
    char artist_tag[256] = {0};

    DecoderChapter embedded_ch[DECODER_MAX_CHAPTERS];
    size_t embedded_ch_count = 0;

    if (!is_dir) {
        b->files = calloc(1, sizeof(BookFile));
        if (!b->files) return -ENOMEM;

        DecoderProbeInfo info;
        int rc = probe_or_cached(b->key, b->fullpath, old_cache, new_cache, &info,
                                 need_cover, need_cover ? &raw_cover : NULL, &raw_cover_len);
        if (rc < 0) {
            free(b->files);
            b->files = NULL;
            return rc;
        }

        b->file_count = 1;
        BookFile *bf = &b->files[0];
        bf->relpath[0] = '\0';
        snprintf(bf->fullpath, sizeof(bf->fullpath), "%s", b->fullpath);
        bf->track_number = info.track_number;
        bf->duration_ms = info.duration_ms;
        bf->start_book_ms = 0;
        if (info.title[0]) {
            snprintf(bf->title, sizeof(bf->title), "%s", info.title);
        } else {
            strip_extension(entry_name, bf->title, sizeof(bf->title));
        }

        snprintf(album_tag, sizeof(album_tag), "%s", info.album);
        snprintf(title_tag, sizeof(title_tag), "%s", info.title);
        snprintf(album_artist_tag, sizeof(album_artist_tag), "%s", info.album_artist);
        snprintf(artist_tag, sizeof(artist_tag), "%s", info.artist);

        if (info.chapter_count > 0) {
            embedded_ch_count = info.chapter_count;
            memcpy(embedded_ch, info.chapters, info.chapter_count * sizeof(DecoderChapter));
        }
    } else {
        FolderScan scan;
        memset(&scan, 0, sizeof(scan));
        collect_folder_files_rec(b->fullpath, "", 0, &scan);
        if (scan.count == 0) {
            free(scan.items);
            return -ENOENT;
        }
        qsort(scan.items, scan.count, sizeof(DiscoveredFile), cmp_discovered_natural);

        b->files = calloc(scan.count, sizeof(BookFile));
        if (!b->files) {
            free(scan.items);
            return -ENOMEM;
        }

        for (size_t i = 0; i < scan.count; i++) {
            char rel_key[1024];
            snprintf(rel_key, sizeof(rel_key), "%s/%s", b->key, scan.items[i].relpath);

            DecoderProbeInfo info;
            bool probe_cover = (need_cover && !raw_cover && i == 0);
            if (probe_or_cached(rel_key, scan.items[i].fullpath, old_cache, new_cache, &info,
                                probe_cover, probe_cover ? &raw_cover : NULL, &raw_cover_len) < 0) {
                continue;
            }

            BookFile *bf = &b->files[b->file_count++];
            snprintf(bf->relpath, sizeof(bf->relpath), "%s", scan.items[i].relpath);
            snprintf(bf->fullpath, sizeof(bf->fullpath), "%s", scan.items[i].fullpath);
            bf->track_number = info.track_number;
            bf->duration_ms = info.duration_ms;

            const char *slash = strrchr(bf->relpath, '/');
            const char *bname = slash ? (slash + 1) : bf->relpath;
            if (info.title[0]) {
                snprintf(bf->title, sizeof(bf->title), "%s", info.title);
            } else {
                strip_extension(bname, bf->title, sizeof(bf->title));
            }

            if (!album_tag[0] && info.album[0]) snprintf(album_tag, sizeof(album_tag), "%s", info.album);
            if (!title_tag[0] && info.title[0]) snprintf(title_tag, sizeof(title_tag), "%s", info.title);
            if (!album_artist_tag[0] && info.album_artist[0]) snprintf(album_artist_tag, sizeof(album_artist_tag), "%s", info.album_artist);
            if (!artist_tag[0] && info.artist[0]) snprintf(artist_tag, sizeof(artist_tag), "%s", info.artist);

            if (scan.count == 1 && info.chapter_count > 0) {
                embedded_ch_count = info.chapter_count;
                memcpy(embedded_ch, info.chapters, info.chapter_count * sizeof(DecoderChapter));
            }
        }

        if (b->file_count == 0) {
            free(scan.items);
            free(b->files);
            b->files = NULL;
            free(raw_cover);
            return -ENOENT;
        }

        /* Fallback to cover.jpg / folder.jpg in the folder if no embedded art. */
        if (need_cover && !raw_cover && scan.cover_path[0]) {
            raw_cover = read_whole_file(scan.cover_path, &raw_cover_len);
        }
        free(scan.items);

        qsort(b->files, b->file_count, sizeof(BookFile), cmp_book_files);
    }

    /* Compute cumulative book offsets across all files. */
    int64_t accum_ms = 0;
    for (size_t i = 0; i < b->file_count; i++) {
        b->files[i].start_book_ms = accum_ms;
        if (b->files[i].duration_ms > 0) {
            accum_ms += b->files[i].duration_ms;
        }
    }
    b->total_duration_ms = accum_ms;

    /* Title & author rules per DESIGN.md:
     * Title: album tag -> title tag (if single file) -> folder/file name.
     * Author: album_artist -> artist -> "Author - Title" fallback -> "Unknown author". */
    char clean_name[256];
    if (is_dir) {
        snprintf(clean_name, sizeof(clean_name), "%s", entry_name);
    } else {
        strip_extension(entry_name, clean_name, sizeof(clean_name));
    }
    char fallback_title[256] = {0};
    char fallback_author[256] = {0};
    split_author_title_fallback(clean_name, fallback_title, sizeof(fallback_title),
                                fallback_author, sizeof(fallback_author));

    if (album_tag[0]) {
        snprintf(b->title, sizeof(b->title), "%s", album_tag);
    } else if (!is_dir && title_tag[0]) {
        snprintf(b->title, sizeof(b->title), "%s", title_tag);
    } else {
        snprintf(b->title, sizeof(b->title), "%s", fallback_title);
    }

    if (album_artist_tag[0]) {
        snprintf(b->author, sizeof(b->author), "%s", album_artist_tag);
    } else if (artist_tag[0]) {
        snprintf(b->author, sizeof(b->author), "%s", artist_tag);
    } else {
        snprintf(b->author, sizeof(b->author), "%s", fallback_author);
    }

    /* Build chapters: embedded chapters when present; otherwise one chapter per file. */
    if (embedded_ch_count > 0) {
        b->chapters = calloc(embedded_ch_count, sizeof(BookChapter));
        if (b->chapters) {
            b->chapter_count = embedded_ch_count;
            for (size_t i = 0; i < embedded_ch_count; i++) {
                BookChapter *ch = &b->chapters[i];
                snprintf(ch->title, sizeof(ch->title), "%s", embedded_ch[i].title);
                ch->file_index = 0;
                ch->start_book_ms = embedded_ch[i].start_ms;
                ch->duration_ms = embedded_ch[i].end_ms - embedded_ch[i].start_ms;
                if (ch->duration_ms < 0) ch->duration_ms = 0;
            }
        }
    } else {
        b->chapters = calloc(b->file_count, sizeof(BookChapter));
        if (b->chapters) {
            b->chapter_count = b->file_count;
            for (size_t i = 0; i < b->file_count; i++) {
                BookChapter *ch = &b->chapters[i];
                snprintf(ch->title, sizeof(ch->title), "%s", b->files[i].title);
                ch->file_index = (int)i;
                ch->start_book_ms = b->files[i].start_book_ms;
                ch->duration_ms = b->files[i].duration_ms;
            }
        }
    }

    /* Decode and Floyd-Steinberg dither cover art if found. */
    if (raw_cover && raw_cover_len > 0) {
        uint8_t *dithered = NULL;
        if (canvas_decode_and_dither_cover(raw_cover, raw_cover_len, LIB_THUMB_SIZE, &dithered) == 0) {
            b->cover_gray = dithered;
            save_cached_thumb(state_dir, b->key, dithered);
        }
        free(raw_cover);
    }

    return 0;
}

static int cmp_books_for_library(const void *pa, const void *pb) {
    const Book *a = (const Book *)pa;
    const Book *b = (const Book *)pb;
    bool a_played = (a->updated_epoch > 0);
    bool b_played = (b->updated_epoch > 0);
    if (a_played != b_played) {
        return a_played ? -1 : 1;
    }
    if (a_played && b_played && a->updated_epoch != b->updated_epoch) {
        return (a->updated_epoch > b->updated_epoch) ? -1 : 1;
    }
    int tcmp = lib_natural_cmp(a->title, b->title);
    if (tcmp != 0) return tcmp;
    return lib_natural_cmp(a->key, b->key);
}

int64_t lib_book_pos_from_file(const Book *b, int file_index, int64_t file_pos_ms) {
    if (!b || b->file_count == 0) return 0;
    if (file_index < 0) file_index = 0;
    if ((size_t)file_index >= b->file_count) {
        file_index = (int)b->file_count - 1;
    }
    if (file_pos_ms < 0) file_pos_ms = 0;
    int64_t dur = b->files[file_index].duration_ms;
    if (dur > 0 && file_pos_ms > dur) {
        file_pos_ms = dur;
    }
    int64_t book_pos = b->files[file_index].start_book_ms + file_pos_ms;
    if (b->total_duration_ms > 0 && book_pos > b->total_duration_ms) {
        book_pos = b->total_duration_ms;
    }
    return book_pos;
}

void lib_file_pos_from_book(const Book *b, int64_t book_pos_ms,
                            int *out_file_index, int64_t *out_file_pos_ms) {
    if (out_file_index) *out_file_index = 0;
    if (out_file_pos_ms) *out_file_pos_ms = 0;
    if (!b || b->file_count == 0) return;

    if (book_pos_ms <= 0) return;
    if (b->total_duration_ms > 0 && book_pos_ms >= b->total_duration_ms) {
        int last = (int)b->file_count - 1;
        if (out_file_index) *out_file_index = last;
        if (out_file_pos_ms) *out_file_pos_ms = b->files[last].duration_ms;
        return;
    }

    for (size_t i = 0; i < b->file_count; i++) {
        int64_t start = b->files[i].start_book_ms;
        int64_t end = start + b->files[i].duration_ms;
        if (book_pos_ms < end || i + 1 == b->file_count) {
            if (out_file_index) *out_file_index = (int)i;
            int64_t fpos = book_pos_ms - start;
            if (fpos < 0) fpos = 0;
            if (out_file_pos_ms) *out_file_pos_ms = fpos;
            return;
        }
    }
}

int lib_chapter_at_pos(const Book *b, int64_t book_pos_ms) {
    if (!b || b->chapter_count == 0) return 0;
    if (book_pos_ms <= 0) return 0;
    for (size_t i = 0; i < b->chapter_count; i++) {
        int64_t next_start = (i + 1 < b->chapter_count)
                                 ? b->chapters[i + 1].start_book_ms
                                 : b->total_duration_ms;
        if (book_pos_ms < next_start || i + 1 == b->chapter_count) {
            return (int)i;
        }
    }
    return (int)b->chapter_count - 1;
}

void lib_sync_store(Library *lib, const Store *store) {
    if (!lib) return;
    for (size_t i = 0; i < lib->count; i++) {
        Book *b = &lib->books[i];
        const StoreEntry *se = store ? store_get(store, b->key) : NULL;
        if (se) {
            b->saved_file_index = se->file_index;
            b->saved_file_pos_ms = se->position_ms;
            b->saved_book_pos_ms = lib_book_pos_from_file(b, se->file_index, se->position_ms);
            b->saved_speed_x100 = se->speed_x100;
            b->updated_epoch = se->updated_epoch;
            b->finished = se->finished;
        } else {
            b->saved_file_index = 0;
            b->saved_file_pos_ms = 0;
            b->saved_book_pos_ms = 0;
            b->saved_speed_x100 = 100;
            b->updated_epoch = 0;
            b->finished = false;
        }
    }
    if (lib->count > 1) {
        qsort(lib->books, lib->count, sizeof(Book), cmp_books_for_library);
    }
}

int lib_scan(Library *lib, const char *library_dir, const char *state_dir, const Store *store) {
    if (!lib || !library_dir) return -EINVAL;
    lib_free(lib);
    snprintf(lib->root_dir, sizeof(lib->root_dir), "%s", library_dir);
    if (state_dir) {
        snprintf(lib->state_dir, sizeof(lib->state_dir), "%s", state_dir);
    }

    DIR *d = opendir(library_dir);
    if (!d) {
        /* Missing library directory is treated as an empty library, not a fatal error. */
        return 0;
    }

    MetaCache old_cache, new_cache;
    cache_load(&old_cache, state_dir);
    memset(&new_cache, 0, sizeof(new_cache));

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (lib->count >= LIB_MAX_BOOKS) break;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", library_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        bool is_file = S_ISREG(st.st_mode) && lib_is_supported_audio_ext(de->d_name);
        if (!is_dir && !is_file) continue;

        Book candidate;
        if (build_book_from_entry(&candidate, library_dir, de->d_name, is_dir,
                                  state_dir, &old_cache, &new_cache) == 0) {
            lib->books[lib->count++] = candidate;
        }
    }
    closedir(d);

    cache_save(&new_cache, state_dir);
    cache_free(&old_cache);
    cache_free(&new_cache);

    lib_sync_store(lib, store);
    return 0;
}
