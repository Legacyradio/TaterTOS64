/*
 * ts_dash.h — TaterSurf DASH (Dynamic Adaptive Streaming over HTTP) parser
 *
 * Header-only. Parses MPD (Media Presentation Description) manifests
 * to extract video/audio stream URLs and quality levels.
 *
 * YouTube uses DASH with fragmented MP4 (fMP4) segments.
 *
 * Depends on: ts_xml.h
 */

#ifndef TS_DASH_H
#define TS_DASH_H

#include "ts_xml.h"
#include <stdint.h>

/* ================================================================== */
/* Data structures                                                     */
/* ================================================================== */

#define TS_DASH_MAX_REPS     16   /* max representations per adaptation set */
#define TS_DASH_MAX_ADAPT     8   /* max adaptation sets */
#define TS_DASH_URL_MAX     512

struct ts_dash_representation {
    char id[32];
    uint32_t bandwidth;        /* bits per second */
    uint16_t width, height;    /* video resolution (0 for audio) */
    char codecs[64];           /* e.g. "avc1.640028", "mp4a.40.2", "opus" */
    char mime_type[64];        /* e.g. "video/mp4", "audio/mp4", "audio/webm" */
    /* Segment template */
    char init_url[TS_DASH_URL_MAX];    /* initialization segment URL */
    char media_url[TS_DASH_URL_MAX];   /* media segment URL template ($Number$) */
    uint32_t start_number;
    uint32_t segment_duration;  /* in timescale units */
    uint32_t timescale;         /* ticks per second */
};

struct ts_dash_adaptation_set {
    char mime_type[64];
    char codecs[64];
    int is_video;
    int is_audio;
    struct ts_dash_representation reps[TS_DASH_MAX_REPS];
    int rep_count;
};

struct ts_dash_manifest {
    char base_url[TS_DASH_URL_MAX];    /* base URL from MPD or manifest URL */
    uint32_t duration_ms;               /* total duration in milliseconds */
    struct ts_dash_adaptation_set adapt[TS_DASH_MAX_ADAPT];
    int adapt_count;
};

/* ================================================================== */
/* Parser                                                              */
/* ================================================================== */

static uint32_t ts_dash__parse_duration(const char *str) {
    /* Parse ISO 8601 duration: PT5M30S, PT3600S, etc. */
    uint32_t total = 0;
    const char *p = str;
    if (*p == 'P') p++;
    if (*p == 'T') p++;
    while (*p) {
        uint32_t val = 0;
        while (*p >= '0' && *p <= '9') { val = val * 10 + (uint32_t)(*p - '0'); p++; }
        if (*p == 'H') { total += val * 3600000; p++; }
        else if (*p == 'M') { total += val * 60000; p++; }
        else if (*p == 'S') { total += val * 1000; p++; }
        else if (*p == '.') {
            p++; /* skip fractional seconds */
            while (*p >= '0' && *p <= '9') p++;
            if (*p == 'S') p++;
            total += val * 1000;
        }
        else break;
    }
    return total;
}

static uint32_t ts_dash__atou(const char *s) {
    uint32_t v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    return v;
}

static void ts_dash__strlcpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    if (!src) { dst[0] = '\0'; return; }
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/*
 * ts_dash_parse — parse an MPD manifest.
 */
static int ts_dash_parse(struct ts_dash_manifest *manifest,
                           const char *mpd_xml, size_t mpd_len,
                           const char *base_url) {
    struct ts_xml_doc *doc;
    int mpd_node;

    memset(manifest, 0, sizeof(*manifest));
    ts_dash__strlcpy(manifest->base_url, base_url, TS_DASH_URL_MAX);

    doc = (struct ts_xml_doc *)malloc(sizeof(struct ts_xml_doc));
    if (!doc) return -1;

    ts_xml_parse(doc, mpd_xml, mpd_len);

    /* Find <MPD> element */
    mpd_node = ts_xml_find_child(doc, doc->root, "MPD");
    if (mpd_node < 0) { free(doc); return -1; }

    /* Duration */
    {
        const char *dur = ts_xml_get_attr(&doc->nodes[mpd_node], "mediaPresentationDuration");
        if (dur) manifest->duration_ms = ts_dash__parse_duration(dur);
    }

    /* Find <Period> */
    int period = ts_xml_find_child(doc, mpd_node, "Period");
    if (period < 0) { free(doc); return -1; }

    /* BaseURL (if present in Period or MPD) */
    {
        int bu = ts_xml_find_child(doc, mpd_node, "BaseURL");
        if (bu >= 0) {
            const char *bt = ts_xml_get_text(doc, bu);
            if (bt[0]) ts_dash__strlcpy(manifest->base_url, bt, TS_DASH_URL_MAX);
        }
        bu = ts_xml_find_child(doc, period, "BaseURL");
        if (bu >= 0) {
            const char *bt = ts_xml_get_text(doc, bu);
            if (bt[0]) ts_dash__strlcpy(manifest->base_url, bt, TS_DASH_URL_MAX);
        }
    }

    /* Parse AdaptationSets */
    {
        int adapt_ids[TS_DASH_MAX_ADAPT];
        int adapt_count = ts_xml_find_children(doc, period, "AdaptationSet",
                                                adapt_ids, TS_DASH_MAX_ADAPT);
        int ai;
        for (ai = 0; ai < adapt_count && manifest->adapt_count < TS_DASH_MAX_ADAPT; ai++) {
            struct ts_dash_adaptation_set *as = &manifest->adapt[manifest->adapt_count];
            int as_node = adapt_ids[ai];
            memset(as, 0, sizeof(*as));

            /* AdaptationSet attributes */
            {
                const char *mt = ts_xml_get_attr(&doc->nodes[as_node], "mimeType");
                if (mt) ts_dash__strlcpy(as->mime_type, mt, sizeof(as->mime_type));
                const char *co = ts_xml_get_attr(&doc->nodes[as_node], "codecs");
                if (co) ts_dash__strlcpy(as->codecs, co, sizeof(as->codecs));
            }

            /* Determine if video or audio */
            if (strstr(as->mime_type, "video")) as->is_video = 1;
            else if (strstr(as->mime_type, "audio")) as->is_audio = 1;

            /* SegmentTemplate at AdaptationSet level (shared by all Representations) */
            char shared_init[TS_DASH_URL_MAX] = "";
            char shared_media[TS_DASH_URL_MAX] = "";
            uint32_t shared_start = 1, shared_duration = 0, shared_timescale = 1;
            {
                int st = ts_xml_find_child(doc, as_node, "SegmentTemplate");
                if (st >= 0) {
                    const char *v;
                    v = ts_xml_get_attr(&doc->nodes[st], "initialization");
                    if (v) ts_dash__strlcpy(shared_init, v, TS_DASH_URL_MAX);
                    v = ts_xml_get_attr(&doc->nodes[st], "media");
                    if (v) ts_dash__strlcpy(shared_media, v, TS_DASH_URL_MAX);
                    v = ts_xml_get_attr(&doc->nodes[st], "startNumber");
                    if (v) shared_start = ts_dash__atou(v);
                    v = ts_xml_get_attr(&doc->nodes[st], "duration");
                    if (v) shared_duration = ts_dash__atou(v);
                    v = ts_xml_get_attr(&doc->nodes[st], "timescale");
                    if (v) shared_timescale = ts_dash__atou(v);
                }
            }

            /* Parse Representations */
            {
                int rep_ids[TS_DASH_MAX_REPS];
                int rep_count = ts_xml_find_children(doc, as_node, "Representation",
                                                      rep_ids, TS_DASH_MAX_REPS);
                int ri;
                for (ri = 0; ri < rep_count && as->rep_count < TS_DASH_MAX_REPS; ri++) {
                    struct ts_dash_representation *rep = &as->reps[as->rep_count];
                    int rn = rep_ids[ri];
                    memset(rep, 0, sizeof(*rep));

                    const char *v;
                    v = ts_xml_get_attr(&doc->nodes[rn], "id");
                    if (v) ts_dash__strlcpy(rep->id, v, sizeof(rep->id));
                    v = ts_xml_get_attr(&doc->nodes[rn], "bandwidth");
                    if (v) rep->bandwidth = ts_dash__atou(v);
                    v = ts_xml_get_attr(&doc->nodes[rn], "width");
                    if (v) rep->width = (uint16_t)ts_dash__atou(v);
                    v = ts_xml_get_attr(&doc->nodes[rn], "height");
                    if (v) rep->height = (uint16_t)ts_dash__atou(v);
                    v = ts_xml_get_attr(&doc->nodes[rn], "codecs");
                    if (v) ts_dash__strlcpy(rep->codecs, v, sizeof(rep->codecs));
                    else ts_dash__strlcpy(rep->codecs, as->codecs, sizeof(rep->codecs));
                    v = ts_xml_get_attr(&doc->nodes[rn], "mimeType");
                    if (v) ts_dash__strlcpy(rep->mime_type, v, sizeof(rep->mime_type));
                    else ts_dash__strlcpy(rep->mime_type, as->mime_type, sizeof(rep->mime_type));

                    /* SegmentTemplate (per-Representation or inherit from AdaptationSet) */
                    {
                        int st = ts_xml_find_child(doc, rn, "SegmentTemplate");
                        if (st >= 0) {
                            v = ts_xml_get_attr(&doc->nodes[st], "initialization");
                            if (v) ts_dash__strlcpy(rep->init_url, v, TS_DASH_URL_MAX);
                            v = ts_xml_get_attr(&doc->nodes[st], "media");
                            if (v) ts_dash__strlcpy(rep->media_url, v, TS_DASH_URL_MAX);
                            v = ts_xml_get_attr(&doc->nodes[st], "startNumber");
                            rep->start_number = v ? ts_dash__atou(v) : 1;
                            v = ts_xml_get_attr(&doc->nodes[st], "duration");
                            rep->segment_duration = v ? ts_dash__atou(v) : 0;
                            v = ts_xml_get_attr(&doc->nodes[st], "timescale");
                            rep->timescale = v ? ts_dash__atou(v) : 1;
                        } else {
                            /* Inherit from AdaptationSet SegmentTemplate */
                            ts_dash__strlcpy(rep->init_url, shared_init, TS_DASH_URL_MAX);
                            ts_dash__strlcpy(rep->media_url, shared_media, TS_DASH_URL_MAX);
                            rep->start_number = shared_start;
                            rep->segment_duration = shared_duration;
                            rep->timescale = shared_timescale;
                        }
                    }

                    as->rep_count++;
                }
            }

            manifest->adapt_count++;
        }
    }

    free(doc);
    return 0;
}

/* ================================================================== */
/* Quality selection                                                   */
/* ================================================================== */

/*
 * ts_dash_select_video — pick the best video representation that
 * fits within the given bandwidth budget (bits per second).
 * Returns pointer to representation or NULL.
 */
static struct ts_dash_representation *ts_dash_select_video(
        struct ts_dash_manifest *m, uint32_t max_bandwidth) {
    struct ts_dash_representation *best = NULL;
    int ai, ri;
    for (ai = 0; ai < m->adapt_count; ai++) {
        if (!m->adapt[ai].is_video) continue;
        for (ri = 0; ri < m->adapt[ai].rep_count; ri++) {
            struct ts_dash_representation *r = &m->adapt[ai].reps[ri];
            if (r->bandwidth <= max_bandwidth) {
                if (!best || r->bandwidth > best->bandwidth)
                    best = r;
            }
        }
    }
    /* If nothing fits, pick the lowest bandwidth */
    if (!best) {
        for (ai = 0; ai < m->adapt_count; ai++) {
            if (!m->adapt[ai].is_video) continue;
            for (ri = 0; ri < m->adapt[ai].rep_count; ri++) {
                struct ts_dash_representation *r = &m->adapt[ai].reps[ri];
                if (!best || r->bandwidth < best->bandwidth)
                    best = r;
            }
        }
    }
    return best;
}

/*
 * ts_dash_select_audio — pick the best audio representation.
 */
static struct ts_dash_representation *ts_dash_select_audio(
        struct ts_dash_manifest *m) {
    struct ts_dash_representation *best = NULL;
    int ai, ri;
    for (ai = 0; ai < m->adapt_count; ai++) {
        if (!m->adapt[ai].is_audio) continue;
        for (ri = 0; ri < m->adapt[ai].rep_count; ri++) {
            struct ts_dash_representation *r = &m->adapt[ai].reps[ri];
            if (!best || r->bandwidth > best->bandwidth)
                best = r;
        }
    }
    return best;
}

/*
 * ts_dash_segment_url — build the URL for a specific segment number.
 * Replaces $Number$ and $RepresentationID$ in the template.
 */
static void ts_dash_segment_url(const struct ts_dash_representation *rep,
                                  const char *base_url,
                                  uint32_t segment_number,
                                  char *url_out, size_t url_max) {
    const char *tmpl = rep->media_url;
    size_t pos = 0;
    const char *p = tmpl;

    while (*p && pos < url_max - 1) {
        if (*p == '$') {
            if (strncmp(p, "$Number$", 8) == 0) {
                char num[16];
                int ni = 0;
                uint32_t n = segment_number;
                char tmp[16]; int ti = 0;
                if (n == 0) tmp[ti++] = '0';
                else while (n > 0) { tmp[ti++] = '0' + (char)(n % 10); n /= 10; }
                while (ti > 0) num[ni++] = tmp[--ti];
                num[ni] = '\0';
                size_t nl = (size_t)ni;
                if (pos + nl < url_max) {
                    memcpy(url_out + pos, num, nl);
                    pos += nl;
                }
                p += 8;
            } else if (strncmp(p, "$RepresentationID$", 18) == 0) {
                size_t rl = strlen(rep->id);
                if (pos + rl < url_max) {
                    memcpy(url_out + pos, rep->id, rl);
                    pos += rl;
                }
                p += 18;
            } else if (strncmp(p, "$Bandwidth$", 11) == 0) {
                char bw[16];
                int bi = 0;
                uint32_t b = rep->bandwidth;
                char tmp[16]; int ti = 0;
                if (b == 0) tmp[ti++] = '0';
                else while (b > 0) { tmp[ti++] = '0' + (char)(b % 10); b /= 10; }
                while (ti > 0) bw[bi++] = tmp[--ti];
                bw[bi] = '\0';
                if (pos + (size_t)bi < url_max) {
                    memcpy(url_out + pos, bw, (size_t)bi);
                    pos += (size_t)bi;
                }
                p += 11;
            } else {
                url_out[pos++] = *p++;
            }
        } else {
            url_out[pos++] = *p++;
        }
    }
    url_out[pos] = '\0';
    (void)base_url; /* base_url prepended by caller if media_url is relative */
}

#endif /* TS_DASH_H */
