#define _POSIX_C_SOURCE 200809L
#include "mp4lite.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Table caps: far beyond any real audiobook, small enough that a hostile file can't make
 * us allocate much. Exceeding one means "let libavformat deal with it". */
#define MAX_RUN_ENTRIES (1u << 20)    /* stts, stsc */
#define MAX_CHUNKS (1u << 21)         /* stco/co64: 16 MiB worst case */
#define MAX_SAMPLE_BYTES (1u << 20)   /* one AAC frame is < 8 KiB */
#define STSZ_WINDOW 1024

typedef struct { uint32_t count, delta; } SttsEntry;
typedef struct { uint32_t first_chunk, per_chunk; } StscEntry;

struct Mp4Lite {
    int fd;
    uint64_t file_size;

    SttsEntry *stts;
    uint32_t stts_n;
    StscEntry *stsc;
    uint32_t stsc_n;
    uint64_t *chunk_off;
    uint32_t chunk_n;

    uint32_t fixed_size;   /* stsz sample_size; 0 = per-sample table */
    uint64_t stsz_off;     /* file offset of the per-sample table */
    uint32_t sample_n;
    int64_t media_time;    /* edit-list start (priming), in media timescale */

    uint32_t win[STSZ_WINDOW];
    uint32_t win_start, win_len;

    /* Cursor: next sample to read. */
    uint32_t cur;
    uint32_t chunk;          /* 0-based */
    uint32_t in_chunk;       /* index of cur within its chunk */
    uint32_t stsc_i;
    uint32_t stts_i, stts_used;
    int64_t dts;
    uint64_t off;
    uint32_t peek_size;
};

/* ---------- byte helpers ---------- */

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static int read_at(const Mp4Lite *m, uint64_t off, void *buf, size_t len) {
    if (off > m->file_size || len > m->file_size - off) return -EIO;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(m->fd, (uint8_t *)buf + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (n == 0) return -EIO;
        done += (size_t)n;
    }
    return 0;
}

/* Allocate and read a whole (small) box payload. */
static uint8_t *read_payload(const Mp4Lite *m, uint64_t off, uint64_t len, uint64_t cap) {
    if (len == 0 || len > cap) return NULL;
    uint8_t *b = malloc((size_t)len);
    if (b && read_at(m, off, b, (size_t)len) < 0) {
        free(b);
        b = NULL;
    }
    return b;
}

typedef struct {
    uint32_t type;
    uint64_t start;    /* offset of the box header */
    uint64_t payload;  /* offset of the payload */
    uint64_t end;      /* offset one past the box */
} Box;

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Read the box header at `off`, which must lie within [off, limit). */
static int box_at(const Mp4Lite *m, uint64_t off, uint64_t limit, Box *b) {
    uint8_t h[16];
    if (limit < off || limit - off < 8 || read_at(m, off, h, 8) < 0) return -EINVAL;
    uint64_t size = be32(h);
    b->type = be32(h + 4);
    b->start = off;
    b->payload = off + 8;
    if (size == 1) {
        if (limit - off < 16 || read_at(m, off + 8, h + 8, 8) < 0) return -EINVAL;
        size = be64(h + 8);
        b->payload = off + 16;
    } else if (size == 0) {
        size = limit - off; /* extends to the end of the enclosing space */
    }
    if (size < b->payload - off || size > limit - off) return -EINVAL;
    b->end = off + size;
    return 0;
}

/* Find the first child of [from, limit) with the given type. */
static int find_child(const Mp4Lite *m, uint64_t from, uint64_t limit, uint32_t type, Box *out) {
    uint64_t off = from;
    for (int guard = 0; off < limit && guard < 4096; guard++) {
        Box b;
        if (box_at(m, off, limit, &b) < 0) return -EINVAL;
        if (b.type == type) {
            *out = b;
            return 0;
        }
        off = b.end;
    }
    return -ENOENT;
}

/* ---------- sample description (mp4a/esds) ---------- */

/* MPEG-4 descriptor length: 1-4 bytes, 7 bits each. */
static int desc_len(const uint8_t *p, size_t n, size_t *pos, uint32_t *len) {
    *len = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= n) return -EINVAL;
        uint8_t c = p[(*pos)++];
        *len = (*len << 7) | (c & 0x7f);
        if (!(c & 0x80)) return 0;
    }
    return 0;
}

static int parse_esds(const uint8_t *p, size_t n, Mp4LiteInfo *info) {
    size_t pos = 4; /* version + flags */
    if (pos >= n || p[pos++] != 0x03) return -EINVAL; /* ES_Descriptor */
    uint32_t len;
    if (desc_len(p, n, &pos, &len) < 0 || pos + 3 > n) return -EINVAL;
    pos += 2; /* ES_ID */
    uint8_t flags = p[pos++];
    if (flags & 0x80) pos += 2;
    if (flags & 0x40) {
        if (pos >= n) return -EINVAL;
        pos += 1u + p[pos];
    }
    if (flags & 0x20) pos += 2;
    if (pos >= n || p[pos++] != 0x04) return -EINVAL; /* DecoderConfigDescriptor */
    if (desc_len(p, n, &pos, &len) < 0 || pos + 13 > n) return -EINVAL;
    uint8_t oti = p[pos];
    /* 0x40 MPEG-4 audio (AAC LC/HE), 0x66-0x68 MPEG-2 AAC profiles. Not MP3-in-MP4 etc. */
    if (oti != 0x40 && oti != 0x66 && oti != 0x67 && oti != 0x68) return -ENOTSUP;
    pos += 13;
    if (pos >= n || p[pos++] != 0x05) return -EINVAL; /* DecoderSpecificInfo */
    if (desc_len(p, n, &pos, &len) < 0) return -EINVAL;
    if (len < 2 || len > sizeof(info->asc) || pos + len > n) return -EINVAL;
    memcpy(info->asc, p + pos, len);
    info->asc_len = len;
    return 0;
}

static int parse_stsd(Mp4Lite *m, const Box *stsd, Mp4LiteInfo *info) {
    uint8_t *p = read_payload(m, stsd->payload, stsd->end - stsd->payload, 64 * 1024);
    if (!p) return -EINVAL;
    size_t n = (size_t)(stsd->end - stsd->payload);
    int rc = -ENOTSUP;
    /* fullbox(4) entry_count(4) then the first sample entry. */
    if (n < 8 + 36 || be32(p + 4) < 1) goto out;
    const uint8_t *e = p + 8;
    size_t elen = be32(e);
    if (elen < 36 || elen > n - 8 || be32(e + 4) != FOURCC('m', 'p', '4', 'a')) goto out;
    uint16_t version = be16(e + 16);
    info->channels = be16(e + 24);
    info->sample_rate = (int)(be32(e + 32) >> 16);
    size_t child = 36;
    if (version == 1) child += 16;
    else if (version != 0) goto out; /* v2 (QuickTime LPCM-style) is not AAC from libro */
    /* Children: esds directly, or inside a QuickTime `wave` box. */
    for (int depth = 0; depth < 2; depth++) {
        size_t limit = elen;
        bool descended = false;
        while (child + 8 <= limit) {
            size_t clen = be32(e + child);
            uint32_t ctype = be32(e + child + 4);
            if (clen < 8 || clen > limit - child) goto out;
            if (ctype == FOURCC('e', 's', 'd', 's')) {
                rc = parse_esds(e + child + 8, clen - 8, info);
                goto out;
            }
            if (ctype == FOURCC('w', 'a', 'v', 'e')) {
                elen = child + clen;
                child += 8;
                descended = true;
                break;
            }
            child += clen;
        }
        if (!descended) break;
    }
out:
    free(p);
    return rc;
}

/* ---------- sample tables ---------- */

static int load_stts(Mp4Lite *m, const Box *b) {
    uint8_t h[8];
    if (b->end - b->payload < 8 || read_at(m, b->payload, h, 8) < 0) return -EINVAL;
    uint32_t n = be32(h + 4);
    if (n == 0 || n > MAX_RUN_ENTRIES || (uint64_t)n * 8 > b->end - b->payload - 8) return -EINVAL;
    uint8_t *raw = read_payload(m, b->payload + 8, (uint64_t)n * 8, (uint64_t)MAX_RUN_ENTRIES * 8);
    m->stts = malloc((size_t)n * sizeof(SttsEntry));
    if (!raw || !m->stts) {
        free(raw);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < n; i++) {
        m->stts[i].count = be32(raw + i * 8);
        m->stts[i].delta = be32(raw + i * 8 + 4);
    }
    free(raw);
    m->stts_n = n;
    return 0;
}

static int load_stsc(Mp4Lite *m, const Box *b) {
    uint8_t h[8];
    if (b->end - b->payload < 8 || read_at(m, b->payload, h, 8) < 0) return -EINVAL;
    uint32_t n = be32(h + 4);
    if (n == 0 || n > MAX_RUN_ENTRIES || (uint64_t)n * 12 > b->end - b->payload - 8) return -EINVAL;
    uint8_t *raw = read_payload(m, b->payload + 8, (uint64_t)n * 12, (uint64_t)MAX_RUN_ENTRIES * 12);
    m->stsc = malloc((size_t)n * sizeof(StscEntry));
    if (!raw || !m->stsc) {
        free(raw);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < n; i++) {
        m->stsc[i].first_chunk = be32(raw + i * 12);
        m->stsc[i].per_chunk = be32(raw + i * 12 + 4);
        /* Runs must start at chunk 1, strictly increase, and hold samples. */
        uint32_t want_min = i ? m->stsc[i - 1].first_chunk + 1 : 1;
        if (m->stsc[i].per_chunk == 0 || m->stsc[i].first_chunk < want_min ||
            (i == 0 && m->stsc[0].first_chunk != 1)) {
            free(raw);
            return -EINVAL;
        }
    }
    free(raw);
    m->stsc_n = n;
    return 0;
}

static int load_chunks(Mp4Lite *m, const Box *b, bool is64) {
    uint8_t h[8];
    if (b->end - b->payload < 8 || read_at(m, b->payload, h, 8) < 0) return -EINVAL;
    uint32_t n = be32(h + 4);
    unsigned w = is64 ? 8 : 4;
    if (n == 0 || n > MAX_CHUNKS || (uint64_t)n * w > b->end - b->payload - 8) return -EINVAL;
    uint8_t *raw = read_payload(m, b->payload + 8, (uint64_t)n * w, (uint64_t)MAX_CHUNKS * 8);
    m->chunk_off = malloc((size_t)n * sizeof(uint64_t));
    if (!raw || !m->chunk_off) {
        free(raw);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < n; i++) {
        m->chunk_off[i] = is64 ? be64(raw + (size_t)i * 8) : be32(raw + (size_t)i * 4);
    }
    free(raw);
    m->chunk_n = n;
    return 0;
}

static int load_stsz(Mp4Lite *m, const Box *b) {
    uint8_t h[12];
    if (b->end - b->payload < 12 || read_at(m, b->payload, h, 12) < 0) return -EINVAL;
    m->fixed_size = be32(h + 4);
    m->sample_n = be32(h + 8);
    m->stsz_off = b->payload + 12;
    if (m->sample_n == 0) return -EINVAL;
    if (m->fixed_size == 0 && (uint64_t)m->sample_n * 4 > b->end - m->stsz_off) return -EINVAL;
    if (m->fixed_size > MAX_SAMPLE_BYTES) return -EINVAL;
    return 0;
}

/* ---------- track discovery ---------- */

static int read_timescale(const Mp4Lite *m, const Box *b, uint32_t *ts, uint64_t *dur) {
    uint8_t p[32];
    uint64_t n = b->end - b->payload;
    if (n < 24 || read_at(m, b->payload, p, n < 32 ? (size_t)n : 32) < 0) return -EINVAL;
    if (p[0] == 1) {
        if (n < 32) return -EINVAL;
        *ts = be32(p + 20);
        if (dur) *dur = be64(p + 24);
    } else {
        *ts = be32(p + 12);
        if (dur) *dur = be32(p + 16);
    }
    return *ts ? 0 : -EINVAL;
}

/* Accept no edit list, or a single edit (the usual AAC priming offset). */
static int read_edit(const Mp4Lite *m, const Box *trak, int64_t *media_time, uint64_t *seg_dur) {
    *media_time = 0;
    *seg_dur = 0;
    Box edts, elst;
    if (find_child(m, trak->payload, trak->end, FOURCC('e', 'd', 't', 's'), &edts) < 0) return 0;
    if (find_child(m, edts.payload, edts.end, FOURCC('e', 'l', 's', 't'), &elst) < 0) return 0;
    uint8_t p[28];
    uint64_t n = elst.end - elst.payload;
    if (n < 8 || read_at(m, elst.payload, p, 8) < 0) return -EINVAL;
    uint32_t count = be32(p + 4);
    if (count == 0) return 0;
    if (count != 1) return -ENOTSUP;
    if (p[0] == 1) {
        if (n < 8 + 20 || read_at(m, elst.payload + 8, p, 20) < 0) return -EINVAL;
        *seg_dur = be64(p);
        *media_time = (int64_t)be64(p + 8);
    } else {
        if (n < 8 + 12 || read_at(m, elst.payload + 8, p, 12) < 0) return -EINVAL;
        *seg_dur = be32(p);
        *media_time = (int32_t)be32(p + 4);
    }
    return *media_time < 0 ? -ENOTSUP : 0; /* empty edits: leave to libavformat */
}

static int open_sound_track(Mp4Lite *m, const Box *moov, Mp4LiteInfo *info) {
    uint32_t movie_ts = 0;
    Box b;
    if (find_child(m, moov->payload, moov->end, FOURCC('m', 'v', 'h', 'd'), &b) < 0 ||
        read_timescale(m, &b, &movie_ts, NULL) < 0) {
        return -EINVAL;
    }
    if (find_child(m, moov->payload, moov->end, FOURCC('m', 'v', 'e', 'x'), &b) == 0) {
        return -ENOTSUP; /* fragmented: samples live in moof boxes */
    }

    uint64_t off = moov->payload;
    for (int guard = 0; off < moov->end && guard < 64; guard++) {
        Box trak;
        if (box_at(m, off, moov->end, &trak) < 0) return -EINVAL;
        off = trak.end;
        if (trak.type != FOURCC('t', 'r', 'a', 'k')) continue;

        Box mdia, hdlr, mdhd, minf, stbl;
        if (find_child(m, trak.payload, trak.end, FOURCC('m', 'd', 'i', 'a'), &mdia) < 0 ||
            find_child(m, mdia.payload, mdia.end, FOURCC('h', 'd', 'l', 'r'), &hdlr) < 0) {
            continue;
        }
        uint8_t hp[12];
        if (hdlr.end - hdlr.payload < 12 || read_at(m, hdlr.payload, hp, 12) < 0 ||
            be32(hp + 8) != FOURCC('s', 'o', 'u', 'n')) {
            continue;
        }
        /* First sound track is the book, as libavformat's default stream would be. */
        if (find_child(m, mdia.payload, mdia.end, FOURCC('m', 'd', 'h', 'd'), &mdhd) < 0 ||
            find_child(m, mdia.payload, mdia.end, FOURCC('m', 'i', 'n', 'f'), &minf) < 0 ||
            find_child(m, minf.payload, minf.end, FOURCC('s', 't', 'b', 'l'), &stbl) < 0) {
            return -EINVAL;
        }
        uint64_t media_dur = 0;
        if (read_timescale(m, &mdhd, &info->timescale, &media_dur) < 0) return -EINVAL;

        uint64_t seg_dur = 0;
        int rc = read_edit(m, &trak, &m->media_time, &seg_dur);
        if (rc < 0) return rc;

        Box stsd, stts, stsc, stsz, stco;
        if (find_child(m, stbl.payload, stbl.end, FOURCC('s', 't', 's', 'd'), &stsd) < 0 ||
            find_child(m, stbl.payload, stbl.end, FOURCC('s', 't', 't', 's'), &stts) < 0 ||
            find_child(m, stbl.payload, stbl.end, FOURCC('s', 't', 's', 'c'), &stsc) < 0 ||
            find_child(m, stbl.payload, stbl.end, FOURCC('s', 't', 's', 'z'), &stsz) < 0) {
            return -ENOTSUP; /* e.g. stz2 compact sizes */
        }
        bool is64 = false;
        if (find_child(m, stbl.payload, stbl.end, FOURCC('s', 't', 'c', 'o'), &stco) < 0) {
            if (find_child(m, stbl.payload, stbl.end, FOURCC('c', 'o', '6', '4'), &stco) < 0) {
                return -EINVAL;
            }
            is64 = true;
        }
        if ((rc = parse_stsd(m, &stsd, info)) < 0) return rc;
        if ((rc = load_stts(m, &stts)) < 0) return rc;
        if ((rc = load_stsc(m, &stsc)) < 0) return rc;
        if ((rc = load_stsz(m, &stsz)) < 0) return rc;
        if ((rc = load_chunks(m, &stco, is64)) < 0) return rc;
        if (m->stsc[m->stsc_n - 1].first_chunk > m->chunk_n) return -EINVAL;

        /* Timing table and size table must describe the same samples. */
        uint64_t stts_samples = 0, stts_total = 0;
        for (uint32_t i = 0; i < m->stts_n; i++) {
            stts_samples += m->stts[i].count;
            stts_total += (uint64_t)m->stts[i].count * m->stts[i].delta;
        }
        if (stts_samples < m->sample_n) return -EINVAL;
        info->sample_count = m->sample_n;

        if (seg_dur > 0) {
            info->duration = (int64_t)((seg_dur * info->timescale) / movie_ts);
        } else {
            info->duration = (int64_t)stts_total - m->media_time;
        }
        if (info->duration <= 0) info->duration = (int64_t)media_dur;
        return 0;
    }
    return -ENOENT;
}

/* ---------- cursor ---------- */

static int sample_size(Mp4Lite *m, uint32_t i, uint32_t *out) {
    if (i >= m->sample_n) return -EINVAL;
    if (m->fixed_size) {
        *out = m->fixed_size;
        return 0;
    }
    if (i < m->win_start || i >= m->win_start + m->win_len) {
        uint32_t len = m->sample_n - i;
        if (len > STSZ_WINDOW) len = STSZ_WINDOW;
        uint8_t raw[STSZ_WINDOW * 4];
        int rc = read_at(m, m->stsz_off + (uint64_t)i * 4, raw, (size_t)len * 4);
        if (rc < 0) return rc;
        for (uint32_t k = 0; k < len; k++) m->win[k] = be32(raw + k * 4);
        m->win_start = i;
        m->win_len = len;
    }
    *out = m->win[i - m->win_start];
    return *out <= MAX_SAMPLE_BYTES ? 0 : -EINVAL;
}

static uint32_t stsc_run_chunks(const Mp4Lite *m, uint32_t i) {
    uint32_t next = (i + 1 < m->stsc_n) ? m->stsc[i + 1].first_chunk : m->chunk_n + 1;
    return next - m->stsc[i].first_chunk;
}

/* Place the cursor on sample n (n == sample_n means end of track). */
static int set_cursor(Mp4Lite *m, uint32_t n) {
    if (n > m->sample_n) n = m->sample_n;
    m->cur = n;
    m->peek_size = 0;

    /* Timing: dts of sample n. */
    m->dts = 0;
    m->stts_i = 0;
    m->stts_used = 0;
    uint32_t left = n;
    while (m->stts_i < m->stts_n && left >= m->stts[m->stts_i].count) {
        left -= m->stts[m->stts_i].count;
        m->dts += (int64_t)m->stts[m->stts_i].count * m->stts[m->stts_i].delta;
        m->stts_i++;
    }
    m->stts_used = left;
    if (m->stts_i < m->stts_n) m->dts += (int64_t)left * m->stts[m->stts_i].delta;
    if (n == m->sample_n) return 0;

    /* Location: chunk holding sample n, then the sizes of its predecessors in that chunk. */
    uint64_t first = 0;
    for (uint32_t i = 0; i < m->stsc_n; i++) {
        uint64_t run = (uint64_t)stsc_run_chunks(m, i) * m->stsc[i].per_chunk;
        if (n < first + run) {
            uint32_t rel = (uint32_t)(n - first);
            m->stsc_i = i;
            m->chunk = m->stsc[i].first_chunk - 1 + rel / m->stsc[i].per_chunk;
            m->in_chunk = rel % m->stsc[i].per_chunk;
            m->off = m->chunk_off[m->chunk];
            if (m->fixed_size) {
                m->off += (uint64_t)m->in_chunk * m->fixed_size;
            } else {
                for (uint32_t k = n - m->in_chunk; k < n; k++) {
                    uint32_t sz;
                    int rc = sample_size(m, k, &sz);
                    if (rc < 0) return rc;
                    m->off += sz;
                }
            }
            return 0;
        }
        first += run;
    }
    /* Chunk table describes fewer samples than stsz: treat the rest as missing. */
    m->sample_n = n;
    return 0;
}

int mp4lite_peek(Mp4Lite *m, uint32_t *size, int64_t *pts) {
    if (!m) return -EINVAL;
    if (m->cur >= m->sample_n) return 0;
    uint32_t sz;
    int rc = sample_size(m, m->cur, &sz);
    if (rc < 0) return rc;
    m->peek_size = sz;
    if (size) *size = sz;
    if (pts) *pts = m->dts - m->media_time;
    return 1;
}

int mp4lite_read(Mp4Lite *m, uint8_t *dst, uint32_t size) {
    if (!m || !dst || m->cur >= m->sample_n || size != m->peek_size) return -EINVAL;
    int rc = read_at(m, m->off, dst, size);
    if (rc < 0) return rc;

    m->cur++;
    m->peek_size = 0;
    if (m->stts_i < m->stts_n) {
        m->dts += m->stts[m->stts_i].delta;
        if (++m->stts_used >= m->stts[m->stts_i].count) {
            m->stts_i++;
            m->stts_used = 0;
        }
    }
    m->off += size;
    if (++m->in_chunk >= m->stsc[m->stsc_i].per_chunk) {
        m->in_chunk = 0;
        m->chunk++;
        if (m->stsc_i + 1 < m->stsc_n && m->chunk + 1 >= m->stsc[m->stsc_i + 1].first_chunk) {
            m->stsc_i++;
        }
        if (m->chunk >= m->chunk_n) {
            m->sample_n = m->cur; /* tables end here, whatever stsz claims */
        } else {
            m->off = m->chunk_off[m->chunk];
        }
    }
    return 0;
}

int mp4lite_seek(Mp4Lite *m, int64_t target_pts, int64_t *out_pts) {
    if (!m) return -EINVAL;
    int64_t target = target_pts + m->media_time;
    if (target < 0) target = 0;
    /* Last sample whose dts <= target, via the run-length timing table. */
    uint64_t n = 0;
    int64_t dts = 0;
    for (uint32_t i = 0; i < m->stts_n; i++) {
        int64_t span = (int64_t)m->stts[i].count * m->stts[i].delta;
        if (m->stts[i].delta > 0 && target < dts + span) {
            n += (uint64_t)((target - dts) / m->stts[i].delta);
            break;
        }
        dts += span;
        n += m->stts[i].count;
    }
    if (n >= m->sample_n) n = m->sample_n ? m->sample_n - 1 : 0;
    int rc = set_cursor(m, (uint32_t)n);
    if (rc < 0) return rc;
    if (out_pts) *out_pts = m->dts - m->media_time;
    return 0;
}

size_t mp4lite_heap_bytes(const Mp4Lite *m) {
    if (!m) return 0;
    return sizeof(*m) + (size_t)m->stts_n * sizeof(SttsEntry) +
           (size_t)m->stsc_n * sizeof(StscEntry) + (size_t)m->chunk_n * sizeof(uint64_t);
}

void mp4lite_close(Mp4Lite *m) {
    if (!m) return;
    if (m->fd >= 0) close(m->fd);
    free(m->stts);
    free(m->stsc);
    free(m->chunk_off);
    free(m);
}

int mp4lite_open(Mp4Lite **out, const char *path, Mp4LiteInfo *info) {
    if (!out || !path || !info) return -EINVAL;
    *out = NULL;
    memset(info, 0, sizeof(*info));

    Mp4Lite *m = calloc(1, sizeof(*m));
    if (!m) return -ENOMEM;
    m->fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (m->fd < 0 || fstat(m->fd, &st) < 0 || st.st_size < 16) {
        mp4lite_close(m);
        return -ENOENT;
    }
    m->file_size = (uint64_t)st.st_size;

    /* Cheap rejection of non-MP4 files before walking anything. */
    Box b;
    int rc = box_at(m, 0, m->file_size, &b);
    if (rc < 0 || b.type != FOURCC('f', 't', 'y', 'p')) {
        mp4lite_close(m);
        return -ENOTSUP;
    }
    Box moov;
    rc = find_child(m, 0, m->file_size, FOURCC('m', 'o', 'o', 'v'), &moov);
    if (rc == 0) rc = open_sound_track(m, &moov, info);
    if (rc == 0) rc = set_cursor(m, 0);
    if (rc < 0) {
        mp4lite_close(m);
        return rc;
    }
    *out = m;
    return 0;
}
