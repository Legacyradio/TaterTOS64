/*
 * ts_xml.h — TaterSurf minimal XML parser
 *
 * Header-only. Parses XML into a simple element tree for DASH MPD
 * manifest processing. Not a full XML parser — no DTD validation,
 * no namespaces, no entity handling beyond basic &amp;&lt;&gt;&quot;.
 *
 * Uses a fixed-size node pool. Designed for manifests up to ~64KB.
 */

#ifndef TS_XML_H
#define TS_XML_H

#include <stddef.h>
#include <stdint.h>

#define TS_XML_MAX_NODES     512
#define TS_XML_MAX_ATTRS      16
#define TS_XML_ATTR_NAME_MAX  64
#define TS_XML_ATTR_VALUE_MAX 256
#define TS_XML_TAG_MAX        64
#define TS_XML_TEXT_MAX      1024

struct ts_xml_attr {
    char name[TS_XML_ATTR_NAME_MAX];
    char value[TS_XML_ATTR_VALUE_MAX];
};

struct ts_xml_node {
    int used;
    int type;                          /* 1=element, 3=text */
    char tag[TS_XML_TAG_MAX];          /* element tag name */
    char text[TS_XML_TEXT_MAX];        /* text content (for text nodes) */
    struct ts_xml_attr attrs[TS_XML_MAX_ATTRS];
    int attr_count;
    int parent;                        /* parent node index, -1 for root */
    int first_child;
    int last_child;
    int next_sibling;
};

struct ts_xml_doc {
    struct ts_xml_node nodes[TS_XML_MAX_NODES];
    int node_count;
    int root;                          /* root element index */
};

/* ---- Helpers ---- */

static int ts_xml__is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static struct ts_xml_node *ts_xml_alloc(struct ts_xml_doc *doc) {
    if (doc->node_count >= TS_XML_MAX_NODES) return NULL;
    struct ts_xml_node *n = &doc->nodes[doc->node_count];
    memset(n, 0, sizeof(*n));
    n->used = 1;
    n->parent = -1;
    n->first_child = -1;
    n->last_child = -1;
    n->next_sibling = -1;
    int id = doc->node_count++;
    (void)id;
    return n;
}

static int ts_xml_id(struct ts_xml_doc *doc, struct ts_xml_node *n) {
    return (int)(n - doc->nodes);
}

static void ts_xml_append(struct ts_xml_doc *doc, int parent, int child) {
    struct ts_xml_node *p = &doc->nodes[parent];
    struct ts_xml_node *c = &doc->nodes[child];
    c->parent = parent;
    c->next_sibling = -1;
    if (p->last_child >= 0) {
        doc->nodes[p->last_child].next_sibling = child;
    }
    p->last_child = child;
    if (p->first_child < 0)
        p->first_child = child;
}

/* ---- Parser ---- */

static void ts_xml_parse(struct ts_xml_doc *doc, const char *xml, size_t len) {
    const char *p = xml;
    const char *end = xml + len;
    int current = -1; /* current parent element */

    memset(doc, 0, sizeof(*doc));
    doc->root = -1;

    /* Create document root */
    {
        struct ts_xml_node *root = ts_xml_alloc(doc);
        if (!root) return;
        root->type = 1;
        strcpy(root->tag, "#document");
        doc->root = ts_xml_id(doc, root);
        current = doc->root;
    }

    while (p < end) {
        /* Skip whitespace */
        while (p < end && ts_xml__is_space(*p)) p++;
        if (p >= end) break;

        if (*p == '<') {
            p++;
            if (p >= end) break;

            /* XML declaration <?xml ...?> */
            if (*p == '?') {
                while (p + 1 < end && !(p[0] == '?' && p[1] == '>')) p++;
                if (p + 1 < end) p += 2;
                continue;
            }

            /* Comment <!-- --> */
            if (p + 2 < end && p[0] == '!' && p[1] == '-' && p[2] == '-') {
                p += 3;
                while (p + 2 < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) p++;
                if (p + 2 < end) p += 3;
                continue;
            }

            /* CDATA <![CDATA[...]]> */
            if (p + 7 < end && p[0] == '!' && p[1] == '[' && p[2] == 'C') {
                p += 9; /* skip <![CDATA[ */
                const char *cdata_start = p;
                while (p + 2 < end && !(p[0] == ']' && p[1] == ']' && p[2] == '>')) p++;
                /* Store as text node */
                if (current >= 0 && p > cdata_start) {
                    struct ts_xml_node *text = ts_xml_alloc(doc);
                    if (text) {
                        text->type = 3;
                        size_t tlen = (size_t)(p - cdata_start);
                        if (tlen >= TS_XML_TEXT_MAX) tlen = TS_XML_TEXT_MAX - 1;
                        memcpy(text->text, cdata_start, tlen);
                        text->text[tlen] = '\0';
                        ts_xml_append(doc, current, ts_xml_id(doc, text));
                    }
                }
                if (p + 2 < end) p += 3;
                continue;
            }

            /* DOCTYPE <!DOCTYPE ...> */
            if (*p == '!') {
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            /* Closing tag </tag> */
            if (*p == '/') {
                p++;
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                /* Pop to parent */
                if (current >= 0 && doc->nodes[current].parent >= 0)
                    current = doc->nodes[current].parent;
                continue;
            }

            /* Opening tag <tag attrs...> or <tag .../> */
            {
                struct ts_xml_node *elem = ts_xml_alloc(doc);
                if (!elem) { p = end; break; }
                elem->type = 1;

                /* Tag name */
                {
                    const char *tag_start = p;
                    while (p < end && !ts_xml__is_space(*p) && *p != '>' && *p != '/') p++;
                    size_t tl = (size_t)(p - tag_start);
                    if (tl >= TS_XML_TAG_MAX) tl = TS_XML_TAG_MAX - 1;
                    memcpy(elem->tag, tag_start, tl);
                    elem->tag[tl] = '\0';
                }

                /* Attributes */
                while (p < end && *p != '>' && *p != '/') {
                    while (p < end && ts_xml__is_space(*p)) p++;
                    if (p >= end || *p == '>' || *p == '/') break;

                    /* Attribute name */
                    const char *aname = p;
                    while (p < end && *p != '=' && !ts_xml__is_space(*p) && *p != '>' && *p != '/') p++;
                    size_t anl = (size_t)(p - aname);

                    /* Skip = and whitespace */
                    while (p < end && (ts_xml__is_space(*p) || *p == '=')) p++;

                    /* Attribute value */
                    char aval[TS_XML_ATTR_VALUE_MAX];
                    aval[0] = '\0';
                    if (p < end && (*p == '"' || *p == '\'')) {
                        char quote = *p++;
                        const char *vstart = p;
                        while (p < end && *p != quote) p++;
                        size_t vl = (size_t)(p - vstart);
                        if (vl >= TS_XML_ATTR_VALUE_MAX) vl = TS_XML_ATTR_VALUE_MAX - 1;
                        memcpy(aval, vstart, vl);
                        aval[vl] = '\0';
                        if (p < end) p++; /* skip closing quote */
                    }

                    /* Store attribute */
                    if (elem->attr_count < TS_XML_MAX_ATTRS && anl > 0) {
                        struct ts_xml_attr *a = &elem->attrs[elem->attr_count++];
                        if (anl >= TS_XML_ATTR_NAME_MAX) anl = TS_XML_ATTR_NAME_MAX - 1;
                        memcpy(a->name, aname, anl);
                        a->name[anl] = '\0';
                        strcpy(a->value, aval);
                    }
                }

                /* Self-closing? */
                int self_close = 0;
                if (p < end && *p == '/') { self_close = 1; p++; }
                if (p < end && *p == '>') p++;

                /* Add to tree */
                if (current >= 0) {
                    ts_xml_append(doc, current, ts_xml_id(doc, elem));
                }

                if (!self_close) {
                    current = ts_xml_id(doc, elem);
                }
            }
        } else {
            /* Text content */
            const char *text_start = p;
            while (p < end && *p != '<') p++;
            if (p > text_start && current >= 0) {
                /* Skip whitespace-only text */
                const char *t = text_start;
                int only_ws = 1;
                while (t < p) { if (!ts_xml__is_space(*t)) { only_ws = 0; break; } t++; }
                if (!only_ws) {
                    struct ts_xml_node *text = ts_xml_alloc(doc);
                    if (text) {
                        text->type = 3;
                        size_t tlen = (size_t)(p - text_start);
                        if (tlen >= TS_XML_TEXT_MAX) tlen = TS_XML_TEXT_MAX - 1;
                        memcpy(text->text, text_start, tlen);
                        text->text[tlen] = '\0';
                        ts_xml_append(doc, current, ts_xml_id(doc, text));
                    }
                }
            }
        }
    }
}

/* ---- Query helpers ---- */

static const char *ts_xml_get_attr(struct ts_xml_node *n, const char *name) {
    int i;
    for (i = 0; i < n->attr_count; i++) {
        if (strcmp(n->attrs[i].name, name) == 0)
            return n->attrs[i].value;
    }
    return NULL;
}

/* Find first child element with given tag */
static int ts_xml_find_child(struct ts_xml_doc *doc, int parent, const char *tag) {
    if (parent < 0) return -1;
    int child = doc->nodes[parent].first_child;
    while (child >= 0) {
        struct ts_xml_node *c = &doc->nodes[child];
        if (c->type == 1 && strcmp(c->tag, tag) == 0) return child;
        child = c->next_sibling;
    }
    return -1;
}

/* Find all child elements with given tag */
static int ts_xml_find_children(struct ts_xml_doc *doc, int parent,
                                 const char *tag, int *results, int max) {
    int count = 0;
    if (parent < 0) return 0;
    int child = doc->nodes[parent].first_child;
    while (child >= 0 && count < max) {
        struct ts_xml_node *c = &doc->nodes[child];
        if (c->type == 1 && strcmp(c->tag, tag) == 0)
            results[count++] = child;
        child = c->next_sibling;
    }
    return count;
}

/* Get text content of first text child */
static const char *ts_xml_get_text(struct ts_xml_doc *doc, int node) {
    if (node < 0) return "";
    int child = doc->nodes[node].first_child;
    while (child >= 0) {
        if (doc->nodes[child].type == 3)
            return doc->nodes[child].text;
        child = doc->nodes[child].next_sibling;
    }
    return "";
}

#endif /* TS_XML_H */
