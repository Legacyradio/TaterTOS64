// ACPI namespace

#include <stdint.h>
#include "namespace.h"
#include "aml_exec.h"
#include "aml_types.h"
#include "../mm/heap.h"

static struct acpi_node *g_root;

static struct acpi_node *alloc_node(void) {
    struct acpi_node *n = (struct acpi_node *)kmalloc(sizeof(struct acpi_node));
    if (!n) {
        return 0;
    }
    for (uint32_t i = 0; i < sizeof(struct acpi_node); i++) {
        ((uint8_t *)n)[i] = 0;
    }
    return n;
}

static struct acpi_node *node_resolve_alias(struct acpi_node *node) {
    uint32_t depth = 0;
    while (node && node->type == ACPI_NODE_ALIAS && node->u.alias && depth < 16) {
        node = node->u.alias;
        depth++;
    }
    return node;
}

static void set_name(char dst[4], const char src[4]) {
    for (int i = 0; i < 4; i++) {
        dst[i] = src[i];
    }
}

static struct acpi_node *find_child(struct acpi_node *parent, const char name[4]) {
    for (struct acpi_node *c = parent->first_child; c; c = c->next_sibling) {
        if (c->name[0]==name[0] && c->name[1]==name[1] && c->name[2]==name[2] && c->name[3]==name[3]) {
            return c;
        }
    }
    return 0;
}

struct acpi_node *ns_root(void) {
    return g_root;
}

struct acpi_node *ns_find_child(struct acpi_node *parent, const char name[4]) {
    if (!parent) return 0;
    return find_child(parent, name);
}

struct acpi_node *ns_create(struct acpi_node *scope, const char name[4], uint8_t type) {
    if (!scope) {
        scope = g_root;
    }

    struct acpi_node *existing = find_child(scope, name);
    if (existing) {
        /*
         * AML commonly opens paths via Scope() before a later Device()/Method()
         * declaration in another table. Promote placeholder nodes so discovery
         * code sees the effective object type.
         */
        if (existing->type == ACPI_NODE_UNKNOWN ||
            (existing->type == ACPI_NODE_SCOPE &&
             type != ACPI_NODE_SCOPE &&
             type != ACPI_NODE_ROOT)) {
            existing->type = type;
        }
        return existing;
    }

    struct acpi_node *n = alloc_node();
    if (!n) {
        return 0;
    }
    set_name(n->name, name);
    n->type = type;
    n->parent = scope;

    n->next_sibling = scope->first_child;
    scope->first_child = n;
    return n;
}

static const char *parse_segment(const char *p, char name[4]) {
    int i = 0;
    while (*p && *p != '.' && i < 4) {
        name[i++] = *p++;
    }
    while (i < 4) {
        name[i++] = '_';
    }
    if (*p == '.') {
        p++;
    }
    return p;
}

struct acpi_node *ns_lookup(struct acpi_node *scope, const char *path) {
    if (!path || !*path) {
        return 0;
    }

    struct acpi_node *cur = scope ? scope : g_root;
    const char *p = path;
    if (*p == '\\') {
        cur = g_root;
        p++;
    }
    while (*p == '^') {
        if (cur->parent) {
            cur = cur->parent;
        }
        p++;
    }

    cur = node_resolve_alias(cur);
    while (*p) {
        char name[4];
        p = parse_segment(p, name);
        cur = find_child(cur, name);
        if (!cur) {
            return 0;
        }
        cur = node_resolve_alias(cur);
    }

    return node_resolve_alias(cur);
}

static void walk_node(struct acpi_node *n, void (*cb)(struct acpi_node *, void *), void *ctx) {
    if (!n) return;
    cb(n, ctx);
    for (struct acpi_node *c = n->first_child; c; c = c->next_sibling) {
        walk_node(c, cb, ctx);
    }
}

void ns_walk(void (*cb)(struct acpi_node *node, void *ctx), void *ctx) {
    if (!cb) return;
    walk_node(g_root, cb, ctx);
}

void ns_build_path(struct acpi_node *n, char *buf, uint32_t max) {
    struct acpi_node *stack[64];
    uint32_t count = 0;
    while (n && n->parent && count < 64) {
        stack[count++] = n;
        n = n->parent;
    }
    uint32_t pos = 0;
    if (pos < max) buf[pos++] = '\\';
    for (uint32_t i = count; i > 0; i--) {
        struct acpi_node *s = stack[i - 1];
        if (pos + 4 >= max) break;
        buf[pos++] = s->name[0];
        buf[pos++] = s->name[1];
        buf[pos++] = s->name[2];
        buf[pos++] = s->name[3];
        if (i > 1) {
            if (pos < max) buf[pos++] = '.';
        }
    }
    if (pos < max) buf[pos] = 0;
}

static void count_cb(struct acpi_node *n, void *ctx) {
    (void)n;
    (*(uint32_t *)ctx)++;
}

uint32_t ns_node_count(void) {
    uint32_t c = 0;
    ns_walk(count_cb, &c);
    return c;
}

struct ns_find_ctx {
    const char *hid;
    struct acpi_node *found;
};

static char to_upper_ascii(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static int hid_str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (to_upper_ascii(*a) != to_upper_ascii(*b)) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static char hex_upper(uint8_t v) {
    return (v < 10) ? (char)('0' + v) : (char)('A' + (v - 10));
}

static void eisa_id_to_str(uint32_t id, char out[8]) {
    uint8_t c1 = (uint8_t)((id >> 26) & 0x1F);
    uint8_t c2 = (uint8_t)((id >> 21) & 0x1F);
    uint8_t c3 = (uint8_t)((id >> 16) & 0x1F);
    out[0] = c1 ? (char)('A' + c1 - 1) : '?';
    out[1] = c2 ? (char)('A' + c2 - 1) : '?';
    out[2] = c3 ? (char)('A' + c3 - 1) : '?';
    uint16_t prod = (uint16_t)(id & 0xFFFF);
    out[3] = hex_upper((uint8_t)((prod >> 12) & 0xF));
    out[4] = hex_upper((uint8_t)((prod >> 8) & 0xF));
    out[5] = hex_upper((uint8_t)((prod >> 4) & 0xF));
    out[6] = hex_upper((uint8_t)(prod & 0xF));
    out[7] = 0;
}

static uint32_t bswap32(uint32_t v) {
    return (v >> 24) |
           ((v >> 8) & 0x0000FF00u) |
           ((v << 8) & 0x00FF0000u) |
           (v << 24);
}

static int hid_obj_matches(struct acpi_object *o, const char *hid_str) {
    if (!o || !hid_str) return 0;
    if (o->type == AML_OBJ_REFERENCE && o->v.ref) {
        struct acpi_node *n = (struct acpi_node *)o->v.ref;
        if (n->object) {
            return hid_obj_matches((struct acpi_object *)n->object, hid_str);
        }
        char path[256];
        ns_build_path(n, path, sizeof(path));
        return hid_obj_matches(aml_eval(path), hid_str);
    }
    if (o->type == AML_OBJ_STRING && o->v.string) {
        return hid_str_eq(o->v.string, hid_str);
    }
    if (o->type == AML_OBJ_INTEGER) {
        char tmp[8];
        eisa_id_to_str((uint32_t)o->v.integer, tmp);
        if (hid_str_eq(tmp, hid_str)) return 1;
        eisa_id_to_str(bswap32((uint32_t)o->v.integer), tmp);
        if (hid_str_eq(tmp, hid_str)) return 1;
    }
    if (o->type == AML_OBJ_BUFFER && o->v.buffer.data && o->v.buffer.length >= 4) {
        uint32_t v = (uint32_t)o->v.buffer.data[0]
                   | ((uint32_t)o->v.buffer.data[1] << 8)
                   | ((uint32_t)o->v.buffer.data[2] << 16)
                   | ((uint32_t)o->v.buffer.data[3] << 24);
        char tmp[8];
        eisa_id_to_str(v, tmp);
        if (hid_str_eq(tmp, hid_str)) return 1;
        eisa_id_to_str(bswap32(v), tmp);
        if (hid_str_eq(tmp, hid_str)) return 1;
    }
    return 0;
}

static int cid_obj_matches(struct acpi_object *o, const char *hid_str) {
    if (!o || !hid_str) return 0;
    if (hid_obj_matches(o, hid_str)) return 1;
    if (o->type == AML_OBJ_PACKAGE) {
        for (uint32_t i = 0; i < o->v.package.count; i++) {
            if (hid_obj_matches(o->v.package.items[i], hid_str)) return 1;
        }
    }
    return 0;
}

int ns_hid_match(struct acpi_node *node, const char *hid_str) {
    if (!node || !hid_str || !*hid_str) return 0;
    node = node_resolve_alias(node);
    if (!node) return 0;
    static const char hid_name[4] = {'_','H','I','D'};
    struct acpi_node *hid = ns_find_child(node, hid_name);
    if (hid) {
        if (hid->object) {
            if (hid_obj_matches((struct acpi_object *)hid->object, hid_str)) return 1;
        } else {
            char path[256];
            ns_build_path(hid, path, sizeof(path));
            struct acpi_object *o = aml_eval(path);
            if (hid_obj_matches(o, hid_str)) return 1;
        }
    }

    static const char cid_name[4] = {'_','C','I','D'};
    struct acpi_node *cid = ns_find_child(node, cid_name);
    if (!cid) return 0;
    if (cid->object) {
        return cid_obj_matches((struct acpi_object *)cid->object, hid_str);
    }
    char path[256];
    ns_build_path(cid, path, sizeof(path));
    struct acpi_object *o = aml_eval(path);
    return cid_obj_matches(o, hid_str);
}

static void ns_find_device_cb(struct acpi_node *node, void *ctx) {
    struct ns_find_ctx *c = (struct ns_find_ctx *)ctx;
    if (!c || c->found) return;
    if (ns_hid_match(node, c->hid)) {
        c->found = node;
    }
}

struct acpi_node *ns_find_device(const char *hid_str) {
    if (!hid_str || !*hid_str) return 0;
    struct ns_find_ctx ctx = {hid_str, 0};
    ns_walk(ns_find_device_cb, &ctx);
    return ctx.found;
}

struct adr_find_ctx {
    uint32_t adr;
    struct acpi_node *found;
};

static void ns_find_device_adr_cb(struct acpi_node *node, void *ctx) {
    struct adr_find_ctx *c = (struct adr_find_ctx *)ctx;
    if (!c || c->found) return;
    node = node_resolve_alias(node);
    if (!node) return;
    static const char adr_name[4] = {'_','A','D','R'};
    struct acpi_node *adr = ns_find_child(node, adr_name);
    if (!adr) return;
    uint64_t val = 0;
    if (adr->object) {
        struct acpi_object *o = (struct acpi_object *)adr->object;
        if (o->type == AML_OBJ_INTEGER) val = o->v.integer;
    } else {
        char path[256];
        ns_build_path(adr, path, sizeof(path));
        struct acpi_object *o = aml_eval(path);
        if (o && o->type == AML_OBJ_INTEGER) val = o->v.integer;
    }
    if ((uint32_t)val == c->adr) {
        c->found = node;
    }
}

struct acpi_node *ns_find_device_by_adr(uint32_t adr) {
    struct adr_find_ctx ctx = {adr, 0};
    ns_walk(ns_find_device_adr_cb, &ctx);
    return ctx.found;
}

void acpi_ns_init(void) {
    g_root = alloc_node();
    if (!g_root) {
        return;
    }
    g_root->type = ACPI_NODE_ROOT;

    static const char *roots[] = {"_SB_", "_PR_", "_SI_", "_TZ_", "_GPE"};
    for (uint32_t i = 0; i < sizeof(roots)/sizeof(roots[0]); i++) {
        char name[4] = {0,0,0,0};
        for (int j = 0; j < 4; j++) {
            name[j] = roots[i][j];
        }
        ns_create(g_root, name, ACPI_NODE_SCOPE);
    }
}
