/*
 * ts_dom_bindings.h — QuickJS bindings for the TaterSurf DOM
 *
 * Header-only. Registers C functions as JavaScript methods on
 * document, window, Element, Node, and console objects.
 *
 * This is the bridge that makes JavaScript see a real DOM.
 * React, Lit, and Next.js all call these APIs to render pages.
 *
 * Depends on: ts_dom.h (DOM data structures), quickjs.h
 */

#ifndef TS_DOM_BINDINGS_H
#define TS_DOM_BINDINGS_H

#include "ts_dom.h"

/* ================================================================== */
/* Pointer to DOM context — set before calling any binding             */
/* ================================================================== */

static struct ts_dom_ctx *g_dom = NULL;
static struct ts_document *g_doc_ref = NULL;  /* set in ts_dom_register_globals */

/* Forward declarations */
static void ts_dom_bind_node_methods(JSContext *ctx, JSValue obj);
static JSValue ts_js_wrap_node(JSContext *ctx, struct ts_dom_ctx *dom, int node_id);
static int ts_js__deep_clone(struct ts_dom_ctx *dom, int src_id);
static JSValue ts_js_canvas_get_context(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv);

/* ================================================================== */
/* Helper: get DOM node from JS "this" object                          */
/* ================================================================== */

/* Each JS Element/Node wrapper stores its pool index as an opaque int */
#define TS_DOM_NODE_CLASS_ID 1

static int ts_js_get_node_id(JSContext *ctx, JSValueConst this_val) {
    JSValue v = JS_GetPropertyStr(ctx, this_val, "__node_id");
    int id = -1;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &id, v);
    JS_FreeValue(ctx, v);
    return id;
}

/* Create a JS wrapper object for a DOM node */
static JSValue ts_js_wrap_node(JSContext *ctx, struct ts_dom_ctx *dom, int node_id) {
    struct ts_dom_node *n = ts_dom_get_node(dom, node_id);
    if (!n) return JS_NULL;

    /* Return existing wrapper if already created */
    if (!JS_IsUndefined(n->js_obj))
        return JS_DupValue(ctx, n->js_obj);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "__node_id", JS_NewInt32(ctx, node_id));

    /* Set nodeType */
    JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, n->type));

    /* Set nodeName / tagName */
    if (n->type == TS_DOM_ELEMENT_NODE) {
        char upper_tag[TS_DOM_TAG_MAX];
        size_t i;
        for (i = 0; n->tag[i] && i < TS_DOM_TAG_MAX - 1; i++)
            upper_tag[i] = (n->tag[i] >= 'a' && n->tag[i] <= 'z')
                            ? n->tag[i] - 32 : n->tag[i];
        upper_tag[i] = '\0';
        JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, upper_tag));
        JS_SetPropertyStr(ctx, obj, "tagName", JS_NewString(ctx, upper_tag));
    } else if (n->type == TS_DOM_TEXT_NODE) {
        JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, "#text"));
    } else if (n->type == TS_DOM_COMMENT_NODE) {
        JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, "#comment"));
    } else if (n->type == TS_DOM_DOCUMENT_NODE) {
        JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, "#document"));
    }

    /* Cache the wrapper */
    n->js_obj = JS_DupValue(ctx, obj);
    return obj;
}

/* ================================================================== */
/* Node property getters                                               */
/* ================================================================== */

static JSValue ts_js_node_get_parent(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->parent < 0) return JS_NULL;
    return ts_js_wrap_node(ctx, g_dom, n->parent);
}

static JSValue ts_js_node_get_first_child(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->first_child < 0) return JS_NULL;
    return ts_js_wrap_node(ctx, g_dom, n->first_child);
}

static JSValue ts_js_node_get_last_child(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->last_child < 0) return JS_NULL;
    return ts_js_wrap_node(ctx, g_dom, n->last_child);
}

static JSValue ts_js_node_get_next_sibling(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->next_sibling < 0) return JS_NULL;
    return ts_js_wrap_node(ctx, g_dom, n->next_sibling);
}

static JSValue ts_js_node_get_prev_sibling(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->prev_sibling < 0) return JS_NULL;
    return ts_js_wrap_node(ctx, g_dom, n->prev_sibling);
}

static JSValue ts_js_node_get_text_content(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    char buf[4096];
    int len = ts_dom_get_text_content(g_dom, id, buf, sizeof(buf));
    if (len <= 0) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, buf, (size_t)len);
}

static JSValue ts_js_node_get_children(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    JSValue arr = JS_NewArray(ctx);
    if (!n) return arr;
    int idx = 0;
    int child = n->first_child;
    while (child >= 0) {
        struct ts_dom_node *c = ts_dom_get_node(g_dom, child);
        if (c && c->type == TS_DOM_ELEMENT_NODE) {
            JS_SetPropertyUint32(ctx, arr, (uint32_t)idx++,
                                  ts_js_wrap_node(ctx, g_dom, child));
        }
        child = c ? c->next_sibling : -1;
    }
    return arr;
}

static JSValue ts_js_node_get_child_nodes(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    JSValue arr = JS_NewArray(ctx);
    if (!n) return arr;
    int idx = 0;
    int child = n->first_child;
    while (child >= 0) {
        struct ts_dom_node *c = ts_dom_get_node(g_dom, child);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)idx++,
                              ts_js_wrap_node(ctx, g_dom, child));
        child = c ? c->next_sibling : -1;
    }
    return arr;
}

/* ================================================================== */
/* Node methods                                                        */
/* ================================================================== */

static JSValue ts_js_node_append_child(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    int parent_id = ts_js_get_node_id(ctx, this_val);
    int child_id = ts_js_get_node_id(ctx, argv[0]);
    ts_dom_append_child(g_dom, parent_id, child_id);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue ts_js_node_remove_child(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    int parent_id = ts_js_get_node_id(ctx, this_val);
    int child_id = ts_js_get_node_id(ctx, argv[0]);
    ts_dom_remove_child(g_dom, parent_id, child_id);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue ts_js_node_insert_before(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    int parent_id = ts_js_get_node_id(ctx, this_val);
    int new_id = ts_js_get_node_id(ctx, argv[0]);
    int ref_id = (argc > 1 && !JS_IsNull(argv[1])) ? ts_js_get_node_id(ctx, argv[1]) : -1;
    ts_dom_insert_before(g_dom, parent_id, new_id, ref_id);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue ts_js_node_replace_child(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    int parent_id = ts_js_get_node_id(ctx, this_val);
    int new_id = ts_js_get_node_id(ctx, argv[0]);
    int old_id = ts_js_get_node_id(ctx, argv[1]);
    ts_dom_insert_before(g_dom, parent_id, new_id, old_id);
    ts_dom_remove_child(g_dom, parent_id, old_id);
    return JS_DupValue(ctx, argv[1]);
}

static JSValue ts_js_node_contains(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    int parent_id = ts_js_get_node_id(ctx, this_val);
    int child_id = ts_js_get_node_id(ctx, argv[0]);
    /* Walk up from child to see if we reach parent */
    int cur = child_id;
    while (cur >= 0) {
        if (cur == parent_id) return JS_TRUE;
        cur = g_dom->nodes[cur].parent;
    }
    return JS_FALSE;
}

static JSValue ts_js_node_clone_node(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *src = ts_dom_get_node(g_dom, id);
    if (!src) return JS_NULL;

    int deep = 0;
    if (argc > 0) deep = JS_ToBool(ctx, argv[0]);

    if (deep) {
        int clone_id = ts_js__deep_clone(g_dom, id);
        if (clone_id < 0) return JS_NULL;
        { JSValue wrapper = ts_js_wrap_node(ctx, g_dom, clone_id);
          ts_dom_bind_node_methods(ctx, wrapper);
          return wrapper;
        }
    }

    { struct ts_dom_node *clone = ts_dom_alloc_node(g_dom, src->type);
      if (!clone) return JS_NULL;
      memcpy(clone->tag, src->tag, sizeof(clone->tag));
      memcpy(clone->attrs, src->attrs, sizeof(clone->attrs));
      clone->attr_count = src->attr_count;
      memcpy(clone->text, src->text, sizeof(clone->text));
      clone->text_len = src->text_len;
      memcpy(clone->inline_style, src->inline_style, sizeof(clone->inline_style));
      clone->inline_style_count = src->inline_style_count;
      { JSValue wrapper = ts_js_wrap_node(ctx, g_dom, clone->id);
        ts_dom_bind_node_methods(ctx, wrapper);
        return wrapper;
      }
    }
}

/* ================================================================== */
/* Element methods                                                     */
/* ================================================================== */

static JSValue ts_js_elem_get_attribute(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    const char *val = ts_dom_get_attr(n, name);
    JS_FreeCString(ctx, name);
    if (!val) return JS_NULL;
    return JS_NewString(ctx, val);
}

static JSValue ts_js_elem_set_attribute(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (name && val) ts_dom_set_attr(n, name, val);
    if (name) JS_FreeCString(ctx, name);
    if (val) JS_FreeCString(ctx, val);
    g_dom->dirty = 1;
    return JS_UNDEFINED;
}

static JSValue ts_js_elem_remove_attribute(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name) { ts_dom_remove_attr(n, name); JS_FreeCString(ctx, name); }
    g_dom->dirty = 1;
    return JS_UNDEFINED;
}

static JSValue ts_js_elem_has_attribute(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    const char *val = ts_dom_get_attr(n, name);
    JS_FreeCString(ctx, name);
    return val ? JS_TRUE : JS_FALSE;
}

static JSValue ts_js_elem_get_id(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_NewString(ctx, "");
    const char *val = ts_dom_get_attr(n, "id");
    return JS_NewString(ctx, val ? val : "");
}

static JSValue ts_js_elem_get_class_name(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_NewString(ctx, "");
    const char *val = ts_dom_get_attr(n, "class");
    return JS_NewString(ctx, val ? val : "");
}

static JSValue ts_js_elem_get_inner_html(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    if (id < 0) return JS_NewString(ctx, "");
    { int ser_cap = 1024 * 1024;  /* 1 MB serialization buffer */
      char *buf = (char *)malloc((size_t)ser_cap);
      JSValue result;
      if (!buf) return JS_NewString(ctx, "");
      { int len = ts_dom_serialize_children(g_dom, id, buf, ser_cap);
        result = (len > 0) ? JS_NewStringLen(ctx, buf, (size_t)len)
                           : JS_NewString(ctx, "");
      }
      free(buf);
      return result;
    }
}

static JSValue ts_js_elem_get_outer_html(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    if (id < 0) return JS_NewString(ctx, "");
    { int ser_cap = 1024 * 1024;  /* 1 MB serialization buffer */
      char *buf = (char *)malloc((size_t)ser_cap);
      JSValue result;
      if (!buf) return JS_NewString(ctx, "");
      { int len = ts_dom_serialize_node(g_dom, id, buf, ser_cap);
        result = (len > 0) ? JS_NewStringLen(ctx, buf, (size_t)len)
                           : JS_NewString(ctx, "");
      }
      free(buf);
      return result;
    }
}

static JSValue ts_js_elem_set_outer_html(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->parent < 0) return JS_UNDEFINED;

    int parent_id = n->parent;
    const char *html = JS_ToCString(ctx, argv[0]);
    if (!html) return JS_UNDEFINED;

    /* Parse new HTML into parent, before this node */
    { int next_sib = n->next_sibling;
      /* Parse into a temp fragment, then insert nodes before the ref sibling */
      struct ts_dom_node *frag = ts_dom_alloc_node(g_dom, TS_DOM_FRAGMENT_NODE);
      if (frag) {
          ts_dom_build_from_html(g_dom, html, strlen(html), frag->id);
          /* Move fragment children into parent before next_sib */
          { int child = frag->first_child;
            while (child >= 0) {
                int next = g_dom->nodes[child].next_sibling;
                if (next_sib >= 0)
                    ts_dom_insert_before(g_dom, parent_id, child, next_sib);
                else
                    ts_dom_append_child(g_dom, parent_id, child);
                child = next;
            }
          }
          ts_dom_free_node(g_dom, frag->id);
      }
      /* Remove the original node */
      ts_dom_remove_child(g_dom, parent_id, id);
      ts_dom_free_node(g_dom, id);
    }

    JS_FreeCString(ctx, html);
    g_dom->dirty = 1;
    return JS_UNDEFINED;
}

/* textContent setter */
static JSValue ts_js_node_set_text_content(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    int id = ts_js_get_node_id(ctx, this_val);
    if (id < 0) return JS_UNDEFINED;
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_UNDEFINED;
    ts_dom_set_text_content(g_dom, id, text, strlen(text));
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

/* id setter */
static JSValue ts_js_elem_set_id(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_UNDEFINED;
    { const char *val = JS_ToCString(ctx, argv[0]);
      if (val) { ts_dom_set_attr(n, "id", val); JS_FreeCString(ctx, val); }
    }
    g_dom->dirty = 1;
    return JS_UNDEFINED;
}

/* className setter */
static JSValue ts_js_elem_set_class_name(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_UNDEFINED;
    { const char *val = JS_ToCString(ctx, argv[0]);
      if (val) { ts_dom_set_attr(n, "class", val); JS_FreeCString(ctx, val); }
    }
    g_dom->dirty = 1;
    return JS_UNDEFINED;
}

static JSValue ts_js_elem_set_inner_html(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    int id = ts_js_get_node_id(ctx, this_val);
    if (id < 0) return JS_UNDEFINED;

    const char *html = JS_ToCString(ctx, argv[0]);
    if (!html) return JS_UNDEFINED;

    /* Remove existing children */
    {
        struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
        if (n) {
            int child = n->first_child;
            while (child >= 0) {
                int next = g_dom->nodes[child].next_sibling;
                ts_dom_free_node(g_dom, child);
                child = next;
            }
            n->first_child = -1;
            n->last_child = -1;
        }
    }

    /* Parse new HTML into children */
    ts_dom_build_from_html(g_dom, html, strlen(html), id);
    JS_FreeCString(ctx, html);
    g_dom->dirty = 1;
    return JS_UNDEFINED;
}

/* addEventListener */
static JSValue ts_js_elem_add_event_listener(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->listener_count >= TS_DOM_MAX_LISTENERS) return JS_UNDEFINED;

    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;

    int capture = 0;
    if (argc > 2 && JS_IsBool(argv[2]))
        capture = JS_ToBool(ctx, argv[2]);

    strncpy(n->listeners[n->listener_count].type, type, 31);
    n->listeners[n->listener_count].type[31] = '\0';
    n->listeners[n->listener_count].callback = JS_DupValue(ctx, argv[1]);
    n->listeners[n->listener_count].capture = capture;
    n->listener_count++;

    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

/* removeEventListener */
static JSValue ts_js_elem_remove_event_listener(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_UNDEFINED;

    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;

    int i;
    for (i = 0; i < n->listener_count; i++) {
        if (strcmp(n->listeners[i].type, type) == 0) {
            /* Simple match by type — full match would compare function identity */
            JS_FreeValue(ctx, n->listeners[i].callback);
            memmove(&n->listeners[i], &n->listeners[i + 1],
                    (size_t)(n->listener_count - i - 1) * sizeof(n->listeners[0]));
            n->listener_count--;
            break;
        }
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

/* querySelector on element */
static JSValue ts_js_elem_query_selector(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_NULL;
    int result = ts_dom_query_selector(g_dom, 0, sel);
    JS_FreeCString(ctx, sel);
    if (result < 0) return JS_NULL;
    return ts_js_wrap_node(ctx, g_dom, result);
}

/* querySelectorAll on element */
static JSValue ts_js_elem_query_selector_all(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_NewArray(ctx);
    int results[256];
    int count = ts_dom_query_selector_all(g_dom, sel, results, 256);
    JS_FreeCString(ctx, sel);
    JSValue arr = JS_NewArray(ctx);
    int i;
    for (i = 0; i < count; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                              ts_js_wrap_node(ctx, g_dom, results[i]));
    return arr;
}

/* ================================================================== */
/* Step 9: classList object                                            */
/* ================================================================== */

/* Helper: check if class_list (space-separated) contains cls */
static int ts_js__class_contains(const char *class_list, const char *cls) {
    size_t clen = strlen(cls);
    const char *p = class_list;
    if (!clen) return 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        { const char *word = p;
          while (*p && *p != ' ') p++;
          if ((size_t)(p - word) == clen && strncmp(word, cls, clen) == 0)
              return 1;
        }
    }
    return 0;
}

/* Helper: rebuild class attribute from buffer, removing cls */
static void ts_js__class_remove(char *class_list, size_t max, const char *cls) {
    char tmp[TS_DOM_ATTR_VALUE_MAX];
    size_t clen = strlen(cls);
    size_t out = 0;
    const char *p = class_list;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        { const char *word = p;
          while (*p && *p != ' ') p++;
          { size_t wlen = (size_t)(p - word);
            if (wlen == clen && strncmp(word, cls, clen) == 0)
                continue; /* skip this class */
            if (out > 0 && out < max - 1) tmp[out++] = ' ';
            if (out + wlen < max - 1) { memcpy(tmp + out, word, wlen); out += wlen; }
          }
        }
    }
    tmp[out] = '\0';
    strncpy(class_list, tmp, max - 1);
    class_list[max - 1] = '\0';
}

static JSValue ts_js_classlist_add(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_UNDEFINED;
    { char cls_buf[TS_DOM_ATTR_VALUE_MAX];
      const char *existing = ts_dom_get_attr(n, "class");
      if (existing) { strncpy(cls_buf, existing, sizeof(cls_buf)-1); cls_buf[sizeof(cls_buf)-1]='\0'; }
      else cls_buf[0] = '\0';
      { int i;
        for (i = 0; i < argc; i++) {
            const char *cls = JS_ToCString(ctx, argv[i]);
            if (!cls) continue;
            if (!ts_js__class_contains(cls_buf, cls)) {
                size_t cur_len = strlen(cls_buf);
                size_t add_len = strlen(cls);
                if (cur_len + add_len + 2 < sizeof(cls_buf)) {
                    if (cur_len > 0) cls_buf[cur_len++] = ' ';
                    memcpy(cls_buf + cur_len, cls, add_len);
                    cls_buf[cur_len + add_len] = '\0';
                }
            }
            JS_FreeCString(ctx, cls);
        }
      }
      ts_dom_set_attr(n, "class", cls_buf);
      g_dom->dirty = 1;
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_classlist_remove(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_UNDEFINED;
    { char cls_buf[TS_DOM_ATTR_VALUE_MAX];
      const char *existing = ts_dom_get_attr(n, "class");
      if (existing) { strncpy(cls_buf, existing, sizeof(cls_buf)-1); cls_buf[sizeof(cls_buf)-1]='\0'; }
      else return JS_UNDEFINED;
      { int i;
        for (i = 0; i < argc; i++) {
            const char *cls = JS_ToCString(ctx, argv[i]);
            if (!cls) continue;
            ts_js__class_remove(cls_buf, sizeof(cls_buf), cls);
            JS_FreeCString(ctx, cls);
        }
      }
      ts_dom_set_attr(n, "class", cls_buf);
      g_dom->dirty = 1;
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_classlist_toggle(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    { int id = ts_js_get_node_id(ctx, this_val);
      struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
      if (!n) return JS_FALSE;
      { const char *cls = JS_ToCString(ctx, argv[0]);
        if (!cls) return JS_FALSE;
        { char cls_buf[TS_DOM_ATTR_VALUE_MAX];
          const char *existing = ts_dom_get_attr(n, "class");
          int result;
          if (existing) { strncpy(cls_buf, existing, sizeof(cls_buf)-1); cls_buf[sizeof(cls_buf)-1]='\0'; }
          else cls_buf[0] = '\0';
          /* Optional force parameter */
          if (argc > 1 && !JS_IsUndefined(argv[1])) {
              int force = JS_ToBool(ctx, argv[1]);
              if (force) {
                  if (!ts_js__class_contains(cls_buf, cls)) {
                      size_t cur_len = strlen(cls_buf);
                      size_t add_len = strlen(cls);
                      if (cur_len + add_len + 2 < sizeof(cls_buf)) {
                          if (cur_len > 0) cls_buf[cur_len++] = ' ';
                          memcpy(cls_buf + cur_len, cls, add_len);
                          cls_buf[cur_len + add_len] = '\0';
                      }
                  }
                  result = 1;
              } else {
                  ts_js__class_remove(cls_buf, sizeof(cls_buf), cls);
                  result = 0;
              }
          } else if (ts_js__class_contains(cls_buf, cls)) {
              ts_js__class_remove(cls_buf, sizeof(cls_buf), cls);
              result = 0;
          } else {
              size_t cur_len = strlen(cls_buf);
              size_t add_len = strlen(cls);
              if (cur_len + add_len + 2 < sizeof(cls_buf)) {
                  if (cur_len > 0) cls_buf[cur_len++] = ' ';
                  memcpy(cls_buf + cur_len, cls, add_len);
                  cls_buf[cur_len + add_len] = '\0';
              }
              result = 1;
          }
          ts_dom_set_attr(n, "class", cls_buf);
          g_dom->dirty = 1;
          JS_FreeCString(ctx, cls);
          return result ? JS_TRUE : JS_FALSE;
        }
      }
    }
}

static JSValue ts_js_classlist_contains(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    { int id = ts_js_get_node_id(ctx, this_val);
      struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
      if (!n) return JS_FALSE;
      { const char *cls = JS_ToCString(ctx, argv[0]);
        if (!cls) return JS_FALSE;
        { const char *existing = ts_dom_get_attr(n, "class");
          int result = existing ? ts_js__class_contains(existing, cls) : 0;
          JS_FreeCString(ctx, cls);
          return result ? JS_TRUE : JS_FALSE;
        }
      }
    }
}

static JSValue ts_js_classlist_item(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    { int id = ts_js_get_node_id(ctx, this_val);
      struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
      if (!n) return JS_NULL;
      { int32_t index; JS_ToInt32(ctx, &index, argv[0]);
        { const char *existing = ts_dom_get_attr(n, "class");
          if (!existing) return JS_NULL;
          { const char *p = existing; int cur = 0;
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                { const char *word = p;
                  while (*p && *p != ' ') p++;
                  if (cur == index)
                      return JS_NewStringLen(ctx, word, (size_t)(p - word));
                  cur++;
                }
            }
          }
        }
      }
    }
    return JS_NULL;
}

/* Build classList sub-object for a node wrapper */
static void ts_js__bind_classlist(JSContext *ctx, JSValue obj, int node_id) {
    JSValue cl = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cl, "__node_id", JS_NewInt32(ctx, node_id));
    JS_SetPropertyStr(ctx, cl, "add",
        JS_NewCFunction(ctx, ts_js_classlist_add, "add", 1));
    JS_SetPropertyStr(ctx, cl, "remove",
        JS_NewCFunction(ctx, ts_js_classlist_remove, "remove", 1));
    JS_SetPropertyStr(ctx, cl, "toggle",
        JS_NewCFunction(ctx, ts_js_classlist_toggle, "toggle", 2));
    JS_SetPropertyStr(ctx, cl, "contains",
        JS_NewCFunction(ctx, ts_js_classlist_contains, "contains", 1));
    JS_SetPropertyStr(ctx, cl, "item",
        JS_NewCFunction(ctx, ts_js_classlist_item, "item", 1));
    JS_SetPropertyStr(ctx, obj, "classList", cl);
}

/* ================================================================== */
/* Step 9: dataset proxy (data-* attributes)                           */
/* ================================================================== */

/* Convert "data-foo-bar" to camelCase "fooBar" */
static void ts_js__data_attr_to_camel(const char *attr_name, char *out, size_t max) {
    const char *p = attr_name + 5; /* skip "data-" */
    size_t o = 0;
    int next_upper = 0;
    while (*p && o < max - 1) {
        if (*p == '-') { next_upper = 1; }
        else {
            out[o++] = next_upper ? ((*p >= 'a' && *p <= 'z') ? *p - 32 : *p) : *p;
            next_upper = 0;
        }
        p++;
    }
    out[o] = '\0';
}

/* Convert camelCase "fooBar" to "data-foo-bar" */
static void ts_js__camel_to_data_attr(const char *camel, char *out, size_t max) {
    size_t o = 0;
    const char *p = camel;
    memcpy(out, "data-", 5); o = 5;
    while (*p && o < max - 2) {
        if (*p >= 'A' && *p <= 'Z') {
            out[o++] = '-';
            if (o < max - 1) out[o++] = *p + 32;
        } else {
            out[o++] = *p;
        }
        p++;
    }
    out[o] = '\0';
}

static JSValue ts_js_elem_get_dataset(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    JSValue ds = JS_NewObject(ctx);
    if (!n) return ds;
    { int i;
      for (i = 0; i < n->attr_count; i++) {
          if (strncmp(n->attrs[i].name, "data-", 5) == 0) {
              char camel[TS_DOM_ATTR_NAME_MAX];
              ts_js__data_attr_to_camel(n->attrs[i].name, camel, sizeof(camel));
              JS_SetPropertyStr(ctx, ds, camel,
                  JS_NewString(ctx, n->attrs[i].value));
          }
      }
    }
    return ds;
}

/* ================================================================== */
/* Step 9: element.style CSSStyleDeclaration                           */
/* ================================================================== */

/* Convert camelCase to kebab-case: backgroundColor → background-color */
static void ts_js__camel_to_kebab(const char *camel, char *out, size_t max) {
    size_t o = 0;
    const char *p = camel;
    while (*p && o < max - 2) {
        if (*p >= 'A' && *p <= 'Z') {
            out[o++] = '-';
            if (o < max - 1) out[o++] = *p + 32;
        } else {
            out[o++] = *p;
        }
        p++;
    }
    out[o] = '\0';
}

static JSValue ts_js_style_set_property(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    { int id = ts_js_get_node_id(ctx, this_val);
      struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
      if (!n) return JS_UNDEFINED;
      { const char *prop = JS_ToCString(ctx, argv[0]);
        const char *val = JS_ToCString(ctx, argv[1]);
        if (prop && val) {
            /* Add/update in inline_style array */
            int i, found = 0;
            for (i = 0; i < n->inline_style_count; i++) {
                if (strcmp(n->inline_style[i].name, prop) == 0) {
                    strncpy(n->inline_style[i].value, val,
                            sizeof(n->inline_style[i].value) - 1);
                    n->inline_style[i].value[sizeof(n->inline_style[i].value)-1] = '\0';
                    found = 1;
                    break;
                }
            }
            if (!found && n->inline_style_count < 16) {
                strncpy(n->inline_style[n->inline_style_count].name, prop,
                        sizeof(n->inline_style[0].name) - 1);
                n->inline_style[n->inline_style_count].name[sizeof(n->inline_style[0].name)-1] = '\0';
                strncpy(n->inline_style[n->inline_style_count].value, val,
                        sizeof(n->inline_style[0].value) - 1);
                n->inline_style[n->inline_style_count].value[sizeof(n->inline_style[0].value)-1] = '\0';
                n->inline_style_count++;
            }
            g_dom->dirty = 1;
        }
        if (prop) JS_FreeCString(ctx, prop);
        if (val) JS_FreeCString(ctx, val);
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_style_get_property(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    { int id = ts_js_get_node_id(ctx, this_val);
      struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
      if (!n) return JS_NewString(ctx, "");
      { const char *prop = JS_ToCString(ctx, argv[0]);
        if (!prop) return JS_NewString(ctx, "");
        { int i;
          for (i = 0; i < n->inline_style_count; i++) {
              if (strcmp(n->inline_style[i].name, prop) == 0) {
                  JSValue result = JS_NewString(ctx, n->inline_style[i].value);
                  JS_FreeCString(ctx, prop);
                  return result;
              }
          }
          JS_FreeCString(ctx, prop);
        }
      }
    }
    return JS_NewString(ctx, "");
}

static JSValue ts_js_style_remove_property(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    { int id = ts_js_get_node_id(ctx, this_val);
      struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
      if (!n) return JS_NewString(ctx, "");
      { const char *prop = JS_ToCString(ctx, argv[0]);
        if (!prop) return JS_NewString(ctx, "");
        { int i;
          for (i = 0; i < n->inline_style_count; i++) {
              if (strcmp(n->inline_style[i].name, prop) == 0) {
                  JSValue old_val = JS_NewString(ctx, n->inline_style[i].value);
                  /* Shift remaining */
                  memmove(&n->inline_style[i], &n->inline_style[i + 1],
                          (size_t)(n->inline_style_count - i - 1) * sizeof(n->inline_style[0]));
                  n->inline_style_count--;
                  g_dom->dirty = 1;
                  JS_FreeCString(ctx, prop);
                  return old_val;
              }
          }
          JS_FreeCString(ctx, prop);
        }
      }
    }
    return JS_NewString(ctx, "");
}

static JSValue ts_js_style_get_csstext(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_NewString(ctx, "");
    { char buf[2048]; size_t pos = 0; int i;
      for (i = 0; i < n->inline_style_count && pos < sizeof(buf) - 64; i++) {
          int wrote = snprintf(buf + pos, sizeof(buf) - pos, "%s: %s; ",
                                n->inline_style[i].name, n->inline_style[i].value);
          if (wrote > 0) pos += (size_t)wrote;
      }
      return JS_NewStringLen(ctx, buf, pos);
    }
}

/* Build style sub-object for a node wrapper */
static void ts_js__bind_style(JSContext *ctx, JSValue obj, int node_id) {
    JSValue st = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, st, "__node_id", JS_NewInt32(ctx, node_id));
    JS_SetPropertyStr(ctx, st, "setProperty",
        JS_NewCFunction(ctx, ts_js_style_set_property, "setProperty", 2));
    JS_SetPropertyStr(ctx, st, "getPropertyValue",
        JS_NewCFunction(ctx, ts_js_style_get_property, "getPropertyValue", 1));
    JS_SetPropertyStr(ctx, st, "removeProperty",
        JS_NewCFunction(ctx, ts_js_style_remove_property, "removeProperty", 1));
    JS_SetPropertyStr(ctx, st, "cssText",
        JS_NewCFunction(ctx, ts_js_style_get_csstext, "cssText", 0));
    /* Populate current inline style values as plain properties for read access */
    { struct ts_dom_node *n = ts_dom_get_node(g_dom, node_id);
      if (n) { int i;
        for (i = 0; i < n->inline_style_count; i++) {
            /* Set both kebab-case and camelCase versions */
            JS_SetPropertyStr(ctx, st, n->inline_style[i].name,
                JS_NewString(ctx, n->inline_style[i].value));
            { char camel[128]; const char *p = n->inline_style[i].name;
              size_t o = 0; int next_upper = 0;
              while (*p && o < sizeof(camel) - 1) {
                  if (*p == '-') next_upper = 1;
                  else { camel[o++] = next_upper ? ((*p >= 'a' && *p <= 'z') ? *p-32 : *p) : *p; next_upper = 0; }
                  p++;
              }
              camel[o] = '\0';
              if (strcmp(camel, n->inline_style[i].name) != 0)
                  JS_SetPropertyStr(ctx, st, camel,
                      JS_NewString(ctx, n->inline_style[i].value));
            }
        }
      }
    }
    JS_SetPropertyStr(ctx, obj, "style", st);
}

/* ================================================================== */
/* Step 9: closest() and matches()                                     */
/* ================================================================== */

static JSValue ts_js_elem_matches(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    { int id = ts_js_get_node_id(ctx, this_val);
      const char *sel = JS_ToCString(ctx, argv[0]);
      if (!sel) return JS_FALSE;
      { int result = ts_dom_matches_selector(g_dom, id, sel);
        JS_FreeCString(ctx, sel);
        return result ? JS_TRUE : JS_FALSE;
      }
    }
}

static JSValue ts_js_elem_closest(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    { int id = ts_js_get_node_id(ctx, this_val);
      const char *sel = JS_ToCString(ctx, argv[0]);
      if (!sel) return JS_NULL;
      { int cur = id;
        while (cur >= 0) {
            if (ts_dom_matches_selector(g_dom, cur, sel)) {
                JS_FreeCString(ctx, sel);
                { JSValue wrapper = ts_js_wrap_node(ctx, g_dom, cur);
                  ts_dom_bind_node_methods(ctx, wrapper);
                  return wrapper;
                }
            }
            cur = g_dom->nodes[cur].parent;
        }
        JS_FreeCString(ctx, sel);
      }
    }
    return JS_NULL;
}

/* ================================================================== */
/* Step 9: element.remove(), replaceWith(), before(), after(),         */
/*         append(), prepend()                                         */
/* ================================================================== */

static JSValue ts_js_elem_remove(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->parent < 0) return JS_UNDEFINED;
    ts_dom_remove_child(g_dom, n->parent, id);
    return JS_UNDEFINED;
}

static JSValue ts_js_elem_replace_with(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->parent < 0) return JS_UNDEFINED;
    { int parent_id = n->parent; int i;
      /* Insert all args before self */
      for (i = 0; i < argc; i++) {
          int arg_id = ts_js_get_node_id(ctx, argv[i]);
          if (arg_id >= 0) ts_dom_insert_before(g_dom, parent_id, arg_id, id);
      }
      /* Remove self */
      ts_dom_remove_child(g_dom, parent_id, id);
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_elem_before(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->parent < 0) return JS_UNDEFINED;
    { int i;
      for (i = 0; i < argc; i++) {
          int arg_id = ts_js_get_node_id(ctx, argv[i]);
          if (arg_id >= 0) ts_dom_insert_before(g_dom, n->parent, arg_id, id);
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_elem_after(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || n->parent < 0) return JS_UNDEFINED;
    { int ref = n->next_sibling; int i;
      for (i = 0; i < argc; i++) {
          int arg_id = ts_js_get_node_id(ctx, argv[i]);
          if (arg_id < 0) continue;
          if (ref >= 0)
              ts_dom_insert_before(g_dom, n->parent, arg_id, ref);
          else
              ts_dom_append_child(g_dom, n->parent, arg_id);
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_elem_append(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    int id = ts_js_get_node_id(ctx, this_val);
    { int i;
      for (i = 0; i < argc; i++) {
          int arg_id = ts_js_get_node_id(ctx, argv[i]);
          if (arg_id >= 0) ts_dom_append_child(g_dom, id, arg_id);
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_elem_prepend(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n) return JS_UNDEFINED;
    { int ref = n->first_child; int i;
      for (i = 0; i < argc; i++) {
          int arg_id = ts_js_get_node_id(ctx, argv[i]);
          if (arg_id < 0) continue;
          if (ref >= 0)
              ts_dom_insert_before(g_dom, id, arg_id, ref);
          else
              ts_dom_append_child(g_dom, id, arg_id);
      }
    }
    return JS_UNDEFINED;
}

/* ================================================================== */
/* Step 9: Deep cloneNode                                              */
/* ================================================================== */

static int ts_js__deep_clone(struct ts_dom_ctx *dom, int src_id) {
    struct ts_dom_node *src = ts_dom_get_node(dom, src_id);
    if (!src) return -1;
    { struct ts_dom_node *clone = ts_dom_alloc_node(dom, src->type);
      if (!clone) return -1;
      memcpy(clone->tag, src->tag, sizeof(clone->tag));
      memcpy(clone->attrs, src->attrs, sizeof(clone->attrs));
      clone->attr_count = src->attr_count;
      memcpy(clone->text, src->text, sizeof(clone->text));
      clone->text_len = src->text_len;
      memcpy(clone->inline_style, src->inline_style, sizeof(clone->inline_style));
      clone->inline_style_count = src->inline_style_count;
      /* Recursively clone children */
      { int child = src->first_child;
        while (child >= 0) {
            int child_clone = ts_js__deep_clone(dom, child);
            if (child_clone >= 0)
                ts_dom_append_child(dom, clone->id, child_clone);
            child = dom->nodes[child].next_sibling;
        }
      }
      return clone->id;
    }
}

/* ================================================================== */
/* Step 9: getBoundingClientRect with real layout values                */
/* ================================================================== */

static JSValue ts_js_elem_get_bounding_rect(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    int min_x = 999999, min_y = 999999, max_x = 0, max_y = 0;
    int found = 0;

    /* Search render nodes for ones belonging to this DOM node */
    if (g_doc_ref && id >= 0) {
        int i;
        for (i = 0; i < g_doc_ref->node_count; i++) {
            struct ts_render_node *rn = &g_doc_ref->nodes[i];
            if (rn->dom_node_id == id) {
                if (rn->x < min_x) min_x = rn->x;
                if (rn->y < min_y) min_y = rn->y;
                if (rn->x + rn->w > max_x) max_x = rn->x + rn->w;
                if (rn->y + rn->h > max_y) max_y = rn->y + rn->h;
                found = 1;
            }
        }
        /* Also check boxes for this DOM node's box_index */
        if (!found) {
            for (i = 0; i < g_doc_ref->node_count; i++) {
                struct ts_render_node *rn = &g_doc_ref->nodes[i];
                if (rn->dom_node_id == id && rn->style.box_index >= 0 &&
                    rn->style.box_index < g_doc_ref->box_count) {
                    struct ts_block_box *box = &g_doc_ref->boxes[rn->style.box_index];
                    min_x = box->x; min_y = box->y;
                    max_x = box->x + box->total_w;
                    max_y = box->y + box->total_h;
                    found = 1;
                    break;
                }
            }
        }
    }

    if (!found) { min_x = 0; min_y = 0; max_x = 0; max_y = 0; }

    { JSValue obj = JS_NewObject(ctx);
      int w = max_x - min_x, h = max_y - min_y;
      JS_SetPropertyStr(ctx, obj, "x", JS_NewInt32(ctx, min_x));
      JS_SetPropertyStr(ctx, obj, "y", JS_NewInt32(ctx, min_y));
      JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
      JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
      JS_SetPropertyStr(ctx, obj, "top", JS_NewInt32(ctx, min_y));
      JS_SetPropertyStr(ctx, obj, "right", JS_NewInt32(ctx, max_x));
      JS_SetPropertyStr(ctx, obj, "bottom", JS_NewInt32(ctx, max_y));
      JS_SetPropertyStr(ctx, obj, "left", JS_NewInt32(ctx, min_x));
      return obj;
    }
}

/* ================================================================== */
/* Element layout properties — real values from render tree            */
/* ================================================================== */

/* Helper: get bounding box for a DOM node from the render tree */
static void ts_js__get_node_rect(int dom_id, int *out_x, int *out_y,
                                  int *out_w, int *out_h) {
    int min_x = 0, min_y = 0, max_x = 0, max_y = 0, found = 0;
    if (g_doc_ref && dom_id >= 0) {
        int i;
        for (i = 0; i < g_doc_ref->node_count; i++) {
            struct ts_render_node *rn = &g_doc_ref->nodes[i];
            if (rn->dom_node_id == dom_id) {
                if (!found || rn->x < min_x) min_x = rn->x;
                if (!found || rn->y < min_y) min_y = rn->y;
                if (rn->x + rn->w > max_x) max_x = rn->x + rn->w;
                if (rn->y + rn->h > max_y) max_y = rn->y + rn->h;
                found = 1;
            }
        }
        if (!found) {
            for (i = 0; i < g_doc_ref->node_count; i++) {
                struct ts_render_node *rn = &g_doc_ref->nodes[i];
                if (rn->dom_node_id == dom_id && rn->style.box_index >= 0 &&
                    rn->style.box_index < g_doc_ref->box_count) {
                    struct ts_block_box *box = &g_doc_ref->boxes[rn->style.box_index];
                    min_x = box->x; min_y = box->y;
                    max_x = box->x + box->total_w;
                    max_y = box->y + box->total_h;
                    found = 1;
                    break;
                }
            }
        }
    }
    *out_x = min_x; *out_y = min_y;
    *out_w = max_x - min_x; *out_h = max_y - min_y;
}

/* offsetWidth / offsetHeight / offsetLeft / offsetTop */
static JSValue ts_js_elem_get_offset_width(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    ts_js__get_node_rect(ts_js_get_node_id(ctx, this_val), &x, &y, &w, &h);
    return JS_NewInt32(ctx, w);
}
static JSValue ts_js_elem_get_offset_height(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    ts_js__get_node_rect(ts_js_get_node_id(ctx, this_val), &x, &y, &w, &h);
    return JS_NewInt32(ctx, h);
}
static JSValue ts_js_elem_get_offset_left(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    ts_js__get_node_rect(ts_js_get_node_id(ctx, this_val), &x, &y, &w, &h);
    return JS_NewInt32(ctx, x);
}
static JSValue ts_js_elem_get_offset_top(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    ts_js__get_node_rect(ts_js_get_node_id(ctx, this_val), &x, &y, &w, &h);
    return JS_NewInt32(ctx, y);
}
/* clientWidth / clientHeight (same as offset minus border, approximate with offset) */
static JSValue ts_js_elem_get_client_width(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    ts_js__get_node_rect(ts_js_get_node_id(ctx, this_val), &x, &y, &w, &h);
    return JS_NewInt32(ctx, w);
}
static JSValue ts_js_elem_get_client_height(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    ts_js__get_node_rect(ts_js_get_node_id(ctx, this_val), &x, &y, &w, &h);
    return JS_NewInt32(ctx, h);
}
/* scrollWidth / scrollHeight (approximate: same as client for now) */
static JSValue ts_js_elem_get_scroll_width(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    ts_js__get_node_rect(ts_js_get_node_id(ctx, this_val), &x, &y, &w, &h);
    return JS_NewInt32(ctx, w > 0 ? w : 900);
}
static JSValue ts_js_elem_get_scroll_height(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    ts_js__get_node_rect(ts_js_get_node_id(ctx, this_val), &x, &y, &w, &h);
    return JS_NewInt32(ctx, h > 0 ? h : 548);
}
/* scrollTop / scrollLeft — return 0 (no scroll state tracked yet) */
static JSValue ts_js_elem_get_scroll_zero(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)argc; (void)argv; (void)this_val;
    return JS_NewInt32(ctx, 0);
}
/* clientTop / clientLeft (border widths — approximate as 0) */
/* offsetParent — return parentNode */
static JSValue ts_js_elem_get_offset_parent(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (n && n->parent >= 0) {
        JSValue p = ts_js_wrap_node(ctx, g_dom, n->parent);
        ts_dom_bind_node_methods(ctx, p);
        return p;
    }
    return JS_NULL;
}
/* scrollIntoView — no-op */
static JSValue ts_js_elem_scroll_into_view(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}
/* focus / blur — no-op */
static JSValue ts_js_elem_noop(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}
/* getAttributeNames */
static JSValue ts_js_elem_get_attribute_names(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    JSValue arr = JS_NewArray(ctx);
    if (n) {
        int i;
        for (i = 0; i < n->attr_count; i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                JS_NewString(ctx, n->attrs[i].name));
    }
    return arr;
}
/* toggleAttribute */
static JSValue ts_js_elem_toggle_attribute(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    { const char *name = JS_ToCString(ctx, argv[0]);
      int id = ts_js_get_node_id(ctx, this_val);
      struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
      int result = 0;
      if (name && n) {
          int i, found = -1;
          for (i = 0; i < n->attr_count; i++) {
              if (strcmp(n->attrs[i].name, name) == 0) { found = i; break; }
          }
          if (argc >= 2) {
              int force = JS_ToBool(ctx, argv[1]);
              if (force && found < 0 && n->attr_count < TS_DOM_MAX_ATTRS) {
                  strncpy(n->attrs[n->attr_count].name, name, TS_DOM_ATTR_NAME_MAX - 1);
                  n->attrs[n->attr_count].value[0] = '\0';
                  n->attr_count++;
                  result = 1;
              } else if (!force && found >= 0) {
                  for (i = found; i < n->attr_count - 1; i++) n->attrs[i] = n->attrs[i + 1];
                  n->attr_count--;
                  result = 0;
              } else {
                  result = (found >= 0) ? 1 : 0;
              }
          } else {
              if (found >= 0) {
                  for (i = found; i < n->attr_count - 1; i++) n->attrs[i] = n->attrs[i + 1];
                  n->attr_count--;
                  result = 0;
              } else if (n->attr_count < TS_DOM_MAX_ATTRS) {
                  strncpy(n->attrs[n->attr_count].name, name, TS_DOM_ATTR_NAME_MAX - 1);
                  n->attrs[n->attr_count].value[0] = '\0';
                  n->attr_count++;
                  result = 1;
              }
          }
      }
      if (name) JS_FreeCString(ctx, name);
      return JS_NewBool(ctx, result);
    }
}
/* Node.compareDocumentPosition */
static JSValue ts_js_node_compare_doc_pos(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewInt32(ctx, 0);
    { int id_a = ts_js_get_node_id(ctx, this_val);
      int id_b = ts_js_get_node_id(ctx, argv[0]);
      if (id_a == id_b) return JS_NewInt32(ctx, 0);
      /* Walk up from B looking for A → A contains B (16+4=20) */
      { struct ts_dom_node *nb = ts_dom_get_node(g_dom, id_b);
        while (nb && nb->parent >= 0) {
            if (nb->parent == id_a) return JS_NewInt32(ctx, 20); /* CONTAINS|FOLLOWING */
            nb = ts_dom_get_node(g_dom, nb->parent);
        }
      }
      /* Walk up from A looking for B → B contains A (8+2=10) */
      { struct ts_dom_node *na = ts_dom_get_node(g_dom, id_a);
        while (na && na->parent >= 0) {
            if (na->parent == id_b) return JS_NewInt32(ctx, 10); /* CONTAINED_BY|PRECEDING */
            na = ts_dom_get_node(g_dom, na->parent);
        }
      }
      return JS_NewInt32(ctx, id_a < id_b ? 4 : 2); /* FOLLOWING or PRECEDING */
    }
}
/* Node.isEqualNode / isSameNode */
static JSValue ts_js_node_is_same_node(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, ts_js_get_node_id(ctx, this_val) ==
                           ts_js_get_node_id(ctx, argv[0]));
}
/* Node.normalize (merge adjacent text nodes — no-op for now) */
/* Node.getRootNode */
static JSValue ts_js_node_get_root(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    while (n && n->parent >= 0) {
        id = n->parent;
        n = ts_dom_get_node(g_dom, id);
    }
    if (n) { JSValue w = ts_js_wrap_node(ctx, g_dom, id); ts_dom_bind_node_methods(ctx, w); return w; }
    return JS_NULL;
}

/* document.getElementsByClassName */
static JSValue ts_js_doc_get_elements_by_class(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !g_dom) return JS_NewArray(ctx);
    { const char *cls = JS_ToCString(ctx, argv[0]);
      JSValue arr = JS_NewArray(ctx);
      int count = 0, i;
      if (!cls) return arr;
      for (i = 0; i < TS_DOM_MAX_NODES; i++) {
          struct ts_dom_node *n = &g_dom->nodes[i];
          if (!n->used || n->type != TS_DOM_ELEMENT_NODE) continue;
          /* Check class attribute */
          { int j;
            for (j = 0; j < n->attr_count; j++) {
                if (strcmp(n->attrs[j].name, "class") == 0) {
                    /* Check if cls is in the space-separated class list */
                    const char *p = n->attrs[j].value;
                    size_t clen = strlen(cls);
                    while (*p) {
                        while (*p == ' ') p++;
                        if (!*p) break;
                        { const char *start = p;
                          while (*p && *p != ' ') p++;
                          if ((size_t)(p - start) == clen && strncmp(start, cls, clen) == 0) {
                              JSValue el = ts_js_wrap_node(ctx, g_dom, i);
                              ts_dom_bind_node_methods(ctx, el);
                              JS_SetPropertyUint32(ctx, arr, (uint32_t)count++, el);
                              break;
                          }
                        }
                    }
                    break;
                }
            }
          }
      }
      JS_FreeCString(ctx, cls);
      /* length property */
      JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, count));
      return arr;
    }
}

/* document.createEvent */
static JSValue ts_js_doc_create_event(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, ev, "bubbles", JS_FALSE);
    JS_SetPropertyStr(ctx, ev, "cancelable", JS_FALSE);
    JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_FALSE);
    /* initEvent method */
    { const char *code = "(function(type,bubbles,cancelable){"
                         "this.type=type;this.bubbles=!!bubbles;this.cancelable=!!cancelable})";
      JSValue fn = JS_Eval(ctx, code, strlen(code), "<initEvent>", JS_EVAL_TYPE_GLOBAL);
      JS_SetPropertyStr(ctx, ev, "initEvent", fn);
    }
    JS_SetPropertyStr(ctx, ev, "preventDefault",
        JS_Eval(ctx, "(function(){this.defaultPrevented=true})",
                39, "<ep>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, ev, "stopPropagation",
        JS_Eval(ctx, "(function(){})", 14, "<sp>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, ev, "stopImmediatePropagation",
        JS_Eval(ctx, "(function(){})", 14, "<sip>", JS_EVAL_TYPE_GLOBAL));
    return ev;
}

/* crypto.getRandomValues — uses kernel RNG */
static JSValue ts_js_crypto_get_random_values(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    { JSValue arr = argv[0];
      JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
      int32_t len = 0;
      JS_ToInt32(ctx, &len, len_val);
      JS_FreeValue(ctx, len_val);
      if (len > 0 && len <= 65536) {
          unsigned char *buf = (unsigned char *)malloc((size_t)len);
          if (buf) {
              /* Use kernel random source */
              fry_getrandom(buf, (size_t)len, 0);
              { int i;
                for (i = 0; i < len; i++)
                    JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                        JS_NewInt32(ctx, buf[i]));
              }
              free(buf);
          }
      }
      return JS_DupValue(ctx, arr);
    }
}

/* ================================================================== */
/* Step 9: window.getComputedStyle with real values                    */
/* ================================================================== */

static JSValue ts_js_get_computed_style(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewObject(ctx);
    { int id = ts_js_get_node_id(ctx, argv[0]);
      struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
      JSValue obj = JS_NewObject(ctx);
      if (!n) return obj;

      /* Start with inline styles */
      { int i;
        for (i = 0; i < n->inline_style_count; i++) {
            JS_SetPropertyStr(ctx, obj, n->inline_style[i].name,
                JS_NewString(ctx, n->inline_style[i].value));
            /* Also set camelCase version */
            { char camel[128]; const char *p = n->inline_style[i].name;
              size_t o = 0; int next_upper = 0;
              while (*p && o < sizeof(camel) - 1) {
                  if (*p == '-') next_upper = 1;
                  else { camel[o++] = next_upper ? ((*p >= 'a' && *p <= 'z') ? *p-32 : *p) : *p; next_upper = 0; }
                  p++;
              }
              camel[o] = '\0';
              if (strcmp(camel, n->inline_style[i].name) != 0)
                  JS_SetPropertyStr(ctx, obj, camel,
                      JS_NewString(ctx, n->inline_style[i].value));
            }
        }
      }

      /* Overlay stylesheet rules (if g_doc_ref available) */
      if (g_doc_ref && n->type == TS_DOM_ELEMENT_NODE) {
          const char *cls = ts_dom_get_attr(n, "class");
          const char *id_attr = ts_dom_get_attr(n, "id");
          int ri;
          { int __rc = g_doc_ref->stylesheet.rule_count;
            if (__rc < 0 || __rc > TS_CSS_MAX_RULES) __rc = 0;
          for (ri = 0; ri < __rc; ri++) {
              struct ts_css_rule *rule = &g_doc_ref->stylesheet.rules[ri];
              int si, __sc = rule->selector_count;
              if (__sc < 0 || __sc > TS_CSS_MAX_SELECTORS) __sc = 0;
              for (si = 0; si < __sc; si++) {
                  struct ts_css_selector *sel = &rule->selectors[si];
                  if (sel->part_count > 0 && sel->part_count <= TS_CSS_MAX_SELECTOR_PARTS) {
                      struct ts_css_selector_part *last = &sel->parts[sel->part_count - 1];
                      /* Simple check: match last part against this element */
                      if (ts_css_match_part(last, n->tag,
                              cls ? cls : "", id_attr ? id_attr : "")) {
                          int pi;
                          for (pi = 0; pi < rule->prop_count; pi++) {
                              JS_SetPropertyStr(ctx, obj, rule->props[pi].name,
                                  JS_NewString(ctx, rule->props[pi].value));
                              /* camelCase version */
                              { char camel[128]; const char *p = rule->props[pi].name;
                                size_t o = 0; int next_upper = 0;
                                while (*p && o < sizeof(camel) - 1) {
                                    if (*p == '-') next_upper = 1;
                                    else { camel[o++] = next_upper ? ((*p >= 'a' && *p <= 'z') ? *p-32 : *p) : *p; next_upper = 0; }
                                    p++;
                                }
                                camel[o] = '\0';
                                if (strcmp(camel, rule->props[pi].name) != 0)
                                    JS_SetPropertyStr(ctx, obj, camel,
                                        JS_NewString(ctx, rule->props[pi].value));
                              }
                          }
                          break;
                      }
                  }
              }
          }
          } /* end __rc scope */
      }

      /* Add getPropertyValue method for spec compatibility */
      { /* Use a closure-like approach: store computed values on obj, then add method */
        const char *gpv_code =
            "(function getPropertyValue(name){"
            "return this[name]||''"
            "})";
        JSValue gpv = JS_Eval(ctx, gpv_code, strlen(gpv_code), "<gpv>",
                               JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, obj, "getPropertyValue", gpv);
      }
      return obj;
    }
}

/* ================================================================== */
/* Document methods                                                    */
/* ================================================================== */

static JSValue ts_js_doc_create_element(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_NULL;

    struct ts_dom_node *elem = ts_dom_alloc_node(g_dom, TS_DOM_ELEMENT_NODE);
    if (!elem) { JS_FreeCString(ctx, tag); return JS_NULL; }

    /* Lowercase tag name */
    {
        size_t i;
        for (i = 0; tag[i] && i < TS_DOM_TAG_MAX - 1; i++)
            elem->tag[i] = (tag[i] >= 'A' && tag[i] <= 'Z') ? tag[i] + 32 : tag[i];
        elem->tag[i] = '\0';
    }
    JS_FreeCString(ctx, tag);

    JSValue wrapper = ts_js_wrap_node(ctx, g_dom, elem->id);
    ts_dom_bind_node_methods(ctx, wrapper);
    /* Call webcomp enhance hook (adds attachShadow, template.content etc.) */
    if (g_dom->on_element_created)
        g_dom->on_element_created(ctx, wrapper, elem->tag);
    return wrapper;
}

static JSValue ts_js_doc_create_text_node(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_NULL;

    struct ts_dom_node *node = ts_dom_alloc_node(g_dom, TS_DOM_TEXT_NODE);
    if (!node) { JS_FreeCString(ctx, text); return JS_NULL; }

    ts_dom_set_text(node, text, (int)strlen(text));
    JS_FreeCString(ctx, text);

    JSValue wrapper = ts_js_wrap_node(ctx, g_dom, node->id);
    ts_dom_bind_node_methods(ctx, wrapper);
    return wrapper;
}

static JSValue ts_js_doc_create_comment(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    struct ts_dom_node *node = ts_dom_alloc_node(g_dom, TS_DOM_COMMENT_NODE);
    if (!node) return JS_NULL;
    if (argc > 0) {
        const char *text = JS_ToCString(ctx, argv[0]);
        if (text) {
            ts_dom_set_text(node, text, (int)strlen(text));
            JS_FreeCString(ctx, text);
        }
    }
    return ts_js_wrap_node(ctx, g_dom, node->id);
}

static JSValue ts_js_doc_create_fragment(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    struct ts_dom_node *node = ts_dom_alloc_node(g_dom, TS_DOM_FRAGMENT_NODE);
    if (!node) return JS_NULL;
    JSValue wrapper = ts_js_wrap_node(ctx, g_dom, node->id);
    ts_dom_bind_node_methods(ctx, wrapper);
    return wrapper;
}

static JSValue ts_js_doc_get_element_by_id(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    int result = ts_dom_get_element_by_id(g_dom, 0, id);
    JS_FreeCString(ctx, id);
    if (result < 0) return JS_NULL;
    JSValue wrapper = ts_js_wrap_node(ctx, g_dom, result);
    ts_dom_bind_node_methods(ctx, wrapper);
    return wrapper;
}

static JSValue ts_js_doc_get_elements_by_tag(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewArray(ctx);
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_NewArray(ctx);
    /* Lowercase */
    char lower[64];
    { size_t i; for (i = 0; tag[i] && i < 63; i++) lower[i] = (tag[i] >= 'A' && tag[i] <= 'Z') ? tag[i]+32 : tag[i]; lower[i] = 0; }
    int results[256];
    int count = ts_dom_get_elements_by_tag(g_dom, lower, results, 256);
    JS_FreeCString(ctx, tag);
    JSValue arr = JS_NewArray(ctx);
    int i;
    for (i = 0; i < count; i++) {
        JSValue w = ts_js_wrap_node(ctx, g_dom, results[i]);
        ts_dom_bind_node_methods(ctx, w);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, w);
    }
    return arr;
}

/* ================================================================== */
/* Window methods                                                      */
/* ================================================================== */

static JSValue ts_js_window_set_timeout(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, -1);
    uint64_t delay = 0;
    if (argc > 1) { int32_t d; JS_ToInt32(ctx, &d, argv[1]); delay = (uint64_t)(d > 0 ? d : 0); }
    int id = ts_dom_set_timer(g_dom, argv[0], delay, 0);
    return JS_NewInt32(ctx, id);
}

static JSValue ts_js_window_set_interval(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, -1);
    uint64_t interval = 10;
    if (argc > 1) { int32_t d; JS_ToInt32(ctx, &d, argv[1]); interval = (uint64_t)(d > 0 ? d : 10); }
    int id = ts_dom_set_timer(g_dom, argv[0], interval, interval);
    return JS_NewInt32(ctx, id);
}

static JSValue ts_js_window_clear_timeout(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id; JS_ToInt32(ctx, &id, argv[0]);
    ts_dom_clear_timer(g_dom, id);
    return JS_UNDEFINED;
}

static JSValue ts_js_window_request_anim_frame(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
    /* requestAnimationFrame = setTimeout(fn, 16) — ~60fps */
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, -1);
    int id = ts_dom_set_timer(g_dom, argv[0], 16, 0);
    return JS_NewInt32(ctx, id);
}

/* ================================================================== */
/* Console methods                                                     */
/* ================================================================== */

static JSValue ts_js_console_log(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    int i;
    for (i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            ts_dom_console_append(g_dom, "[LOG] ", str);
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_console_warn(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val;
    int i;
    for (i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            ts_dom_console_append(g_dom, "[WRN] ", str);
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_console_error(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    int i;
    for (i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            ts_dom_console_append(g_dom, "[ERR] ", str);
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

/* ================================================================== */
/* Bind methods to a node wrapper object                               */
/* ================================================================== */

static void ts_dom_bind_node_methods(JSContext *ctx, JSValue obj) {
    /* Node properties (getter functions) */
    JS_SetPropertyStr(ctx, obj, "parentNode",
        JS_NewCFunction(ctx, ts_js_node_get_parent, "parentNode", 0));
    JS_SetPropertyStr(ctx, obj, "parentElement",
        JS_NewCFunction(ctx, ts_js_node_get_parent, "parentElement", 0));
    JS_SetPropertyStr(ctx, obj, "firstChild",
        JS_NewCFunction(ctx, ts_js_node_get_first_child, "firstChild", 0));
    JS_SetPropertyStr(ctx, obj, "lastChild",
        JS_NewCFunction(ctx, ts_js_node_get_last_child, "lastChild", 0));
    JS_SetPropertyStr(ctx, obj, "nextSibling",
        JS_NewCFunction(ctx, ts_js_node_get_next_sibling, "nextSibling", 0));
    JS_SetPropertyStr(ctx, obj, "previousSibling",
        JS_NewCFunction(ctx, ts_js_node_get_prev_sibling, "previousSibling", 0));
    JS_SetPropertyStr(ctx, obj, "children",
        JS_NewCFunction(ctx, ts_js_node_get_children, "children", 0));
    JS_SetPropertyStr(ctx, obj, "childNodes",
        JS_NewCFunction(ctx, ts_js_node_get_child_nodes, "childNodes", 0));
    /* textContent: getter/setter pair */
    { JSAtom atom = JS_NewAtom(ctx, "textContent");
      JS_DefinePropertyGetSet(ctx, obj, atom,
          JS_NewCFunction(ctx, ts_js_node_get_text_content, "get textContent", 0),
          JS_NewCFunction(ctx, ts_js_node_set_text_content, "set textContent", 1),
          JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
      JS_FreeAtom(ctx, atom);
    }

    /* Node methods */
    JS_SetPropertyStr(ctx, obj, "appendChild",
        JS_NewCFunction(ctx, ts_js_node_append_child, "appendChild", 1));
    JS_SetPropertyStr(ctx, obj, "removeChild",
        JS_NewCFunction(ctx, ts_js_node_remove_child, "removeChild", 1));
    JS_SetPropertyStr(ctx, obj, "insertBefore",
        JS_NewCFunction(ctx, ts_js_node_insert_before, "insertBefore", 2));
    JS_SetPropertyStr(ctx, obj, "replaceChild",
        JS_NewCFunction(ctx, ts_js_node_replace_child, "replaceChild", 2));
    JS_SetPropertyStr(ctx, obj, "contains",
        JS_NewCFunction(ctx, ts_js_node_contains, "contains", 1));
    JS_SetPropertyStr(ctx, obj, "cloneNode",
        JS_NewCFunction(ctx, ts_js_node_clone_node, "cloneNode", 1));

    /* Element methods */
    JS_SetPropertyStr(ctx, obj, "getAttribute",
        JS_NewCFunction(ctx, ts_js_elem_get_attribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, obj, "setAttribute",
        JS_NewCFunction(ctx, ts_js_elem_set_attribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, obj, "removeAttribute",
        JS_NewCFunction(ctx, ts_js_elem_remove_attribute, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, obj, "hasAttribute",
        JS_NewCFunction(ctx, ts_js_elem_has_attribute, "hasAttribute", 1));
    JS_SetPropertyStr(ctx, obj, "addEventListener",
        JS_NewCFunction(ctx, ts_js_elem_add_event_listener, "addEventListener", 3));
    JS_SetPropertyStr(ctx, obj, "removeEventListener",
        JS_NewCFunction(ctx, ts_js_elem_remove_event_listener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, obj, "querySelector",
        JS_NewCFunction(ctx, ts_js_elem_query_selector, "querySelector", 1));
    JS_SetPropertyStr(ctx, obj, "querySelectorAll",
        JS_NewCFunction(ctx, ts_js_elem_query_selector_all, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, obj, "getBoundingClientRect",
        JS_NewCFunction(ctx, ts_js_elem_get_bounding_rect, "getBoundingClientRect", 0));

    /* Element properties: getter/setter pairs via JS_DefinePropertyGetSet */
    /* id */
    { JSAtom atom = JS_NewAtom(ctx, "id");
      JS_DefinePropertyGetSet(ctx, obj, atom,
          JS_NewCFunction(ctx, ts_js_elem_get_id, "get id", 0),
          JS_NewCFunction(ctx, ts_js_elem_set_id, "set id", 1),
          JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
      JS_FreeAtom(ctx, atom);
    }
    /* className */
    { JSAtom atom = JS_NewAtom(ctx, "className");
      JS_DefinePropertyGetSet(ctx, obj, atom,
          JS_NewCFunction(ctx, ts_js_elem_get_class_name, "get className", 0),
          JS_NewCFunction(ctx, ts_js_elem_set_class_name, "set className", 1),
          JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
      JS_FreeAtom(ctx, atom);
    }
    /* innerHTML */
    { JSAtom atom = JS_NewAtom(ctx, "innerHTML");
      JS_DefinePropertyGetSet(ctx, obj, atom,
          JS_NewCFunction(ctx, ts_js_elem_get_inner_html, "get innerHTML", 0),
          JS_NewCFunction(ctx, ts_js_elem_set_inner_html, "set innerHTML", 1),
          JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
      JS_FreeAtom(ctx, atom);
    }
    /* outerHTML */
    { JSAtom atom = JS_NewAtom(ctx, "outerHTML");
      JS_DefinePropertyGetSet(ctx, obj, atom,
          JS_NewCFunction(ctx, ts_js_elem_get_outer_html, "get outerHTML", 0),
          JS_NewCFunction(ctx, ts_js_elem_set_outer_html, "set outerHTML", 1),
          JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
      JS_FreeAtom(ctx, atom);
    }

    /* Step 9: closest / matches */
    JS_SetPropertyStr(ctx, obj, "closest",
        JS_NewCFunction(ctx, ts_js_elem_closest, "closest", 1));
    JS_SetPropertyStr(ctx, obj, "matches",
        JS_NewCFunction(ctx, ts_js_elem_matches, "matches", 1));

    /* Step 9: DOM mutation convenience methods */
    JS_SetPropertyStr(ctx, obj, "remove",
        JS_NewCFunction(ctx, ts_js_elem_remove, "remove", 0));
    JS_SetPropertyStr(ctx, obj, "replaceWith",
        JS_NewCFunction(ctx, ts_js_elem_replace_with, "replaceWith", 1));
    JS_SetPropertyStr(ctx, obj, "before",
        JS_NewCFunction(ctx, ts_js_elem_before, "before", 1));
    JS_SetPropertyStr(ctx, obj, "after",
        JS_NewCFunction(ctx, ts_js_elem_after, "after", 1));
    JS_SetPropertyStr(ctx, obj, "append",
        JS_NewCFunction(ctx, ts_js_elem_append, "append", 1));
    JS_SetPropertyStr(ctx, obj, "prepend",
        JS_NewCFunction(ctx, ts_js_elem_prepend, "prepend", 1));

    /* Step 9: dataset */
    JS_SetPropertyStr(ctx, obj, "dataset",
        JS_NewCFunction(ctx, ts_js_elem_get_dataset, "dataset", 0));

    /* Element layout properties — real values from render tree */
    JS_SetPropertyStr(ctx, obj, "offsetWidth",
        JS_NewCFunction(ctx, ts_js_elem_get_offset_width, "offsetWidth", 0));
    JS_SetPropertyStr(ctx, obj, "offsetHeight",
        JS_NewCFunction(ctx, ts_js_elem_get_offset_height, "offsetHeight", 0));
    JS_SetPropertyStr(ctx, obj, "offsetLeft",
        JS_NewCFunction(ctx, ts_js_elem_get_offset_left, "offsetLeft", 0));
    JS_SetPropertyStr(ctx, obj, "offsetTop",
        JS_NewCFunction(ctx, ts_js_elem_get_offset_top, "offsetTop", 0));
    JS_SetPropertyStr(ctx, obj, "offsetParent",
        JS_NewCFunction(ctx, ts_js_elem_get_offset_parent, "offsetParent", 0));
    JS_SetPropertyStr(ctx, obj, "clientWidth",
        JS_NewCFunction(ctx, ts_js_elem_get_client_width, "clientWidth", 0));
    JS_SetPropertyStr(ctx, obj, "clientHeight",
        JS_NewCFunction(ctx, ts_js_elem_get_client_height, "clientHeight", 0));
    JS_SetPropertyStr(ctx, obj, "clientTop",
        JS_NewCFunction(ctx, ts_js_elem_get_scroll_zero, "clientTop", 0));
    JS_SetPropertyStr(ctx, obj, "clientLeft",
        JS_NewCFunction(ctx, ts_js_elem_get_scroll_zero, "clientLeft", 0));
    JS_SetPropertyStr(ctx, obj, "scrollWidth",
        JS_NewCFunction(ctx, ts_js_elem_get_scroll_width, "scrollWidth", 0));
    JS_SetPropertyStr(ctx, obj, "scrollHeight",
        JS_NewCFunction(ctx, ts_js_elem_get_scroll_height, "scrollHeight", 0));
    JS_SetPropertyStr(ctx, obj, "scrollTop",
        JS_NewCFunction(ctx, ts_js_elem_get_scroll_zero, "scrollTop", 0));
    JS_SetPropertyStr(ctx, obj, "scrollLeft",
        JS_NewCFunction(ctx, ts_js_elem_get_scroll_zero, "scrollLeft", 0));
    JS_SetPropertyStr(ctx, obj, "scrollIntoView",
        JS_NewCFunction(ctx, ts_js_elem_scroll_into_view, "scrollIntoView", 1));
    JS_SetPropertyStr(ctx, obj, "focus",
        JS_NewCFunction(ctx, ts_js_elem_noop, "focus", 0));
    JS_SetPropertyStr(ctx, obj, "blur",
        JS_NewCFunction(ctx, ts_js_elem_noop, "blur", 0));
    JS_SetPropertyStr(ctx, obj, "click",
        JS_NewCFunction(ctx, ts_js_elem_noop, "click", 0));
    JS_SetPropertyStr(ctx, obj, "getAttributeNames",
        JS_NewCFunction(ctx, ts_js_elem_get_attribute_names, "getAttributeNames", 0));
    JS_SetPropertyStr(ctx, obj, "toggleAttribute",
        JS_NewCFunction(ctx, ts_js_elem_toggle_attribute, "toggleAttribute", 2));
    JS_SetPropertyStr(ctx, obj, "compareDocumentPosition",
        JS_NewCFunction(ctx, ts_js_node_compare_doc_pos, "compareDocumentPosition", 1));
    JS_SetPropertyStr(ctx, obj, "isSameNode",
        JS_NewCFunction(ctx, ts_js_node_is_same_node, "isSameNode", 1));
    JS_SetPropertyStr(ctx, obj, "isEqualNode",
        JS_NewCFunction(ctx, ts_js_node_is_same_node, "isEqualNode", 1));
    JS_SetPropertyStr(ctx, obj, "getRootNode",
        JS_NewCFunction(ctx, ts_js_node_get_root, "getRootNode", 0));
    JS_SetPropertyStr(ctx, obj, "normalize",
        JS_NewCFunction(ctx, ts_js_elem_noop, "normalize", 0));
    JS_SetPropertyStr(ctx, obj, "dispatchEvent",
        JS_NewCFunction(ctx, ts_js_elem_noop, "dispatchEvent", 1));

    /* nodeType / nodeName / tagName as properties */
    { int node_id = ts_js_get_node_id(ctx, obj);
      struct ts_dom_node *dn = ts_dom_get_node(g_dom, node_id);
      if (dn) {
          JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, dn->type));
          if (dn->type == TS_DOM_ELEMENT_NODE) {
              char upper[TS_DOM_TAG_MAX];
              { const char *s = dn->tag; int k = 0;
                while (s[k] && k < TS_DOM_TAG_MAX - 1) {
                    upper[k] = (s[k] >= 'a' && s[k] <= 'z') ? s[k] - 32 : s[k]; k++;
                }
                upper[k] = '\0';
              }
              JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, upper));
              JS_SetPropertyStr(ctx, obj, "tagName", JS_NewString(ctx, upper));
              JS_SetPropertyStr(ctx, obj, "localName", JS_NewString(ctx, dn->tag));
              JS_SetPropertyStr(ctx, obj, "namespaceURI",
                  JS_NewString(ctx, "http://www.w3.org/1999/xhtml"));
          } else if (dn->type == TS_DOM_TEXT_NODE) {
              JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, "#text"));
          } else if (dn->type == TS_DOM_COMMENT_NODE) {
              JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, "#comment"));
          } else if (dn->type == TS_DOM_DOCUMENT_NODE) {
              JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, "#document"));
          }
          JS_SetPropertyStr(ctx, obj, "ownerDocument",
              JS_DupValue(ctx, g_dom->js_document));
          JS_SetPropertyStr(ctx, obj, "isConnected", JS_TRUE);
          JS_SetPropertyStr(ctx, obj, "baseURI",
              JS_NewString(ctx, g_dom->url));
      }
    }

    /* Step 9: classList + style sub-objects */
    { int node_id = ts_js_get_node_id(ctx, obj);
      if (node_id >= 0) {
          ts_js__bind_classlist(ctx, obj, node_id);
          ts_js__bind_style(ctx, obj, node_id);
          /* Step 14: Canvas getContext for <canvas> elements */
          { struct ts_dom_node *dn = ts_dom_get_node(g_dom, node_id);
            if (dn && strcmp(dn->tag, "canvas") == 0) {
                JS_SetPropertyStr(ctx, obj, "getContext",
                    JS_NewCFunction(ctx, ts_js_canvas_get_context, "getContext", 1));
            }
          }
      }
    }
}

/* ================================================================== */
/* Step 10: Real fetch() API                                           */
/* ================================================================== */

/*
 * Synchronous fetch — performs full HTTP request in the C function,
 * returns a resolved/rejected Promise. Blocks the JS thread during I/O.
 * This is the only option on a bare-metal OS without threading.
 */

static JSValue ts_js_response_text(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JSValue body = JS_GetPropertyStr(ctx, this_val, "__body");
    if (JS_IsUndefined(body)) return JS_NewString(ctx, "");
    /* Return a resolved promise with the body string */
    { JSValue resolve_funcs[2];
      JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);
      JS_Call(ctx, resolve_funcs[0], JS_UNDEFINED, 1, &body);
      JS_FreeValue(ctx, resolve_funcs[0]);
      JS_FreeValue(ctx, resolve_funcs[1]);
      JS_FreeValue(ctx, body);
      return promise;
    }
}

static JSValue ts_js_response_json(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JSValue body = JS_GetPropertyStr(ctx, this_val, "__body");
    if (JS_IsUndefined(body)) {
        JSValue resolve_funcs[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);
        JS_Call(ctx, resolve_funcs[1], JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, resolve_funcs[0]);
        JS_FreeValue(ctx, resolve_funcs[1]);
        return promise;
    }
    { const char *str = JS_ToCString(ctx, body);
      JS_FreeValue(ctx, body);
      if (!str) {
          JSValue resolve_funcs[2];
          JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);
          JS_Call(ctx, resolve_funcs[1], JS_UNDEFINED, 0, NULL);
          JS_FreeValue(ctx, resolve_funcs[0]);
          JS_FreeValue(ctx, resolve_funcs[1]);
          return promise;
      }
      { JSValue parsed = JS_ParseJSON(ctx, str, strlen(str), "<fetch>");
        JS_FreeCString(ctx, str);
        { JSValue resolve_funcs[2];
          JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);
          if (JS_IsException(parsed)) {
              JSValue exc = JS_GetException(ctx);
              JS_Call(ctx, resolve_funcs[1], JS_UNDEFINED, 1, &exc);
              JS_FreeValue(ctx, exc);
          } else {
              JS_Call(ctx, resolve_funcs[0], JS_UNDEFINED, 1, &parsed);
          }
          JS_FreeValue(ctx, parsed);
          JS_FreeValue(ctx, resolve_funcs[0]);
          JS_FreeValue(ctx, resolve_funcs[1]);
          return promise;
        }
      }
    }
}

static JSValue ts_js_fetch(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    JSValue resolve_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);

    if (argc < 1) {
        JSValue err = JS_NewString(ctx, "fetch requires a URL argument");
        JS_Call(ctx, resolve_funcs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, err);
        goto done;
    }

    { const char *url = JS_ToCString(ctx, argv[0]);
      if (!url) {
          JSValue err = JS_NewString(ctx, "Invalid URL");
          JS_Call(ctx, resolve_funcs[1], JS_UNDEFINED, 1, &err);
          JS_FreeValue(ctx, err);
          goto done;
      }

      fprintf(stderr, "FETCH: %s\n", url);

      /* Resolve relative URLs against page URL */
      { char resolved[2048];
        if (url[0] == '/' && g_dom && g_dom->url[0]) {
            struct ts_url base;
            if (ts_url_parse(g_dom->url, &base) == 0) {
                snprintf(resolved, sizeof(resolved), "%s://%s%s",
                         base.scheme, base.host, url);
            } else {
                strncpy(resolved, url, sizeof(resolved) - 1);
                resolved[sizeof(resolved)-1] = '\0';
            }
        } else {
            strncpy(resolved, url, sizeof(resolved) - 1);
            resolved[sizeof(resolved)-1] = '\0';
        }
        JS_FreeCString(ctx, url);

        /* Check for POST method/body from options */
        { const char *method = "GET";
          const char *post_body = NULL;
          size_t post_body_len = 0;
          char method_buf[8];

          if (argc > 1 && JS_IsObject(argv[1])) {
              JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
              if (JS_IsString(m)) {
                  const char *ms = JS_ToCString(ctx, m);
                  if (ms) {
                      strncpy(method_buf, ms, 7); method_buf[7] = '\0';
                      /* Uppercase */
                      { size_t i; for (i = 0; method_buf[i]; i++)
                          if (method_buf[i] >= 'a' && method_buf[i] <= 'z')
                              method_buf[i] -= 32;
                      }
                      method = method_buf;
                      JS_FreeCString(ctx, ms);
                  }
              }
              JS_FreeValue(ctx, m);

              if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
                  strcmp(method, "PATCH") == 0) {
                  JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
                  if (JS_IsString(b)) {
                      post_body = JS_ToCString(ctx, b);
                      if (post_body) post_body_len = strlen(post_body);
                  }
                  JS_FreeValue(ctx, b);
              }
          }

          /* Perform synchronous HTTP request */
          { struct ts_http *req = (struct ts_http *)malloc(sizeof(struct ts_http));
            if (!req) {
                JSValue err = JS_NewString(ctx, "Out of memory for fetch");
                JS_Call(ctx, resolve_funcs[1], JS_UNDEFINED, 1, &err);
                JS_FreeValue(ctx, err);
                if (post_body) JS_FreeCString(ctx, post_body);
                goto done;
            }
            ts_http_init(req);

            { int rc = ts_http_get(req, resolved);
              if (rc < 0) {
                  JSValue err = JS_NewString(ctx, req->error[0] ? req->error : "Network error");
                  JS_Call(ctx, resolve_funcs[1], JS_UNDEFINED, 1, &err);
                  JS_FreeValue(ctx, err);
                  ts_http_free(req);
                  free(req);
                  if (post_body) JS_FreeCString(ctx, post_body);
                  goto done;
              }
            }

            /* Poll until done (blocking) */
            { int max_polls = 50000; /* safety limit */
              while (req->state != TS_HTTP_DONE && req->state != TS_HTTP_ERROR &&
                     max_polls-- > 0) {
                  ts_http_poll(req);
              }
            }

            if (req->state != TS_HTTP_DONE) {
                JSValue err = JS_NewString(ctx, req->error[0] ? req->error : "Fetch timeout/error");
                JS_Call(ctx, resolve_funcs[1], JS_UNDEFINED, 1, &err);
                JS_FreeValue(ctx, err);
                ts_http_free(req);
                free(req);
                if (post_body) JS_FreeCString(ctx, post_body);
                goto done;
            }

            fprintf(stderr, "FETCH: done status=%d len=%zu\n",
                    req->response.status_code, req->response.body_len);

            /* Build Response object */
            { JSValue resp = JS_NewObject(ctx);
              JS_SetPropertyStr(ctx, resp, "status",
                  JS_NewInt32(ctx, req->response.status_code));
              JS_SetPropertyStr(ctx, resp, "ok",
                  JS_NewBool(ctx, req->response.status_code >= 200 &&
                                  req->response.status_code < 300));
              JS_SetPropertyStr(ctx, resp, "statusText",
                  JS_NewString(ctx, req->response.status_text));
              JS_SetPropertyStr(ctx, resp, "url",
                  JS_NewString(ctx, resolved));

              /* Headers object */
              { JSValue hdrs = JS_NewObject(ctx);
                int hi;
                for (hi = 0; hi < req->response.header_count; hi++) {
                    /* Lowercase header name for consistency */
                    char lower[TS_HTTP_MAX_HEADER_NAME];
                    { size_t li;
                      for (li = 0; req->response.headers[hi].name[li] &&
                           li < sizeof(lower) - 1; li++)
                          lower[li] = (req->response.headers[hi].name[li] >= 'A' &&
                                       req->response.headers[hi].name[li] <= 'Z')
                              ? req->response.headers[hi].name[li] + 32
                              : req->response.headers[hi].name[li];
                      lower[li] = '\0';
                    }
                    JS_SetPropertyStr(ctx, hdrs, lower,
                        JS_NewString(ctx, req->response.headers[hi].value));
                }
                /* headers.get() method */
                {   const char *hget_code =
                        "(function get(name){return this[name.toLowerCase()]||null})";
                    JSValue hget = JS_Eval(ctx, hget_code, strlen(hget_code),
                                            "<hget>", JS_EVAL_TYPE_GLOBAL);
                    JS_SetPropertyStr(ctx, hdrs, "get", hget);
                }
                JS_SetPropertyStr(ctx, resp, "headers", hdrs);
              }

              /* Store body as __body for .text() and .json() */
              if (req->response.body && req->response.body_len > 0) {
                  JS_SetPropertyStr(ctx, resp, "__body",
                      JS_NewStringLen(ctx, req->response.body,
                                       req->response.body_len));
              } else {
                  JS_SetPropertyStr(ctx, resp, "__body", JS_NewString(ctx, ""));
              }

              JS_SetPropertyStr(ctx, resp, "text",
                  JS_NewCFunction(ctx, ts_js_response_text, "text", 0));
              JS_SetPropertyStr(ctx, resp, "json",
                  JS_NewCFunction(ctx, ts_js_response_json, "json", 0));

              /* Resolve the promise with the Response */
              JS_Call(ctx, resolve_funcs[0], JS_UNDEFINED, 1, &resp);
              JS_FreeValue(ctx, resp);
            }

            ts_http_free(req);
            free(req);
            if (post_body) JS_FreeCString(ctx, post_body);
          }
        }
      }
    }

done:
    JS_FreeValue(ctx, resolve_funcs[0]);
    JS_FreeValue(ctx, resolve_funcs[1]);
    return promise;
}

/* ================================================================== */
/* XMLHttpRequest sync fetch helper                                    */
/* ================================================================== */

/*
 * ts_js_xhr_sync_fetch — synchronous HTTP fetch for XMLHttpRequest.
 * Args: url (string), opts_json (string with {method, headers, body}).
 * Returns JSON string: {status, statusText, body} or empty string on error.
 */
static JSValue ts_js_xhr_sync_fetch(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    { const char *url = JS_ToCString(ctx, argv[0]);
      if (!url) return JS_NewString(ctx, "");

      /* Resolve relative URLs */
      { char resolved[2048];
        if (url[0] == '/' && g_dom && g_dom->url[0]) {
            struct ts_url base;
            if (ts_url_parse(g_dom->url, &base) == 0)
                snprintf(resolved, sizeof(resolved), "%s://%s%s",
                         base.scheme, base.host, url);
            else { strncpy(resolved, url, sizeof(resolved) - 1); resolved[sizeof(resolved)-1] = '\0'; }
        } else { strncpy(resolved, url, sizeof(resolved) - 1); resolved[sizeof(resolved)-1] = '\0'; }
        JS_FreeCString(ctx, url);

        { struct ts_http *req = (struct ts_http *)malloc(sizeof(struct ts_http));
          if (!req) return JS_NewString(ctx, "");
          ts_http_init(req);
          { int rc = ts_http_get(req, resolved);
            if (rc < 0) { ts_http_free(req); free(req); return JS_NewString(ctx, ""); }
          }
          { int max_polls = 50000;
            while (req->state != TS_HTTP_DONE && req->state != TS_HTTP_ERROR &&
                   max_polls-- > 0)
                ts_http_poll(req);
          }
          if (req->state != TS_HTTP_DONE) { ts_http_free(req); free(req); return JS_NewString(ctx, ""); }

          /* Build JSON result: {"status":NNN,"statusText":"...","body":"..."} */
          { size_t body_len = req->response.body_len;
            size_t json_cap = body_len * 2 + 256;  /* pessimistic: body may need escaping */
            char *json = (char *)malloc(json_cap);
            if (!json) { ts_http_free(req); free(req); return JS_NewString(ctx, ""); }
            { int pos = snprintf(json, json_cap,
                  "{\"status\":%d,\"statusText\":\"%s\",\"body\":\"",
                  req->response.status_code, req->response.status_text);
              /* Escape body for JSON */
              { size_t i;
                for (i = 0; i < body_len && (size_t)pos < json_cap - 8; i++) {
                    unsigned char c = (unsigned char)(req->response.body ? req->response.body[i] : 0);
                    if (c == '"') { json[pos++] = '\\'; json[pos++] = '"'; }
                    else if (c == '\\') { json[pos++] = '\\'; json[pos++] = '\\'; }
                    else if (c == '\n') { json[pos++] = '\\'; json[pos++] = 'n'; }
                    else if (c == '\r') { json[pos++] = '\\'; json[pos++] = 'r'; }
                    else if (c == '\t') { json[pos++] = '\\'; json[pos++] = 't'; }
                    else if (c < 0x20) { pos += snprintf(json + pos, json_cap - (size_t)pos, "\\u%04x", c); }
                    else json[pos++] = (char)c;
                }
              }
              pos += snprintf(json + pos, json_cap - (size_t)pos, "\"}");
              { JSValue result = JS_NewStringLen(ctx, json, (size_t)pos);
                free(json);
                ts_http_free(req);
                free(req);
                return result;
              }
            }
          }
        }
      }
    }
}

/* ================================================================== */
/* Step 10: localStorage / sessionStorage                              */
/* ================================================================== */

#define TS_STORAGE_MAX_ITEMS 128
#define TS_STORAGE_KEY_MAX   128
#define TS_STORAGE_VAL_MAX   4096

static struct {
    char key[TS_STORAGE_KEY_MAX];
    char value[TS_STORAGE_VAL_MAX];
    int used;
} ts_local_storage[TS_STORAGE_MAX_ITEMS];
static int ts_storage_initialized = 0;

static void ts_storage_init(void) {
    if (ts_storage_initialized) return;
    memset(ts_local_storage, 0, sizeof(ts_local_storage));
    ts_storage_initialized = 1;
}

static JSValue ts_js_storage_get_item(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    { const char *key = JS_ToCString(ctx, argv[0]);
      if (!key) return JS_NULL;
      ts_storage_init();
      { int i;
        for (i = 0; i < TS_STORAGE_MAX_ITEMS; i++) {
            if (ts_local_storage[i].used &&
                strcmp(ts_local_storage[i].key, key) == 0) {
                JS_FreeCString(ctx, key);
                return JS_NewString(ctx, ts_local_storage[i].value);
            }
        }
      }
      JS_FreeCString(ctx, key);
    }
    return JS_NULL;
}

static JSValue ts_js_storage_set_item(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    { const char *key = JS_ToCString(ctx, argv[0]);
      const char *val = JS_ToCString(ctx, argv[1]);
      if (!key || !val) {
          if (key) JS_FreeCString(ctx, key);
          if (val) JS_FreeCString(ctx, val);
          return JS_UNDEFINED;
      }
      ts_storage_init();
      /* Update existing */
      { int i;
        for (i = 0; i < TS_STORAGE_MAX_ITEMS; i++) {
            if (ts_local_storage[i].used &&
                strcmp(ts_local_storage[i].key, key) == 0) {
                strncpy(ts_local_storage[i].value, val, TS_STORAGE_VAL_MAX - 1);
                ts_local_storage[i].value[TS_STORAGE_VAL_MAX - 1] = '\0';
                JS_FreeCString(ctx, key);
                JS_FreeCString(ctx, val);
                return JS_UNDEFINED;
            }
        }
      }
      /* Add new */
      { int i;
        for (i = 0; i < TS_STORAGE_MAX_ITEMS; i++) {
            if (!ts_local_storage[i].used) {
                ts_local_storage[i].used = 1;
                strncpy(ts_local_storage[i].key, key, TS_STORAGE_KEY_MAX - 1);
                ts_local_storage[i].key[TS_STORAGE_KEY_MAX - 1] = '\0';
                strncpy(ts_local_storage[i].value, val, TS_STORAGE_VAL_MAX - 1);
                ts_local_storage[i].value[TS_STORAGE_VAL_MAX - 1] = '\0';
                break;
            }
        }
      }
      JS_FreeCString(ctx, key);
      JS_FreeCString(ctx, val);
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_storage_remove_item(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    { const char *key = JS_ToCString(ctx, argv[0]);
      if (!key) return JS_UNDEFINED;
      ts_storage_init();
      { int i;
        for (i = 0; i < TS_STORAGE_MAX_ITEMS; i++) {
            if (ts_local_storage[i].used &&
                strcmp(ts_local_storage[i].key, key) == 0) {
                ts_local_storage[i].used = 0;
                break;
            }
        }
      }
      JS_FreeCString(ctx, key);
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_storage_clear(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    ts_storage_init();
    { int i;
      for (i = 0; i < TS_STORAGE_MAX_ITEMS; i++)
          ts_local_storage[i].used = 0;
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_storage_key(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    { int32_t index; JS_ToInt32(ctx, &index, argv[0]);
      ts_storage_init();
      { int count = 0; int i;
        for (i = 0; i < TS_STORAGE_MAX_ITEMS; i++) {
            if (ts_local_storage[i].used) {
                if (count == index)
                    return JS_NewString(ctx, ts_local_storage[i].key);
                count++;
            }
        }
      }
    }
    return JS_NULL;
}

static JSValue ts_js_storage_length(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    ts_storage_init();
    { int count = 0; int i;
      for (i = 0; i < TS_STORAGE_MAX_ITEMS; i++)
          if (ts_local_storage[i].used) count++;
      return JS_NewInt32(ctx, count);
    }
}

static JSValue ts_js__build_storage_obj(JSContext *ctx) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "getItem",
        JS_NewCFunction(ctx, ts_js_storage_get_item, "getItem", 1));
    JS_SetPropertyStr(ctx, obj, "setItem",
        JS_NewCFunction(ctx, ts_js_storage_set_item, "setItem", 2));
    JS_SetPropertyStr(ctx, obj, "removeItem",
        JS_NewCFunction(ctx, ts_js_storage_remove_item, "removeItem", 1));
    JS_SetPropertyStr(ctx, obj, "clear",
        JS_NewCFunction(ctx, ts_js_storage_clear, "clear", 0));
    JS_SetPropertyStr(ctx, obj, "key",
        JS_NewCFunction(ctx, ts_js_storage_key, "key", 1));
    JS_SetPropertyStr(ctx, obj, "length",
        JS_NewCFunction(ctx, ts_js_storage_length, "length", 0));
    return obj;
}

/* ================================================================== */
/* Step 10: Real URL / URLSearchParams                                 */
/* ================================================================== */

static JSValue ts_js_url_constructor(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    { const char *url_str = JS_ToCString(ctx, argv[0]);
      if (!url_str) return JS_NULL;

      /* Resolve relative URL against base if provided */
      { char resolved[2048];
        if (argc > 1 && JS_IsString(argv[1])) {
            const char *base = JS_ToCString(ctx, argv[1]);
            if (base && url_str[0] == '/') {
                struct ts_url bp;
                if (ts_url_parse(base, &bp) == 0) {
                    snprintf(resolved, sizeof(resolved), "%s://%s%s",
                             bp.scheme, bp.host, url_str);
                } else {
                    strncpy(resolved, url_str, sizeof(resolved) - 1);
                    resolved[sizeof(resolved)-1] = '\0';
                }
            } else {
                strncpy(resolved, url_str, sizeof(resolved) - 1);
                resolved[sizeof(resolved)-1] = '\0';
            }
            if (base) JS_FreeCString(ctx, base);
        } else {
            strncpy(resolved, url_str, sizeof(resolved) - 1);
            resolved[sizeof(resolved)-1] = '\0';
        }
        JS_FreeCString(ctx, url_str);

        { struct ts_url parsed;
          JSValue obj = JS_NewObject(ctx);
          if (ts_url_parse(resolved, &parsed) == 0) {
              char protocol[20], origin[280], search[520], hash_buf[130];

              JS_SetPropertyStr(ctx, obj, "href", JS_NewString(ctx, resolved));
              JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, parsed.host));
              JS_SetPropertyStr(ctx, obj, "host", JS_NewString(ctx, parsed.host));
              JS_SetPropertyStr(ctx, obj, "pathname", JS_NewString(ctx, parsed.path));

              snprintf(protocol, sizeof(protocol), "%s:", parsed.scheme);
              JS_SetPropertyStr(ctx, obj, "protocol", JS_NewString(ctx, protocol));

              snprintf(origin, sizeof(origin), "%s://%s", parsed.scheme, parsed.host);
              JS_SetPropertyStr(ctx, obj, "origin", JS_NewString(ctx, origin));

              if (parsed.query[0]) {
                  snprintf(search, sizeof(search), "?%s", parsed.query);
              } else { search[0] = '\0'; }
              JS_SetPropertyStr(ctx, obj, "search", JS_NewString(ctx, search));

              if (parsed.fragment[0]) {
                  snprintf(hash_buf, sizeof(hash_buf), "#%s", parsed.fragment);
              } else { hash_buf[0] = '\0'; }
              JS_SetPropertyStr(ctx, obj, "hash", JS_NewString(ctx, hash_buf));

              { char port_str[8];
                snprintf(port_str, sizeof(port_str), "%d", parsed.port);
                JS_SetPropertyStr(ctx, obj, "port", JS_NewString(ctx, port_str));
              }

              /* searchParams sub-object */
              { JSValue sp = JS_NewObject(ctx);
                /* Parse query string into params */
                if (parsed.query[0]) {
                    const char *q = parsed.query;
                    while (*q) {
                        const char *key_start = q;
                        while (*q && *q != '=' && *q != '&') q++;
                        { size_t klen = (size_t)(q - key_start);
                          char key[256]; if (klen >= sizeof(key)) klen = sizeof(key)-1;
                          memcpy(key, key_start, klen); key[klen] = '\0';
                          if (*q == '=') q++;
                          { const char *val_start = q;
                            while (*q && *q != '&') q++;
                            { size_t vlen = (size_t)(q - val_start);
                              char val[1024]; if (vlen >= sizeof(val)) vlen = sizeof(val)-1;
                              memcpy(val, val_start, vlen); val[vlen] = '\0';
                              JS_SetPropertyStr(ctx, sp, key, JS_NewString(ctx, val));
                            }
                          }
                        }
                        if (*q == '&') q++;
                    }
                }
                /* get() method */
                { const char *sp_get_code =
                      "(function get(name){return this[name]||null})";
                  JSValue get_fn = JS_Eval(ctx, sp_get_code, strlen(sp_get_code),
                                            "<sp_get>", JS_EVAL_TYPE_GLOBAL);
                  JS_SetPropertyStr(ctx, sp, "get", get_fn);
                }
                { const char *sp_has_code =
                      "(function has(name){return name in this&&typeof this[name]==='string'})";
                  JSValue has_fn = JS_Eval(ctx, sp_has_code, strlen(sp_has_code),
                                            "<sp_has>", JS_EVAL_TYPE_GLOBAL);
                  JS_SetPropertyStr(ctx, sp, "has", has_fn);
                }
                JS_SetPropertyStr(ctx, obj, "searchParams", sp);
              }
          } else {
              JS_SetPropertyStr(ctx, obj, "href", JS_NewString(ctx, resolved));
              JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, ""));
              JS_SetPropertyStr(ctx, obj, "pathname", JS_NewString(ctx, ""));
              JS_SetPropertyStr(ctx, obj, "protocol", JS_NewString(ctx, ""));
              JS_SetPropertyStr(ctx, obj, "origin", JS_NewString(ctx, ""));
              JS_SetPropertyStr(ctx, obj, "search", JS_NewString(ctx, ""));
              JS_SetPropertyStr(ctx, obj, "hash", JS_NewString(ctx, ""));
              JS_SetPropertyStr(ctx, obj, "port", JS_NewString(ctx, ""));
          }

          /* toString() */
          { const char *ts_code = "(function toString(){return this.href})";
            JSValue ts_fn = JS_Eval(ctx, ts_code, strlen(ts_code), "<url_ts>",
                                     JS_EVAL_TYPE_GLOBAL);
            JS_SetPropertyStr(ctx, obj, "toString", ts_fn);
          }
          return obj;
        }
      }
    }
}

/* ================================================================== */
/* Step 10: performance.now() — real timestamps                        */
/* ================================================================== */

static long ts_js__perf_start_time = 0;

static JSValue ts_js_performance_now(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    long now = fry_gettime();
    return JS_NewFloat64(ctx, (double)(now - ts_js__perf_start_time));
}

/* ================================================================== */
/* Step 10: atob / btoa (Base64)                                       */
/* ================================================================== */

static const char ts_b64_enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static JSValue ts_js_btoa(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    { const char *input = JS_ToCString(ctx, argv[0]);
      if (!input) return JS_NewString(ctx, "");
      { size_t ilen = strlen(input);
        size_t olen = ((ilen + 2) / 3) * 4;
        char *out = (char *)malloc(olen + 1);
        if (!out) { JS_FreeCString(ctx, input); return JS_NewString(ctx, ""); }
        { size_t i, o = 0;
          for (i = 0; i < ilen; i += 3) {
              unsigned int b0 = (unsigned char)input[i];
              unsigned int b1 = (i+1 < ilen) ? (unsigned char)input[i+1] : 0;
              unsigned int b2 = (i+2 < ilen) ? (unsigned char)input[i+2] : 0;
              unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
              out[o++] = ts_b64_enc[(triple >> 18) & 0x3F];
              out[o++] = ts_b64_enc[(triple >> 12) & 0x3F];
              out[o++] = (i+1 < ilen) ? ts_b64_enc[(triple >> 6) & 0x3F] : '=';
              out[o++] = (i+2 < ilen) ? ts_b64_enc[triple & 0x3F] : '=';
          }
          out[o] = '\0';
        }
        JS_FreeCString(ctx, input);
        { JSValue result = JS_NewString(ctx, out);
          free(out);
          return result;
        }
      }
    }
}

static JSValue ts_js_atob(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    { const char *input = JS_ToCString(ctx, argv[0]);
      if (!input) return JS_NewString(ctx, "");
      { size_t ilen = strlen(input);
        size_t olen = (ilen / 4) * 3;
        size_t o = 0;
        char *out = (char *)malloc(olen + 1);
        if (!out) { JS_FreeCString(ctx, input); return JS_NewString(ctx, ""); }
        { size_t i;
          for (i = 0; i < ilen; i += 4) {
              int vals[4]; int j;
              for (j = 0; j < 4; j++) {
                  char c = (i+j < ilen) ? input[i+j] : '=';
                  if (c >= 'A' && c <= 'Z') vals[j] = c - 'A';
                  else if (c >= 'a' && c <= 'z') vals[j] = c - 'a' + 26;
                  else if (c >= '0' && c <= '9') vals[j] = c - '0' + 52;
                  else if (c == '+') vals[j] = 62;
                  else if (c == '/') vals[j] = 63;
                  else vals[j] = 0;
              }
              { unsigned int triple = ((unsigned)vals[0] << 18) | ((unsigned)vals[1] << 12) |
                                       ((unsigned)vals[2] << 6) | (unsigned)vals[3];
                out[o++] = (char)((triple >> 16) & 0xFF);
                if (i+2 < ilen && input[i+2] != '=') out[o++] = (char)((triple >> 8) & 0xFF);
                if (i+3 < ilen && input[i+3] != '=') out[o++] = (char)(triple & 0xFF);
              }
          }
          out[o] = '\0';
        }
        JS_FreeCString(ctx, input);
        { JSValue result = JS_NewStringLen(ctx, out, o);
          free(out);
          return result;
        }
      }
    }
}

/* ================================================================== */
/* Step 10: DOMParser                                                  */
/* ================================================================== */

static JSValue ts_js_domparser_parse(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    { const char *html = JS_ToCString(ctx, argv[0]);
      if (!html) return JS_NULL;
      /* Create a document fragment, parse HTML into it via DOM builder */
      { struct ts_dom_node *frag = ts_dom_alloc_node(g_dom, TS_DOM_FRAGMENT_NODE);
        if (!frag) { JS_FreeCString(ctx, html); return JS_NULL; }
        ts_dom_build_from_html(g_dom, html, strlen(html), frag->id);
        JS_FreeCString(ctx, html);
        { JSValue wrapper = ts_js_wrap_node(ctx, g_dom, frag->id);
          ts_dom_bind_node_methods(ctx, wrapper);
          /* Add documentElement property (first element child) */
          { int child = frag->first_child;
            while (child >= 0) {
                struct ts_dom_node *cn = ts_dom_get_node(g_dom, child);
                if (cn && cn->type == TS_DOM_ELEMENT_NODE) {
                    JSValue de = ts_js_wrap_node(ctx, g_dom, child);
                    ts_dom_bind_node_methods(ctx, de);
                    JS_SetPropertyStr(ctx, wrapper, "documentElement", de);
                    /* Also set body if it's <html> with <body> inside */
                    if (strcmp(cn->tag, "html") == 0) {
                        int bc = cn->first_child;
                        while (bc >= 0) {
                            struct ts_dom_node *bn = ts_dom_get_node(g_dom, bc);
                            if (bn && strcmp(bn->tag, "body") == 0) {
                                JSValue bv = ts_js_wrap_node(ctx, g_dom, bc);
                                ts_dom_bind_node_methods(ctx, bv);
                                JS_SetPropertyStr(ctx, wrapper, "body", bv);
                                break;
                            }
                            bc = bn ? bn->next_sibling : -1;
                        }
                    }
                    break;
                }
                child = cn ? cn->next_sibling : -1;
            }
          }
          /* querySelector/querySelectorAll on the parsed doc */
          JS_SetPropertyStr(ctx, wrapper, "querySelector",
              JS_NewCFunction(ctx, ts_js_elem_query_selector, "querySelector", 1));
          JS_SetPropertyStr(ctx, wrapper, "querySelectorAll",
              JS_NewCFunction(ctx, ts_js_elem_query_selector_all, "querySelectorAll", 1));
          return wrapper;
        }
      }
    }
}

/* ================================================================== */
/* Step 14: Canvas 2D API                                              */
/* ================================================================== */

#define TS_CANVAS_STATE_MAX 16

struct ts_canvas_state {
    uint32_t fill_color;
    uint32_t stroke_color;
    int line_width;
    int font_size;         /* in multiples of 8 (char width) */
    int translate_x, translate_y;
};

struct ts_canvas_ctx {
    uint32_t *pixels;
    int w, h;
    int cache_idx;         /* index in doc->canvas_cache */
    struct ts_canvas_state state;
    struct ts_canvas_state stack[TS_CANVAS_STATE_MAX];
    int stack_depth;
    /* Simple path state */
    int path_x, path_y;
    int path_started;
};

static struct ts_canvas_ctx ts_canvases[16];
static int ts_canvas_count = 0;

static uint32_t ts_canvas__parse_color(const char *str) {
    /* Reuse the CSS color parser */
    return ts_css_color(str);
}

static int ts_canvas__get_idx(JSContext *ctx, JSValueConst this_val) {
    JSValue v = JS_GetPropertyStr(ctx, this_val, "__canvas_idx");
    int idx = -1;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &idx, v);
    JS_FreeValue(ctx, v);
    return idx;
}

/* ---- Drawing methods ---- */

static JSValue ts_js_canvas_fill_rect(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 4) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci < 0 || ci >= ts_canvas_count) return JS_UNDEFINED;
      { struct ts_canvas_ctx *c = &ts_canvases[ci];
        int32_t rx, ry, rw, rh;
        JS_ToInt32(ctx, &rx, argv[0]); JS_ToInt32(ctx, &ry, argv[1]);
        JS_ToInt32(ctx, &rw, argv[2]); JS_ToInt32(ctx, &rh, argv[3]);
        rx += c->state.translate_x; ry += c->state.translate_y;
        { int y, x;
          for (y = ry; y < ry + rh && y < c->h; y++) {
              if (y < 0) continue;
              for (x = rx; x < rx + rw && x < c->w; x++) {
                  if (x < 0) continue;
                  c->pixels[y * c->w + x] = c->state.fill_color;
              }
          }
        }
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_stroke_rect(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    if (argc < 4) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci < 0 || ci >= ts_canvas_count) return JS_UNDEFINED;
      { struct ts_canvas_ctx *c = &ts_canvases[ci];
        int32_t rx, ry, rw, rh;
        JS_ToInt32(ctx, &rx, argv[0]); JS_ToInt32(ctx, &ry, argv[1]);
        JS_ToInt32(ctx, &rw, argv[2]); JS_ToInt32(ctx, &rh, argv[3]);
        rx += c->state.translate_x; ry += c->state.translate_y;
        { int lw = c->state.line_width; int i, x, y;
          for (i = 0; i < lw; i++) {
              /* Top and bottom edges */
              for (x = rx; x < rx + rw && x < c->w; x++) {
                  if (x < 0) continue;
                  if (ry + i >= 0 && ry + i < c->h) c->pixels[(ry+i) * c->w + x] = c->state.stroke_color;
                  if (ry+rh-1-i >= 0 && ry+rh-1-i < c->h) c->pixels[(ry+rh-1-i) * c->w + x] = c->state.stroke_color;
              }
              /* Left and right edges */
              for (y = ry; y < ry + rh && y < c->h; y++) {
                  if (y < 0) continue;
                  if (rx + i >= 0 && rx + i < c->w) c->pixels[y * c->w + (rx+i)] = c->state.stroke_color;
                  if (rx+rw-1-i >= 0 && rx+rw-1-i < c->w) c->pixels[y * c->w + (rx+rw-1-i)] = c->state.stroke_color;
              }
          }
        }
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_clear_rect(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    if (argc < 4) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci < 0 || ci >= ts_canvas_count) return JS_UNDEFINED;
      { struct ts_canvas_ctx *c = &ts_canvases[ci];
        int32_t rx, ry, rw, rh;
        JS_ToInt32(ctx, &rx, argv[0]); JS_ToInt32(ctx, &ry, argv[1]);
        JS_ToInt32(ctx, &rw, argv[2]); JS_ToInt32(ctx, &rh, argv[3]);
        rx += c->state.translate_x; ry += c->state.translate_y;
        { int y, x;
          for (y = ry; y < ry + rh && y < c->h; y++) {
              if (y < 0) continue;
              for (x = rx; x < rx + rw && x < c->w; x++) {
                  if (x < 0) continue;
                  c->pixels[y * c->w + x] = 0;
              }
          }
        }
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_fill_text(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 3) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci < 0 || ci >= ts_canvas_count) return JS_UNDEFINED;
      { struct ts_canvas_ctx *c = &ts_canvases[ci];
        const char *text = JS_ToCString(ctx, argv[0]);
        if (!text) return JS_UNDEFINED;
        { int32_t tx, ty;
          JS_ToInt32(ctx, &tx, argv[1]); JS_ToInt32(ctx, &ty, argv[2]);
          tx += c->state.translate_x; ty += c->state.translate_y;
          /* Draw text using 8x16 font directly into canvas buffer */
          { gfx_ctx_t tmp;
            gfx_init(&tmp, c->pixels, (uint32_t)c->w, (uint32_t)c->h, (uint32_t)c->w);
            gfx_draw_text(&tmp, (uint32_t)(tx > 0 ? tx : 0),
                          (uint32_t)(ty > 0 ? ty : 0), text,
                          c->state.fill_color, TS_COL_TRANSPARENT);
          }
          JS_FreeCString(ctx, text);
        }
      }
    }
    return JS_UNDEFINED;
}

/* Path operations */
static JSValue ts_js_canvas_begin_path(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          ts_canvases[ci].path_started = 0;
          ts_canvases[ci].path_x = 0;
          ts_canvases[ci].path_y = 0;
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_move_to(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          int32_t mx, my;
          JS_ToInt32(ctx, &mx, argv[0]); JS_ToInt32(ctx, &my, argv[1]);
          ts_canvases[ci].path_x = mx + ts_canvases[ci].state.translate_x;
          ts_canvases[ci].path_y = my + ts_canvases[ci].state.translate_y;
          ts_canvases[ci].path_started = 1;
      }
    }
    return JS_UNDEFINED;
}

/* Bresenham line into canvas buffer */
static void ts_canvas__line(struct ts_canvas_ctx *c, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    { int err = dx - dy;
      while (1) {
          if (x0 >= 0 && x0 < c->w && y0 >= 0 && y0 < c->h)
              c->pixels[y0 * c->w + x0] = color;
          if (x0 == x1 && y0 == y1) break;
          { int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx) { err += dx; y0 += sy; }
          }
      }
    }
}

static JSValue ts_js_canvas_line_to(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          struct ts_canvas_ctx *c = &ts_canvases[ci];
          int32_t lx, ly;
          JS_ToInt32(ctx, &lx, argv[0]); JS_ToInt32(ctx, &ly, argv[1]);
          lx += c->state.translate_x; ly += c->state.translate_y;
          if (c->path_started)
              ts_canvas__line(c, c->path_x, c->path_y, lx, ly, c->state.stroke_color);
          c->path_x = lx; c->path_y = ly;
          c->path_started = 1;
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_stroke(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    /* Lines already drawn by lineTo — stroke is a no-op for our simple path model */
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_fill(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    /* Fill is complex (flood fill of path). For now, no-op. */
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_close_path(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    /* No-op in our simple model */
    return JS_UNDEFINED;
}

/* Arc (simplified: draw circle outline using midpoint algorithm) */
static JSValue ts_js_canvas_arc(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    if (argc < 3) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          struct ts_canvas_ctx *c = &ts_canvases[ci];
          int32_t cx32, cy32, r32;
          JS_ToInt32(ctx, &cx32, argv[0]); JS_ToInt32(ctx, &cy32, argv[1]);
          JS_ToInt32(ctx, &r32, argv[2]);
          cx32 += c->state.translate_x; cy32 += c->state.translate_y;
          /* Midpoint circle algorithm */
          { int x = r32, y = 0, err = 1 - r32;
            uint32_t col = c->state.stroke_color;
            while (x >= y) {
                int pts[][2] = {{cx32+x,cy32+y},{cx32-x,cy32+y},{cx32+x,cy32-y},{cx32-x,cy32-y},
                                 {cx32+y,cy32+x},{cx32-y,cy32+x},{cx32+y,cy32-x},{cx32-y,cy32-x}};
                int pi;
                for (pi = 0; pi < 8; pi++) {
                    int px = pts[pi][0], py = pts[pi][1];
                    if (px >= 0 && px < c->w && py >= 0 && py < c->h)
                        c->pixels[py * c->w + px] = col;
                }
                y++;
                if (err < 0) { err += 2 * y + 1; }
                else { x--; err += 2 * (y - x) + 1; }
            }
          }
      }
    }
    return JS_UNDEFINED;
}

/* State save/restore */
static JSValue ts_js_canvas_save(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          struct ts_canvas_ctx *c = &ts_canvases[ci];
          if (c->stack_depth < TS_CANVAS_STATE_MAX)
              c->stack[c->stack_depth++] = c->state;
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_restore(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          struct ts_canvas_ctx *c = &ts_canvases[ci];
          if (c->stack_depth > 0)
              c->state = c->stack[--c->stack_depth];
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_translate(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          int32_t tx, ty;
          JS_ToInt32(ctx, &tx, argv[0]); JS_ToInt32(ctx, &ty, argv[1]);
          ts_canvases[ci].state.translate_x += tx;
          ts_canvases[ci].state.translate_y += ty;
      }
    }
    return JS_UNDEFINED;
}

/* Style setters */
static JSValue ts_js_canvas_set_fill_style(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          const char *str = JS_ToCString(ctx, argv[0]);
          if (str) { ts_canvases[ci].state.fill_color = ts_canvas__parse_color(str); JS_FreeCString(ctx, str); }
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_set_stroke_style(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          const char *str = JS_ToCString(ctx, argv[0]);
          if (str) { ts_canvases[ci].state.stroke_color = ts_canvas__parse_color(str); JS_FreeCString(ctx, str); }
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_set_line_width(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    { int ci = ts_canvas__get_idx(ctx, this_val);
      if (ci >= 0 && ci < ts_canvas_count) {
          int32_t lw; JS_ToInt32(ctx, &lw, argv[0]);
          if (lw < 1) lw = 1;
          ts_canvases[ci].state.line_width = lw;
      }
    }
    return JS_UNDEFINED;
}

static JSValue ts_js_canvas_measure_text(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
    if (argc < 1) { JSValue r = JS_NewObject(ctx); JS_SetPropertyStr(ctx, r, "width", JS_NewInt32(ctx, 0)); return r; }
    { const char *text = JS_ToCString(ctx, argv[0]);
      int len = text ? (int)strlen(text) : 0;
      if (text) JS_FreeCString(ctx, text);
      { JSValue r = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, r, "width", JS_NewInt32(ctx, len * 8)); /* 8px per char */
        return r;
      }
    }
}

/* getContext('2d') — creates the 2D context object */
static JSValue ts_js_canvas_get_context(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    { const char *type = JS_ToCString(ctx, argv[0]);
      if (!type) return JS_NULL;
      if (strcmp(type, "2d") != 0) { JS_FreeCString(ctx, type); return JS_NULL; }
      JS_FreeCString(ctx, type);

      /* Check if already created */
      { JSValue existing = JS_GetPropertyStr(ctx, this_val, "__ctx2d");
        if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) return existing;
        JS_FreeValue(ctx, existing);
      }

      /* Get canvas dimensions */
      { int32_t cw = 300, ch = 150;
        { JSValue wv = JS_GetPropertyStr(ctx, this_val, "width");
          if (JS_IsNumber(wv)) JS_ToInt32(ctx, &cw, wv);
          JS_FreeValue(ctx, wv);
        }
        { JSValue hv = JS_GetPropertyStr(ctx, this_val, "height");
          if (JS_IsNumber(hv)) JS_ToInt32(ctx, &ch, hv);
          JS_FreeValue(ctx, hv);
        }

        if (ts_canvas_count >= 16) return JS_NULL;

        /* Allocate pixel buffer */
        { int ci = ts_canvas_count;
          struct ts_canvas_ctx *c = &ts_canvases[ci];
          memset(c, 0, sizeof(*c));
          c->w = cw; c->h = ch;
          c->pixels = (uint32_t *)calloc((size_t)(cw * ch), sizeof(uint32_t));
          if (!c->pixels) return JS_NULL;
          c->state.fill_color = 0x000000;
          c->state.stroke_color = 0x000000;
          c->state.line_width = 1;

          /* Register in document canvas cache */
          if (g_doc_ref && g_doc_ref->canvas_cache_count < 16) {
              c->cache_idx = g_doc_ref->canvas_cache_count;
              g_doc_ref->canvas_cache[c->cache_idx].pixels = c->pixels;
              g_doc_ref->canvas_cache[c->cache_idx].w = cw;
              g_doc_ref->canvas_cache[c->cache_idx].h = ch;
              g_doc_ref->canvas_cache[c->cache_idx].used = 1;
              g_doc_ref->canvas_cache_count++;
          }

          ts_canvas_count++;

          /* Build JS context object */
          { JSValue ctx2d = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, ctx2d, "__canvas_idx", JS_NewInt32(ctx, ci));
            JS_SetPropertyStr(ctx, ctx2d, "canvas", JS_DupValue(ctx, this_val));

            /* Drawing */
            JS_SetPropertyStr(ctx, ctx2d, "fillRect", JS_NewCFunction(ctx, ts_js_canvas_fill_rect, "fillRect", 4));
            JS_SetPropertyStr(ctx, ctx2d, "strokeRect", JS_NewCFunction(ctx, ts_js_canvas_stroke_rect, "strokeRect", 4));
            JS_SetPropertyStr(ctx, ctx2d, "clearRect", JS_NewCFunction(ctx, ts_js_canvas_clear_rect, "clearRect", 4));
            JS_SetPropertyStr(ctx, ctx2d, "fillText", JS_NewCFunction(ctx, ts_js_canvas_fill_text, "fillText", 3));
            JS_SetPropertyStr(ctx, ctx2d, "strokeText", JS_NewCFunction(ctx, ts_js_canvas_fill_text, "strokeText", 3));
            JS_SetPropertyStr(ctx, ctx2d, "measureText", JS_NewCFunction(ctx, ts_js_canvas_measure_text, "measureText", 1));

            /* Path */
            JS_SetPropertyStr(ctx, ctx2d, "beginPath", JS_NewCFunction(ctx, ts_js_canvas_begin_path, "beginPath", 0));
            JS_SetPropertyStr(ctx, ctx2d, "moveTo", JS_NewCFunction(ctx, ts_js_canvas_move_to, "moveTo", 2));
            JS_SetPropertyStr(ctx, ctx2d, "lineTo", JS_NewCFunction(ctx, ts_js_canvas_line_to, "lineTo", 2));
            JS_SetPropertyStr(ctx, ctx2d, "closePath", JS_NewCFunction(ctx, ts_js_canvas_close_path, "closePath", 0));
            JS_SetPropertyStr(ctx, ctx2d, "arc", JS_NewCFunction(ctx, ts_js_canvas_arc, "arc", 5));
            JS_SetPropertyStr(ctx, ctx2d, "stroke", JS_NewCFunction(ctx, ts_js_canvas_stroke, "stroke", 0));
            JS_SetPropertyStr(ctx, ctx2d, "fill", JS_NewCFunction(ctx, ts_js_canvas_fill, "fill", 0));

            /* State */
            JS_SetPropertyStr(ctx, ctx2d, "save", JS_NewCFunction(ctx, ts_js_canvas_save, "save", 0));
            JS_SetPropertyStr(ctx, ctx2d, "restore", JS_NewCFunction(ctx, ts_js_canvas_restore, "restore", 0));
            JS_SetPropertyStr(ctx, ctx2d, "translate", JS_NewCFunction(ctx, ts_js_canvas_translate, "translate", 2));

            /* Style setters (as methods since we can't do property set interception) */
            JS_SetPropertyStr(ctx, ctx2d, "__setFillStyle", JS_NewCFunction(ctx, ts_js_canvas_set_fill_style, "__setFillStyle", 1));
            JS_SetPropertyStr(ctx, ctx2d, "__setStrokeStyle", JS_NewCFunction(ctx, ts_js_canvas_set_stroke_style, "__setStrokeStyle", 1));
            JS_SetPropertyStr(ctx, ctx2d, "__setLineWidth", JS_NewCFunction(ctx, ts_js_canvas_set_line_width, "__setLineWidth", 1));

            /* Default style values */
            JS_SetPropertyStr(ctx, ctx2d, "fillStyle", JS_NewString(ctx, "#000000"));
            JS_SetPropertyStr(ctx, ctx2d, "strokeStyle", JS_NewString(ctx, "#000000"));
            JS_SetPropertyStr(ctx, ctx2d, "lineWidth", JS_NewInt32(ctx, 1));
            JS_SetPropertyStr(ctx, ctx2d, "font", JS_NewString(ctx, "16px monospace"));
            JS_SetPropertyStr(ctx, ctx2d, "textAlign", JS_NewString(ctx, "start"));
            JS_SetPropertyStr(ctx, ctx2d, "textBaseline", JS_NewString(ctx, "alphabetic"));
            JS_SetPropertyStr(ctx, ctx2d, "globalAlpha", JS_NewFloat64(ctx, 1.0));

            /* Cache on the canvas element */
            JS_SetPropertyStr(ctx, this_val, "__ctx2d", JS_DupValue(ctx, ctx2d));

            g_dom->dirty = 1;
            return ctx2d;
          }
        }
      }
    }
}

/* ================================================================== */
/* Register all global objects (document, window, console)             */
/* ================================================================== */

static void ts_dom_register_globals(struct ts_dom_ctx *dom) {
    JSContext *ctx = dom->ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    g_dom = dom;

    /* ---- document ---- */
    JSValue doc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, doc, "__node_id",
                       JS_NewInt32(ctx, dom->document_node));

    /* document methods */
    JS_SetPropertyStr(ctx, doc, "createElement",
        JS_NewCFunction(ctx, ts_js_doc_create_element, "createElement", 1));
    JS_SetPropertyStr(ctx, doc, "createTextNode",
        JS_NewCFunction(ctx, ts_js_doc_create_text_node, "createTextNode", 1));
    JS_SetPropertyStr(ctx, doc, "createComment",
        JS_NewCFunction(ctx, ts_js_doc_create_comment, "createComment", 1));
    JS_SetPropertyStr(ctx, doc, "createDocumentFragment",
        JS_NewCFunction(ctx, ts_js_doc_create_fragment, "createDocumentFragment", 0));
    JS_SetPropertyStr(ctx, doc, "getElementById",
        JS_NewCFunction(ctx, ts_js_doc_get_element_by_id, "getElementById", 1));
    JS_SetPropertyStr(ctx, doc, "getElementsByTagName",
        JS_NewCFunction(ctx, ts_js_doc_get_elements_by_tag, "getElementsByTagName", 1));
    JS_SetPropertyStr(ctx, doc, "querySelector",
        JS_NewCFunction(ctx, ts_js_elem_query_selector, "querySelector", 1));
    JS_SetPropertyStr(ctx, doc, "querySelectorAll",
        JS_NewCFunction(ctx, ts_js_elem_query_selector_all, "querySelectorAll", 1));

    /* document.body / document.head / document.documentElement */
    if (dom->body_node >= 0) {
        JSValue body = ts_js_wrap_node(ctx, dom, dom->body_node);
        ts_dom_bind_node_methods(ctx, body);
        JS_SetPropertyStr(ctx, doc, "body", body);
    }
    if (dom->head_node >= 0) {
        JSValue head = ts_js_wrap_node(ctx, dom, dom->head_node);
        ts_dom_bind_node_methods(ctx, head);
        JS_SetPropertyStr(ctx, doc, "head", head);
    }
    if (dom->html_node >= 0) {
        JSValue html = ts_js_wrap_node(ctx, dom, dom->html_node);
        ts_dom_bind_node_methods(ctx, html);
        JS_SetPropertyStr(ctx, doc, "documentElement", html);
    }

    ts_dom_bind_node_methods(ctx, doc);
    JS_SetPropertyStr(ctx, global, "document", doc);
    dom->js_document = JS_DupValue(ctx, doc);

    /* ---- window ---- */
    /* window IS the global object in browsers */
    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, ts_js_window_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, ts_js_window_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, ts_js_window_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, ts_js_window_clear_timeout, "clearInterval", 1));
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame",
        JS_NewCFunction(ctx, ts_js_window_request_anim_frame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, global, "cancelAnimationFrame",
        JS_NewCFunction(ctx, ts_js_window_clear_timeout, "cancelAnimationFrame", 1));

    /* window.addEventListener / removeEventListener (stub — no-op) */
    JS_SetPropertyStr(ctx, global, "addEventListener",
        JS_NewCFunction(ctx, ts_js_elem_add_event_listener, "addEventListener", 3));
    JS_SetPropertyStr(ctx, global, "removeEventListener",
        JS_NewCFunction(ctx, ts_js_elem_remove_event_listener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, global, "dispatchEvent",
        JS_NewCFunction(ctx, ts_js_elem_remove_event_listener, "dispatchEvent", 1));

    /* window dimensions */
    JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, 900));
    JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, 548));
    JS_SetPropertyStr(ctx, global, "outerWidth", JS_NewInt32(ctx, 900));
    JS_SetPropertyStr(ctx, global, "outerHeight", JS_NewInt32(ctx, 600));
    JS_SetPropertyStr(ctx, global, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, global, "scrollX", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "scrollY", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageXOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageYOffset", JS_NewInt32(ctx, 0));

    /* window.scroll / scrollTo / scrollBy — no-ops */
    { JSValue noop = JS_Eval(ctx, "(function(){})", 14, "<noop>", JS_EVAL_TYPE_GLOBAL);
      JS_SetPropertyStr(ctx, global, "scroll", JS_DupValue(ctx, noop));
      JS_SetPropertyStr(ctx, global, "scrollTo", JS_DupValue(ctx, noop));
      JS_SetPropertyStr(ctx, global, "scrollBy", JS_DupValue(ctx, noop));
      JS_SetPropertyStr(ctx, global, "print", JS_DupValue(ctx, noop));
      JS_SetPropertyStr(ctx, global, "alert", JS_DupValue(ctx, noop));
      JS_SetPropertyStr(ctx, global, "confirm",
          JS_Eval(ctx, "(function(){return true})", 25, "<conf>", JS_EVAL_TYPE_GLOBAL));
      JS_SetPropertyStr(ctx, global, "prompt",
          JS_Eval(ctx, "(function(){return null})", 25, "<pr>", JS_EVAL_TYPE_GLOBAL));
      JS_FreeValue(ctx, noop);
    }

    /* window.open / close */
    JS_SetPropertyStr(ctx, global, "open",
        JS_Eval(ctx, "(function(){return null})", 25, "<wo>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, global, "close",
        JS_Eval(ctx, "(function(){})", 14, "<wc>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, global, "focus",
        JS_Eval(ctx, "(function(){})", 14, "<wf>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, global, "blur",
        JS_Eval(ctx, "(function(){})", 14, "<wb>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, global, "stop",
        JS_Eval(ctx, "(function(){})", 14, "<ws>", JS_EVAL_TYPE_GLOBAL));

    /* window.getSelection */
    JS_SetPropertyStr(ctx, global, "getSelection",
        JS_Eval(ctx, "(function(){return{anchorNode:null,focusNode:null,"
                "anchorOffset:0,focusOffset:0,isCollapsed:true,rangeCount:0,"
                "type:'None',toString:function(){return''},"
                "getRangeAt:function(){return null},"
                "addRange:function(){},"
                "removeAllRanges:function(){},"
                "collapse:function(){}}})",
                308, "<gs>", JS_EVAL_TYPE_GLOBAL));

    /* window reference to self */
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "globalThis", JS_DupValue(ctx, global));

    /* screen object */
    {
        JSValue scr = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, scr, "width", JS_NewInt32(ctx, 900));
        JS_SetPropertyStr(ctx, scr, "height", JS_NewInt32(ctx, 600));
        JS_SetPropertyStr(ctx, scr, "availWidth", JS_NewInt32(ctx, 900));
        JS_SetPropertyStr(ctx, scr, "availHeight", JS_NewInt32(ctx, 548));
        JS_SetPropertyStr(ctx, scr, "colorDepth", JS_NewInt32(ctx, 32));
        JS_SetPropertyStr(ctx, scr, "pixelDepth", JS_NewInt32(ctx, 32));
        JS_SetPropertyStr(ctx, scr, "orientation",
            JS_Eval(ctx, "({type:'landscape-primary',angle:0,addEventListener:function(){}})",
                    65, "<so>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, global, "screen", scr);
    }

    /* window.location */
    {
        JSValue loc = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, loc, "href", JS_NewString(ctx, dom->url));
        /* Parse URL components */
        struct ts_url parsed;
        if (ts_url_parse(dom->url, &parsed) == 0) {
            JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, parsed.host));
            JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, parsed.path));
            char search[520];
            if (parsed.query[0]) { search[0] = '?'; strncpy(search+1, parsed.query, 518); search[519] = 0; }
            else search[0] = 0;
            JS_SetPropertyStr(ctx, loc, "search", JS_NewString(ctx, search));
            char hash[130];
            if (parsed.fragment[0]) { hash[0] = '#'; strncpy(hash+1, parsed.fragment, 127); hash[128] = 0; }
            else hash[0] = 0;
            JS_SetPropertyStr(ctx, loc, "hash", JS_NewString(ctx, hash));
            char origin[280];
            snprintf(origin, sizeof(origin), "%s://%s", parsed.scheme, parsed.host);
            JS_SetPropertyStr(ctx, loc, "origin", JS_NewString(ctx, origin));
            char protocol[20];
            snprintf(protocol, sizeof(protocol), "%s:", parsed.scheme);
            JS_SetPropertyStr(ctx, loc, "protocol", JS_NewString(ctx, protocol));
        }
        /* location methods */
        JS_SetPropertyStr(ctx, loc, "assign",
            JS_Eval(ctx, "(function(u){})", 15, "<la>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, loc, "replace",
            JS_Eval(ctx, "(function(u){})", 15, "<lr>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, loc, "reload",
            JS_Eval(ctx, "(function(){})", 14, "<lrl>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, loc, "toString",
            JS_Eval(ctx, "(function(){return this.href})", 29, "<ls>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, global, "location", loc);
        /* Also set on document */
        JS_SetPropertyStr(ctx, dom->js_document, "location", JS_DupValue(ctx, loc));
    }

    /* navigator — full implementation */
    {
        JSValue nav = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, nav, "userAgent",
            JS_NewString(ctx, "Mozilla/5.0 (TaterTOS64v3; x86_64) AppleWebKit/537.36 "
                        "(KHTML, like Gecko) TaterSurf/2.0 Chrome/120.0.0.0 Safari/537.36"));
        JS_SetPropertyStr(ctx, nav, "appName", JS_NewString(ctx, "Netscape"));
        JS_SetPropertyStr(ctx, nav, "appVersion",
            JS_NewString(ctx, "5.0 (TaterTOS64v3; x86_64)"));
        JS_SetPropertyStr(ctx, nav, "platform", JS_NewString(ctx, "TaterTOS64v3"));
        JS_SetPropertyStr(ctx, nav, "language", JS_NewString(ctx, "en-US"));
        { JSValue langs = JS_NewArray(ctx);
          JS_SetPropertyUint32(ctx, langs, 0, JS_NewString(ctx, "en-US"));
          JS_SetPropertyUint32(ctx, langs, 1, JS_NewString(ctx, "en"));
          JS_SetPropertyStr(ctx, nav, "languages", langs);
        }
        JS_SetPropertyStr(ctx, nav, "onLine", JS_TRUE);
        JS_SetPropertyStr(ctx, nav, "cookieEnabled", JS_TRUE);
        JS_SetPropertyStr(ctx, nav, "doNotTrack", JS_NewString(ctx, "1"));
        JS_SetPropertyStr(ctx, nav, "maxTouchPoints", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, nav, "hardwareConcurrency", JS_NewInt32(ctx, 1));
        JS_SetPropertyStr(ctx, nav, "vendor", JS_NewString(ctx, "TaterTOS"));
        JS_SetPropertyStr(ctx, nav, "product", JS_NewString(ctx, "Gecko"));
        JS_SetPropertyStr(ctx, nav, "productSub", JS_NewString(ctx, "20030107"));
        /* navigator.mediaDevices stub */
        { JSValue md = JS_NewObject(ctx);
          JS_SetPropertyStr(ctx, md, "enumerateDevices",
              JS_Eval(ctx, "(function(){return Promise.resolve([])})",
                      41, "<md>", JS_EVAL_TYPE_GLOBAL));
          JS_SetPropertyStr(ctx, nav, "mediaDevices", md);
        }
        /* navigator.serviceWorker stub */
        { JSValue sw = JS_NewObject(ctx);
          JS_SetPropertyStr(ctx, sw, "register",
              JS_Eval(ctx, "(function(){return Promise.resolve({active:null})})",
                      52, "<sw>", JS_EVAL_TYPE_GLOBAL));
          JS_SetPropertyStr(ctx, sw, "ready",
              JS_Eval(ctx, "Promise.resolve({active:null})", 31, "<swr>", JS_EVAL_TYPE_GLOBAL));
          JS_SetPropertyStr(ctx, nav, "serviceWorker", sw);
        }
        /* navigator.clipboard stub */
        { JSValue cb = JS_NewObject(ctx);
          JS_SetPropertyStr(ctx, cb, "writeText",
              JS_Eval(ctx, "(function(t){return Promise.resolve()})",
                      39, "<cb>", JS_EVAL_TYPE_GLOBAL));
          JS_SetPropertyStr(ctx, cb, "readText",
              JS_Eval(ctx, "(function(){return Promise.resolve('')})",
                      40, "<cbr>", JS_EVAL_TYPE_GLOBAL));
          JS_SetPropertyStr(ctx, nav, "clipboard", cb);
        }
        /* navigator.permissions stub */
        { JSValue perms = JS_NewObject(ctx);
          JS_SetPropertyStr(ctx, perms, "query",
              JS_Eval(ctx, "(function(d){return Promise.resolve({state:'granted',onchange:null})})",
                      71, "<pm>", JS_EVAL_TYPE_GLOBAL));
          JS_SetPropertyStr(ctx, nav, "permissions", perms);
        }
        /* navigator.sendBeacon */
        JS_SetPropertyStr(ctx, nav, "sendBeacon",
            JS_Eval(ctx, "(function(){return true})", 25, "<sb>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, global, "navigator", nav);
    }

    /* ---- console ---- */
    {
        JSValue con = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, con, "log",
            JS_NewCFunction(ctx, ts_js_console_log, "log", 1));
        JS_SetPropertyStr(ctx, con, "warn",
            JS_NewCFunction(ctx, ts_js_console_warn, "warn", 1));
        JS_SetPropertyStr(ctx, con, "error",
            JS_NewCFunction(ctx, ts_js_console_error, "error", 1));
        JS_SetPropertyStr(ctx, con, "info",
            JS_NewCFunction(ctx, ts_js_console_log, "info", 1));
        JS_SetPropertyStr(ctx, con, "debug",
            JS_NewCFunction(ctx, ts_js_console_log, "debug", 1));
        JS_SetPropertyStr(ctx, global, "console", con);
        dom->js_console = JS_DupValue(ctx, con);
    }

    /* ---- Web APIs (Step 10 real implementations + remaining stubs) ---- */

    /* performance.now() — real microsecond timer */
    ts_js__perf_start_time = fry_gettime();
    {
        JSValue perf = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, perf, "now",
            JS_NewCFunction(ctx, ts_js_performance_now, "now", 0));
        /* performance.timing stub */
        { JSValue timing = JS_NewObject(ctx);
          JS_SetPropertyStr(ctx, timing, "navigationStart",
              JS_NewFloat64(ctx, (double)ts_js__perf_start_time));
          JS_SetPropertyStr(ctx, perf, "timing", timing);
        }
        JS_SetPropertyStr(ctx, global, "performance", perf);
    }

    /* localStorage / sessionStorage — real in-memory store */
    JS_SetPropertyStr(ctx, global, "localStorage", ts_js__build_storage_obj(ctx));
    JS_SetPropertyStr(ctx, global, "sessionStorage", ts_js__build_storage_obj(ctx));

    /* getComputedStyle — real implementation */
    JS_SetPropertyStr(ctx, global, "getComputedStyle",
        JS_NewCFunction(ctx, ts_js_get_computed_style, "getComputedStyle", 1));

    /* matchMedia stub */
    {
        /* Returns { matches: false, addEventListener: noop } */
        const char *mm_code =
            "(function(q){return{matches:false,media:q,"
            "addEventListener:function(){},"
            "removeEventListener:function(){}}})";
        JSValue mm = JS_Eval(ctx, mm_code, strlen(mm_code), "<matchMedia>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "matchMedia", mm);
    }

    /* MutationObserver stub */
    {
        const char *mo_code =
            "(function MutationObserver(cb){"
            "this.observe=function(){};"
            "this.disconnect=function(){};"
            "this.takeRecords=function(){return[]};"
            "})";
        JSValue mo = JS_Eval(ctx, mo_code, strlen(mo_code), "<MutationObserver>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "MutationObserver", mo);
    }

    /* IntersectionObserver stub */
    {
        const char *io_code =
            "(function IntersectionObserver(cb,opts){"
            "this.observe=function(el){cb([{isIntersecting:true,target:el}])};"
            "this.unobserve=function(){};"
            "this.disconnect=function(){};"
            "})";
        JSValue io = JS_Eval(ctx, io_code, strlen(io_code), "<IntersectionObserver>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "IntersectionObserver", io);
    }

    /* ResizeObserver stub */
    {
        const char *ro_code =
            "(function ResizeObserver(cb){"
            "this.observe=function(){};"
            "this.unobserve=function(){};"
            "this.disconnect=function(){};"
            "})";
        JSValue ro = JS_Eval(ctx, ro_code, strlen(ro_code), "<ResizeObserver>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "ResizeObserver", ro);
    }

    /* queueMicrotask */
    {
        const char *qmt_code = "(function queueMicrotask(fn){Promise.resolve().then(fn)})";
        JSValue qmt = JS_Eval(ctx, qmt_code, strlen(qmt_code), "<qmt>",
                               JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "queueMicrotask", qmt);
    }

    /* TextEncoder / TextDecoder stubs */
    {
        const char *te_code =
            "(function TextEncoder(){"
            "this.encode=function(s){var a=new Uint8Array(s.length);"
            "for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return a};"
            "this.encoding='utf-8';"
            "})";
        JSValue te = JS_Eval(ctx, te_code, strlen(te_code), "<TextEncoder>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "TextEncoder", te);
    }
    {
        const char *td_code =
            "(function TextDecoder(){"
            "this.decode=function(a){var s='';if(a&&a.length)"
            "for(var i=0;i<a.length;i++)s+=String.fromCharCode(a[i]);return s};"
            "this.encoding='utf-8';"
            "})";
        JSValue td = JS_Eval(ctx, td_code, strlen(td_code), "<TextDecoder>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "TextDecoder", td);
    }

    /* URL constructor — real parser using ts_url */
    JS_SetPropertyStr(ctx, global, "URL",
        JS_NewCFunction(ctx, ts_js_url_constructor, "URL", 2));

    /* AbortController stub */
    {
        const char *ac_code =
            "(function AbortController(){"
            "this.signal={aborted:false,addEventListener:function(){}};"
            "this.abort=function(){this.signal.aborted=true};"
            "})";
        JSValue ac = JS_Eval(ctx, ac_code, strlen(ac_code), "<AbortController>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "AbortController", ac);
    }

    /* history stub */
    {
        JSValue hist = JS_NewObject(ctx);
        const char *ps_code = "(function(s,t,u){})";
        JSValue ps = JS_Eval(ctx, ps_code, strlen(ps_code), "<pushState>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, hist, "pushState", JS_DupValue(ctx, ps));
        JS_SetPropertyStr(ctx, hist, "replaceState", ps);
        JS_SetPropertyStr(ctx, global, "history", hist);
    }

    /* fetch — real HTTP implementation */
    JS_SetPropertyStr(ctx, global, "fetch",
        JS_NewCFunction(ctx, ts_js_fetch, "fetch", 2));

    /* XMLHttpRequest — full implementation using fetch internally */
    {
        const char *xhr_code =
            "(function XMLHttpRequest(){"
            "var self=this;"
            "this.UNSENT=0;this.OPENED=1;this.HEADERS_RECEIVED=2;"
            "this.LOADING=3;this.DONE=4;"
            "this.readyState=0;this.status=0;this.statusText='';"
            "this.responseText='';this.responseXML=null;this.response='';"
            "this.responseType='';this.responseURL='';"
            "this.timeout=0;this.withCredentials=false;"
            "this._headers={};this._method='GET';this._url='';"
            "this._listeners={};"
            "this.addEventListener=function(t,fn){(this._listeners[t]=this._listeners[t]||[]).push(fn)};"
            "this.removeEventListener=function(t,fn){var l=this._listeners[t];if(l){var i=l.indexOf(fn);if(i>=0)l.splice(i,1)}};"
            "this._fire=function(t){var l=this._listeners[t];if(l)l.forEach(function(fn){fn.call(self,{type:t,target:self})});"
            "if(self['on'+t])self['on'+t]({type:t,target:self})};"
            "this.open=function(m,u){this._method=m;this._url=u;this.readyState=1;this._fire('readystatechange')};"
            "this.setRequestHeader=function(k,v){this._headers[k]=v};"
            "this.getResponseHeader=function(k){return this._respHeaders?this._respHeaders[k.toLowerCase()]||null:null};"
            "this.getAllResponseHeaders=function(){return ''};"
            "this.overrideMimeType=function(){};"
            "this.send=function(body){"
            "try{var opts={method:this._method,headers:this._headers};"
            "if(body)opts.body=body;"
            "var r=__xhr_sync_fetch(this._url,JSON.stringify(opts));"
            "if(r){var p=JSON.parse(r);this.status=p.status||0;this.statusText=p.statusText||'';"
            "this.responseText=p.body||'';this.response=this.responseText;this.responseURL=this._url;"
            "this.readyState=4;this._fire('readystatechange');this._fire('load');this._fire('loadend')"
            "}else{this.readyState=4;this._fire('readystatechange');this._fire('error');this._fire('loadend')}"
            "}catch(e){this.readyState=4;this._fire('error');this._fire('loadend')}};"
            "this.abort=function(){this.readyState=0;this._fire('abort')};"
            "})";
        JSValue xhr = JS_Eval(ctx, xhr_code, strlen(xhr_code), "<XHR>",
                               JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "XMLHttpRequest", xhr);
        /* Sync fetch helper for XHR — uses the same C fetch infrastructure */
        JS_SetPropertyStr(ctx, global, "__xhr_sync_fetch",
            JS_NewCFunction(ctx, ts_js_xhr_sync_fetch, "__xhr_sync_fetch", 2));
    }

    /* Image constructor — tracks src for beacon/preload */
    {
        const char *img_code =
            "(function Image(w,h){"
            "this.src='';this.width=w||0;this.height=h||0;this.naturalWidth=0;this.naturalHeight=0;"
            "this.complete=false;this.onload=null;this.onerror=null;"
            "this.addEventListener=function(t,fn){this['on'+t]=fn};"
            "this.removeEventListener=function(){};"
            "this.decode=function(){return Promise.resolve()};"
            "})";
        JSValue img = JS_Eval(ctx, img_code, strlen(img_code), "<Image>",
                               JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "Image", img);
    }

    /* Event constructor — proper options handling */
    {
        const char *ev_code =
            "(function Event(type,opts){"
            "opts=opts||{};"
            "this.type=type;this.bubbles=!!opts.bubbles;this.cancelable=!!opts.cancelable;"
            "this.composed=!!opts.composed;"
            "this.defaultPrevented=false;this.isTrusted=false;"
            "this.timeStamp=performance.now();this.target=null;this.currentTarget=null;"
            "this.eventPhase=0;this.returnValue=true;"
            "this.preventDefault=function(){if(this.cancelable)this.defaultPrevented=true;this.returnValue=false};"
            "this.stopPropagation=function(){this._stop=true};"
            "this.stopImmediatePropagation=function(){this._stop=true;this._stopImm=true};"
            "this.composedPath=function(){return[]};"
            "})";
        JSValue ev = JS_Eval(ctx, ev_code, strlen(ev_code), "<Event>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "Event", ev);
    }

    /* CustomEvent — extends Event with detail */
    {
        const char *ce_code =
            "(function CustomEvent(type,opts){"
            "opts=opts||{};"
            "this.type=type;this.bubbles=!!opts.bubbles;this.cancelable=!!opts.cancelable;"
            "this.composed=!!opts.composed;this.detail=opts.detail||null;"
            "this.defaultPrevented=false;this.isTrusted=false;"
            "this.timeStamp=performance.now();this.target=null;this.currentTarget=null;"
            "this.preventDefault=function(){if(this.cancelable)this.defaultPrevented=true};"
            "this.stopPropagation=function(){this._stop=true};"
            "this.stopImmediatePropagation=function(){this._stop=true;this._stopImm=true};"
            "this.composedPath=function(){return[]};"
            "this.initCustomEvent=function(t,b,c,d){this.type=t;this.bubbles=!!b;this.cancelable=!!c;this.detail=d};"
            "})";
        JSValue ce = JS_Eval(ctx, ce_code, strlen(ce_code), "<CustomEvent>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "CustomEvent", ce);
    }

    /* ErrorEvent, MessageEvent, ProgressEvent, PopStateEvent, HashChangeEvent, FocusEvent */
    {
        const char *extra_events =
            "globalThis.ErrorEvent=function(t,o){o=o||{};Event.call(this,t,o);this.message=o.message||'';this.filename=o.filename||'';this.lineno=o.lineno||0;this.colno=o.colno||0;this.error=o.error||null};"
            "globalThis.MessageEvent=function(t,o){o=o||{};Event.call(this,t,o);this.data=o.data;this.origin=o.origin||'';this.source=o.source||null;this.ports=o.ports||[]};"
            "globalThis.ProgressEvent=function(t,o){o=o||{};Event.call(this,t,o);this.loaded=o.loaded||0;this.total=o.total||0;this.lengthComputable=!!o.lengthComputable};"
            "globalThis.PopStateEvent=function(t,o){o=o||{};Event.call(this,t,o);this.state=o.state||null};"
            "globalThis.HashChangeEvent=function(t,o){o=o||{};Event.call(this,t,o);this.oldURL=o.oldURL||'';this.newURL=o.newURL||''};"
            "globalThis.FocusEvent=function(t,o){o=o||{};Event.call(this,t,o);this.relatedTarget=o.relatedTarget||null};"
            "globalThis.KeyboardEvent=function(t,o){o=o||{};Event.call(this,t,o);this.key=o.key||'';this.code=o.code||'';this.keyCode=o.keyCode||0;this.which=o.which||o.keyCode||0;this.ctrlKey=!!o.ctrlKey;this.shiftKey=!!o.shiftKey;this.altKey=!!o.altKey;this.metaKey=!!o.metaKey;this.repeat=!!o.repeat};"
            "globalThis.MouseEvent=function(t,o){o=o||{};Event.call(this,t,o);this.clientX=o.clientX||0;this.clientY=o.clientY||0;this.pageX=o.pageX||0;this.pageY=o.pageY||0;this.screenX=o.screenX||0;this.screenY=o.screenY||0;this.button=o.button||0;this.buttons=o.buttons||0;this.relatedTarget=o.relatedTarget||null;this.ctrlKey=!!o.ctrlKey;this.shiftKey=!!o.shiftKey;this.altKey=!!o.altKey;this.metaKey=!!o.metaKey};"
            "globalThis.PointerEvent=globalThis.MouseEvent;"
            "globalThis.TouchEvent=function(t,o){Event.call(this,t,o||{});this.touches=[];this.targetTouches=[];this.changedTouches=[]};"
            "globalThis.WheelEvent=function(t,o){o=o||{};MouseEvent.call(this,t,o);this.deltaX=o.deltaX||0;this.deltaY=o.deltaY||0;this.deltaZ=o.deltaZ||0;this.deltaMode=o.deltaMode||0};"
            "globalThis.InputEvent=function(t,o){o=o||{};Event.call(this,t,o);this.data=o.data||null;this.inputType=o.inputType||'';this.isComposing=!!o.isComposing};"
            "globalThis.AnimationEvent=function(t,o){o=o||{};Event.call(this,t,o);this.animationName=o.animationName||'';this.elapsedTime=o.elapsedTime||0;this.pseudoElement=o.pseudoElement||''};"
            "globalThis.TransitionEvent=function(t,o){o=o||{};Event.call(this,t,o);this.propertyName=o.propertyName||'';this.elapsedTime=o.elapsedTime||0;this.pseudoElement=o.pseudoElement||''};"
            "globalThis.ClipboardEvent=function(t,o){Event.call(this,t,o||{});this.clipboardData={getData:function(){return''},setData:function(){}}};"
            "globalThis.DragEvent=function(t,o){MouseEvent.call(this,t,o||{});this.dataTransfer={getData:function(){return''},setData:function(){},items:[],files:[],types:[]}};"
            "globalThis.UIEvent=function(t,o){Event.call(this,t,o||{});this.detail=(o&&o.detail)||0;this.view=globalThis};"
            ;
        JS_Eval(ctx, extra_events, strlen(extra_events), "<events>",
                JS_EVAL_TYPE_GLOBAL);
    }

    /* atob / btoa — real base64 */
    JS_SetPropertyStr(ctx, global, "atob",
        JS_NewCFunction(ctx, ts_js_atob, "atob", 1));
    JS_SetPropertyStr(ctx, global, "btoa",
        JS_NewCFunction(ctx, ts_js_btoa, "btoa", 1));

    /* DOMParser — real HTML parser */
    {
        const char *dp_code =
            "(function DOMParser(){"
            "this.parseFromString=function(html,type){return __domparser_parse(html)};"
            "})";
        JSValue dp = JS_Eval(ctx, dp_code, strlen(dp_code), "<DOMParser>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "DOMParser", dp);
        JS_SetPropertyStr(ctx, global, "__domparser_parse",
            JS_NewCFunction(ctx, ts_js_domparser_parse, "__domparser_parse", 1));
    }

    /* FormData — full implementation with iteration */
    {
        const char *fd_code =
            "(function FormData(form){"
            "this._entries=[];"
            "this.append=function(k,v){this._entries.push([k,v])};"
            "this.get=function(k){for(var i=0;i<this._entries.length;i++)if(this._entries[i][0]===k)return this._entries[i][1];return null};"
            "this.getAll=function(k){return this._entries.filter(function(e){return e[0]===k}).map(function(e){return e[1]})};"
            "this.set=function(k,v){for(var i=0;i<this._entries.length;i++)if(this._entries[i][0]===k){this._entries[i][1]=v;return}this._entries.push([k,v])};"
            "this.has=function(k){return this._entries.some(function(e){return e[0]===k})};"
            "this.delete=function(k){this._entries=this._entries.filter(function(e){return e[0]!==k})};"
            "this.entries=function(){return this._entries[Symbol.iterator]()};"
            "this.keys=function(){return this._entries.map(function(e){return e[0]})[Symbol.iterator]()};"
            "this.values=function(){return this._entries.map(function(e){return e[1]})[Symbol.iterator]()};"
            "this.forEach=function(fn,t){this._entries.forEach(function(e){fn.call(t,e[1],e[0],this)},this)};"
            "this[Symbol.iterator]=function(){return this.entries()};"
            "})";
        JSValue fd = JS_Eval(ctx, fd_code, strlen(fd_code), "<FormData>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "FormData", fd);
    }

    /* Headers constructor */
    {
        const char *hdr_code =
            "(function Headers(init){"
            "this._h={};"
            "if(init){if(init instanceof Headers)Object.assign(this._h,init._h);"
            "else if(typeof init==='object')for(var k in init)this._h[k.toLowerCase()]=init[k]};"
            "this.append=function(k,v){k=k.toLowerCase();this._h[k]=this._h[k]?this._h[k]+', '+v:v};"
            "this.delete=function(k){delete this._h[k.toLowerCase()]};"
            "this.get=function(k){return this._h[k.toLowerCase()]||null};"
            "this.has=function(k){return k.toLowerCase() in this._h};"
            "this.set=function(k,v){this._h[k.toLowerCase()]=v};"
            "this.entries=function(){return Object.entries(this._h)[Symbol.iterator]()};"
            "this.keys=function(){return Object.keys(this._h)[Symbol.iterator]()};"
            "this.values=function(){return Object.values(this._h)[Symbol.iterator]()};"
            "this.forEach=function(fn,t){for(var k in this._h)fn.call(t,this._h[k],k,this)};"
            "this[Symbol.iterator]=function(){return this.entries()};"
            "})";
        JSValue hdr = JS_Eval(ctx, hdr_code, strlen(hdr_code), "<Headers>",
                               JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "Headers", hdr);
    }

    /* Request constructor */
    {
        const char *req_code =
            "(function Request(input,init){"
            "init=init||{};"
            "if(typeof input==='string'){this.url=input}else if(input&&input.url){this.url=input.url}else{this.url=''};"
            "this.method=(init.method||'GET').toUpperCase();"
            "this.headers=new Headers(init.headers);"
            "this.body=init.body||null;"
            "this.mode=init.mode||'cors';"
            "this.credentials=init.credentials||'same-origin';"
            "this.cache=init.cache||'default';"
            "this.redirect=init.redirect||'follow';"
            "this.referrer=init.referrer||'';"
            "this.referrerPolicy=init.referrerPolicy||'';"
            "this.signal=init.signal||{aborted:false,addEventListener:function(){}};"
            "this.clone=function(){return new Request(this.url,{method:this.method,headers:this.headers,body:this.body})};"
            "})";
        JSValue req = JS_Eval(ctx, req_code, strlen(req_code), "<Request>",
                               JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "Request", req);
    }

    /* Response constructor */
    {
        const char *resp_code =
            "(function Response(body,init){"
            "init=init||{};"
            "this.status=init.status||200;"
            "this.statusText=init.statusText||'OK';"
            "this.ok=this.status>=200&&this.status<300;"
            "this.headers=new Headers(init.headers);"
            "this.type='basic';this.url='';"
            "this.redirected=false;this.bodyUsed=false;"
            "var _body=body||'';"
            "this.text=function(){this.bodyUsed=true;return Promise.resolve(typeof _body==='string'?_body:JSON.stringify(_body))};"
            "this.json=function(){this.bodyUsed=true;return this.text().then(JSON.parse)};"
            "this.blob=function(){this.bodyUsed=true;return this.text().then(function(t){return new Blob([t])})};"
            "this.arrayBuffer=function(){this.bodyUsed=true;return Promise.resolve(new ArrayBuffer(0))};"
            "this.clone=function(){return new Response(_body,{status:this.status,statusText:this.statusText,headers:this.headers})};"
            "});"
            "Response.json=function(data,init){return new Response(JSON.stringify(data),Object.assign({},{headers:{'Content-Type':'application/json'}},init))};"
            "Response.error=function(){return new Response(null,{status:0})};"
            "Response.redirect=function(url,status){return new Response(null,{status:status||302,headers:{Location:url}})};"
            ;
        JS_Eval(ctx, resp_code, strlen(resp_code), "<Response>",
                JS_EVAL_TYPE_GLOBAL);
    }

    /* URLSearchParams constructor */
    {
        const char *usp_code =
            "(function URLSearchParams(init){"
            "this._p=[];"
            "if(typeof init==='string'){init=init.replace(/^\\?/,'');init.split('&').forEach(function(pair){"
            "var kv=pair.split('=');if(kv[0])this._p.push([decodeURIComponent(kv[0]),decodeURIComponent(kv[1]||'')])},this)}"
            "else if(init&&typeof init==='object'){for(var k in init)this._p.push([k,String(init[k])])};"
            "this.append=function(k,v){this._p.push([k,String(v)])};"
            "this.delete=function(k){this._p=this._p.filter(function(e){return e[0]!==k})};"
            "this.get=function(k){for(var i=0;i<this._p.length;i++)if(this._p[i][0]===k)return this._p[i][1];return null};"
            "this.getAll=function(k){return this._p.filter(function(e){return e[0]===k}).map(function(e){return e[1]})};"
            "this.has=function(k){return this._p.some(function(e){return e[0]===k})};"
            "this.set=function(k,v){var found=false;this._p=this._p.filter(function(e){if(e[0]===k&&!found){e[1]=String(v);found=true;return true}return e[0]!==k});if(!found)this._p.push([k,String(v)])};"
            "this.sort=function(){this._p.sort(function(a,b){return a[0]<b[0]?-1:a[0]>b[0]?1:0})};"
            "this.entries=function(){return this._p[Symbol.iterator]()};"
            "this.keys=function(){return this._p.map(function(e){return e[0]})[Symbol.iterator]()};"
            "this.values=function(){return this._p.map(function(e){return e[1]})[Symbol.iterator]()};"
            "this.forEach=function(fn,t){this._p.forEach(function(e){fn.call(t,e[1],e[0],this)},this)};"
            "this.toString=function(){return this._p.map(function(e){return encodeURIComponent(e[0])+'='+encodeURIComponent(e[1])}).join('&')};"
            "this[Symbol.iterator]=function(){return this.entries()};"
            "this.size=this._p.length;"
            "})";
        JSValue usp = JS_Eval(ctx, usp_code, strlen(usp_code), "<URLSearchParams>",
                               JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "URLSearchParams", usp);
    }

    /* Blob constructor */
    {
        const char *blob_code =
            "(function Blob(parts,opts){"
            "opts=opts||{};"
            "this.type=opts.type||'';"
            "var content='';"
            "if(parts){for(var i=0;i<parts.length;i++){"
            "if(typeof parts[i]==='string')content+=parts[i];"
            "else if(parts[i]&&parts[i].constructor===ArrayBuffer)content+=String.fromCharCode.apply(null,new Uint8Array(parts[i]));"
            "else if(parts[i]&&parts[i].length)for(var j=0;j<parts[i].length;j++)content+=String.fromCharCode(parts[i][j]);"
            "else content+=String(parts[i])"
            "}};"
            "this.size=content.length;"
            "this._data=content;"
            "this.slice=function(s,e,t){return new Blob([content.slice(s,e)],{type:t||this.type})};"
            "this.text=function(){return Promise.resolve(content)};"
            "this.arrayBuffer=function(){var ab=new ArrayBuffer(content.length);var v=new Uint8Array(ab);for(var i=0;i<content.length;i++)v[i]=content.charCodeAt(i);return Promise.resolve(ab)};"
            "this.stream=function(){return{getReader:function(){var done=false;return{read:function(){if(done)return Promise.resolve({done:true,value:undefined});done=true;var enc=new TextEncoder();return Promise.resolve({done:false,value:enc.encode(content)})},cancel:function(){done=true}}}};};"
            "})";
        JSValue blob = JS_Eval(ctx, blob_code, strlen(blob_code), "<Blob>",
                                JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "Blob", blob);
    }

    /* File constructor (extends Blob) */
    {
        const char *file_code =
            "(function File(bits,name,opts){"
            "Blob.call(this,bits,opts);"
            "this.name=name||'';"
            "this.lastModified=Date.now();"
            "})";
        JS_Eval(ctx, file_code, strlen(file_code), "<File>", JS_EVAL_TYPE_GLOBAL);
    }

    /* FileReader */
    {
        const char *fr_code =
            "(function FileReader(){"
            "this.EMPTY=0;this.LOADING=1;this.DONE=2;"
            "this.readyState=0;this.result=null;this.error=null;"
            "this.onload=null;this.onerror=null;this.onloadend=null;this.onabort=null;this.onprogress=null;"
            "this._listeners={};"
            "this.addEventListener=function(t,fn){(this._listeners[t]=this._listeners[t]||[]).push(fn)};"
            "this.removeEventListener=function(t,fn){var l=this._listeners[t];if(l){var i=l.indexOf(fn);if(i>=0)l.splice(i,1)}};"
            "this._fire=function(t){var self=this;var l=this._listeners[t];if(l)l.forEach(function(fn){fn({type:t,target:self})});"
            "if(this['on'+t])this['on'+t]({type:t,target:this})};"
            "this.readAsText=function(blob){var self=this;this.readyState=1;"
            "Promise.resolve().then(function(){self.result=blob._data||'';self.readyState=2;self._fire('load');self._fire('loadend')})};"
            "this.readAsDataURL=function(blob){var self=this;this.readyState=1;"
            "Promise.resolve().then(function(){self.result='data:'+(blob.type||'application/octet-stream')+';base64,'+btoa(blob._data||'');self.readyState=2;self._fire('load');self._fire('loadend')})};"
            "this.readAsArrayBuffer=function(blob){var self=this;this.readyState=1;"
            "Promise.resolve().then(function(){var d=blob._data||'';var ab=new ArrayBuffer(d.length);var v=new Uint8Array(ab);for(var i=0;i<d.length;i++)v[i]=d.charCodeAt(i);self.result=ab;self.readyState=2;self._fire('load');self._fire('loadend')})};"
            "this.abort=function(){this.readyState=2;this._fire('abort');this._fire('loadend')};"
            "})";
        JSValue fr = JS_Eval(ctx, fr_code, strlen(fr_code), "<FileReader>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "FileReader", fr);
    }

    /* crypto.getRandomValues — real kernel RNG */
    {
        JSValue crypto = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, crypto, "getRandomValues",
            JS_NewCFunction(ctx, ts_js_crypto_get_random_values, "getRandomValues", 1));
        /* crypto.randomUUID */
        { const char *uuid_code =
              "(function(){var b=new Uint8Array(16);crypto.getRandomValues(b);"
              "b[6]=(b[6]&0x0f)|0x40;b[8]=(b[8]&0x3f)|0x80;"
              "var h='0123456789abcdef',s='';for(var i=0;i<16;i++){"
              "s+=h[b[i]>>4]+h[b[i]&0xf];"
              "if(i===3||i===5||i===7||i===9)s+='-'}return s})";
          JSValue fn = JS_Eval(ctx, uuid_code, strlen(uuid_code), "<uuid>",
                                JS_EVAL_TYPE_GLOBAL);
          JS_SetPropertyStr(ctx, crypto, "randomUUID", fn);
        }
        /* SubtleCrypto stub */
        { JSValue subtle = JS_NewObject(ctx);
          const char *stub = "(function(){return Promise.resolve(new ArrayBuffer(0))})";
          JSValue fn = JS_Eval(ctx, stub, strlen(stub), "<subtle>", JS_EVAL_TYPE_GLOBAL);
          JS_SetPropertyStr(ctx, subtle, "digest", JS_DupValue(ctx, fn));
          JS_SetPropertyStr(ctx, subtle, "encrypt", JS_DupValue(ctx, fn));
          JS_SetPropertyStr(ctx, subtle, "decrypt", JS_DupValue(ctx, fn));
          JS_SetPropertyStr(ctx, subtle, "sign", JS_DupValue(ctx, fn));
          JS_SetPropertyStr(ctx, subtle, "verify",
              JS_Eval(ctx, "(function(){return Promise.resolve(true)})", 42, "<sv>", JS_EVAL_TYPE_GLOBAL));
          JS_SetPropertyStr(ctx, subtle, "generateKey", JS_DupValue(ctx, fn));
          JS_SetPropertyStr(ctx, subtle, "importKey", JS_DupValue(ctx, fn));
          JS_SetPropertyStr(ctx, subtle, "exportKey", JS_DupValue(ctx, fn));
          JS_SetPropertyStr(ctx, subtle, "deriveBits", JS_DupValue(ctx, fn));
          JS_SetPropertyStr(ctx, subtle, "deriveKey", JS_DupValue(ctx, fn));
          JS_FreeValue(ctx, fn);
          JS_SetPropertyStr(ctx, crypto, "subtle", subtle);
        }
        JS_SetPropertyStr(ctx, global, "crypto", crypto);
    }

    /* structuredClone */
    {
        const char *sc_code = "(function structuredClone(v){return JSON.parse(JSON.stringify(v))})";
        JSValue sc = JS_Eval(ctx, sc_code, strlen(sc_code), "<structuredClone>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "structuredClone", sc);
    }

    /* CSS.supports */
    {
        JSValue css_obj = JS_NewObject(ctx);
        const char *cs_code =
            "(function supports(prop,val){"
            "if(arguments.length===1)return true;"  /* liberal — pretend we support everything */
            "return true})";
        JS_SetPropertyStr(ctx, css_obj, "supports",
            JS_Eval(ctx, cs_code, strlen(cs_code), "<css>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, css_obj, "escape",
            JS_Eval(ctx, "(function(s){return s.replace(/([\\\\!\"#$%&'()*+,./:;<=>?@\\[\\]^`{|}~])/g,'\\\\$1')})",
                    88, "<ce>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, global, "CSS", css_obj);
    }

    /* Web Components: customElements registry */
    {
        const char *ce_code =
            "(function(){"
            "var registry={};"
            "var waiting={};"
            "globalThis.customElements={"
            "define:function(name,ctor,opts){"
            "registry[name]={ctor:ctor,opts:opts||{}};"
            "if(waiting[name]){waiting[name].forEach(function(r){r()});delete waiting[name]}},"
            "get:function(name){var e=registry[name];return e?e.ctor:undefined},"
            "whenDefined:function(name){"
            "if(registry[name])return Promise.resolve(registry[name].ctor);"
            "return new Promise(function(r){(waiting[name]=waiting[name]||[]).push(function(){r(registry[name].ctor)})})},"
            "upgrade:function(){}"
            "}})()";
        JS_Eval(ctx, ce_code, strlen(ce_code), "<customElements>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Shadow DOM: Element.attachShadow polyfill (stored as child container) */
    {
        const char *shadow_code =
            "(function(){"
            "var origBind=globalThis.__ts_bind_node;"
            "Element.prototype=Element.prototype||{};"
            "})()";
        /* We'll handle attachShadow in the node binding instead */
        JS_Eval(ctx, shadow_code, strlen(shadow_code), "<shadow>", JS_EVAL_TYPE_GLOBAL);
    }

    /* document extra properties and methods */
    JS_SetPropertyStr(ctx, dom->js_document, "readyState",
        JS_NewString(ctx, "complete"));
    JS_SetPropertyStr(ctx, dom->js_document, "visibilityState",
        JS_NewString(ctx, "visible"));
    JS_SetPropertyStr(ctx, dom->js_document, "hidden", JS_FALSE);
    JS_SetPropertyStr(ctx, dom->js_document, "compatMode",
        JS_NewString(ctx, "CSS1Compat"));
    JS_SetPropertyStr(ctx, dom->js_document, "characterSet",
        JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, dom->js_document, "charset",
        JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, dom->js_document, "contentType",
        JS_NewString(ctx, "text/html"));
    JS_SetPropertyStr(ctx, dom->js_document, "doctype", JS_NULL);
    JS_SetPropertyStr(ctx, dom->js_document, "domain",
        JS_NewString(ctx, dom->url));
    JS_SetPropertyStr(ctx, dom->js_document, "referrer",
        JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, dom->js_document, "title",
        JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, dom->js_document, "dir",
        JS_NewString(ctx, "ltr"));
    JS_SetPropertyStr(ctx, dom->js_document, "defaultView",
        JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, dom->js_document, "activeElement",
        dom->body_node >= 0 ? ts_js_wrap_node(ctx, dom, dom->body_node) : JS_NULL);
    JS_SetPropertyStr(ctx, dom->js_document, "getElementsByClassName",
        JS_NewCFunction(ctx, ts_js_doc_get_elements_by_class, "getElementsByClassName", 1));
    JS_SetPropertyStr(ctx, dom->js_document, "createEvent",
        JS_NewCFunction(ctx, ts_js_doc_create_event, "createEvent", 1));
    JS_SetPropertyStr(ctx, dom->js_document, "createRange",
        JS_Eval(ctx, "(function(){return{collapsed:true,startOffset:0,endOffset:0,"
                "startContainer:null,endContainer:null,"
                "setStart:function(){},setEnd:function(){},"
                "setStartBefore:function(){},setStartAfter:function(){},"
                "setEndBefore:function(){},setEndAfter:function(){},"
                "collapse:function(){},selectNode:function(){},"
                "selectNodeContents:function(){},"
                "cloneContents:function(){return document.createDocumentFragment()},"
                "deleteContents:function(){},extractContents:function(){return document.createDocumentFragment()},"
                "insertNode:function(){},surroundContents:function(){},"
                "getBoundingClientRect:function(){return{x:0,y:0,width:0,height:0,top:0,right:0,bottom:0,left:0}},"
                "getClientRects:function(){return[]},"
                "toString:function(){return''}}})",
                720, "<range>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, dom->js_document, "createNodeIterator",
        JS_Eval(ctx, "(function(root){return{nextNode:function(){return null},previousNode:function(){return null}}})",
                90, "<ni>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, dom->js_document, "createTreeWalker",
        JS_Eval(ctx, "(function(root){return{currentNode:root,nextNode:function(){return null},"
                "previousNode:function(){return null},firstChild:function(){return null},"
                "lastChild:function(){return null},nextSibling:function(){return null},"
                "previousSibling:function(){return null},parentNode:function(){return null}}})",
                280, "<tw>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, dom->js_document, "hasFocus",
        JS_Eval(ctx, "(function(){return true})", 25, "<hf>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, dom->js_document, "getSelection",
        JS_GetPropertyStr(ctx, global, "getSelection"));
    JS_SetPropertyStr(ctx, dom->js_document, "exitFullscreen",
        JS_Eval(ctx, "(function(){return Promise.resolve()})", 38, "<ef>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, dom->js_document, "fullscreenElement", JS_NULL);
    JS_SetPropertyStr(ctx, dom->js_document, "fullscreenEnabled", JS_FALSE);
    JS_SetPropertyStr(ctx, dom->js_document, "designMode",
        JS_NewString(ctx, "off"));
    /* document.implementation */
    { JSValue impl = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, impl, "hasFeature",
          JS_Eval(ctx, "(function(){return true})", 25, "<hf2>", JS_EVAL_TYPE_GLOBAL));
      JS_SetPropertyStr(ctx, impl, "createHTMLDocument",
          JS_Eval(ctx, "(function(title){return document})", 34, "<chd>", JS_EVAL_TYPE_GLOBAL));
      JS_SetPropertyStr(ctx, dom->js_document, "implementation", impl);
    }
    /* document.cookie */
    JS_SetPropertyStr(ctx, dom->js_document, "cookie", JS_NewString(ctx, ""));

    /* document.fonts stub (FontFaceSet) */
    { JSValue fonts = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, fonts, "ready",
          JS_Eval(ctx, "Promise.resolve()", 17, "<fr>", JS_EVAL_TYPE_GLOBAL));
      JS_SetPropertyStr(ctx, fonts, "check",
          JS_Eval(ctx, "(function(){return true})", 25, "<fc>", JS_EVAL_TYPE_GLOBAL));
      JS_SetPropertyStr(ctx, fonts, "load",
          JS_Eval(ctx, "(function(){return Promise.resolve([])})", 40, "<fl>", JS_EVAL_TYPE_GLOBAL));
      JS_SetPropertyStr(ctx, fonts, "forEach",
          JS_Eval(ctx, "(function(){})", 14, "<ff>", JS_EVAL_TYPE_GLOBAL));
      JS_SetPropertyStr(ctx, fonts, "status", JS_NewString(ctx, "loaded"));
      JS_SetPropertyStr(ctx, dom->js_document, "fonts", fonts);
    }

    /* document.scrollingElement */
    if (dom->html_node >= 0) {
        JSValue se = ts_js_wrap_node(ctx, dom, dom->html_node);
        ts_dom_bind_node_methods(ctx, se);
        JS_SetPropertyStr(ctx, dom->js_document, "scrollingElement", se);
    }

    /* ---- Miscellaneous global APIs ---- */

    /* EventTarget base (for non-DOM event targets) */
    {
        const char *et_code =
            "(function EventTarget(){"
            "this._l={};"
            "this.addEventListener=function(t,fn){(this._l[t]=this._l[t]||[]).push(fn)};"
            "this.removeEventListener=function(t,fn){var l=this._l[t];if(l){var i=l.indexOf(fn);if(i>=0)l.splice(i,1)}};"
            "this.dispatchEvent=function(e){var l=this._l[e.type];if(l)l.forEach(function(fn){fn(e)})};"
            "})";
        JSValue et = JS_Eval(ctx, et_code, strlen(et_code), "<EventTarget>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "EventTarget", et);
    }

    /* MessageChannel / MessagePort */
    {
        const char *mc_code =
            "(function(){"
            "globalThis.MessagePort=function(){this.onmessage=null;this.onmessageerror=null;"
            "this.postMessage=function(){};this.start=function(){};this.close=function(){};"
            "this.addEventListener=function(){};this.removeEventListener=function(){}};"
            "globalThis.MessageChannel=function(){this.port1=new MessagePort();this.port2=new MessagePort()};"
            "globalThis.BroadcastChannel=function(name){this.name=name;this.onmessage=null;"
            "this.postMessage=function(){};this.close=function(){};"
            "this.addEventListener=function(){};this.removeEventListener=function(){}};"
            "})()";
        JS_Eval(ctx, mc_code, strlen(mc_code), "<MessageChannel>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Worker stub */
    {
        const char *w_code =
            "(function Worker(url){"
            "this.onmessage=null;this.onerror=null;"
            "this.postMessage=function(){};"
            "this.terminate=function(){};"
            "this.addEventListener=function(){};"
            "this.removeEventListener=function(){};"
            "})";
        JSValue w = JS_Eval(ctx, w_code, strlen(w_code), "<Worker>", JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "Worker", w);
    }

    /* WebSocket stub */
    {
        const char *ws_code =
            "(function WebSocket(url,protocols){"
            "this.url=url;this.readyState=0;this.CONNECTING=0;this.OPEN=1;this.CLOSING=2;this.CLOSED=3;"
            "this.protocol='';this.extensions='';this.bufferedAmount=0;this.binaryType='blob';"
            "this.onopen=null;this.onclose=null;this.onerror=null;this.onmessage=null;"
            "this.send=function(){};"
            "this.close=function(){this.readyState=3;if(this.onclose)this.onclose({code:1000,reason:'',wasClean:true})};"
            "this.addEventListener=function(){};this.removeEventListener=function(){};"
            "var self=this;Promise.resolve().then(function(){self.readyState=3;"
            "if(self.onerror)self.onerror(new Event('error'));"
            "if(self.onclose)self.onclose({code:1006,reason:'Not supported',wasClean:false})});"
            "})";
        JSValue ws = JS_Eval(ctx, ws_code, strlen(ws_code), "<WebSocket>", JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "WebSocket", ws);
    }

    /* ReadableStream / WritableStream / TransformStream stubs */
    {
        const char *stream_code =
            "(function(){"
            "globalThis.ReadableStream=function(src){"
            "this.locked=false;"
            "this.cancel=function(){return Promise.resolve()};"
            "this.getReader=function(){var done=false;return{read:function(){return done?Promise.resolve({done:true}):("
            "done=true,src&&src.start?Promise.resolve({done:false,value:undefined}):Promise.resolve({done:true}))}"
            ",releaseLock:function(){},cancel:function(){return Promise.resolve()},closed:Promise.resolve()}};"
            "this.pipeThrough=function(t){return t.readable};"
            "this.pipeTo=function(){return Promise.resolve()};"
            "this.tee=function(){return[this,this]};"
            "};"
            "globalThis.WritableStream=function(){this.locked=false;"
            "this.getWriter=function(){return{write:function(){return Promise.resolve()},close:function(){return Promise.resolve()},"
            "abort:function(){return Promise.resolve()},releaseLock:function(){},closed:Promise.resolve(),ready:Promise.resolve()}};"
            "this.abort=function(){return Promise.resolve()};"
            "this.close=function(){return Promise.resolve()}};"
            "globalThis.TransformStream=function(){this.readable=new ReadableStream();"
            "this.writable=new WritableStream()};"
            "})()";
        JS_Eval(ctx, stream_code, strlen(stream_code), "<Streams>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Intl object stubs (some sites check for Intl.DateTimeFormat etc.) */
    {
        const char *intl_code =
            "(function(){"
            "if(!globalThis.Intl)globalThis.Intl={};"
            "var I=globalThis.Intl;"
            "if(!I.DateTimeFormat)I.DateTimeFormat=function(l,o){this.format=function(d){return(d||new Date()).toISOString()};"
            "this.resolvedOptions=function(){return{locale:'en-US',calendar:'gregory',numberingSystem:'latn',timeZone:'UTC'}}};"
            "if(!I.NumberFormat)I.NumberFormat=function(l,o){this.format=function(n){return String(n)};"
            "this.resolvedOptions=function(){return{locale:'en-US',numberingSystem:'latn',style:'decimal'}}};"
            "if(!I.Collator)I.Collator=function(l,o){this.compare=function(a,b){return a<b?-1:a>b?1:0}};"
            "if(!I.PluralRules)I.PluralRules=function(l,o){this.select=function(n){return n===1?'one':'other'}};"
            "if(!I.RelativeTimeFormat)I.RelativeTimeFormat=function(l,o){this.format=function(v,u){return v+' '+u+'s ago'}};"
            "if(!I.ListFormat)I.ListFormat=function(l,o){this.format=function(list){return list.join(', ')}};"
            "if(!I.Segmenter)I.Segmenter=function(l,o){"
            "this.segment=function(s){var r=[];for(var i=0;i<s.length;i++)r.push({segment:s[i],index:i,input:s});r[Symbol.iterator]=function(){var j=0;return{next:function(){return j<r.length?{value:r[j++],done:false}:{done:true}}}};return r}};"
            "})()";
        JS_Eval(ctx, intl_code, strlen(intl_code), "<Intl>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Notification stub */
    {
        const char *notif_code =
            "(function(){"
            "globalThis.Notification=function(title,opts){this.title=title;this.body=(opts&&opts.body)||'';this.close=function(){}};"
            "Notification.permission='default';"
            "Notification.requestPermission=function(){return Promise.resolve('denied')};"
            "})()";
        JS_Eval(ctx, notif_code, strlen(notif_code), "<Notification>", JS_EVAL_TYPE_GLOBAL);
    }

    /* IntersectionObserverEntry */
    {
        const char *ioe_code =
            "(function(){"
            "globalThis.IntersectionObserverEntry=function(opts){"
            "this.boundingClientRect=opts.boundingClientRect||{x:0,y:0,width:0,height:0,top:0,right:0,bottom:0,left:0};"
            "this.intersectionRatio=opts.intersectionRatio||0;"
            "this.intersectionRect=opts.intersectionRect||{x:0,y:0,width:0,height:0,top:0,right:0,bottom:0,left:0};"
            "this.isIntersecting=opts.isIntersecting||false;"
            "this.rootBounds=opts.rootBounds||null;"
            "this.target=opts.target||null;"
            "this.time=opts.time||0;"
            "};"
            "})()";
        JS_Eval(ctx, ioe_code, strlen(ioe_code), "<IOE>", JS_EVAL_TYPE_GLOBAL);
    }

    /* IdleCallback API (requestIdleCallback / cancelIdleCallback) */
    {
        const char *ric_code =
            "(function(){"
            "var _ricId=0;"
            "globalThis.requestIdleCallback=function(cb,opts){"
            "return setTimeout(function(){cb({didTimeout:false,timeRemaining:function(){return 50}})},1)};"
            "globalThis.cancelIdleCallback=function(id){clearTimeout(id)};"
            "})()";
        JS_Eval(ctx, ric_code, strlen(ric_code), "<RIC>", JS_EVAL_TYPE_GLOBAL);
    }

    /* ResizeObserverEntry */
    {
        const char *roe_code =
            "(function(){"
            "globalThis.ResizeObserverEntry=function(target){"
            "this.target=target;"
            "this.contentRect={x:0,y:0,width:0,height:0,top:0,left:0,bottom:0,right:0};"
            "this.borderBoxSize=[{blockSize:0,inlineSize:0}];"
            "this.contentBoxSize=[{blockSize:0,inlineSize:0}];"
            "this.devicePixelContentBoxSize=[{blockSize:0,inlineSize:0}];"
            "};"
            "})()";
        JS_Eval(ctx, roe_code, strlen(roe_code), "<ROE>", JS_EVAL_TYPE_GLOBAL);
    }

    /* PerformanceObserver stub */
    {
        const char *po_code =
            "(function(){"
            "globalThis.PerformanceObserver=function(cb){"
            "this.observe=function(){};this.disconnect=function(){};"
            "this.takeRecords=function(){return[]}};"
            "PerformanceObserver.supportedEntryTypes=['measure','navigation','resource','paint','largest-contentful-paint','first-input','layout-shift'];"
            "})()";
        JS_Eval(ctx, po_code, strlen(po_code), "<PO>", JS_EVAL_TYPE_GLOBAL);
    }

    /* ReportingObserver stub */
    {
        const char *rpo_code =
            "(function(){"
            "globalThis.ReportingObserver=function(cb,opts){"
            "this.observe=function(){};this.disconnect=function(){};"
            "this.takeRecords=function(){return[]}};"
            "})()";
        JS_Eval(ctx, rpo_code, strlen(rpo_code), "<RPO>", JS_EVAL_TYPE_GLOBAL);
    }

    /* document.adoptNode / importNode */
    JS_SetPropertyStr(ctx, dom->js_document, "adoptNode",
        JS_Eval(ctx, "(function(n){return n})", 23, "<an>", JS_EVAL_TYPE_GLOBAL));
    JS_SetPropertyStr(ctx, dom->js_document, "importNode",
        JS_Eval(ctx, "(function(n,deep){return n.cloneNode(!!deep)})", 47, "<in>", JS_EVAL_TYPE_GLOBAL));

    /* CSSStyleSheet / StyleSheet constructors */
    {
        const char *css_sheet_code =
            "(function(){"
            "globalThis.CSSStyleSheet=function(){"
            "this.cssRules=[];this.rules=this.cssRules;this.disabled=false;"
            "this.insertRule=function(rule,idx){this.cssRules.splice(idx||0,0,{cssText:rule});return idx||0};"
            "this.deleteRule=function(idx){this.cssRules.splice(idx,1)};"
            "this.addRule=function(sel,style){this.insertRule(sel+'{'+style+'}',this.cssRules.length)};"
            "this.removeRule=function(idx){this.deleteRule(idx)};"
            "this.replace=function(text){return Promise.resolve(this)};"
            "this.replaceSync=function(text){return this};"
            "};"
            "globalThis.StyleSheet=globalThis.CSSStyleSheet;"
            "globalThis.CSSStyleDeclaration=function(){this.length=0;this.getPropertyValue=function(){return''};"
            "this.setProperty=function(){};this.removeProperty=function(){return''};this.cssText=''};"
            "})()";
        JS_Eval(ctx, css_sheet_code, strlen(css_sheet_code), "<CSSSheet>", JS_EVAL_TYPE_GLOBAL);
    }

    /* document.styleSheets */
    { JSValue sheets = JS_NewArray(ctx);
      JS_SetPropertyStr(ctx, sheets, "length", JS_NewInt32(ctx, 0));
      JS_SetPropertyStr(ctx, dom->js_document, "styleSheets", sheets);
    }

    /* Element.prototype helpers installed via eval */
    {
        const char *proto_code =
            "(function(){"
            /* NodeList.prototype.forEach */
            "if(typeof NodeList!=='undefined'&&NodeList.prototype&&!NodeList.prototype.forEach)"
            "NodeList.prototype.forEach=Array.prototype.forEach;"
            /* HTMLCollection support */
            "globalThis.HTMLCollection=globalThis.HTMLCollection||function(){};"
            "globalThis.NodeList=globalThis.NodeList||function(){};"
            "globalThis.HTMLElement=globalThis.HTMLElement||function(){};"
            "globalThis.HTMLDivElement=globalThis.HTMLDivElement||function(){};"
            "globalThis.HTMLSpanElement=globalThis.HTMLSpanElement||function(){};"
            "globalThis.HTMLInputElement=globalThis.HTMLInputElement||function(){};"
            "globalThis.HTMLButtonElement=globalThis.HTMLButtonElement||function(){};"
            "globalThis.HTMLAnchorElement=globalThis.HTMLAnchorElement||function(){};"
            "globalThis.HTMLImageElement=globalThis.HTMLImageElement||function(){};"
            "globalThis.HTMLCanvasElement=globalThis.HTMLCanvasElement||function(){};"
            "globalThis.HTMLVideoElement=globalThis.HTMLVideoElement||function(){};"
            "globalThis.HTMLAudioElement=globalThis.HTMLAudioElement||function(){};"
            "globalThis.HTMLFormElement=globalThis.HTMLFormElement||function(){};"
            "globalThis.HTMLSelectElement=globalThis.HTMLSelectElement||function(){};"
            "globalThis.HTMLTextAreaElement=globalThis.HTMLTextAreaElement||function(){};"
            "globalThis.HTMLTemplateElement=globalThis.HTMLTemplateElement||function(){};"
            "globalThis.HTMLScriptElement=globalThis.HTMLScriptElement||function(){};"
            "globalThis.HTMLStyleElement=globalThis.HTMLStyleElement||function(){};"
            "globalThis.HTMLLinkElement=globalThis.HTMLLinkElement||function(){};"
            "globalThis.HTMLMetaElement=globalThis.HTMLMetaElement||function(){};"
            "globalThis.HTMLHeadElement=globalThis.HTMLHeadElement||function(){};"
            "globalThis.HTMLBodyElement=globalThis.HTMLBodyElement||function(){};"
            "globalThis.HTMLHtmlElement=globalThis.HTMLHtmlElement||function(){};"
            "globalThis.HTMLIFrameElement=globalThis.HTMLIFrameElement||function(){};"
            "globalThis.HTMLTableElement=globalThis.HTMLTableElement||function(){};"
            "globalThis.HTMLTableRowElement=globalThis.HTMLTableRowElement||function(){};"
            "globalThis.HTMLTableCellElement=globalThis.HTMLTableCellElement||function(){};"
            "globalThis.HTMLParagraphElement=globalThis.HTMLParagraphElement||function(){};"
            "globalThis.HTMLPreElement=globalThis.HTMLPreElement||function(){};"
            "globalThis.HTMLUListElement=globalThis.HTMLUListElement||function(){};"
            "globalThis.HTMLOListElement=globalThis.HTMLOListElement||function(){};"
            "globalThis.HTMLLIElement=globalThis.HTMLLIElement||function(){};"
            "globalThis.HTMLHeadingElement=globalThis.HTMLHeadingElement||function(){};"
            "globalThis.SVGElement=globalThis.SVGElement||function(){};"
            "globalThis.SVGSVGElement=globalThis.SVGSVGElement||function(){};"
            "globalThis.DocumentFragment=globalThis.DocumentFragment||function(){};"
            "globalThis.Document=globalThis.Document||function(){};"
            "globalThis.Node=globalThis.Node||function(){};"
            "globalThis.CharacterData=globalThis.CharacterData||function(){};"
            "globalThis.Text=globalThis.Text||function(){};"
            "globalThis.Comment=globalThis.Comment||function(){};"
            "globalThis.Element=globalThis.Element||function(){};"
            "globalThis.DOMTokenList=globalThis.DOMTokenList||function(){};"
            "globalThis.DOMStringMap=globalThis.DOMStringMap||function(){};"
            "globalThis.DOMRect=function(x,y,w,h){this.x=x||0;this.y=y||0;this.width=w||0;this.height=h||0;"
            "this.top=this.y;this.left=this.x;this.right=this.x+this.width;this.bottom=this.y+this.height};"
            "globalThis.DOMRectReadOnly=globalThis.DOMRect;"
            "globalThis.DOMPoint=function(x,y,z,w){this.x=x||0;this.y=y||0;this.z=z||0;this.w=w||1};"
            "globalThis.DOMPointReadOnly=globalThis.DOMPoint;"
            "globalThis.DOMMatrix=function(){this.a=1;this.b=0;this.c=0;this.d=1;this.e=0;this.f=0;this.is2D=true;this.isIdentity=true};"
            "globalThis.DOMMatrixReadOnly=globalThis.DOMMatrix;"
            /* Node constants */
            "Node.ELEMENT_NODE=1;Node.ATTRIBUTE_NODE=2;Node.TEXT_NODE=3;"
            "Node.CDATA_SECTION_NODE=4;Node.COMMENT_NODE=8;Node.DOCUMENT_NODE=9;"
            "Node.DOCUMENT_TYPE_NODE=10;Node.DOCUMENT_FRAGMENT_NODE=11;"
            "Node.DOCUMENT_POSITION_DISCONNECTED=1;Node.DOCUMENT_POSITION_PRECEDING=2;"
            "Node.DOCUMENT_POSITION_FOLLOWING=4;Node.DOCUMENT_POSITION_CONTAINS=8;"
            "Node.DOCUMENT_POSITION_CONTAINED_BY=16;Node.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC=32;"
            "})()";
        JS_Eval(ctx, proto_code, strlen(proto_code), "<protos>", JS_EVAL_TYPE_GLOBAL);
    }

    /* ShadowRoot / attachShadow support */
    {
        const char *shadow_impl =
            "(function(){"
            "var origCreateElement=document.createElement.bind(document);"
            "globalThis.__shadowRoots=new WeakMap();"
            "})()";
        JS_Eval(ctx, shadow_impl, strlen(shadow_impl), "<shadow2>", JS_EVAL_TYPE_GLOBAL);
    }

    /* window.visualViewport */
    {
        JSValue vv = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, vv, "width", JS_NewInt32(ctx, 900));
        JS_SetPropertyStr(ctx, vv, "height", JS_NewInt32(ctx, 548));
        JS_SetPropertyStr(ctx, vv, "offsetLeft", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, vv, "offsetTop", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, vv, "pageLeft", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, vv, "pageTop", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, vv, "scale", JS_NewFloat64(ctx, 1.0));
        JS_SetPropertyStr(ctx, vv, "addEventListener",
            JS_Eval(ctx, "(function(){})", 14, "<vv>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, vv, "removeEventListener",
            JS_Eval(ctx, "(function(){})", 14, "<vv2>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, global, "visualViewport", vv);
    }

    /* window.isSecureContext */
    JS_SetPropertyStr(ctx, global, "isSecureContext", JS_TRUE);

    /* window.origin */
    { struct ts_url parsed;
      if (ts_url_parse(dom->url, &parsed) == 0) {
          char origin[280];
          snprintf(origin, sizeof(origin), "%s://%s", parsed.scheme, parsed.host);
          JS_SetPropertyStr(ctx, global, "origin", JS_NewString(ctx, origin));
      } else {
          JS_SetPropertyStr(ctx, global, "origin", JS_NewString(ctx, "null"));
      }
    }

    /* Misc: requestIdleCallback was already added, ensure we have these too */
    JS_SetPropertyStr(ctx, global, "postMessage",
        JS_Eval(ctx, "(function(){})", 14, "<pm>", JS_EVAL_TYPE_GLOBAL));

    /* createObjectURL / revokeObjectURL */
    {
        const char *url_obj_code =
            "(function(){"
            "var _nextId=0;"
            "URL.createObjectURL=function(blob){return'blob:'+location.origin+'/'+(++_nextId)};"
            "URL.revokeObjectURL=function(){};"
            "})()";
        JS_Eval(ctx, url_obj_code, strlen(url_obj_code), "<ObjectURL>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Audio constructor (used by many sites for sound) */
    {
        const char *audio_code =
            "(function Audio(src){"
            "this.src=src||'';this.currentTime=0;this.duration=0;this.paused=true;"
            "this.volume=1;this.muted=false;this.loop=false;this.playbackRate=1;"
            "this.readyState=0;this.networkState=0;this.ended=false;"
            "this.play=function(){return Promise.resolve()};"
            "this.pause=function(){this.paused=true};"
            "this.load=function(){};"
            "this.canPlayType=function(){return''};"
            "this.addEventListener=function(){};this.removeEventListener=function(){};"
            "})";
        JSValue audio = JS_Eval(ctx, audio_code, strlen(audio_code), "<Audio>",
                                 JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "Audio", audio);
    }

    /* Video element support (constructor) */
    {
        const char *video_code =
            "(function(){"
            "globalThis.HTMLMediaElement=function(){};"
            "HTMLMediaElement.HAVE_NOTHING=0;HTMLMediaElement.HAVE_METADATA=1;"
            "HTMLMediaElement.HAVE_CURRENT_DATA=2;HTMLMediaElement.HAVE_FUTURE_DATA=3;"
            "HTMLMediaElement.HAVE_ENOUGH_DATA=4;"
            "HTMLMediaElement.NETWORK_EMPTY=0;HTMLMediaElement.NETWORK_IDLE=1;"
            "HTMLMediaElement.NETWORK_LOADING=2;HTMLMediaElement.NETWORK_NO_SOURCE=3;"
            "})()";
        JS_Eval(ctx, video_code, strlen(video_code), "<Video>", JS_EVAL_TYPE_GLOBAL);
    }

    /* FontFace constructor */
    {
        const char *ff_code =
            "(function FontFace(family,source,descriptors){"
            "this.family=family;this.status='loaded';this.loaded=Promise.resolve(this);"
            "this.load=function(){return Promise.resolve(this)};"
            "})";
        JSValue ff = JS_Eval(ctx, ff_code, strlen(ff_code), "<FontFace>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "FontFace", ff);
    }

    /* MutationRecord */
    {
        const char *mr_code =
            "(function(){"
            "globalThis.MutationRecord=function(){"
            "this.type='';this.target=null;this.addedNodes=[];this.removedNodes=[];"
            "this.previousSibling=null;this.nextSibling=null;"
            "this.attributeName=null;this.attributeNamespace=null;"
            "this.oldValue=null};"
            "})()";
        JS_Eval(ctx, mr_code, strlen(mr_code), "<MR>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Promise.withResolvers polyfill (recent API) */
    {
        const char *pwr_code =
            "(function(){"
            "if(!Promise.withResolvers)Promise.withResolvers=function(){"
            "var resolve,reject;var promise=new Promise(function(res,rej){resolve=res;reject=rej});"
            "return{promise:promise,resolve:resolve,reject:reject}};"
            "})()";
        JS_Eval(ctx, pwr_code, strlen(pwr_code), "<PWR>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Array.fromAsync polyfill */
    {
        const char *afa_code =
            "(function(){"
            "if(!Array.fromAsync)Array.fromAsync=function(it){return Promise.resolve(Array.from(it))};"
            "})()";
        JS_Eval(ctx, afa_code, strlen(afa_code), "<AFA>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Object.hasOwn polyfill */
    {
        const char *oho_code =
            "(function(){"
            "if(!Object.hasOwn)Object.hasOwn=function(obj,prop){return Object.prototype.hasOwnProperty.call(obj,prop)};"
            "if(!Object.groupBy)Object.groupBy=function(items,fn){var r={};items.forEach(function(item,i){var k=fn(item,i);(r[k]=r[k]||[]).push(item)});return r};"
            "if(!Array.prototype.at)Array.prototype.at=function(n){n=Math.trunc(n)||0;if(n<0)n+=this.length;return this[n]};"
            "if(!String.prototype.at)String.prototype.at=function(n){n=Math.trunc(n)||0;if(n<0)n+=this.length;return this[n]};"
            "if(!String.prototype.replaceAll)String.prototype.replaceAll=function(s,r){return this.split(s).join(r)};"
            "if(!Array.prototype.findLast)Array.prototype.findLast=function(fn){for(var i=this.length-1;i>=0;i--)if(fn(this[i],i,this))return this[i]};"
            "if(!Array.prototype.findLastIndex)Array.prototype.findLastIndex=function(fn){for(var i=this.length-1;i>=0;i--)if(fn(this[i],i,this))return i;return-1};"
            "if(!Array.prototype.toReversed)Array.prototype.toReversed=function(){return this.slice().reverse()};"
            "if(!Array.prototype.toSorted)Array.prototype.toSorted=function(fn){return this.slice().sort(fn)};"
            "if(!Array.prototype.toSpliced)Array.prototype.toSpliced=function(){var a=this.slice();a.splice.apply(a,arguments);return a};"
            "if(!Array.prototype.with)Array.prototype.with=function(i,v){var a=this.slice();a[i]=v;return a};"
            "if(!Array.prototype.flat)Array.prototype.flat=function(d){d=d===undefined?1:d;return d>0?this.reduce(function(a,v){return a.concat(Array.isArray(v)?v.flat(d-1):v)},[]):this.slice()};"
            "if(!Array.prototype.flatMap)Array.prototype.flatMap=function(fn){return this.map(fn).flat()};"
            "})()";
        JS_Eval(ctx, oho_code, strlen(oho_code), "<polyfills>", JS_EVAL_TYPE_GLOBAL);
    }

    /* AbortSignal.timeout / AbortSignal.any polyfills */
    {
        const char *as_code =
            "(function(){"
            "if(typeof AbortSignal==='undefined'){"
            "globalThis.AbortSignal=function(){this.aborted=false;this.reason=undefined;this.onabort=null;"
            "this._listeners=[];this.addEventListener=function(t,fn){this._listeners.push(fn)};"
            "this.removeEventListener=function(t,fn){this._listeners=this._listeners.filter(function(f){return f!==fn})};"
            "this.throwIfAborted=function(){if(this.aborted)throw this.reason||new DOMException('Aborted','AbortError')}};"
            "AbortSignal.abort=function(reason){var s=new AbortSignal();s.aborted=true;s.reason=reason||new DOMException('Aborted','AbortError');return s};"
            "AbortSignal.timeout=function(ms){var c=new AbortController();setTimeout(function(){c.abort(new DOMException('Timeout','TimeoutError'))},ms);return c.signal};"
            "AbortSignal.any=function(signals){var c=new AbortController();signals.forEach(function(s){if(s.aborted)c.abort(s.reason);else s.addEventListener('abort',function(){c.abort(s.reason)})});return c.signal};"
            "}})()";
        JS_Eval(ctx, as_code, strlen(as_code), "<AbortSignal>", JS_EVAL_TYPE_GLOBAL);
    }

    /* DOMException */
    {
        const char *de_code =
            "(function(){"
            "globalThis.DOMException=function(message,name){"
            "this.message=message||'';this.name=name||'Error';this.code=0;"
            "this.stack=(new Error()).stack};"
            "DOMException.prototype=Object.create(Error.prototype);"
            "DOMException.prototype.constructor=DOMException;"
            "DOMException.INDEX_SIZE_ERR=1;DOMException.DOMSTRING_SIZE_ERR=2;"
            "DOMException.HIERARCHY_REQUEST_ERR=3;DOMException.WRONG_DOCUMENT_ERR=4;"
            "DOMException.INVALID_CHARACTER_ERR=5;DOMException.NO_MODIFICATION_ALLOWED_ERR=7;"
            "DOMException.NOT_FOUND_ERR=8;DOMException.NOT_SUPPORTED_ERR=9;"
            "DOMException.SYNTAX_ERR=12;DOMException.INVALID_STATE_ERR=11;"
            "DOMException.NAMESPACE_ERR=14;DOMException.INVALID_ACCESS_ERR=15;"
            "DOMException.ABORT_ERR=20;DOMException.QUOTA_EXCEEDED_ERR=22;"
            "DOMException.TIMEOUT_ERR=23;DOMException.DATA_CLONE_ERR=25;"
            "})()";
        JS_Eval(ctx, de_code, strlen(de_code), "<DOMException>", JS_EVAL_TYPE_GLOBAL);
    }

    /* HTMLTemplateElement.content support */
    {
        const char *tmpl_code =
            "(function(){"
            "var origCreate=document.createElement;"
            "})()";
        JS_Eval(ctx, tmpl_code, strlen(tmpl_code), "<tmpl>", JS_EVAL_TYPE_GLOBAL);
    }

    /* ---------------------------------------------------------------- */
    /* Bing.com core helper library stubs                                */
    /* Bing scripts depend on these globals from their shared JS lib.    */
    /* Without them, sj_evt.bind fails and all scripts after script[1]   */
    /* cascade into ReferenceErrors.                                     */
    /* ---------------------------------------------------------------- */
    {
        const char *bing_code =
            "(function(){"
            /* Core aliases */
            "window._w=window;"
            "window._d=document;"
            "window._ge=function(id){return document.getElementById(id)};"
            "window.sj_ce=function(tag,id){var e=document.createElement(tag);if(id)e.id=id;return e};"
            "window.sj_gx=function(){return new XMLHttpRequest()};"
            /* Event bind/unbind */
            "window.sj_be=function(el,ev,fn,cap){el.addEventListener(ev,fn,!!cap)};"
            "window.sj_ue=function(el,ev,fn,cap){el.removeEventListener(ev,fn,!!cap)};"
            /* Timers */
            "window.sb_st=function(fn,ms){return setTimeout(fn,ms)};"
            "window.sb_ct=function(id){clearTimeout(id)};"
            "window.sb_si=function(fn,ms){return setInterval(fn,ms)};"
            "window.sb_ci=function(id){clearInterval(id)};"
            /* sj_evt — Bing's custom event bus */
            "window.sj_evt=(function(){"
            "  var handlers={};"
            "  return {"
            "    bind:function(name,fn,first){"
            "      if(!handlers[name])handlers[name]=[];"
            "      if(first)handlers[name].unshift(fn);"
            "      else handlers[name].push(fn);"
            "    },"
            "    unbind:function(name,fn){"
            "      if(!handlers[name])return;"
            "      handlers[name]=handlers[name].filter(function(f){return f!==fn});"
            "    },"
            "    fire:function(name){"
            "      var args=Array.prototype.slice.call(arguments);"
            "      var h=handlers[name];"
            "      if(h)for(var i=0;i<h.length;i++)try{h[i](args)}catch(e){}"
            "    }"
            "  };"
            "})();"
            /* sj_sp / sj_pd — event helpers (stopPropagation / preventDefault) */
            "window.sj_sp=function(e){e&&e.stopPropagation&&e.stopPropagation()};"
            "window.sj_pd=function(e){e&&e.preventDefault&&e.preventDefault()};"
            /* sj_cook — cookie helper */
            "window.sj_cook={"
            "  get:function(n,k){return null},"
            "  set:function(n,k,v){}"
            "};"
            /* BM — Bing metrics trigger */
            "window.BM={trigger:function(){}};"
            /* Log object */
            "window.Log={"
            "  Log:function(){},"
            "  LogFilterFlare:function(){},"
            "  LogPerf:function(){},"
            "  DirectLog:function(){}"
            "};"
            "})()";
        JS_Eval(ctx, bing_code, strlen(bing_code), "<bing-compat>", JS_EVAL_TYPE_GLOBAL);
    }

    JS_FreeValue(ctx, global);
}

#endif /* TS_DOM_BINDINGS_H */
