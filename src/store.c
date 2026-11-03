#define _POSIX_C_SOURCE 200809L
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Header + footer sentinel ensure a truncated file (even if cut at a newline
 * boundary) is detected as corrupt and triggers fallback to positions.tsv.bak. */
#define STORE_HEADER "# aloud-positions-v1\n"
#define STORE_FOOTER "# end\n"

void store_init(Store *s) {
    memset(s, 0, sizeof(*s));
}

static int mkdir_p(const char *dir) {
    char tmp[1024];
    size_t len = strlen(dir);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, dir, len + 1);
    if (tmp[len - 1] == '/' && len > 1) {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int parse_file(const char *path, Store *out) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -ENOENT;
    }

    Store tmp;
    store_init(&tmp);

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -EINVAL;
    }
    if (strcmp(line, STORE_HEADER) != 0) {
        fclose(fp);
        return -EINVAL;
    }

    bool saw_footer = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strcmp(line, STORE_FOOTER) == 0) {
            saw_footer = true;
            break;
        }
        size_t l = strlen(line);
        if (l == 0 || line[l - 1] != '\n') {
            /* Truncated line without newline. */
            fclose(fp);
            return -EINVAL;
        }
        line[l - 1] = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        /* Columns: key \t file_index \t position_ms \t speed_x100 \t updated_epoch [\t finished] */
        char *saveptr = NULL;
        char *key = strtok_r(line, "\t", &saveptr);
        char *fidx_s = strtok_r(NULL, "\t", &saveptr);
        char *pos_s = strtok_r(NULL, "\t", &saveptr);
        char *spd_s = strtok_r(NULL, "\t", &saveptr);
        char *upd_s = strtok_r(NULL, "\t", &saveptr);
        char *fin_s = strtok_r(NULL, "\t", &saveptr);

        if (!key || !fidx_s || !pos_s || !spd_s || !upd_s || key[0] == '\0') {
            fclose(fp);
            return -EINVAL;
        }

        char *end = NULL;
        long fidx = strtol(fidx_s, &end, 10);
        if (!end || *end != '\0' || fidx < 0 || fidx > 100000) {
            fclose(fp);
            return -EINVAL;
        }
        int64_t pos_ms = strtoll(pos_s, &end, 10);
        if (!end || *end != '\0' || pos_ms < 0) {
            fclose(fp);
            return -EINVAL;
        }
        long spd = strtol(spd_s, &end, 10);
        if (!end || *end != '\0' || spd < 50 || spd > 300) {
            fclose(fp);
            return -EINVAL;
        }
        int64_t upd = strtoll(upd_s, &end, 10);
        if (!end || *end != '\0' || upd < 0) {
            fclose(fp);
            return -EINVAL;
        }
        bool fin = (fin_s && strcmp(fin_s, "1") == 0);

        if (tmp.count >= STORE_MAX_ENTRIES) {
            break;
        }
        StoreEntry *e = &tmp.entries[tmp.count++];
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->file_index = (int)fidx;
        e->position_ms = pos_ms;
        e->speed_x100 = (int)spd;
        e->updated_epoch = upd;
        e->finished = fin;
    }

    fclose(fp);
    if (!saw_footer) {
        return -EINVAL;
    }

    *out = tmp;
    return 0;
}

static void load_library_dir_file(Store *s, const char *state_dir) {
    char lpath[1024];
    snprintf(lpath, sizeof(lpath), "%s/library_dir.txt", state_dir);
    FILE *fp = fopen(lpath, "r");
    if (!fp) return;
    char line[1024];
    if (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (line[0] != '\0') {
            snprintf(s->library_dir, sizeof(s->library_dir), "%s", line);
        }
    }
    fclose(fp);
}

int store_load(Store *s, const char *state_dir) {
    if (!s || !state_dir) {
        return -EINVAL;
    }
    store_init(s);

    char primary[1024];
    char backup[1024];
    snprintf(primary, sizeof(primary), "%s/positions.tsv", state_dir);
    snprintf(backup, sizeof(backup), "%s/positions.tsv.bak", state_dir);

    int rc = parse_file(primary, s);
    if (rc == 0) {
        load_library_dir_file(s, state_dir);
        return 0;
    }

    int brc = parse_file(backup, s);
    if (brc == 0) {
        s->recovered_from_bak = true;
        load_library_dir_file(s, state_dir);
        return 0;
    }

    if (rc == -ENOENT && brc == -ENOENT) {
        load_library_dir_file(s, state_dir);
        return 0;
    }
    /* Both files were corrupt; start clean rather than crashing (Conventions). */
    store_init(s);
    load_library_dir_file(s, state_dir);
    return -EIO;
}

static int copy_valid_to_bak(const char *primary, const char *backup) {
    Store check;
    if (parse_file(primary, &check) != 0) {
        return -1;
    }
    int in_fd = open(primary, O_RDONLY);
    if (in_fd < 0) {
        return -1;
    }
    int out_fd = open(backup, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        close(in_fd);
        return -1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out_fd, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                close(in_fd);
                close(out_fd);
                return -1;
            }
            off += w;
        }
    }
    fsync(out_fd);
    close(out_fd);
    close(in_fd);
    return 0;
}

int store_save(const Store *s, const char *state_dir) {
    if (!s || !state_dir) {
        return -EINVAL;
    }
    if (mkdir_p(state_dir) != 0) {
        return -errno;
    }

    char primary[1024];
    char backup[1024];
    char tmp_path[1024];
    snprintf(primary, sizeof(primary), "%s/positions.tsv", state_dir);
    snprintf(backup, sizeof(backup), "%s/positions.tsv.bak", state_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/positions.tsv.tmp", state_dir);

    /* Preserve the last valid primary file as .bak before replacing it. */
    copy_valid_to_bak(primary, backup);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -errno;
    }

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        int err = errno;
        close(fd);
        unlink(tmp_path);
        return -err;
    }

    if (fputs(STORE_HEADER, fp) == EOF) {
        fclose(fp);
        unlink(tmp_path);
        return -EIO;
    }

    for (size_t i = 0; i < s->count; i++) {
        const StoreEntry *e = &s->entries[i];
        /* Sanitize any tabs or newlines in key so TSV cannot be corrupted. */
        char clean_key[STORE_KEY_MAX];
        snprintf(clean_key, sizeof(clean_key), "%s", e->key);
        for (char *p = clean_key; *p; p++) {
            if (*p == '\t' || *p == '\n' || *p == '\r') {
                *p = ' ';
            }
        }
        int spd = e->speed_x100;
        if (spd < 50) spd = 50;
        if (spd > 300) spd = 300;
        if (fprintf(fp, "%s\t%d\t%" PRId64 "\t%d\t%" PRId64 "\t%d\n",
                    clean_key, e->file_index, e->position_ms, spd,
                    e->updated_epoch, e->finished ? 1 : 0) < 0) {
            fclose(fp);
            unlink(tmp_path);
            return -EIO;
        }
    }

    if (fputs(STORE_FOOTER, fp) == EOF || fflush(fp) != 0) {
        fclose(fp);
        unlink(tmp_path);
        return -EIO;
    }

    if (fsync(fd) != 0) {
        fclose(fp);
        unlink(tmp_path);
        return -EIO;
    }
    fclose(fp);

    if (rename(tmp_path, primary) != 0) {
        int err = errno;
        unlink(tmp_path);
        return -err;
    }

    /* fsync the parent directory so the directory entry survives a sudden power cut (D8). */
    int dir_fd = open(state_dir, O_RDONLY);
    if (dir_fd >= 0) {
        (void)fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

const StoreEntry *store_get(const Store *s, const char *key) {
    if (!s || !key) {
        return NULL;
    }
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            return &s->entries[i];
        }
    }
    return NULL;
}

int store_set(Store *s, const char *key, int file_index, int64_t position_ms,
              int speed_x100, int64_t updated_epoch, bool finished) {
    if (!s || !key || key[0] == '\0') {
        return -EINVAL;
    }
    if (file_index < 0) file_index = 0;
    if (position_ms < 0) position_ms = 0;
    if (speed_x100 < 50) speed_x100 = 50;
    if (speed_x100 > 300) speed_x100 = 300;

    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) {
            s->entries[i].file_index = file_index;
            s->entries[i].position_ms = position_ms;
            s->entries[i].speed_x100 = speed_x100;
            s->entries[i].updated_epoch = updated_epoch;
            s->entries[i].finished = finished;
            return 0;
        }
    }

    if (s->count >= STORE_MAX_ENTRIES) {
        /* Evict the oldest entry if the table is full. */
        size_t oldest = 0;
        for (size_t i = 1; i < s->count; i++) {
            if (s->entries[i].updated_epoch < s->entries[oldest].updated_epoch) {
                oldest = i;
            }
        }
        StoreEntry *e = &s->entries[oldest];
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->file_index = file_index;
        e->position_ms = position_ms;
        e->speed_x100 = speed_x100;
        e->updated_epoch = updated_epoch;
        e->finished = finished;
        return 0;
    }

    StoreEntry *e = &s->entries[s->count++];
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->file_index = file_index;
    e->position_ms = position_ms;
    e->speed_x100 = speed_x100;
    e->updated_epoch = updated_epoch;
    e->finished = finished;
    return 0;
}

const char *store_get_library_dir(const Store *s) {
    if (!s || s->library_dir[0] == '\0') {
        return NULL;
    }
    return s->library_dir;
}

int store_set_library_dir(Store *s, const char *state_dir, const char *library_dir) {
    if (!s || !library_dir || library_dir[0] == '\0') {
        return -EINVAL;
    }
    snprintf(s->library_dir, sizeof(s->library_dir), "%s", library_dir);
    /* Trim trailing slash unless root "/" */
    size_t len = strlen(s->library_dir);
    while (len > 1 && s->library_dir[len - 1] == '/') {
        s->library_dir[--len] = '\0';
    }

    if (!state_dir || state_dir[0] == '\0') {
        return 0;
    }
    if (mkdir_p(state_dir) != 0) {
        return -errno;
    }

    char primary[1024];
    char tmp_path[1024];
    snprintf(primary, sizeof(primary), "%s/library_dir.txt", state_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/library_dir.txt.tmp", state_dir);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -errno;
    }
    char line[1040];
    int n = snprintf(line, sizeof(line), "%s\n", s->library_dir);
    if (n <= 0 || write(fd, line, (size_t)n) != (ssize_t)n || fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -EIO;
    }
    close(fd);
    if (rename(tmp_path, primary) != 0) {
        int err = errno;
        unlink(tmp_path);
        return -err;
    }
    int dir_fd = open(state_dir, O_RDONLY);
    if (dir_fd >= 0) {
        (void)fsync(dir_fd);
        close(dir_fd);
    }
    return 0;
}

