/*
 * ts_webcomp.h — TaterSurf Web Components implementation
 *
 * Header-only. Provides Custom Elements (customElements.define),
 * Shadow DOM (attachShadow), <template> content, and <slot> assignment.
 *
 * Required by YouTube (Polymer/Lit), which builds its entire UI
 * using custom elements with shadow roots.
 *
 * Depends on: ts_dom.h, ts_dom_bindings.h, quickjs.h
 */

#ifndef TS_WEBCOMP_H
#define TS_WEBCOMP_H

#include "ts_dom.h"
#include <stdint.h>

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define TS_WEBCOMP_MAX_DEFS      128   /* max custom element definitions */
#define TS_WEBCOMP_TAG_MAX        64   /* max custom element tag length */

/* ================================================================== */
/* Custom Element Registry                                             */
/* ================================================================== */

struct ts_custom_element_def {
    char tag[TS_WEBCOMP_TAG_MAX];      /* tag name (must contain hyphen) */
    JSValue constructor;                /* JS constructor function */
    JSValue connected_cb;               /* connectedCallback */
    JSValue disconnected_cb;            /* disconnectedCallback */
    JSValue attribute_changed_cb;       /* attributeChangedCallback */
    JSValue adopted_cb;                 /* adoptedCallback */
    int used;
};

struct ts_webcomp_registry {
    struct ts_custom_element_def defs[TS_WEBCOMP_MAX_DEFS];
    int count;
};

static struct ts_webcomp_registry g_ce_registry;

/* Forward declarations */
static void ts_webcomp_enhance_element(JSContext *ctx, JSValue elem_obj,
                                        const char *tag);

/* ================================================================== */
/* Shadow DOM support in DOM nodes                                     */
/* ================================================================== */

/*
 * Shadow root is represented as a regular DOM node (FRAGMENT_NODE)
 * with a special flag. The shadow root's children are the shadow tree.
 * The host element's regular children are "light DOM" / slot content.
 *
 * We store the shadow root index in a dedicated field on the DOM node.
 * Since ts_dom_node is defined in ts_dom.h and we can't modify it here,
 * we use a side table to track shadow root associations.
 */

#define TS_SHADOW_MAX 256

struct ts_shadow_entry {
    int host_id;       /* element that owns the shadow root */
    int shadow_id;     /* fragment node acting as shadow root */
    int mode;          /* 0 = closed, 1 = open */
    int used;
};

static struct ts_shadow_entry g_shadows[TS_SHADOW_MAX];
static int g_shadow_count = 0;

static int ts_shadow_find(int host_id) {
    int i;
    for (i = 0; i < g_shadow_count; i++) {
        if (g_shadows[i].used && g_shadows[i].host_id == host_id)
            return i;
    }
    return -1;
}

static int ts_shadow_get_root(struct ts_dom_ctx *dom, int host_id) {
    int idx = ts_shadow_find(host_id);
    if (idx < 0) return -1;
    return g_shadows[idx].shadow_id;
}

/* ================================================================== */
/* Custom Element lifecycle                                            */
/* ================================================================== */

static struct ts_custom_element_def *ts_ce_find(const char *tag) {
    int i;
    for (i = 0; i < g_ce_registry.count; i++) {
        if (g_ce_registry.defs[i].used &&
            strcmp(g_ce_registry.defs[i].tag, tag) == 0)
            return &g_ce_registry.defs[i];
    }
    return NULL;
}

/* Called when a custom element is connected to the DOM */
static void ts_ce_connected(struct ts_dom_ctx *dom, int node_id) {
    struct ts_dom_node *n = ts_dom_get_node(dom, node_id);
    if (!n || n->type != TS_DOM_ELEMENT_NODE) return;

    struct ts_custom_element_def *def = ts_ce_find(n->tag);
    if (!def) return;

    if (!JS_IsUndefined(def->connected_cb) && !JS_IsNull(def->connected_cb)) {
        JSValue this_obj = ts_js_wrap_node(dom->ctx, dom, node_id);
        JSValue ret = JS_Call(dom->ctx, def->connected_cb, this_obj, 0, NULL);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(dom->ctx);
            const char *str = JS_ToCString(dom->ctx, exc);
            if (str) { ts_dom_console_append(dom, "[CE] ", str); JS_FreeCString(dom->ctx, str); }
            JS_FreeValue(dom->ctx, exc);
        }
        JS_FreeValue(dom->ctx, ret);
        JS_FreeValue(dom->ctx, this_obj);
    }
}

/* Called when a custom element is disconnected from the DOM */
static void ts_ce_disconnected(struct ts_dom_ctx *dom, int node_id) {
    struct ts_dom_node *n = ts_dom_get_node(dom, node_id);
    if (!n || n->type != TS_DOM_ELEMENT_NODE) return;

    struct ts_custom_element_def *def = ts_ce_find(n->tag);
    if (!def) return;

    if (!JS_IsUndefined(def->disconnected_cb) && !JS_IsNull(def->disconnected_cb)) {
        JSValue this_obj = ts_js_wrap_node(dom->ctx, dom, node_id);
        JSValue ret = JS_Call(dom->ctx, def->disconnected_cb, this_obj, 0, NULL);
        JS_FreeValue(dom->ctx, ret);
        JS_FreeValue(dom->ctx, this_obj);
    }
}

/* ================================================================== */
/* QuickJS bindings: customElements                                    */
/* ================================================================== */

/* customElements.define(tagName, constructor) */
static JSValue ts_js_ce_define(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "constructor must be a function");

    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_UNDEFINED;

    /* Validate tag name: must contain a hyphen (spec requirement) */
    {
        int has_hyphen = 0;
        const char *p = tag;
        while (*p) { if (*p == '-') { has_hyphen = 1; break; } p++; }
        if (!has_hyphen) {
            JS_FreeCString(ctx, tag);
            return JS_ThrowTypeError(ctx, "custom element name must contain a hyphen");
        }
    }

    /* Check for duplicate */
    if (ts_ce_find(tag)) {
        JS_FreeCString(ctx, tag);
        return JS_ThrowTypeError(ctx, "custom element already defined");
    }

    /* Register */
    if (g_ce_registry.count >= TS_WEBCOMP_MAX_DEFS) {
        JS_FreeCString(ctx, tag);
        return JS_ThrowRangeError(ctx, "custom element registry full");
    }

    {
        struct ts_custom_element_def *def = &g_ce_registry.defs[g_ce_registry.count];
        memset(def, 0, sizeof(*def));
        strncpy(def->tag, tag, TS_WEBCOMP_TAG_MAX - 1);
        def->tag[TS_WEBCOMP_TAG_MAX - 1] = '\0';
        /* Lowercase the tag */
        { size_t i; for (i = 0; def->tag[i]; i++) if (def->tag[i] >= 'A' && def->tag[i] <= 'Z') def->tag[i] += 32; }
        def->constructor = JS_DupValue(ctx, argv[1]);

        /* Extract lifecycle callbacks from prototype */
        JSValue proto = JS_GetPropertyStr(ctx, argv[1], "prototype");
        if (!JS_IsUndefined(proto)) {
            def->connected_cb = JS_GetPropertyStr(ctx, proto, "connectedCallback");
            def->disconnected_cb = JS_GetPropertyStr(ctx, proto, "disconnectedCallback");
            def->attribute_changed_cb = JS_GetPropertyStr(ctx, proto, "attributeChangedCallback");
            def->adopted_cb = JS_GetPropertyStr(ctx, proto, "adoptedCallback");
        } else {
            def->connected_cb = JS_UNDEFINED;
            def->disconnected_cb = JS_UNDEFINED;
            def->attribute_changed_cb = JS_UNDEFINED;
            def->adopted_cb = JS_UNDEFINED;
        }
        JS_FreeValue(ctx, proto);

        def->used = 1;
        g_ce_registry.count++;

        /* Upgrade existing DOM elements with this tag name */
        { int i;
          char lower_tag[TS_WEBCOMP_TAG_MAX];
          strncpy(lower_tag, def->tag, sizeof(lower_tag) - 1);
          lower_tag[sizeof(lower_tag) - 1] = '\0';
          for (i = 0; i < TS_DOM_MAX_NODES; i++) {
              struct ts_dom_node *el = &g_dom->nodes[i];
              if (!el->used || el->type != TS_DOM_ELEMENT_NODE) continue;
              if (strcmp(el->tag, lower_tag) != 0) continue;
              /* Call constructor on this element */
              { JSValue el_obj = ts_js_wrap_node(ctx, g_dom, i);
                ts_dom_bind_node_methods(ctx, el_obj);
                /* Run constructor: new Constructor() but with existing element as this */
                { JSValue ctor_ret = JS_Call(ctx, def->constructor, el_obj, 0, NULL);
                  if (JS_IsException(ctor_ret)) {
                      JSValue exc = JS_GetException(ctx);
                      const char *str = JS_ToCString(ctx, exc);
                      if (str) { fprintf(stderr, "CE_UPGRADE: %s err=%s\n", lower_tag, str);
                                 JS_FreeCString(ctx, str); }
                      JS_FreeValue(ctx, exc);
                  }
                  JS_FreeValue(ctx, ctor_ret);
                }
                /* Fire connectedCallback if element is in the document tree */
                if (el->parent >= 0)
                    ts_ce_connected(g_dom, i);
                JS_FreeValue(ctx, el_obj);
              }
          }
          g_dom->dirty = 1;
        }
    }

    JS_FreeCString(ctx, tag);
    return JS_UNDEFINED;
}

/* customElements.get(tagName) */
static JSValue ts_js_ce_get(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_UNDEFINED;

    /* Lowercase */
    char lower[TS_WEBCOMP_TAG_MAX];
    { size_t i; for (i = 0; tag[i] && i < TS_WEBCOMP_TAG_MAX - 1; i++) lower[i] = (tag[i] >= 'A' && tag[i] <= 'Z') ? tag[i]+32 : tag[i]; lower[i] = 0; }
    JS_FreeCString(ctx, tag);

    struct ts_custom_element_def *def = ts_ce_find(lower);
    if (!def) return JS_UNDEFINED;
    return JS_DupValue(ctx, def->constructor);
}

/* customElements.whenDefined(tagName) — returns a resolved Promise (simplified) */
static JSValue ts_js_ce_when_defined(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    /* Return an immediately resolved Promise */
    JSValue resolve_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);
    JS_Call(ctx, resolve_funcs[0], JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, resolve_funcs[0]);
    JS_FreeValue(ctx, resolve_funcs[1]);
    return promise;
}

/* ================================================================== */
/* QuickJS bindings: Element.attachShadow                              */
/* ================================================================== */

static JSValue ts_js_elem_attach_shadow(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    int host_id = ts_js_get_node_id(ctx, this_val);
    if (host_id < 0) return JS_ThrowTypeError(ctx, "invalid element");

    /* Check if already has shadow root */
    if (ts_shadow_find(host_id) >= 0)
        return JS_ThrowTypeError(ctx, "element already has shadow root");

    /* Parse mode from options */
    int mode = 1; /* default: open */
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue mode_val = JS_GetPropertyStr(ctx, argv[0], "mode");
        if (!JS_IsUndefined(mode_val)) {
            const char *mode_str = JS_ToCString(ctx, mode_val);
            if (mode_str) {
                if (strcmp(mode_str, "closed") == 0) mode = 0;
                JS_FreeCString(ctx, mode_str);
            }
        }
        JS_FreeValue(ctx, mode_val);
    }

    /* Create shadow root (a fragment node) */
    struct ts_dom_node *shadow = ts_dom_alloc_node(g_dom, TS_DOM_FRAGMENT_NODE);
    if (!shadow) return JS_ThrowRangeError(ctx, "cannot create shadow root");
    strcpy(shadow->tag, "#shadow-root");

    /* Register in shadow table */
    if (g_shadow_count < TS_SHADOW_MAX) {
        g_shadows[g_shadow_count].host_id = host_id;
        g_shadows[g_shadow_count].shadow_id = shadow->id;
        g_shadows[g_shadow_count].mode = mode;
        g_shadows[g_shadow_count].used = 1;
        g_shadow_count++;
    }

    /* Create JS wrapper for shadow root */
    JSValue shadow_obj = ts_js_wrap_node(ctx, g_dom, shadow->id);
    ts_dom_bind_node_methods(ctx, shadow_obj);

    /* Add innerHTML setter for shadow root (Lit uses this heavily) */
    JS_SetPropertyStr(ctx, shadow_obj, "innerHTML",
        JS_NewCFunction(ctx, ts_js_elem_get_inner_html, "innerHTML", 0));

    /* Store on host element */
    if (mode == 1) { /* open mode — accessible via .shadowRoot */
        JS_SetPropertyStr(ctx, this_val, "shadowRoot", JS_DupValue(ctx, shadow_obj));
    }

    /* Also set host property on shadow root */
    JS_SetPropertyStr(ctx, shadow_obj, "host", JS_DupValue(ctx, this_val));

    g_dom->dirty = 1;
    return shadow_obj;
}

/* ================================================================== */
/* QuickJS bindings: HTMLTemplateElement.content                        */
/* ================================================================== */

static JSValue ts_js_template_get_content(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    int id = ts_js_get_node_id(ctx, this_val);
    struct ts_dom_node *n = ts_dom_get_node(g_dom, id);
    if (!n || strcmp(n->tag, "template") != 0) return JS_NULL;

    /* Create a DocumentFragment containing the template's children */
    struct ts_dom_node *frag = ts_dom_alloc_node(g_dom, TS_DOM_FRAGMENT_NODE);
    if (!frag) return JS_NULL;

    /* Move children to fragment */
    int child = n->first_child;
    while (child >= 0) {
        int next = g_dom->nodes[child].next_sibling;
        /* Don't actually move — clone concept: fragment references same children */
        ts_dom_append_child(g_dom, frag->id, child);
        child = next;
    }

    JSValue frag_obj = ts_js_wrap_node(ctx, g_dom, frag->id);
    ts_dom_bind_node_methods(ctx, frag_obj);
    return frag_obj;
}

/* ================================================================== */
/* Registration: add Web Components APIs to global scope               */
/* ================================================================== */

static void ts_webcomp_register(struct ts_dom_ctx *dom) {
    JSContext *ctx = dom->ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Initialize registry */
    memset(&g_ce_registry, 0, sizeof(g_ce_registry));
    memset(g_shadows, 0, sizeof(g_shadows));
    g_shadow_count = 0;

    /* Wire lifecycle hooks into DOM tree operations */
    dom->on_connected = ts_ce_connected;
    dom->on_disconnected = ts_ce_disconnected;
    dom->on_element_created = ts_webcomp_enhance_element;

    /* customElements object */
    {
        JSValue ce = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ce, "define",
            JS_NewCFunction(ctx, ts_js_ce_define, "define", 2));
        JS_SetPropertyStr(ctx, ce, "get",
            JS_NewCFunction(ctx, ts_js_ce_get, "get", 1));
        JS_SetPropertyStr(ctx, ce, "whenDefined",
            JS_NewCFunction(ctx, ts_js_ce_when_defined, "whenDefined", 1));
        JS_SetPropertyStr(ctx, global, "customElements", ce);
    }

    /* HTMLElement base class (for class MyElement extends HTMLElement) */
    {
        const char *he_code =
            "class HTMLElement {"
            "  constructor() { this.__node_id = -1; }"
            "  connectedCallback() {}"
            "  disconnectedCallback() {}"
            "  attributeChangedCallback(name, oldVal, newVal) {}"
            "  adoptedCallback() {}"
            "  attachShadow(opts) { return null; }" /* overridden by C binding */
            "  get shadowRoot() { return null; }"
            "  static get observedAttributes() { return []; }"
            "}";
        JSValue he = JS_Eval(ctx, he_code, strlen(he_code), "<HTMLElement>",
                              JS_EVAL_TYPE_GLOBAL);
        JS_SetPropertyStr(ctx, global, "HTMLElement", he);
    }

    /* ShadowRoot, DocumentFragment available via DOM */

    /* Patch attachShadow onto all Element wrappers going forward.
       We do this by adding it to the document.createElement output
       via the bind_node_methods enhancement. For existing nodes,
       we patch the body and documentElement. */
    {
        /* Patch existing body node */
        if (!JS_IsUndefined(dom->js_document)) {
            JSValue body = JS_GetPropertyStr(ctx, dom->js_document, "body");
            if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
                JS_SetPropertyStr(ctx, body, "attachShadow",
                    JS_NewCFunction(ctx, ts_js_elem_attach_shadow, "attachShadow", 1));
            }
            JS_FreeValue(ctx, body);
        }
    }

    JS_FreeValue(ctx, global);
}

/*
 * ts_webcomp_enhance_element — call after creating any element wrapper
 * to add Web Component capabilities (attachShadow, template.content).
 */
static void ts_webcomp_enhance_element(JSContext *ctx, JSValue elem_obj,
                                        const char *tag) {
    /* All elements get attachShadow */
    JS_SetPropertyStr(ctx, elem_obj, "attachShadow",
        JS_NewCFunction(ctx, ts_js_elem_attach_shadow, "attachShadow", 1));

    /* <template> elements get .content */
    if (strcmp(tag, "template") == 0) {
        JS_SetPropertyStr(ctx, elem_obj, "content",
            JS_NewCFunction(ctx, ts_js_template_get_content, "content", 0));
    }
}

#endif /* TS_WEBCOMP_H */
