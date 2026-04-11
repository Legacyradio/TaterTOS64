/*
 * ts_dom.h — TaterSurf DOM bridge for QuickJS
 *
 * Header-only. Provides a minimal DOM implementation that JavaScript
 * can interact with via QuickJS C bindings.
 *
 * Implements:
 *   - Node tree (Element, Text, Comment, DocumentFragment)
 *   - Element: attributes, classList, style, innerHTML, textContent
 *   - Document: createElement, getElementById, querySelector, etc.
 *   - Window: location, setTimeout, requestAnimationFrame
 *   - Event system: addEventListener, dispatchEvent, bubbling/capturing
 *   - Console: log, warn, error
 *
 * Architecture:
 *   DOM nodes are allocated from a fixed pool (TS_DOM_MAX_NODES).
 *   Each node has a QuickJS JSValue wrapper. The C node owns the data;
 *   the JS wrapper provides the API surface.
 *
 *   The DOM bridge connects to the existing ts_layout.h document:
 *   - ts_doc_build produces render nodes from HTML
 *   - The DOM bridge creates a parallel node tree that JS can query
 *   - Changes via innerHTML/textContent trigger re-parse + re-layout
 */

#ifndef TS_DOM_H
#define TS_DOM_H

/* Include libc FIRST so its FILE definition takes precedence over shims */
#include "../libc/libc.h"
#include "quickjs/quickjs.h"
#include "ts_html.h"
#include "ts_css.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define TS_DOM_MAX_NODES       4096
#define TS_DOM_MAX_ATTRS         16
#define TS_DOM_MAX_LISTENERS     16
#define TS_DOM_MAX_TIMERS        64
#define TS_DOM_MAX_CLASSES        8
#define TS_DOM_ATTR_NAME_MAX     64
#define TS_DOM_ATTR_VALUE_MAX   256
#define TS_DOM_TEXT_MAX       65536
#define TS_DOM_TAG_MAX           32

/* Node types (DOM Level 1) */
#define TS_DOM_ELEMENT_NODE      1
#define TS_DOM_TEXT_NODE          3
#define TS_DOM_COMMENT_NODE      8
#define TS_DOM_DOCUMENT_NODE     9
#define TS_DOM_FRAGMENT_NODE    11

/* ================================================================== */
/* DOM Node                                                            */
/* ================================================================== */

struct ts_dom_attr {
    char name[TS_DOM_ATTR_NAME_MAX];
    char value[TS_DOM_ATTR_VALUE_MAX];
};

struct ts_dom_node {
    int type;                              /* TS_DOM_*_NODE */
    int id;                                /* pool index */
    int used;                              /* 1 if allocated */

    /* Tree links (pool indices, -1 = none) */
    int parent;
    int first_child;
    int last_child;
    int next_sibling;
    int prev_sibling;

    /* Element data (type == ELEMENT_NODE) */
    char tag[TS_DOM_TAG_MAX];              /* lowercase tag name */
    struct ts_dom_attr attrs[TS_DOM_MAX_ATTRS];
    int attr_count;

    /* Text data (type == TEXT_NODE or COMMENT_NODE) */
    char *text;
    int text_len;
    int text_cap;   /* allocated capacity of text buffer */

    /* JS wrapper (prevents GC while node is alive) */
    JSValue js_obj;                        /* JS_UNDEFINED if not wrapped */

    /* Event listeners */
    struct {
        char type[32];                     /* event type: "click", "keydown", etc. */
        JSValue callback;                  /* JS function */
        int capture;                       /* 1 = capture phase */
    } listeners[TS_DOM_MAX_LISTENERS];
    int listener_count;

    /* Style (for Element nodes — inline style="" properties) */
    struct ts_css_property inline_style[16];
    int inline_style_count;
};

/* ================================================================== */
/* DOM Timer                                                           */
/* ================================================================== */

struct ts_dom_timer {
    int used;
    int id;
    JSValue callback;
    uint64_t fire_at_ms;                   /* absolute time to fire */
    uint64_t interval_ms;                  /* 0 = setTimeout, >0 = setInterval */
};

/* ================================================================== */
/* DOM Context (one per page)                                          */
/* ================================================================== */

struct ts_dom_ctx {
    /* Node pool */
    struct ts_dom_node nodes[TS_DOM_MAX_NODES];
    int node_count;                        /* next allocation index hint */

    /* Special nodes */
    int document_node;                     /* pool index of #document */
    int html_node;                         /* <html> */
    int head_node;                         /* <head> */
    int body_node;                         /* <body> */

    /* Timers */
    struct ts_dom_timer timers[TS_DOM_MAX_TIMERS];
    int next_timer_id;

    /* QuickJS runtime */
    JSRuntime *rt;
    JSContext *ctx;

    /* JS global objects */
    JSValue js_document;
    JSValue js_window;
    JSValue js_console;

    /* Page URL (for location object) */
    char url[2048];

    /* Dirty flag — set when DOM is modified, triggers re-render */
    int dirty;

    /* Console log buffer (for F12 debug panel) */
    char console_buf[4096];
    int console_len;

    /* Lifecycle hooks (set by ts_webcomp.h for Custom Elements) */
    void (*on_connected)(struct ts_dom_ctx *dom, int node_id);
    void (*on_disconnected)(struct ts_dom_ctx *dom, int node_id);
    /* Element enhance hook (set by ts_webcomp.h — adds attachShadow etc.) */
    void (*on_element_created)(JSContext *ctx, JSValue elem_obj, const char *tag);
};

/* ================================================================== */
/* Node pool management                                                */
/* ================================================================== */

static struct ts_dom_node *ts_dom_alloc_node(struct ts_dom_ctx *dom, int type) {
    int i;
    for (i = 0; i < TS_DOM_MAX_NODES; i++) {
        if (!dom->nodes[i].used) {
            struct ts_dom_node *n = &dom->nodes[i];
            memset(n, 0, sizeof(*n));
            n->type = type;
            n->id = i;
            n->used = 1;
            n->parent = -1;
            n->first_child = -1;
            n->last_child = -1;
            n->next_sibling = -1;
            n->prev_sibling = -1;
            n->js_obj = JS_UNDEFINED;
            n->listener_count = 0;
            n->attr_count = 0;
            n->inline_style_count = 0;
            return n;
        }
    }
    return NULL; /* pool exhausted */
}

static struct ts_dom_node *ts_dom_get_node(struct ts_dom_ctx *dom, int id) {
    if (id < 0 || id >= TS_DOM_MAX_NODES) return NULL;
    if (!dom->nodes[id].used) return NULL;
    return &dom->nodes[id];
}

static void ts_dom_free_node(struct ts_dom_ctx *dom, int id) {
    struct ts_dom_node *n = ts_dom_get_node(dom, id);
    if (!n) return;
    /* Free JS listeners */
    {
        int i;
        for (i = 0; i < n->listener_count; i++) {
            JS_FreeValue(dom->ctx, n->listeners[i].callback);
        }
    }
    /* Free JS wrapper */
    if (!JS_IsUndefined(n->js_obj)) {
        JS_FreeValue(dom->ctx, n->js_obj);
    }
    /* Recursively free children */
    {
        int child = n->first_child;
        while (child >= 0) {
            int next = dom->nodes[child].next_sibling;
            ts_dom_free_node(dom, child);
            child = next;
        }
    }
    /* Free dynamically allocated text buffer */
    if (n->text) { free(n->text); n->text = NULL; n->text_len = 0; n->text_cap = 0; }
    n->used = 0;
}

/*
 * ts_dom_set_text — set text content on a node (dynamic allocation).
 */
static void ts_dom_set_text(struct ts_dom_node *n, const char *src, int len) {
    if (len <= 0) { n->text_len = 0; return; }
    int needed = len + 1;
    if (needed > n->text_cap) {
        int cap = needed < 256 ? 256 : needed;
        char *nb = (char *)realloc(n->text, (size_t)cap);
        if (!nb) return;
        n->text = nb;
        n->text_cap = cap;
    }
    memcpy(n->text, src, (size_t)len);
    n->text[len] = '\0';
    n->text_len = len;
}

/* ================================================================== */
/* Tree manipulation                                                   */
/* ================================================================== */

static void ts_dom_append_child(struct ts_dom_ctx *dom, int parent_id, int child_id) {
    struct ts_dom_node *parent = ts_dom_get_node(dom, parent_id);
    struct ts_dom_node *child = ts_dom_get_node(dom, child_id);
    if (!parent || !child) return;

    /* Remove from old parent if any */
    if (child->parent >= 0) {
        struct ts_dom_node *old_parent = ts_dom_get_node(dom, child->parent);
        if (old_parent) {
            if (old_parent->first_child == child_id)
                old_parent->first_child = child->next_sibling;
            if (old_parent->last_child == child_id)
                old_parent->last_child = child->prev_sibling;
        }
        if (child->prev_sibling >= 0)
            dom->nodes[child->prev_sibling].next_sibling = child->next_sibling;
        if (child->next_sibling >= 0)
            dom->nodes[child->next_sibling].prev_sibling = child->prev_sibling;
    }

    /* Append to new parent */
    child->parent = parent_id;
    child->next_sibling = -1;
    child->prev_sibling = parent->last_child;

    if (parent->last_child >= 0)
        dom->nodes[parent->last_child].next_sibling = child_id;
    parent->last_child = child_id;

    if (parent->first_child < 0)
        parent->first_child = child_id;

    dom->dirty = 1;

    /* Fire connectedCallback if hook is set */
    if (dom->on_connected)
        dom->on_connected(dom, child_id);
}

static void ts_dom_remove_child(struct ts_dom_ctx *dom, int parent_id, int child_id) {
    struct ts_dom_node *parent = ts_dom_get_node(dom, parent_id);
    struct ts_dom_node *child = ts_dom_get_node(dom, child_id);
    if (!parent || !child || child->parent != parent_id) return;

    if (parent->first_child == child_id)
        parent->first_child = child->next_sibling;
    if (parent->last_child == child_id)
        parent->last_child = child->prev_sibling;
    if (child->prev_sibling >= 0)
        dom->nodes[child->prev_sibling].next_sibling = child->next_sibling;
    if (child->next_sibling >= 0)
        dom->nodes[child->next_sibling].prev_sibling = child->prev_sibling;

    /* Fire disconnectedCallback before unlinking */
    if (dom->on_disconnected)
        dom->on_disconnected(dom, child_id);

    child->parent = -1;
    child->next_sibling = -1;
    child->prev_sibling = -1;
    dom->dirty = 1;
}

static void ts_dom_insert_before(struct ts_dom_ctx *dom, int parent_id,
                                  int new_id, int ref_id) {
    struct ts_dom_node *parent = ts_dom_get_node(dom, parent_id);
    struct ts_dom_node *new_node = ts_dom_get_node(dom, new_id);
    struct ts_dom_node *ref = ts_dom_get_node(dom, ref_id);
    if (!parent || !new_node) return;

    if (!ref) {
        ts_dom_append_child(dom, parent_id, new_id);
        return;
    }

    /* Remove new_node from old parent */
    if (new_node->parent >= 0)
        ts_dom_remove_child(dom, new_node->parent, new_id);

    new_node->parent = parent_id;
    new_node->next_sibling = ref_id;
    new_node->prev_sibling = ref->prev_sibling;

    if (ref->prev_sibling >= 0)
        dom->nodes[ref->prev_sibling].next_sibling = new_id;
    ref->prev_sibling = new_id;

    if (parent->first_child == ref_id)
        parent->first_child = new_id;

    dom->dirty = 1;
}

/* ================================================================== */
/* Attribute access                                                    */
/* ================================================================== */

static const char *ts_dom_get_attr(struct ts_dom_node *node, const char *name) {
    int i;
    if (!node) return NULL;
    for (i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, name) == 0)
            return node->attrs[i].value;
    }
    return NULL;
}

static void ts_dom_set_attr(struct ts_dom_node *node, const char *name,
                             const char *value) {
    int i;
    if (!node) return;
    /* Update existing */
    for (i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, name) == 0) {
            strncpy(node->attrs[i].value, value, TS_DOM_ATTR_VALUE_MAX - 1);
            node->attrs[i].value[TS_DOM_ATTR_VALUE_MAX - 1] = '\0';
            return;
        }
    }
    /* Add new */
    if (node->attr_count < TS_DOM_MAX_ATTRS) {
        strncpy(node->attrs[node->attr_count].name, name, TS_DOM_ATTR_NAME_MAX - 1);
        node->attrs[node->attr_count].name[TS_DOM_ATTR_NAME_MAX - 1] = '\0';
        strncpy(node->attrs[node->attr_count].value, value, TS_DOM_ATTR_VALUE_MAX - 1);
        node->attrs[node->attr_count].value[TS_DOM_ATTR_VALUE_MAX - 1] = '\0';
        node->attr_count++;
    }
}

static void ts_dom_remove_attr(struct ts_dom_node *node, const char *name) {
    int i;
    if (!node) return;
    for (i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, name) == 0) {
            memmove(&node->attrs[i], &node->attrs[i + 1],
                    (size_t)(node->attr_count - i - 1) * sizeof(node->attrs[0]));
            node->attr_count--;
            return;
        }
    }
}

/* ================================================================== */
/* Text content                                                        */
/* ================================================================== */

/* Get textContent of a node (recursively collects text from children) */
static int ts_dom_get_text_content(struct ts_dom_ctx *dom, int node_id,
                                    char *buf, size_t max) {
    struct ts_dom_node *n = ts_dom_get_node(dom, node_id);
    size_t pos = 0;
    if (!n || !buf || max == 0) return 0;

    if (n->type == TS_DOM_TEXT_NODE || n->type == TS_DOM_COMMENT_NODE) {
        size_t len = (size_t)n->text_len;
        if (!n->text || len == 0) { buf[0] = '\0'; return 0; }
        if (len >= max) len = max - 1;
        memcpy(buf, n->text, len);
        buf[len] = '\0';
        return (int)len;
    }

    /* Recurse children */
    {
        int child = n->first_child;
        while (child >= 0 && pos < max - 1) {
            int wrote = ts_dom_get_text_content(dom, child, buf + pos, max - pos);
            if (wrote > 0) pos += (size_t)wrote;
            child = dom->nodes[child].next_sibling;
        }
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* ================================================================== */
/* HTML entity encoding                                                */
/* ================================================================== */

/* Encode HTML special chars: & < > " → &amp; &lt; &gt; &quot;         */
static int ts_dom_encode_entities(const char *src, size_t src_len,
                                   char *buf, size_t max) {
    size_t pos = 0;
    size_t i;
    for (i = 0; i < src_len && pos < max - 6; i++) {
        char c = src[i];
        if (c == '&') {
            memcpy(buf + pos, "&amp;", 5); pos += 5;
        } else if (c == '<') {
            memcpy(buf + pos, "&lt;", 4); pos += 4;
        } else if (c == '>') {
            memcpy(buf + pos, "&gt;", 4); pos += 4;
        } else if (c == '"') {
            memcpy(buf + pos, "&quot;", 6); pos += 6;
        } else {
            buf[pos++] = c;
        }
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* ================================================================== */
/* DOM→HTML serialization                                              */
/* ================================================================== */

/* Forward declaration for recursion */
static int ts_dom_serialize_node(struct ts_dom_ctx *dom, int node_id,
                                  char *buf, size_t max);

/* Serialize children of a node → innerHTML string */
static int ts_dom_serialize_children(struct ts_dom_ctx *dom, int node_id,
                                      char *buf, size_t max) {
    struct ts_dom_node *n = ts_dom_get_node(dom, node_id);
    size_t pos = 0;
    if (!n || !buf || max == 0) { if (buf && max > 0) buf[0] = '\0'; return 0; }

    {
        int child = n->first_child;
        while (child >= 0 && pos < max - 1) {
            int wrote = ts_dom_serialize_node(dom, child, buf + pos, max - pos);
            if (wrote > 0) pos += (size_t)wrote;
            child = dom->nodes[child].next_sibling;
        }
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* Serialize a single node + its subtree → outerHTML string */
static int ts_dom_serialize_node(struct ts_dom_ctx *dom, int node_id,
                                  char *buf, size_t max) {
    struct ts_dom_node *n = ts_dom_get_node(dom, node_id);
    size_t pos = 0;
    if (!n || !buf || max == 0) { if (buf && max > 0) buf[0] = '\0'; return 0; }

    if (n->type == TS_DOM_TEXT_NODE) {
        /* Encode text with HTML entities */
        return ts_dom_encode_entities(n->text, (size_t)n->text_len, buf, max);
    }

    if (n->type == TS_DOM_COMMENT_NODE) {
        int w = snprintf(buf, max, "<!--%.*s-->", n->text_len, n->text);
        return (w > 0 && (size_t)w < max) ? w : 0;
    }

    if (n->type != TS_DOM_ELEMENT_NODE) {
        buf[0] = '\0';
        return 0;
    }

    /* Opening tag */
    {
        int w = snprintf(buf + pos, max - pos, "<%s", n->tag);
        if (w > 0) pos += (size_t)w;
    }

    /* Attributes */
    {
        int ai;
        for (ai = 0; ai < n->attr_count && pos < max - 1; ai++) {
            char enc_val[TS_DOM_ATTR_VALUE_MAX * 2];
            ts_dom_encode_entities(n->attrs[ai].value,
                                    strlen(n->attrs[ai].value),
                                    enc_val, sizeof(enc_val));
            { int w = snprintf(buf + pos, max - pos, " %s=\"%s\"",
                               n->attrs[ai].name, enc_val);
              if (w > 0) pos += (size_t)w;
            }
        }
    }

    /* Void elements self-close */
    if (ts_html_is_void_element(n->tag, strlen(n->tag))) {
        if (pos < max - 1) buf[pos++] = '>';
        buf[pos] = '\0';
        return (int)pos;
    }

    /* Close opening tag */
    if (pos < max - 1) buf[pos++] = '>';

    /* Serialize children */
    {
        int wrote = ts_dom_serialize_children(dom, node_id, buf + pos, max - pos);
        if (wrote > 0) pos += (size_t)wrote;
    }

    /* Closing tag */
    {
        int w = snprintf(buf + pos, max - pos, "</%s>", n->tag);
        if (w > 0) pos += (size_t)w;
    }

    buf[pos] = '\0';
    return (int)pos;
}

/* ================================================================== */
/* Set text content (removes children, creates single text node)       */
/* ================================================================== */

static void ts_dom_set_text_content(struct ts_dom_ctx *dom, int node_id,
                                     const char *text, size_t text_len) {
    struct ts_dom_node *n = ts_dom_get_node(dom, node_id);
    if (!n) return;

    /* Remove all existing children */
    {
        int child = n->first_child;
        while (child >= 0) {
            int next = dom->nodes[child].next_sibling;
            ts_dom_free_node(dom, child);
            child = next;
        }
        n->first_child = -1;
        n->last_child = -1;
    }

    /* If the node is a text node, just set its text directly */
    if (n->type == TS_DOM_TEXT_NODE) {
        ts_dom_set_text(n, text, (int)text_len);
        dom->dirty = 1;
        return;
    }

    /* For element nodes, create a text child */
    if (text && text_len > 0) {
        struct ts_dom_node *tn = ts_dom_alloc_node(dom, TS_DOM_TEXT_NODE);
        if (tn) {
            ts_dom_set_text(tn, text, (int)text_len);
            ts_dom_append_child(dom, node_id, tn->id);
        }
    }
    dom->dirty = 1;
}

/* ================================================================== */
/* getElementById                                                      */
/* ================================================================== */

static int ts_dom_get_element_by_id(struct ts_dom_ctx *dom, int start, const char *id) {
    int i;
    for (i = 0; i < TS_DOM_MAX_NODES; i++) {
        struct ts_dom_node *n = &dom->nodes[i];
        if (!n->used || n->type != TS_DOM_ELEMENT_NODE) continue;
        {
            const char *node_id = ts_dom_get_attr(n, "id");
            if (node_id && strcmp(node_id, id) == 0)
                return i;
        }
    }
    (void)start;
    return -1;
}

/* ================================================================== */
/* getElementsByTagName                                                */
/* ================================================================== */

static int ts_dom_get_elements_by_tag(struct ts_dom_ctx *dom, const char *tag,
                                       int *results, int max_results) {
    int count = 0;
    int i;
    int match_all = (tag[0] == '*' && tag[1] == '\0');

    for (i = 0; i < TS_DOM_MAX_NODES && count < max_results; i++) {
        struct ts_dom_node *n = &dom->nodes[i];
        if (!n->used || n->type != TS_DOM_ELEMENT_NODE) continue;
        if (match_all || strcmp(n->tag, tag) == 0) {
            results[count++] = i;
        }
    }
    return count;
}

/* ================================================================== */
/* Simple querySelector (tag, #id, .class, tag.class, tag#id)          */
/* ================================================================== */

static int ts_dom_matches_selector(struct ts_dom_ctx *dom, int node_id,
                                    const char *selector) {
    struct ts_dom_node *n = ts_dom_get_node(dom, node_id);
    if (!n || n->type != TS_DOM_ELEMENT_NODE) return 0;

    const char *p = selector;

    /* #id */
    if (*p == '#') {
        p++;
        const char *node_id_attr = ts_dom_get_attr(n, "id");
        if (!node_id_attr) return 0;
        return strcmp(node_id_attr, p) == 0;
    }

    /* .class */
    if (*p == '.') {
        p++;
        const char *cls = ts_dom_get_attr(n, "class");
        if (!cls) return 0;
        /* Check if class is in space-separated list */
        size_t plen = strlen(p);
        const char *c = cls;
        while (*c) {
            while (*c == ' ') c++;
            const char *word = c;
            while (*c && *c != ' ') c++;
            size_t wlen = (size_t)(c - word);
            if (wlen == plen && strncmp(word, p, plen) == 0)
                return 1;
        }
        return 0;
    }

    /* tag or tag.class or tag#id */
    {
        const char *dot = strchr(p, '.');
        const char *hash = strchr(p, '#');

        if (dot) {
            /* tag.class */
            size_t tag_len = (size_t)(dot - p);
            if (tag_len > 0 && strncmp(n->tag, p, tag_len) != 0) return 0;
            return ts_dom_matches_selector(dom, node_id, dot);
        }
        if (hash) {
            /* tag#id */
            size_t tag_len = (size_t)(hash - p);
            if (tag_len > 0 && strncmp(n->tag, p, tag_len) != 0) return 0;
            return ts_dom_matches_selector(dom, node_id, hash);
        }

        /* Plain tag match */
        return strcmp(n->tag, p) == 0;
    }
}

static int ts_dom_query_selector(struct ts_dom_ctx *dom, int start,
                                  const char *selector) {
    int i;
    (void)start;
    for (i = 0; i < TS_DOM_MAX_NODES; i++) {
        if (ts_dom_matches_selector(dom, i, selector))
            return i;
    }
    return -1;
}

static int ts_dom_query_selector_all(struct ts_dom_ctx *dom, const char *selector,
                                      int *results, int max_results) {
    int count = 0;
    int i;
    for (i = 0; i < TS_DOM_MAX_NODES && count < max_results; i++) {
        if (ts_dom_matches_selector(dom, i, selector))
            results[count++] = i;
    }
    return count;
}

/* ================================================================== */
/* Build DOM tree from HTML                                            */
/* ================================================================== */

static void ts_dom_build_from_html(struct ts_dom_ctx *dom,
                                    const char *html, size_t html_len,
                                    int parent_id) {
    struct ts_tokenizer tok;
    struct ts_token t;
    int current_parent = parent_id;

    /* Stack for tracking open elements */
    int parent_stack[64];
    int stack_depth = 0;
    parent_stack[0] = parent_id;

    ts_tok_init(&tok, html, html_len);

    while (ts_tok_next(&tok, &t)) {
        switch (t.type) {
        case TS_TOK_TAG_OPEN:
        case TS_TOK_TAG_SELF_CLOSE: {
            struct ts_dom_node *elem = ts_dom_alloc_node(dom, TS_DOM_ELEMENT_NODE);
            if (!elem) return;

            /* Tag name (lowercase) */
            {
                size_t tl = t.tag_name_len;
                if (tl >= TS_DOM_TAG_MAX) tl = TS_DOM_TAG_MAX - 1;
                size_t i;
                for (i = 0; i < tl; i++) {
                    char c = t.tag_name[i];
                    elem->tag[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
                }
                elem->tag[tl] = '\0';
            }

            /* Parse attributes */
            {
                /* Common attributes: id, class, style, href, src */
                static const char *common_attrs[] = {
                    "id", "class", "style", "href", "src", "type", "name",
                    "value", "action", "method", "placeholder", "alt",
                    "title", "data-id", "data-type", "role", "aria-label",
                    NULL
                };
                int ai;
                for (ai = 0; common_attrs[ai]; ai++) {
                    char val[TS_DOM_ATTR_VALUE_MAX];
                    if (ts_tok_attr_get(&t, common_attrs[ai], val, sizeof(val)) >= 0) {
                        ts_dom_set_attr(elem, common_attrs[ai], val);
                    }
                }
            }

            /* Parse inline style */
            {
                const char *style = ts_dom_get_attr(elem, "style");
                if (style) {
                    ts_css_parse_inline(style, elem->inline_style,
                                        &elem->inline_style_count, 16);
                }
            }

            /* Add to parent */
            ts_dom_append_child(dom, current_parent, elem->id);

            /* Track special elements */
            if (strcmp(elem->tag, "html") == 0) dom->html_node = elem->id;
            else if (strcmp(elem->tag, "head") == 0) dom->head_node = elem->id;
            else if (strcmp(elem->tag, "body") == 0) dom->body_node = elem->id;

            /* Push as parent for children (unless self-closing or void) */
            if (t.type != TS_TOK_TAG_SELF_CLOSE &&
                !ts_html_is_void_element(elem->tag, strlen(elem->tag))) {
                if (stack_depth < 63) {
                    stack_depth++;
                    parent_stack[stack_depth] = elem->id;
                    current_parent = elem->id;
                }
            }
            break;
        }

        case TS_TOK_TAG_CLOSE: {
            /* Pop stack until matching tag found */
            char close_tag[TS_DOM_TAG_MAX];
            {
                size_t tl = t.tag_name_len;
                if (tl >= TS_DOM_TAG_MAX) tl = TS_DOM_TAG_MAX - 1;
                size_t i;
                for (i = 0; i < tl; i++) {
                    char c = t.tag_name[i];
                    close_tag[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
                }
                close_tag[tl] = '\0';
            }
            while (stack_depth > 0) {
                struct ts_dom_node *top = ts_dom_get_node(dom, parent_stack[stack_depth]);
                stack_depth--;
                current_parent = parent_stack[stack_depth];
                if (top && strcmp(top->tag, close_tag) == 0)
                    break;
            }
            break;
        }

        case TS_TOK_TEXT: {
            /* Skip whitespace-only text nodes inside <head> */
            {
                int only_ws = 1;
                size_t i;
                for (i = 0; i < t.len; i++) {
                    char c = t.start[i];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                        only_ws = 0;
                        break;
                    }
                }
                if (only_ws && t.len < 4) break; /* skip trivial whitespace */
            }

            struct ts_dom_node *text = ts_dom_alloc_node(dom, TS_DOM_TEXT_NODE);
            if (!text) return;

            /* Decode entities into dynamically allocated buffer */
            {
                int cap = (int)t.len + 1;
                text->text = (char *)malloc((size_t)cap);
                if (text->text) {
                    text->text_cap = cap;
                    text->text_len = (int)ts_decode_entities(t.start, t.len,
                                                              text->text, (size_t)cap);
                }
            }
            ts_dom_append_child(dom, current_parent, text->id);
            break;
        }

        case TS_TOK_COMMENT: {
            struct ts_dom_node *comment = ts_dom_alloc_node(dom, TS_DOM_COMMENT_NODE);
            if (!comment) return;
            /* Store comment text (skip <!-- and -->) */
            {
                const char *cs = t.start + 4;
                int cl = t.len > 7 ? (int)(t.len - 7) : 0;
                ts_dom_set_text(comment, cs, cl);
                comment->text_len = (int)cl;
            }
            ts_dom_append_child(dom, current_parent, comment->id);
            break;
        }

        default:
            break;
        }
    }
}

/* ================================================================== */
/* Event dispatch                                                      */
/* ================================================================== */

struct ts_dom_event {
    char type[32];
    int target_id;
    int current_target_id;
    int bubbles;
    int cancelable;
    int default_prevented;
    int propagation_stopped;
    /* Mouse event data */
    int client_x, client_y;
    int button;
    /* Key event data */
    int key_code;
    char key[16];
};

static void ts_dom_dispatch_event(struct ts_dom_ctx *dom,
                                   int target_id,
                                   struct ts_dom_event *event) {
    /* Build path from target to document (for bubbling) */
    int path[64];
    int path_len = 0;
    int node_id = target_id;

    while (node_id >= 0 && path_len < 64) {
        path[path_len++] = node_id;
        node_id = dom->nodes[node_id].parent;
    }

    event->target_id = target_id;

    /* Capture phase (top-down) */
    {
        int i;
        for (i = path_len - 1; i >= 0 && !event->propagation_stopped; i--) {
            struct ts_dom_node *n = ts_dom_get_node(dom, path[i]);
            if (!n) continue;
            event->current_target_id = path[i];
            int j;
            for (j = 0; j < n->listener_count; j++) {
                if (n->listeners[j].capture &&
                    strcmp(n->listeners[j].type, event->type) == 0) {
                    JSValue args[1];
                    /* Create event object */
                    JSValue evt_obj = JS_NewObject(dom->ctx);
                    JS_SetPropertyStr(dom->ctx, evt_obj, "type",
                                       JS_NewString(dom->ctx, event->type));
                    JS_SetPropertyStr(dom->ctx, evt_obj, "bubbles",
                                       JS_NewBool(dom->ctx, event->bubbles));
                    JS_SetPropertyStr(dom->ctx, evt_obj, "clientX",
                                       JS_NewInt32(dom->ctx, event->client_x));
                    JS_SetPropertyStr(dom->ctx, evt_obj, "clientY",
                                       JS_NewInt32(dom->ctx, event->client_y));
                    args[0] = evt_obj;
                    JSValue ret = JS_Call(dom->ctx, n->listeners[j].callback,
                                          JS_UNDEFINED, 1, args);
                    JS_FreeValue(dom->ctx, ret);
                    JS_FreeValue(dom->ctx, evt_obj);
                }
            }
        }
    }

    /* Bubble phase (bottom-up) */
    if (event->bubbles) {
        int i;
        for (i = 0; i < path_len && !event->propagation_stopped; i++) {
            struct ts_dom_node *n = ts_dom_get_node(dom, path[i]);
            if (!n) continue;
            event->current_target_id = path[i];
            int j;
            for (j = 0; j < n->listener_count; j++) {
                if (!n->listeners[j].capture &&
                    strcmp(n->listeners[j].type, event->type) == 0) {
                    JSValue args[1];
                    JSValue evt_obj = JS_NewObject(dom->ctx);
                    JS_SetPropertyStr(dom->ctx, evt_obj, "type",
                                       JS_NewString(dom->ctx, event->type));
                    JS_SetPropertyStr(dom->ctx, evt_obj, "bubbles",
                                       JS_NewBool(dom->ctx, event->bubbles));
                    JS_SetPropertyStr(dom->ctx, evt_obj, "clientX",
                                       JS_NewInt32(dom->ctx, event->client_x));
                    JS_SetPropertyStr(dom->ctx, evt_obj, "clientY",
                                       JS_NewInt32(dom->ctx, event->client_y));
                    JS_SetPropertyStr(dom->ctx, evt_obj, "key",
                                       JS_NewString(dom->ctx, event->key));
                    JS_SetPropertyStr(dom->ctx, evt_obj, "keyCode",
                                       JS_NewInt32(dom->ctx, event->key_code));
                    args[0] = evt_obj;
                    JSValue ret = JS_Call(dom->ctx, n->listeners[j].callback,
                                          JS_UNDEFINED, 1, args);
                    JS_FreeValue(dom->ctx, ret);
                    JS_FreeValue(dom->ctx, evt_obj);
                }
            }
        }
    }
}

/* ================================================================== */
/* Timer management                                                    */
/* ================================================================== */

static int ts_dom_set_timer(struct ts_dom_ctx *dom, JSValue callback,
                             uint64_t delay_ms, uint64_t interval_ms) {
    int i;
    for (i = 0; i < TS_DOM_MAX_TIMERS; i++) {
        if (!dom->timers[i].used) {
            dom->timers[i].used = 1;
            dom->timers[i].id = dom->next_timer_id++;
            dom->timers[i].callback = JS_DupValue(dom->ctx, callback);
            dom->timers[i].fire_at_ms = (uint64_t)fry_gettime() + delay_ms;
            dom->timers[i].interval_ms = interval_ms;
            return dom->timers[i].id;
        }
    }
    return -1;
}

static void ts_dom_clear_timer(struct ts_dom_ctx *dom, int timer_id) {
    int i;
    for (i = 0; i < TS_DOM_MAX_TIMERS; i++) {
        if (dom->timers[i].used && dom->timers[i].id == timer_id) {
            JS_FreeValue(dom->ctx, dom->timers[i].callback);
            dom->timers[i].used = 0;
            return;
        }
    }
}

/* Process expired timers — call from main event loop */
static void ts_dom_tick_timers(struct ts_dom_ctx *dom) {
    uint64_t now = (uint64_t)fry_gettime();
    int i;
    for (i = 0; i < TS_DOM_MAX_TIMERS; i++) {
        if (dom->timers[i].used && now >= dom->timers[i].fire_at_ms) {
            JSValue ret = JS_Call(dom->ctx, dom->timers[i].callback,
                                  JS_UNDEFINED, 0, NULL);
            JS_FreeValue(dom->ctx, ret);

            if (dom->timers[i].interval_ms > 0) {
                /* setInterval: reschedule */
                dom->timers[i].fire_at_ms = now + dom->timers[i].interval_ms;
            } else {
                /* setTimeout: one-shot, clear */
                JS_FreeValue(dom->ctx, dom->timers[i].callback);
                dom->timers[i].used = 0;
            }
        }
    }

    /* Run pending JS jobs (Promises, microtasks) */
    {
        JSContext *ctx1;
        while (JS_ExecutePendingJob(dom->rt, &ctx1) > 0) {}
    }
}

/* ================================================================== */
/* Console                                                             */
/* ================================================================== */

static void ts_dom_console_append(struct ts_dom_ctx *dom, const char *prefix,
                                   const char *msg) {
    int plen = (int)strlen(prefix);
    int mlen = (int)strlen(msg);
    int total = plen + mlen + 1; /* +1 for newline */

    if (dom->console_len + total >= (int)sizeof(dom->console_buf) - 1) {
        /* Shift buffer — remove first line */
        char *nl = strchr(dom->console_buf, '\n');
        if (nl) {
            int remove = (int)(nl - dom->console_buf) + 1;
            memmove(dom->console_buf, dom->console_buf + remove,
                    (size_t)(dom->console_len - remove));
            dom->console_len -= remove;
        } else {
            dom->console_len = 0;
        }
    }

    memcpy(dom->console_buf + dom->console_len, prefix, (size_t)plen);
    dom->console_len += plen;
    memcpy(dom->console_buf + dom->console_len, msg, (size_t)mlen);
    dom->console_len += mlen;
    dom->console_buf[dom->console_len++] = '\n';
    dom->console_buf[dom->console_len] = '\0';
}

/* ================================================================== */
/* QuickJS initialization                                              */
/* ================================================================== */

/* ================================================================== */
/* Script execution timeout                                            */
/* ================================================================== */

static uint64_t ts_dom_js_deadline = 0; /* fry_gettime() deadline */

static int ts_dom_js_interrupt(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    if (ts_dom_js_deadline == 0) return 0; /* no deadline set */
    if ((uint64_t)fry_gettime() > ts_dom_js_deadline) {
        fprintf(stderr, "JS_TIMEOUT: script exceeded deadline, aborting\n");
        return 1; /* abort execution */
    }
    return 0;
}

/*
 * ts_dom_init — initialize DOM context with QuickJS runtime.
 *
 * Creates the JS runtime, context, and registers global objects
 * (document, window, console) with their methods.
 *
 * NOTE: The full QuickJS C function bindings (JS_SetPropertyFunctionList
 * calls for document.createElement, element.addEventListener, etc.)
 * are implemented in ts_dom_bindings.h to keep this file manageable.
 * This file provides the C-side DOM data structures and operations.
 */
static void ts_dom_init(struct ts_dom_ctx *dom) {
    memset(dom, 0, sizeof(*dom));

    /* Mark all nodes as unused */
    {
        int i;
        for (i = 0; i < TS_DOM_MAX_NODES; i++) {
            dom->nodes[i].used = 0;
            dom->nodes[i].js_obj = JS_UNDEFINED;
        }
    }

    /* Mark all timers as unused */
    {
        int i;
        for (i = 0; i < TS_DOM_MAX_TIMERS; i++)
            dom->timers[i].used = 0;
    }

    dom->document_node = -1;
    dom->html_node = -1;
    dom->head_node = -1;
    dom->body_node = -1;
    dom->next_timer_id = 1;
    dom->dirty = 0;
    dom->console_len = 0;
    dom->on_connected = NULL;
    dom->on_disconnected = NULL;
    dom->on_element_created = NULL;

    /* Create QuickJS runtime */
    dom->rt = JS_NewRuntime();
    if (!dom->rt) return;

    /* No memory limit — the expanded JS environment (constructors, polyfills,
     * Web APIs) is large, and QuickJS GC cycle detection hangs on complex
     * object graphs. Let JS allocate freely from the process heap. */
    /* JS_SetMemoryLimit removed — unlimited */

    /* Script execution timeout — abort scripts that hang >10 seconds.
     * QuickJS calls the interrupt handler periodically during execution. */
    JS_SetInterruptHandler(dom->rt, ts_dom_js_interrupt, dom);

    /* Create raw context — we add intrinsics explicitly below */
    dom->ctx = JS_NewContextRaw(dom->rt);
    if (!dom->ctx) {
        JS_FreeRuntime(dom->rt);
        dom->rt = NULL;
        return;
    }

    /* Add standard JS objects (Object, Array, String, Math, JSON, etc.) */
    JS_AddIntrinsicBaseObjects(dom->ctx);
    JS_AddIntrinsicDate(dom->ctx);
    JS_AddIntrinsicEval(dom->ctx);
    JS_AddIntrinsicRegExp(dom->ctx);
    JS_AddIntrinsicJSON(dom->ctx);
    JS_AddIntrinsicProxy(dom->ctx);
    JS_AddIntrinsicMapSet(dom->ctx);
    JS_AddIntrinsicTypedArrays(dom->ctx);
    JS_AddIntrinsicPromise(dom->ctx);
    JS_AddIntrinsicBigInt(dom->ctx);

    /* Create #document node */
    {
        struct ts_dom_node *doc_node = ts_dom_alloc_node(dom, TS_DOM_DOCUMENT_NODE);
        if (doc_node) {
            dom->document_node = doc_node->id;
            strcpy(doc_node->tag, "#document");
        }
    }
}

/*
 * ts_dom_load_html — parse HTML into the DOM tree.
 */
static void ts_dom_load_html(struct ts_dom_ctx *dom,
                              const char *html, size_t html_len) {
    /* Clear existing nodes (except #document) */
    {
        int i;
        for (i = 0; i < TS_DOM_MAX_NODES; i++) {
            if (i != dom->document_node && dom->nodes[i].used)
                ts_dom_free_node(dom, i);
        }
    }

    /* Reset document children */
    if (dom->document_node >= 0) {
        dom->nodes[dom->document_node].first_child = -1;
        dom->nodes[dom->document_node].last_child = -1;
    }

    dom->html_node = -1;
    dom->head_node = -1;
    dom->body_node = -1;

    /* Build DOM tree from HTML */
    ts_dom_build_from_html(dom, html, html_len, dom->document_node);

    dom->dirty = 1;
}

/*
 * ts_dom_run_scripts — find and execute all <script> tags in the DOM.
 */
static void ts_dom_run_scripts(struct ts_dom_ctx *dom) {
    int i;
    int script_idx = 0;
    if (!dom->ctx) return;

    for (i = 0; i < TS_DOM_MAX_NODES; i++) {
        struct ts_dom_node *n = &dom->nodes[i];
        if (!n->used || n->type != TS_DOM_ELEMENT_NODE) continue;
        if (strcmp(n->tag, "script") != 0) continue;

        /* Collect script text — estimate size from child text nodes */
        {
        int est_len = 0;
        {
            int child = n->first_child;
            while (child >= 0) {
                struct ts_dom_node *cn = ts_dom_get_node(dom, child);
                if (cn && cn->text_len > 0) est_len += cn->text_len;
                child = cn ? cn->next_sibling : -1;
            }
        }
        if (est_len <= 0) est_len = 256;
        {
        size_t buf_cap = (size_t)est_len + 256;
        char *script_buf = (char *)malloc(buf_cap);
        if (!script_buf) continue;
        int script_len = ts_dom_get_text_content(dom, i, script_buf, buf_cap);
        if (script_len <= 0) { free(script_buf); continue; }

        /* Trace to serial for crash diagnosis */
        fprintf(stderr, "JS_EXEC: script[%d] node=%d len=%d first40=\"%.40s\"\n",
                script_idx, i, script_len, script_buf);
        script_idx++;

        /* Execute with 10-second timeout */
        ts_dom_js_deadline = (uint64_t)fry_gettime() + 10000;
        JSValue result = JS_Eval(dom->ctx, script_buf, (size_t)script_len,
                                  "<script>", JS_EVAL_TYPE_GLOBAL);
        ts_dom_js_deadline = 0;
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(dom->ctx);
            const char *str = JS_ToCString(dom->ctx, exc);
            if (str) {
                fprintf(stderr, "JS_ERR: script[%d] %s\n", script_idx - 1, str);
                ts_dom_console_append(dom, "[ERR] ", str);
                JS_FreeCString(dom->ctx, str);
            }
            JS_FreeValue(dom->ctx, exc);
        } else {
            fprintf(stderr, "JS_OK: script[%d] done\n", script_idx - 1);
        }
        JS_FreeValue(dom->ctx, result);
        free(script_buf);
        }
        }
    }

    fprintf(stderr, "JS_EXEC: all %d scripts done, running pending jobs\n", script_idx);

    /* Run pending jobs */
    {
        JSContext *ctx1;
        int job_count = 0;
        while (JS_ExecutePendingJob(dom->rt, &ctx1) > 0) {
            job_count++;
        }
        fprintf(stderr, "JS_EXEC: %d pending jobs completed\n", job_count);
    }
}

/*
 * ts_dom_destroy — free all resources.
 */
static void ts_dom_destroy(struct ts_dom_ctx *dom) {
    int i;

    fprintf(stderr, "DESTROY[1]: timers\n");
    /* Free timers */
    for (i = 0; i < TS_DOM_MAX_TIMERS; i++) {
        if (dom->timers[i].used)
            JS_FreeValue(dom->ctx, dom->timers[i].callback);
    }

    fprintf(stderr, "DESTROY[2]: nodes\n");
    /* Free nodes */
    for (i = 0; i < TS_DOM_MAX_NODES; i++) {
        if (dom->nodes[i].used) {
            int j;
            for (j = 0; j < dom->nodes[i].listener_count; j++)
                JS_FreeValue(dom->ctx, dom->nodes[i].listeners[j].callback);
            if (!JS_IsUndefined(dom->nodes[i].js_obj))
                JS_FreeValue(dom->ctx, dom->nodes[i].js_obj);
        }
    }

    fprintf(stderr, "DESTROY[3]: globals\n");
    /* Free JS global objects */
    if (!JS_IsUndefined(dom->js_document))
        JS_FreeValue(dom->ctx, dom->js_document);
    if (!JS_IsUndefined(dom->js_window))
        JS_FreeValue(dom->ctx, dom->js_window);
    if (!JS_IsUndefined(dom->js_console))
        JS_FreeValue(dom->ctx, dom->js_console);

    fprintf(stderr, "DESTROY[4]: FreeContext\n");
    /* Free QuickJS context — this releases all JS objects in the context */
    if (dom->ctx) JS_FreeContext(dom->ctx);
    dom->ctx = NULL;

    fprintf(stderr, "DESTROY[5]: FreeRuntime\n");
    /* JS_FreeRuntime hangs when the expanded JS environment has many
     * eval'd constructors/polyfills (GC cycle detection gets stuck).
     * Leak the runtime — on bare-metal with 2GB RAM, ~32MB per navigate
     * is acceptable. The runtime's allocator will be replaced by the
     * fresh one in ts_dom_init(). */
    /* if (dom->rt) JS_FreeRuntime(dom->rt); */
    dom->rt = NULL;
    fprintf(stderr, "DESTROY[6]: done\n");
}

#endif /* TS_DOM_H */
