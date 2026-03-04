// AML executor (core subset)

#include <stdint.h>
#include "aml_exec.h"
#include "aml_types.h"
#include "namespace.h"
#include "aml_ops.h"
#include "notify.h"

void *kmalloc(uint64_t size);

static uint64_t parse_int_from_string(const char *s);
static struct acpi_object *aml_exec_method(struct acpi_node *m, struct acpi_object **args, uint32_t arg_count);

static int name_eq(const char a[4], const char b[4]) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

static const uint8_t *exec_parse_name_string(const uint8_t *p, const uint8_t *end,
                                             struct acpi_node *scope, struct acpi_node **out_node) {
    struct acpi_node *cur = scope ? scope : ns_root();
    uint32_t count = 0;
    char names[16][4];

    while (p < end && *p == 0x5E) {
        if (cur->parent) cur = cur->parent;
        p++;
    }
    if (p < end && *p == 0x5C) {
        cur = ns_root();
        p++;
    }
    if (p >= end) {
        *out_node = cur;
        return p;
    }
    if (*p == 0x00) {
        p++;
        *out_node = cur;
        return p;
    }

    if (*p == 0x2E) {
        p++;
        count = 2;
    } else if (*p == 0x2F) {
        p++;
        if (p < end) count = *p++;
    } else {
        count = 1;
    }
    if (count > 16) count = 16;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 4 > end) {
            count = i;
            break;
        }
        for (int j = 0; j < 4; j++) names[i][j] = (char)p[j];
        p += 4;
    }

    for (uint32_t i = 0; i < count && cur; i++) {
        cur = ns_find_child(cur, names[i]);
    }
    *out_node = cur;
    return p;
}

struct aml_exec_ctx {
    struct acpi_node *scope;
    struct acpi_object *args[7];
    uint32_t arg_count;
    struct acpi_object *locals[8];
    struct acpi_object *ret;
    int have_ret;
    int break_flag;
    int continue_flag;
};

static uint32_t parse_pkg_length_exec(const uint8_t *p, uint32_t *consumed) {
    uint8_t lead = p[0];
    uint8_t byte_count = (lead >> 6) & 0x3;
    uint32_t len = lead & 0x3F;
    *consumed = 1;
    if (byte_count == 0) {
        return len;
    }
    len = lead & 0x0F;
    for (uint8_t i = 0; i < byte_count; i++) {
        len |= ((uint32_t)p[1 + i]) << (4 + 8 * i);
    }
    *consumed = 1 + byte_count;
    return len;
}

static struct acpi_object *eval_builtin(const char name[4]) {
    if (name_eq(name, "_STA")) {
        return aml_obj_make_int(0x0F);
    }
    if (name_eq(name, "_HID")) {
        return aml_obj_make_str("PNP0000");
    }
    return 0;
}

static struct acpi_object *eval_object(struct aml_exec_ctx *ctx, struct acpi_object *obj);

static const uint8_t *exec_parse_name_ref(const uint8_t *p, const uint8_t *end,
                                          struct aml_exec_ctx *ctx, struct acpi_node **out) {
    struct acpi_node *n = 0;
    const uint8_t *q = exec_parse_name_string(p, end, ctx ? ctx->scope : ns_root(), &n);
    *out = n;
    return q;
}

static const uint8_t *exec_parse_data_object(const uint8_t *p, const uint8_t *end,
                                             struct aml_exec_ctx *ctx, struct acpi_object **out);

static const uint8_t *exec_parse_term(const uint8_t *p, const uint8_t *end,
                                      struct aml_exec_ctx *ctx, struct acpi_object **out) {
    if (p >= end) return p;
    uint8_t op = *p++;
    if (op == 0x70) { // Store
        struct acpi_object *src = 0;
        p = exec_parse_term(p, end, ctx, &src);
        struct acpi_object *val = eval_object(ctx, src);
        if (p < end && *p >= 0x60 && *p <= 0x67) {
            uint32_t idx = (uint32_t)(*p - 0x60);
            p++;
            if (ctx && idx < 8) ctx->locals[idx] = val;
        } else if (p < end && *p >= 0x68 && *p <= 0x6E) {
            uint32_t idx = (uint32_t)(*p - 0x68);
            p++;
            if (ctx && idx < ctx->arg_count) ctx->args[idx] = val;
        } else {
            struct acpi_node *target = 0;
            struct acpi_object *tobj = 0;
            const uint8_t *p_before = p;
            p = exec_parse_term(p, end, ctx, &tobj);
            if (tobj && tobj->type == AML_OBJ_REFERENCE) {
                void *ref = tobj->v.ref;
                if (ref) {
                    // If this looks like a node pointer, handle as node/field.
                    struct acpi_node *n = (struct acpi_node *)ref;
                    if (n->type == ACPI_NODE_FIELD) {
                        aml_write_field(n, aml_obj_to_int(val));
                    } else if (n->type != ACPI_NODE_ROOT && n->type != ACPI_NODE_UNKNOWN) {
                        n->object = val;
                    } else {
                        // Treat as raw pointer storage (buffer/package element)
                        uint8_t *b = (uint8_t *)ref;
                        *b = (uint8_t)(aml_obj_to_int(val) & 0xFF);
                    }
                }
            } else {
                p = p_before;
                p = exec_parse_name_ref(p, end, ctx, &target);
                if (target) {
                    if (target->type == ACPI_NODE_FIELD) {
                        aml_write_field(target, aml_obj_to_int(val));
                    } else {
                        target->object = val;
                    }
                }
            }
        }
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0x86) { // Notify
        struct acpi_object *t = 0;
        struct acpi_object *v = 0;
        p = exec_parse_term(p, end, ctx, &t);
        p = exec_parse_term(p, end, ctx, &v);
        struct acpi_node *target = 0;
        struct acpi_object *tv = eval_object(ctx, t);
        if (tv && tv->type == AML_OBJ_REFERENCE) {
            target = (struct acpi_node *)tv->v.ref;
        } else if (tv && tv->type == AML_OBJ_STRING && tv->v.string) {
            target = ns_lookup(ns_root(), tv->v.string);
        }
        uint32_t val = (uint32_t)aml_obj_to_int(eval_object(ctx, v));
        if (target) {
            acpi_notify_dispatch(target, val);
        }
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0xA0) { // If
        uint32_t consumed = 0;
        uint32_t pkg_len = parse_pkg_length_exec(p, &consumed);
        const uint8_t *body = p + consumed;
        const uint8_t *limit = body + pkg_len - consumed;
        if (limit > end) limit = end;
        struct acpi_object *pred = 0;
        const uint8_t *q = exec_parse_term(body, limit, ctx, &pred);
        uint64_t cond = aml_obj_to_int(eval_object(ctx, pred));
        if (cond) {
            while (q < limit) {
                struct acpi_object *tmp = 0;
                q = exec_parse_term(q, limit, ctx, &tmp);
                if (ctx && ctx->have_ret) break;
            }
        }
        p = limit;
        if (p < end && *p == 0xA1) { // Else
            p++;
            uint32_t c2 = 0;
            uint32_t l2 = parse_pkg_length_exec(p, &c2);
            const uint8_t *b2 = p + c2;
            const uint8_t *lim2 = b2 + l2 - c2;
            if (lim2 > end) lim2 = end;
            if (!cond) {
                const uint8_t *qq = b2;
                while (qq < lim2) {
                    struct acpi_object *tmp = 0;
                    qq = exec_parse_term(qq, lim2, ctx, &tmp);
                    if (ctx && ctx->have_ret) break;
                }
            }
            p = lim2;
        }
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0xA4) { // Return
        struct acpi_object *ret = 0;
        p = exec_parse_term(p, end, ctx, &ret);
        if (ctx) {
            ctx->ret = eval_object(ctx, ret);
            ctx->have_ret = 1;
        }
        *out = ret;
        return p;
    }
    if (op == 0xA2) { // While
        uint32_t consumed = 0;
        uint32_t pkg_len = parse_pkg_length_exec(p, &consumed);
        const uint8_t *body = p + consumed;
        const uint8_t *limit = body + pkg_len - consumed;
        if (limit > end) limit = end;

        const uint8_t *pred_start = body;
        struct acpi_object *pred_obj = 0;
        const uint8_t *pred_end = exec_parse_term(pred_start, limit, ctx, &pred_obj);
        uint64_t cond = aml_obj_to_int(eval_object(ctx, pred_obj));

        while (cond) {
            const uint8_t *q = pred_end;
            while (q < limit) {
                struct acpi_object *tmp = 0;
                q = exec_parse_term(q, limit, ctx, &tmp);
                if (ctx && ctx->have_ret) break;
                if (ctx && ctx->break_flag) break;
                if (ctx && ctx->continue_flag) {
                    ctx->continue_flag = 0;
                    break;
                }
            }
            if (ctx && ctx->break_flag) {
                ctx->break_flag = 0;
                break;
            }
            if (ctx && ctx->have_ret) break;
            pred_obj = 0;
            exec_parse_term(pred_start, limit, ctx, &pred_obj);
            cond = aml_obj_to_int(eval_object(ctx, pred_obj));
        }
        p = limit;
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0xA5) { // Break
        if (ctx) ctx->break_flag = 1;
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0x9F) { // Continue
        if (ctx) ctx->continue_flag = 1;
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0x71) { // RefOf
        struct acpi_object *t = 0;
        p = exec_parse_term(p, end, ctx, &t);
        if (t && t->type == AML_OBJ_REFERENCE) {
            *out = t;
            return p;
        }
        if (t && t->type == AML_OBJ_STRING && t->v.string) {
            struct acpi_node *n = ns_lookup(ns_root(), t->v.string);
            if (n) {
                *out = aml_obj_make_ref(n);
                return p;
            }
        }
        *out = aml_obj_make_ref(0);
        return p;
    }
    if (op == 0x83) { // DerefOf
        struct acpi_object *t = 0;
        p = exec_parse_term(p, end, ctx, &t);
        *out = eval_object(ctx, t);
        return p;
    }
    if (op == 0x9E) { // Mid
        struct acpi_object *src = 0;
        struct acpi_object *idx = 0;
        struct acpi_object *len = 0;
        p = exec_parse_term(p, end, ctx, &src);
        p = exec_parse_term(p, end, ctx, &idx);
        p = exec_parse_term(p, end, ctx, &len);
        struct acpi_object *v = eval_object(ctx, src);
        uint32_t i = (uint32_t)aml_obj_to_int(eval_object(ctx, idx));
        uint32_t l = (uint32_t)aml_obj_to_int(eval_object(ctx, len));
        if (v && v->type == AML_OBJ_STRING && v->v.string) {
            const char *s = v->v.string;
            uint32_t sl = 0;
            while (s[sl]) sl++;
            if (i > sl) i = sl;
            if (i + l > sl) l = sl - i;
            char *buf = (char *)kmalloc(l + 1);
            if (!buf) return p;
            for (uint32_t k = 0; k < l; k++) buf[k] = s[i + k];
            buf[l] = 0;
            *out = aml_obj_make_str(buf);
            return p;
        }
        if (v && v->type == AML_OBJ_BUFFER && v->v.buffer.data) {
            uint32_t bl = v->v.buffer.length;
            if (i > bl) i = bl;
            if (i + l > bl) l = bl - i;
            uint8_t *buf = (uint8_t *)kmalloc(l);
            if (!buf) return p;
            for (uint32_t k = 0; k < l; k++) buf[k] = v->v.buffer.data[i + k];
            *out = aml_obj_make_buf(buf, l);
            return p;
        }
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0x75 || op == 0x76) { // Increment / Decrement
        struct acpi_object *t = 0;
        p = exec_parse_term(p, end, ctx, &t);
        struct acpi_object *v = eval_object(ctx, t);
        uint64_t val = aml_obj_to_int(v);
        if (op == 0x75) val++;
        else val--;
        struct acpi_object *nv = aml_obj_make_int(val);
        // If target is reference, store back
        if (t && t->type == AML_OBJ_REFERENCE && t->v.ref) {
            struct acpi_node *n = (struct acpi_node *)t->v.ref;
            if (n->type == ACPI_NODE_FIELD) {
                aml_write_field(n, val);
            } else {
                n->object = nv;
            }
        }
        *out = nv;
        return p;
    }
    if (op == 0x81 || op == 0x82) { // FindSetRightBit / FindSetLeftBit
        struct acpi_object *t = 0;
        p = exec_parse_term(p, end, ctx, &t);
        uint64_t v = aml_obj_to_int(eval_object(ctx, t));
        uint64_t res = 0;
        if (v == 0) {
            res = 0;
        } else if (op == 0x81) {
            uint64_t idx = 1;
            while ((v & 1ULL) == 0) {
                v >>= 1;
                idx++;
            }
            res = idx;
        } else {
            uint64_t idx = 64;
            while ((v & (1ULL << 63)) == 0) {
                v <<= 1;
                idx--;
            }
            res = idx;
        }
        *out = aml_obj_make_int(res);
        return p;
    }
    if (op == 0x87) { // SizeOf
        struct acpi_object *t = 0;
        p = exec_parse_term(p, end, ctx, &t);
        struct acpi_object *v = eval_object(ctx, t);
        if (v && v->type == AML_OBJ_BUFFER) {
            *out = aml_obj_make_int(v->v.buffer.length);
            return p;
        }
        if (v && v->type == AML_OBJ_STRING && v->v.string) {
            uint32_t len = 0;
            while (v->v.string[len]) len++;
            *out = aml_obj_make_int(len);
            return p;
        }
        if (v && v->type == AML_OBJ_PACKAGE) {
            *out = aml_obj_make_int(v->v.package.count);
            return p;
        }
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0x8E) { // ObjectType
        struct acpi_object *t = 0;
        p = exec_parse_term(p, end, ctx, &t);
        struct acpi_object *v = eval_object(ctx, t);
        uint64_t type = 0;
        if (v) {
            if (v->type == AML_OBJ_INTEGER) type = 1;
            else if (v->type == AML_OBJ_STRING) type = 2;
            else if (v->type == AML_OBJ_BUFFER) type = 3;
            else if (v->type == AML_OBJ_PACKAGE) type = 4;
            else if (v->type == AML_OBJ_REFERENCE) type = 6;
        }
        *out = aml_obj_make_int(type);
        return p;
    }
    if (op == 0x9D) { // CopyObject
        struct acpi_object *src = 0;
        p = exec_parse_term(p, end, ctx, &src);
        struct acpi_object *v = eval_object(ctx, src);
        if (!v) {
            *out = aml_obj_make_int(0);
            return p;
        }
        if (v->type == AML_OBJ_INTEGER) {
            *out = aml_obj_make_int(v->v.integer);
            return p;
        }
        if (v->type == AML_OBJ_STRING && v->v.string) {
            const char *s = v->v.string;
            uint32_t len = 0;
            while (s[len]) len++;
            char *buf = (char *)kmalloc(len + 1);
            if (!buf) return p;
            for (uint32_t i = 0; i < len; i++) buf[i] = s[i];
            buf[len] = 0;
            *out = aml_obj_make_str(buf);
            return p;
        }
        if (v->type == AML_OBJ_BUFFER) {
            uint32_t len = v->v.buffer.length;
            uint8_t *buf = (uint8_t *)kmalloc(len);
            if (!buf) return p;
            for (uint32_t i = 0; i < len; i++) buf[i] = v->v.buffer.data[i];
            *out = aml_obj_make_buf(buf, len);
            return p;
        }
        if (v->type == AML_OBJ_PACKAGE) {
            uint32_t count = v->v.package.count;
            struct acpi_object **items = (struct acpi_object **)kmalloc(sizeof(struct acpi_object *) * count);
            if (!items) return p;
            for (uint32_t i = 0; i < count; i++) {
                items[i] = v->v.package.items[i];
            }
            *out = aml_obj_make_pkg(items, count);
            return p;
        }
        *out = v;
        return p;
    }
    if (op == 0x73) { // Concatenate
        struct acpi_object *a = 0;
        struct acpi_object *b = 0;
        p = exec_parse_term(p, end, ctx, &a);
        p = exec_parse_term(p, end, ctx, &b);
        struct acpi_object *va = eval_object(ctx, a);
        struct acpi_object *vb = eval_object(ctx, b);
        if (va && vb && va->type == AML_OBJ_STRING && vb->type == AML_OBJ_STRING) {
            const char *sa = va->v.string ? va->v.string : "";
            const char *sb = vb->v.string ? vb->v.string : "";
            uint32_t la = 0, lb = 0;
            while (sa[la]) la++;
            while (sb[lb]) lb++;
            char *buf = (char *)kmalloc(la + lb + 1);
            if (!buf) return p;
            for (uint32_t i = 0; i < la; i++) buf[i] = sa[i];
            for (uint32_t i = 0; i < lb; i++) buf[la + i] = sb[i];
            buf[la + lb] = 0;
            *out = aml_obj_make_str(buf);
            return p;
        }
        if (va && vb && va->type == AML_OBJ_BUFFER && vb->type == AML_OBJ_BUFFER) {
            uint32_t la = va->v.buffer.length;
            uint32_t lb = vb->v.buffer.length;
            uint8_t *buf = (uint8_t *)kmalloc(la + lb);
            if (!buf) return p;
            for (uint32_t i = 0; i < la; i++) buf[i] = va->v.buffer.data[i];
            for (uint32_t i = 0; i < lb; i++) buf[la + i] = vb->v.buffer.data[i];
            *out = aml_obj_make_buf(buf, la + lb);
            return p;
        }
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0x84) { // ConcatenateResTemplate
        struct acpi_object *a = 0;
        struct acpi_object *b = 0;
        p = exec_parse_term(p, end, ctx, &a);
        p = exec_parse_term(p, end, ctx, &b);
        struct acpi_object *va = eval_object(ctx, a);
        struct acpi_object *vb = eval_object(ctx, b);
        if (va && vb && va->type == AML_OBJ_BUFFER && vb->type == AML_OBJ_BUFFER) {
            uint32_t la = va->v.buffer.length;
            uint32_t lb = vb->v.buffer.length;
            uint8_t *buf = (uint8_t *)kmalloc(la + lb);
            if (!buf) return p;
            for (uint32_t i = 0; i < la; i++) buf[i] = va->v.buffer.data[i];
            for (uint32_t i = 0; i < lb; i++) buf[la + i] = vb->v.buffer.data[i];
            *out = aml_obj_make_buf(buf, la + lb);
            return p;
        }
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0x88) { // Index
        struct acpi_object *src = 0;
        struct acpi_object *idx = 0;
        p = exec_parse_term(p, end, ctx, &src);
        p = exec_parse_term(p, end, ctx, &idx);
        struct acpi_object *v = eval_object(ctx, src);
        uint32_t i = (uint32_t)aml_obj_to_int(eval_object(ctx, idx));
        if (v && v->type == AML_OBJ_PACKAGE && v->v.package.items) {
            if (i < v->v.package.count) {
                *out = aml_obj_make_ref(&v->v.package.items[i]);
                return p;
            }
        }
        if (v && v->type == AML_OBJ_BUFFER && v->v.buffer.data) {
            if (i < v->v.buffer.length) {
                *out = aml_obj_make_ref(&v->v.buffer.data[i]);
                return p;
            }
        }
        if (v && v->type == AML_OBJ_STRING && v->v.string) {
            uint32_t len = 0;
            while (v->v.string[len]) len++;
            if (i < len) {
                *out = aml_obj_make_ref((void *)(uintptr_t)&v->v.string[i]);
                return p;
            }
        }
        *out = aml_obj_make_ref(0);
        return p;
    }
    if (op == 0x5B) { // extended opcode
        if (p >= end) return p;
        uint8_t ext = *p++;
        if (ext == 0x21) { // Stall
            struct acpi_object *t = 0;
            p = exec_parse_term(p, end, ctx, &t);
            uint64_t us = aml_obj_to_int(eval_object(ctx, t));
            // busy-wait microseconds
            for (uint64_t i = 0; i < us * 100; i++) {
                __asm__ volatile("pause");
            }
            *out = aml_obj_make_int(0);
            return p;
        }
        if (ext == 0x22) { // Sleep
            struct acpi_object *t = 0;
            p = exec_parse_term(p, end, ctx, &t);
            uint64_t ms = aml_obj_to_int(eval_object(ctx, t));
            extern void hpet_sleep_ms(uint64_t ms);
            hpet_sleep_ms(ms);
            *out = aml_obj_make_int(0);
            return p;
        }
        if (ext == 0x23) { // Acquire
            struct acpi_object *t = 0;
            struct acpi_object *timeout = 0;
            p = exec_parse_term(p, end, ctx, &t);
            p = exec_parse_term(p, end, ctx, &timeout);
            (void)timeout;
            // Single-threaded AML execution: always succeed immediately.
            // DO NOT spin on sync_level — that field holds the AML-declared
            // sync level (0-15), not a lock state, so spinning on it deadlocks
            // on any mutex with sync_level > 0.
            *out = aml_obj_make_int(0);
            return p;
        }
        if (ext == 0x27) { // Release
            struct acpi_object *t = 0;
            p = exec_parse_term(p, end, ctx, &t);
            (void)t;
            // Single-threaded AML execution: Release is a no-op.
            *out = aml_obj_make_int(0);
            return p;
        }
        if (ext == 0x24) { // Signal
            struct acpi_object *t = 0;
            p = exec_parse_term(p, end, ctx, &t);
            struct acpi_object *tv = eval_object(ctx, t);
            if (tv && tv->type == AML_OBJ_REFERENCE) {
                struct acpi_node *n = (struct acpi_node *)tv->v.ref;
                if (n && n->type == ACPI_NODE_EVENT) {
                    n->u.power_res.system_level = 1;
                    *out = aml_obj_make_int(0);
                    return p;
                }
            }
            *out = aml_obj_make_int(1);
            return p;
        }
        if (ext == 0x25) { // Wait
            struct acpi_object *t = 0;
            struct acpi_object *timeout = 0;
            p = exec_parse_term(p, end, ctx, &t);
            p = exec_parse_term(p, end, ctx, &timeout);
            struct acpi_object *tv = eval_object(ctx, t);
            if (tv && tv->type == AML_OBJ_REFERENCE) {
                struct acpi_node *n = (struct acpi_node *)tv->v.ref;
                if (n && n->type == ACPI_NODE_EVENT) {
                    // basic: if already signaled, consume
                    if (n->u.power_res.system_level) {
                        n->u.power_res.system_level = 0;
                        *out = aml_obj_make_int(0);
                        return p;
                    }
                }
            }
            *out = aml_obj_make_int(1);
            return p;
        }
        if (ext == 0x26) { // Reset
            struct acpi_object *t = 0;
            p = exec_parse_term(p, end, ctx, &t);
            struct acpi_object *tv = eval_object(ctx, t);
            if (tv && tv->type == AML_OBJ_REFERENCE) {
                struct acpi_node *n = (struct acpi_node *)tv->v.ref;
                if (n && n->type == ACPI_NODE_EVENT) {
                    n->u.power_res.system_level = 0;
                    *out = aml_obj_make_int(0);
                    return p;
                }
            }
            *out = aml_obj_make_int(1);
            return p;
        }
        if (ext == 0x12) { // CondRefOf
            struct acpi_object *t = 0;
            p = exec_parse_term(p, end, ctx, &t);
            // Optional target name to receive reference
            struct acpi_node *target = 0;
            if (p < end) {
                p = exec_parse_name_ref(p, end, ctx, &target);
            }
            struct acpi_node *n = 0;
            struct acpi_object *tv = eval_object(ctx, t);
            if (tv && tv->type == AML_OBJ_REFERENCE) {
                n = (struct acpi_node *)tv->v.ref;
            } else if (tv && tv->type == AML_OBJ_STRING && tv->v.string) {
                n = ns_lookup(ns_root(), tv->v.string);
            }
            if (n) {
                if (target) target->object = aml_obj_make_ref(n);
                *out = aml_obj_make_int(1);
                return p;
            }
            *out = aml_obj_make_int(0);
            return p;
        }
        // unknown extended opcode: no-op
        *out = aml_obj_make_int(0);
        return p;
    }
    // Logical
    if (op == 0x90 || op == 0x91 || op == 0x92 || op == 0x93 || op == 0x94 || op == 0x95) {
        struct acpi_object *a = 0;
        struct acpi_object *b = 0;
        if (op == 0x92) { // LNot (also used for LNotEqual/LGreaterEqual/LLessEqual patterns)
            if (p < end && (*p == 0x93 || *p == 0x94 || *p == 0x95)) {
                uint8_t cmp = *p++;
                p = exec_parse_term(p, end, ctx, &a);
                p = exec_parse_term(p, end, ctx, &b);
                uint64_t va = aml_obj_to_int(eval_object(ctx, a));
                uint64_t vb = aml_obj_to_int(eval_object(ctx, b));
                uint64_t res = 0;
                if (cmp == 0x93) res = (va != vb) ? 1 : 0; // LNotEqual
                if (cmp == 0x94) res = (va <= vb) ? 1 : 0; // LLessEqual
                if (cmp == 0x95) res = (va >= vb) ? 1 : 0; // LGreaterEqual
                *out = aml_obj_make_int(res);
                return p;
            }
            p = exec_parse_term(p, end, ctx, &a);
            uint64_t v = aml_obj_to_int(eval_object(ctx, a));
            *out = aml_obj_make_int(v ? 0 : 1);
            return p;
        }
        p = exec_parse_term(p, end, ctx, &a);
        p = exec_parse_term(p, end, ctx, &b);
        uint64_t va = aml_obj_to_int(eval_object(ctx, a));
        uint64_t vb = aml_obj_to_int(eval_object(ctx, b));
        uint64_t res = 0;
        if (op == 0x90) res = (va && vb) ? 1 : 0; // LAnd
        if (op == 0x91) res = (va || vb) ? 1 : 0; // LOr
        if (op == 0x93) res = (va == vb) ? 1 : 0; // LEqual
        if (op == 0x94) res = (va > vb) ? 1 : 0; // LGreater
        if (op == 0x95) res = (va < vb) ? 1 : 0; // LLess
        *out = aml_obj_make_int(res);
        return p;
    }
    // Arithmetic/bitwise
    if (op == 0x72 || op == 0x74 || op == 0x77 || op == 0x78 || op == 0x85 ||
        op == 0x7B || op == 0x7D || op == 0x7F || op == 0x79 || op == 0x7A || op == 0x80) {
        struct acpi_object *a = 0;
        struct acpi_object *b = 0;
        if (op == 0x80) { // Not
            p = exec_parse_term(p, end, ctx, &a);
            uint64_t va = aml_obj_to_int(eval_object(ctx, a));
            *out = aml_obj_make_int(~va);
            return p;
        }
        p = exec_parse_term(p, end, ctx, &a);
        p = exec_parse_term(p, end, ctx, &b);
        uint64_t va = aml_obj_to_int(eval_object(ctx, a));
        uint64_t vb = aml_obj_to_int(eval_object(ctx, b));
        if (op == 0x72) *out = aml_obj_make_int(va + vb);
        if (op == 0x74) *out = aml_obj_make_int(va - vb);
        if (op == 0x77) *out = aml_obj_make_int(va * vb);
        if (op == 0x78) *out = aml_obj_make_int(vb ? (va / vb) : 0);
        if (op == 0x85) *out = aml_obj_make_int(vb ? (va % vb) : 0);
        if (op == 0x7B) *out = aml_obj_make_int(va & vb);
        if (op == 0x7D) *out = aml_obj_make_int(va | vb);
        if (op == 0x7F) *out = aml_obj_make_int(va ^ vb);
        if (op == 0x79) *out = aml_obj_make_int(va << (vb & 63));
        if (op == 0x7A) *out = aml_obj_make_int(va >> (vb & 63));
        return p;
    }

    // Rewind and parse data objects / names
    return exec_parse_data_object(p - 1, end, ctx, out);
}

static const uint8_t *exec_parse_data_object(const uint8_t *p, const uint8_t *end,
                                             struct aml_exec_ctx *ctx, struct acpi_object **out) {
    if (p >= end) return p;
    uint8_t op = *p++;
    if (op == 0x00) { *out = aml_obj_make_int(0); return p; }
    if (op == 0x01) { *out = aml_obj_make_int(1); return p; }
    if (op == 0x0A && p + 1 <= end) { *out = aml_obj_make_int(p[0]); return p + 1; }
    if (op == 0x0B && p + 2 <= end) { *out = aml_obj_make_int((uint16_t)(p[0] | (p[1] << 8))); return p + 2; }
    if (op == 0x0C && p + 4 <= end) {
        *out = aml_obj_make_int((uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)));
        return p + 4;
    }
    if (op == 0x0E && p + 8 <= end) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
        *out = aml_obj_make_int(v);
        return p + 8;
    }
    if (op == 0x0D) { // StringPrefix
        const uint8_t *s = p;
        while (s < end && *s) s++;
        uint32_t len = (uint32_t)(s - p);
        char *buf = (char *)kmalloc(len + 1);
        if (!buf) return s + 1;
        for (uint32_t i = 0; i < len; i++) buf[i] = (char)p[i];
        buf[len] = 0;
        *out = aml_obj_make_str(buf);
        return s + 1;
    }
    if (op == 0x11) { // Buffer
        uint32_t consumed = 0;
        uint32_t pkg_len = parse_pkg_length_exec(p, &consumed);
        const uint8_t *body = p + consumed;
        const uint8_t *limit = body + pkg_len - consumed;
        if (limit > end) limit = end;

        struct acpi_object *size_obj = 0;
        const uint8_t *q = exec_parse_term(body, limit, ctx, &size_obj);
        uint64_t size = aml_obj_to_int(eval_object(ctx, size_obj));
        if (size > 0xFFFFFFFFu) size = 0xFFFFFFFFu;
        uint32_t len = (uint32_t)size;
        uint8_t *buf = (uint8_t *)kmalloc(len);
        if (!buf) return limit;
        for (uint32_t i = 0; i < len; i++) buf[i] = 0;
        uint32_t avail = (uint32_t)(limit - q);
        uint32_t copy = (avail < len) ? avail : len;
        for (uint32_t i = 0; i < copy; i++) buf[i] = q[i];
        *out = aml_obj_make_buf(buf, len);
        return limit;
    }
    if (op == 0x12 || op == 0x13) { // Package / VarPackage
        uint32_t consumed = 0;
        uint32_t pkg_len = parse_pkg_length_exec(p, &consumed);
        const uint8_t *body = p + consumed;
        const uint8_t *limit = body + pkg_len - consumed;
        if (limit > end) limit = end;
        const uint8_t *q = body;
        uint32_t count = 0;
        if (op == 0x13) {
            struct acpi_object *cnt_obj = 0;
            q = exec_parse_term(q, limit, ctx, &cnt_obj);
            count = (uint32_t)aml_obj_to_int(eval_object(ctx, cnt_obj));
        } else if (q < limit) {
            count = *q++;
        }
        struct acpi_object **items = 0;
        if (count > 0) {
            items = (struct acpi_object **)kmalloc(sizeof(struct acpi_object *) * count);
            if (!items) return limit;
            for (uint32_t i = 0; i < count; i++) items[i] = 0;
        }
        for (uint32_t i = 0; i < count && q < limit; i++) {
            q = exec_parse_term(q, limit, ctx, &items[i]);
        }
        *out = aml_obj_make_pkg(items, count);
        return limit;
    }
    if (op == 0x96) { // ToBuffer
        struct acpi_object *src = 0;
        p = exec_parse_term(p, end, ctx, &src);
        struct acpi_object *v = eval_object(ctx, src);
        if (v && v->type == AML_OBJ_INTEGER) {
            uint8_t *buf = (uint8_t *)kmalloc(8);
            if (!buf) return p;
            for (int i = 0; i < 8; i++) buf[i] = (uint8_t)((v->v.integer >> (i * 8)) & 0xFF);
            *out = aml_obj_make_buf(buf, 8);
            return p;
        }
        if (v && v->type == AML_OBJ_STRING && v->v.string) {
            const char *s = v->v.string;
            uint32_t len = 0;
            while (s[len]) len++;
            uint8_t *buf = (uint8_t *)kmalloc(len);
            if (!buf) return p;
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)s[i];
            *out = aml_obj_make_buf(buf, len);
            return p;
        }
        *out = aml_obj_make_buf(0, 0);
        return p;
    }
    if (op == 0x99) { // ToInteger
        struct acpi_object *src = 0;
        p = exec_parse_term(p, end, ctx, &src);
        struct acpi_object *v = eval_object(ctx, src);
        if (v && v->type == AML_OBJ_INTEGER) {
            *out = v;
            return p;
        }
        if (v && v->type == AML_OBJ_STRING && v->v.string) {
            *out = aml_obj_make_int(parse_int_from_string(v->v.string));
            return p;
        }
        if (v && v->type == AML_OBJ_BUFFER && v->v.buffer.data) {
            uint64_t val = 0;
            uint32_t n = v->v.buffer.length;
            if (n > 8) n = 8;
            for (uint32_t i = 0; i < n; i++) {
                val |= ((uint64_t)v->v.buffer.data[i]) << (i * 8);
            }
            *out = aml_obj_make_int(val);
            return p;
        }
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0x97 || op == 0x98) { // ToDecimalString / ToHexString
        struct acpi_object *src = 0;
        p = exec_parse_term(p, end, ctx, &src);
        struct acpi_object *v = eval_object(ctx, src);
        uint64_t val = aml_obj_to_int(v);
        char *buf = (char *)kmalloc(32);
        if (!buf) return p;
        if (op == 0x97) {
            char tmp[32];
            int i = 0;
            if (val == 0) tmp[i++] = '0';
            while (val) {
                tmp[i++] = (char)('0' + (val % 10));
                val /= 10;
            }
            int j = 0;
            while (i > 0) buf[j++] = tmp[--i];
            buf[j] = 0;
        } else {
            const char *hex = "0123456789ABCDEF";
            char tmp[32];
            int i = 0;
            if (val == 0) tmp[i++] = '0';
            while (val) {
                tmp[i++] = hex[val & 0xF];
                val >>= 4;
            }
            int j = 0;
            while (i > 0) buf[j++] = tmp[--i];
            buf[j] = 0;
        }
        *out = aml_obj_make_str(buf);
        return p;
    }
    if (op == 0x9C) { // ToString
        struct acpi_object *src = 0;
        p = exec_parse_term(p, end, ctx, &src);
        struct acpi_object *v = eval_object(ctx, src);
        if (v && v->type == AML_OBJ_STRING) {
            *out = v;
            return p;
        }
        if (v && v->type == AML_OBJ_INTEGER) {
            char *buf = (char *)kmalloc(32);
            if (!buf) return p;
            uint64_t val = v->v.integer;
            char tmp[32];
            int i = 0;
            if (val == 0) tmp[i++] = '0';
            while (val) {
                tmp[i++] = (char)('0' + (val % 10));
                val /= 10;
            }
            int j = 0;
            while (i > 0) buf[j++] = tmp[--i];
            buf[j] = 0;
            *out = aml_obj_make_str(buf);
            return p;
        }
        *out = aml_obj_make_str("");
        return p;
    }
    if (op >= 0x68 && op <= 0x6E) { // Arg0 - Arg6
        uint32_t idx = (uint32_t)(op - 0x68);
        if (ctx && idx < ctx->arg_count && ctx->args[idx]) {
            *out = ctx->args[idx];
        } else {
            *out = aml_obj_make_int(0);
        }
        return p;
    }
    if (op >= 0x60 && op <= 0x67) { // Local0 - Local7 (not stored)
        uint32_t idx = (uint32_t)(op - 0x60);
        if (ctx && idx < 8 && ctx->locals[idx]) {
            *out = ctx->locals[idx];
        } else {
            *out = aml_obj_make_int(0);
        }
        return p;
    }
    // NameString as TermArg
    {
        struct acpi_node *n = 0;
        const uint8_t *q = exec_parse_name_string(p - 1, end, ctx ? ctx->scope : ns_root(), &n);
        if (n) {
            if (n->type == ACPI_NODE_METHOD) {
                struct acpi_object *args[7];
                uint32_t ac = n->u.method.arg_count;
                if (ac > 7) ac = 7;
                for (uint32_t i = 0; i < ac; i++) {
                    args[i] = 0;
                    q = exec_parse_term(q, end, ctx, &args[i]);
                }
                *out = aml_exec_method(n, args, ac);
                return q;
            }
            if (n->object) {
                *out = (struct acpi_object *)n->object;
                return q;
            }
            *out = aml_obj_make_ref(n);
            return q;
        }
    }
    return p;
}

static struct acpi_object *eval_object(struct aml_exec_ctx *ctx, struct acpi_object *obj) {
    if (!obj) return aml_obj_make_int(0);
    if (obj->type == AML_OBJ_REFERENCE) {
        struct acpi_node *n = (struct acpi_node *)obj->v.ref;
        if (!n) return aml_obj_make_int(0);
        if (n->type == ACPI_NODE_FIELD) {
            struct acpi_object *v = aml_read_field(n);
            return v ? v : aml_obj_make_int(0);
        }
        if (n->type == ACPI_NODE_METHOD) {
            return aml_exec_method(n, 0, 0);
        }
        if (n->object) return (struct acpi_object *)n->object;
    }
    return obj;
}

static uint64_t parse_int_from_string(const char *s) {
    if (!s) return 0;
    uint64_t val = 0;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    while (*s) {
        char c = *s++;
        uint64_t digit;
        if (c >= '0' && c <= '9') digit = (uint64_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = (uint64_t)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') digit = (uint64_t)(c - 'A' + 10);
        else break;
        val = val * (uint64_t)base + digit;
    }
    return val;
}
static struct acpi_object *aml_exec_method(struct acpi_node *m, struct acpi_object **args, uint32_t arg_count) {
    if (!m || !m->u.method.bytecode || m->u.method.bc_len == 0) return 0;
    struct aml_exec_ctx ctx;
    ctx.scope = m->parent ? m->parent : ns_root();
    ctx.arg_count = (arg_count > 7) ? 7 : arg_count;
    for (uint32_t i = 0; i < 7; i++) ctx.args[i] = 0;
    for (uint32_t i = 0; i < 8; i++) ctx.locals[i] = 0;
    for (uint32_t i = 0; i < ctx.arg_count; i++) ctx.args[i] = args[i];
    ctx.break_flag = 0;
    ctx.continue_flag = 0;

    const uint8_t *p = m->u.method.bytecode;
    const uint8_t *end = p + m->u.method.bc_len;
    ctx.ret = 0;
    ctx.have_ret = 0;
    while (p < end && !ctx.have_ret) {
        struct acpi_object *res = 0;
        p = exec_parse_term(p, end, &ctx, &res);
    }
    return ctx.ret;
}

struct acpi_object *aml_eval(const char *path) {
    struct acpi_node *n = ns_lookup(ns_root(), path);
    if (!n) {
        return 0;
    }

    if (n->object) {
        return (struct acpi_object *)n->object;
    }

    if (n->type == ACPI_NODE_FIELD) {
        struct acpi_object *val = aml_read_field(n);
        if (val) return val;
    }

    if (n->type == ACPI_NODE_METHOD) {
        struct acpi_object *ret = aml_exec_method(n, 0, 0);
        if (ret) return ret;
    }

    return eval_builtin(n->name);
}

struct acpi_object *aml_eval_with_args(const char *path, struct acpi_object **args, uint32_t arg_count) {
    struct acpi_node *n = ns_lookup(ns_root(), path);
    if (!n) {
        return 0;
    }
    if (n->type == ACPI_NODE_METHOD) {
        return aml_exec_method(n, args, arg_count);
    }
    return aml_eval(path);
}
