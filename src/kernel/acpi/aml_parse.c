// AML parser (populate namespace)

#include <stdint.h>
#include "aml_parse.h"
#include "tables.h"
#include "fadt.h"
#include "namespace.h"
#include "aml_types.h"
#include "../mm/vmm.h"

void kprint(const char *fmt, ...);
void *kmalloc(uint64_t size);

static uint32_t parse_pkg_length(const uint8_t *p, uint32_t *consumed) {
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

static const uint8_t *parse_name_seg(const uint8_t *p, char name[4]) {
    for (int i = 0; i < 4; i++) {
        name[i] = (char)p[i];
    }
    return p + 4;
}

static const uint8_t *parse_name_string(const uint8_t *p, const uint8_t *end,
                                        struct acpi_node *scope,
                                        struct acpi_node **out_scope,
                                        char names[][4], uint32_t *out_count) {
    struct acpi_node *cur = scope ? scope : ns_root();
    uint32_t count = 0;
    if (p >= end) {
        *out_scope = cur;
        *out_count = 0;
        return p;
    }

    while (p < end && *p == 0x5E) {
        if (cur->parent) cur = cur->parent;
        p++;
    }
    if (p < end && *p == 0x5C) {
        cur = ns_root();
        p++;
    }
    if (p >= end) {
        *out_scope = cur;
        *out_count = 0;
        return p;
    }
    if (*p == 0x00) { // NullName
        p++;
        *out_scope = cur;
        *out_count = 0;
        return p;
    }

    if (*p == 0x2E) { // DualNamePrefix
        p++;
        count = 2;
    } else if (*p == 0x2F) { // MultiNamePrefix
        p++;
        if (p < end) {
            count = *p++;
        }
    } else {
        count = 1;
    }

    if (count > 16) count = 16;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 4 > end) {
            count = i;
            break;
        }
        p = parse_name_seg(p, names[i]);
    }

    *out_scope = cur;
    *out_count = count;
    return p;
}

static struct acpi_node *create_path(struct acpi_node *scope, char names[][4], uint32_t count, uint8_t type_last) {
    struct acpi_node *cur = scope ? scope : ns_root();
    for (uint32_t i = 0; i < count; i++) {
        uint8_t type = (i == count - 1) ? type_last : ACPI_NODE_SCOPE;
        cur = ns_create(cur, names[i], type);
        if (!cur) return 0;
    }
    return cur;
}

static struct acpi_node *resolve_path(struct acpi_node *scope, char names[][4], uint32_t count) {
    struct acpi_node *cur = scope ? scope : ns_root();
    for (uint32_t i = 0; i < count; i++) {
        cur = ns_find_child(cur, names[i]);
        if (!cur) return 0;
    }
    return cur;
}

static const uint8_t *parse_data_object(const uint8_t *p, const uint8_t *end, struct acpi_object **out);

static const uint8_t *parse_string(const uint8_t *p, const uint8_t *end, struct acpi_object **out) {
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

static const uint8_t *parse_buffer(const uint8_t *p, const uint8_t *end, struct acpi_object **out) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) limit = end;

    struct acpi_object *size_obj = 0;
    const uint8_t *q = parse_data_object(body, limit, &size_obj);
    uint64_t size = aml_obj_to_int(size_obj);
    if (size > 0xFFFFFFFFu) size = 0xFFFFFFFFu;

    uint32_t avail = (uint32_t)(limit - q);
    uint32_t len = (uint32_t)size;
    uint8_t *buf = (uint8_t *)kmalloc(len);
    if (!buf) return limit;
    for (uint32_t i = 0; i < len; i++) buf[i] = 0;
    uint32_t copy = (avail < len) ? avail : len;
    for (uint32_t i = 0; i < copy; i++) buf[i] = q[i];

    *out = aml_obj_make_buf(buf, len);
    return limit;
}

static const uint8_t *parse_package(const uint8_t *p, const uint8_t *end, struct acpi_object **out, int var_pkg) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) limit = end;

    uint32_t count = 0;
    const uint8_t *q = body;
    if (var_pkg) {
        struct acpi_object *cnt_obj = 0;
        q = parse_data_object(q, limit, &cnt_obj);
        count = (uint32_t)aml_obj_to_int(cnt_obj);
    } else {
        if (q < limit) {
            count = *q++;
        }
    }

    struct acpi_object **items = 0;
    if (count > 0) {
        items = (struct acpi_object **)kmalloc(sizeof(struct acpi_object *) * count);
        if (!items) return limit;
        for (uint32_t i = 0; i < count; i++) items[i] = 0;
    }

    for (uint32_t i = 0; i < count && q < limit; i++) {
        q = parse_data_object(q, limit, &items[i]);
    }

    *out = aml_obj_make_pkg(items, count);
    return limit;
}

static const uint8_t *parse_data_object(const uint8_t *p, const uint8_t *end, struct acpi_object **out) {
    if (p >= end) return p;
    uint8_t op = *p++;
    if (op == 0x00) { // Zero
        *out = aml_obj_make_int(0);
        return p;
    }
    if (op == 0x01) { // One
        *out = aml_obj_make_int(1);
        return p;
    }
    if (op == 0x0A && p + 1 <= end) { // ByteConst
        *out = aml_obj_make_int(p[0]);
        return p + 1;
    }
    if (op == 0x0B && p + 2 <= end) { // WordConst
        *out = aml_obj_make_int((uint16_t)(p[0] | (p[1] << 8)));
        return p + 2;
    }
    if (op == 0x0C && p + 4 <= end) { // DWordConst
        *out = aml_obj_make_int((uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)));
        return p + 4;
    }
    if (op == 0x0E && p + 8 <= end) { // QWordConst
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
        *out = aml_obj_make_int(v);
        return p + 8;
    }
    if (op == 0x0D) { // StringPrefix
        return parse_string(p, end, out);
    }
    if (op == 0x11) { // Buffer
        return parse_buffer(p, end, out);
    }
    if (op == 0x12) { // Package
        return parse_package(p, end, out, 0);
    }
    if (op == 0x13) { // VarPackage
        return parse_package(p, end, out, 1);
    }
    return p;
}

static void parse_block(const uint8_t *p, const uint8_t *end, struct acpi_node *scope);

static void parse_device(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) {
        limit = end;
    }

    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(body, limit, scope, &ns_scope, names, &count);
    struct acpi_node *dev = create_path(ns_scope, names, count, ACPI_NODE_DEVICE);

    parse_block(q, limit, dev);
}

static void parse_method(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) {
        limit = end;
    }

    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(body, limit, scope, &ns_scope, names, &count);
    struct acpi_node *m = create_path(ns_scope, names, count, ACPI_NODE_METHOD);

    // Skip flags (1 byte)
    if (q < limit) {
        if (m) {
            uint8_t flags = *q;
            m->u.method.flags = flags;
            m->u.method.arg_count = flags & 0x7;
            m->u.method.bytecode = (uint8_t *)(uintptr_t)(q + 1);
            m->u.method.bc_len = (uint32_t)(limit - (q + 1));
        }
        q++;
    }
    (void)q;
}

static void parse_scope(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) {
        limit = end;
    }

    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(body, limit, scope, &ns_scope, names, &count);
    struct acpi_node *sc = create_path(ns_scope, names, count, ACPI_NODE_SCOPE);

    parse_block(q, limit, sc);
}

static const uint8_t *parse_nameop(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(p, end, scope, &ns_scope, names, &count);
    struct acpi_node *n = create_path(ns_scope, names, count, ACPI_NODE_UNKNOWN);
    if (!n) return q;

    struct acpi_object *obj = 0;
    q = parse_data_object(q, end, &obj);
    if (obj) {
        n->object = obj;
        if (obj->type == AML_OBJ_INTEGER) n->type = ACPI_NODE_INTEGER;
        if (obj->type == AML_OBJ_STRING) n->type = ACPI_NODE_STRING;
        if (obj->type == AML_OBJ_BUFFER) n->type = ACPI_NODE_BUFFER;
        if (obj->type == AML_OBJ_PACKAGE) n->type = ACPI_NODE_PACKAGE;
    }
    return q;
}

static const uint8_t *parse_alias(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    char names1[16][4], names2[16][4];
    uint32_t count1 = 0, count2 = 0;
    struct acpi_node *scope1 = 0, *scope2 = 0;
    const uint8_t *q = parse_name_string(p, end, scope, &scope1, names1, &count1);
    q = parse_name_string(q, end, scope, &scope2, names2, &count2);
    struct acpi_node *target = resolve_path(scope1, names1, count1);
    struct acpi_node *alias = create_path(scope2, names2, count2, ACPI_NODE_ALIAS);
    if (alias) {
        alias->u.alias = target;
    }
    return q;
}

static const uint8_t *parse_opregion(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    // OpRegion has NO PkgLength. Format: NameString RegionSpace(byte) RegionOffset(TermArg) RegionLen(TermArg)
    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(p, end, scope, &ns_scope, names, &count);
    if (q >= end) return q;
    uint8_t space = *q++;

    struct acpi_object *off_obj = 0;
    struct acpi_object *len_obj = 0;
    q = parse_data_object(q, end, &off_obj);
    q = parse_data_object(q, end, &len_obj);
    uint64_t off = aml_obj_to_int(off_obj);
    uint64_t len = aml_obj_to_int(len_obj);

    struct acpi_node *r = create_path(ns_scope, names, count, ACPI_NODE_OP_REGION);
    if (r) {
        r->u.op_region.space = space;
        r->u.op_region.offset = off;
        r->u.op_region.length = (uint32_t)len;
    }
    return q;
}

static const uint8_t *parse_field(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) limit = end;

    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(body, limit, scope, &ns_scope, names, &count);
    struct acpi_node *region = resolve_path(ns_scope, names, count);
    if (!region) return limit;
    if (q >= limit) return limit;
    uint8_t field_flags = *q++;
    uint8_t access_type = field_flags & 0x0F;
    uint8_t lock_rule = (field_flags >> 4) & 0x01;
    uint8_t update_rule = (field_flags >> 5) & 0x03;

    uint32_t bit_offset = 0;
    while (q < limit) {
        uint8_t op = *q;
        if (op == 0x00) { // ReservedField
            q++;
            uint32_t len_cons = 0;
            uint32_t bits = parse_pkg_length(q, &len_cons);
            q += len_cons;
            bit_offset += bits;
            continue;
        }
        if (op == 0x01 && q + 2 <= limit) { // AccessField
            q++;
            access_type = *q++;
            q++;
            continue;
        }
        if (op == 0x03 && q + 3 <= limit) { // ExtendedAccessField
            q++;
            access_type = *q++;
            q += 2;
            continue;
        }
        if (q + 4 > limit) break;
        char fname[4];
        q = parse_name_seg(q, fname);
        uint32_t len_cons = 0;
        uint32_t bits = parse_pkg_length(q, &len_cons);
        q += len_cons;

        struct acpi_node *f = ns_create(scope, fname, ACPI_NODE_FIELD);
        if (f) {
            f->u.field.region = region;
            f->u.field.bit_offset = bit_offset;
            f->u.field.bit_length = bits;
            f->u.field.access_type = access_type;
            f->u.field.lock_rule = lock_rule;
            f->u.field.update_rule = update_rule;
            f->u.field.field_type = 0;
        }
        bit_offset += bits;
    }
    return limit;
}

static const uint8_t *parse_bank_field(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) limit = end;

    char names1[16][4];
    uint32_t count1 = 0;
    struct acpi_node *scope1 = 0;
    const uint8_t *q = parse_name_string(body, limit, scope, &scope1, names1, &count1);
    struct acpi_node *region = resolve_path(scope1, names1, count1);
    if (!region) return limit;

    char names2[16][4];
    uint32_t count2 = 0;
    struct acpi_node *scope2 = 0;
    q = parse_name_string(q, limit, scope, &scope2, names2, &count2);
    struct acpi_node *bank_reg = resolve_path(scope2, names2, count2);
    if (!bank_reg) return limit;

    struct acpi_object *bank_obj = 0;
    q = parse_data_object(q, limit, &bank_obj);
    uint32_t bank_val = (uint32_t)aml_obj_to_int(bank_obj);

    if (q >= limit) return limit;
    uint8_t field_flags = *q++;
    uint8_t access_type = field_flags & 0x0F;
    uint8_t lock_rule = (field_flags >> 4) & 0x01;
    uint8_t update_rule = (field_flags >> 5) & 0x03;

    uint32_t bit_offset = 0;
    while (q < limit) {
        uint8_t op = *q;
        if (op == 0x00) { // ReservedField
            q++;
            uint32_t len_cons = 0;
            uint32_t bits = parse_pkg_length(q, &len_cons);
            q += len_cons;
            bit_offset += bits;
            continue;
        }
        if (op == 0x01 && q + 2 <= limit) { // AccessField
            q++;
            access_type = *q++;
            q++;
            continue;
        }
        if (op == 0x03 && q + 3 <= limit) { // ExtendedAccessField
            q++;
            access_type = *q++;
            q += 2;
            continue;
        }
        if (q + 4 > limit) break;
        char fname[4];
        q = parse_name_seg(q, fname);
        uint32_t len_cons = 0;
        uint32_t bits = parse_pkg_length(q, &len_cons);
        q += len_cons;

        struct acpi_node *f = ns_create(scope, fname, ACPI_NODE_FIELD);
        if (f) {
            f->u.field.region = region;
            f->u.field.bit_offset = bit_offset;
            f->u.field.bit_length = bits;
            f->u.field.access_type = access_type;
            f->u.field.lock_rule = lock_rule;
            f->u.field.update_rule = update_rule;
            f->u.field.field_type = 1;
            f->u.field.bank_reg = bank_reg;
            f->u.field.bank_value = bank_val;
        }
        bit_offset += bits;
    }
    return limit;
}

static const uint8_t *parse_index_field(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) limit = end;

    char names1[16][4];
    uint32_t count1 = 0;
    struct acpi_node *scope1 = 0;
    const uint8_t *q = parse_name_string(body, limit, scope, &scope1, names1, &count1);
    struct acpi_node *index_reg = resolve_path(scope1, names1, count1);
    if (!index_reg) return limit;

    char names2[16][4];
    uint32_t count2 = 0;
    struct acpi_node *scope2 = 0;
    q = parse_name_string(q, limit, scope, &scope2, names2, &count2);
    struct acpi_node *data_reg = resolve_path(scope2, names2, count2);
    if (!data_reg) return limit;

    if (q >= limit) return limit;
    uint8_t field_flags = *q++;
    uint8_t access_type = field_flags & 0x0F;
    uint8_t lock_rule = (field_flags >> 4) & 0x01;
    uint8_t update_rule = (field_flags >> 5) & 0x03;

    uint32_t bit_offset = 0;
    while (q < limit) {
        uint8_t op = *q;
        if (op == 0x00) { // ReservedField
            q++;
            uint32_t len_cons = 0;
            uint32_t bits = parse_pkg_length(q, &len_cons);
            q += len_cons;
            bit_offset += bits;
            continue;
        }
        if (op == 0x01 && q + 2 <= limit) { // AccessField
            q++;
            access_type = *q++;
            q++;
            continue;
        }
        if (op == 0x03 && q + 3 <= limit) { // ExtendedAccessField
            q++;
            access_type = *q++;
            q += 2;
            continue;
        }
        if (q + 4 > limit) break;
        char fname[4];
        q = parse_name_seg(q, fname);
        uint32_t len_cons = 0;
        uint32_t bits = parse_pkg_length(q, &len_cons);
        q += len_cons;

        struct acpi_node *f = ns_create(scope, fname, ACPI_NODE_FIELD);
        if (f) {
            f->u.field.region = 0;
            f->u.field.bit_offset = bit_offset;
            f->u.field.bit_length = bits;
            f->u.field.access_type = access_type;
            f->u.field.lock_rule = lock_rule;
            f->u.field.update_rule = update_rule;
            f->u.field.field_type = 2;
            f->u.field.index_reg = index_reg;
            f->u.field.data_reg = data_reg;
        }
        bit_offset += bits;
    }
    return limit;
}

static const uint8_t *parse_mutex(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    // Mutex has NO PkgLength. Format: NameString SyncFlags(byte)
    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(p, end, scope, &ns_scope, names, &count);
    uint8_t sync_level = (q < end) ? *q++ : 0;
    struct acpi_node *m = create_path(ns_scope, names, count, ACPI_NODE_MUTEX);
    if (m) m->u.mutex.sync_level = sync_level;
    return q;
}

static const uint8_t *parse_event(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    // Event has NO PkgLength. Format: NameString
    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(p, end, scope, &ns_scope, names, &count);
    create_path(ns_scope, names, count, ACPI_NODE_EVENT);
    return q;
}

static const uint8_t *parse_power_res(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) limit = end;

    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(body, limit, scope, &ns_scope, names, &count);
    uint8_t system_level = (q < limit) ? *q++ : 0;
    uint16_t order = 0;
    if (q + 1 < limit) {
        order = (uint16_t)(q[0] | (q[1] << 8));
    }
    struct acpi_node *pr = create_path(ns_scope, names, count, ACPI_NODE_POWER_RES);
    if (pr) {
        pr->u.power_res.system_level = system_level;
        pr->u.power_res.resource_order = order;
        if (q + 2 <= limit) q += 2;
        parse_block(q, limit, pr);
    }
    return limit;
}

static const uint8_t *parse_processor(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) limit = end;

    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(body, limit, scope, &ns_scope, names, &count);
    uint8_t proc_id = (q < limit) ? *q++ : 0;
    uint32_t pblk = 0;
    if (q + 3 < limit) {
        pblk = (uint32_t)(q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24));
        q += 4;
    }
    uint8_t pblk_len = (q < limit) ? *q++ : 0;
    struct acpi_node *pr = create_path(ns_scope, names, count, ACPI_NODE_PROCESSOR);
    if (pr) {
        pr->u.processor.proc_id = proc_id;
        pr->u.processor.pblk_addr = pblk;
        pr->u.processor.pblk_len = pblk_len;
        parse_block(q, limit, pr);
    }
    return limit;
}

static const uint8_t *parse_thermal_zone(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    uint32_t consumed = 0;
    uint32_t pkg_len = parse_pkg_length(p, &consumed);
    const uint8_t *body = p + consumed;
    const uint8_t *limit = body + pkg_len - consumed;
    if (limit > end) limit = end;

    char names[16][4];
    uint32_t count = 0;
    struct acpi_node *ns_scope = 0;
    const uint8_t *q = parse_name_string(body, limit, scope, &ns_scope, names, &count);
    struct acpi_node *tz = create_path(ns_scope, names, count, ACPI_NODE_THERMAL);
    if (tz) {
        parse_block(q, limit, tz);
    }
    return limit;
}

static void parse_block(const uint8_t *p, const uint8_t *end, struct acpi_node *scope) {
    while (p < end) {
        uint8_t op = *p++;
        if (op == 0x5B) {
            if (p >= end) break;
            uint8_t ext = *p++;
            if (ext == 0x82) { // DeviceOp (has PkgLength)
                parse_device(p, end, scope);
                uint32_t consumed = 0;
                uint32_t pkg_len = parse_pkg_length(p, &consumed);
                p += pkg_len;
                continue;
            }
            if (ext == 0x80) { // OpRegion (NO PkgLength per ACPI spec)
                p = parse_opregion(p, end, scope);
                continue;
            }
            if (ext == 0x81) { // Field (has PkgLength)
                parse_field(p, end, scope);
                uint32_t consumed = 0;
                uint32_t pkg_len = parse_pkg_length(p, &consumed);
                p += pkg_len;
                continue;
            }
            if (ext == 0x87) { // BankField (has PkgLength)
                parse_bank_field(p, end, scope);
                uint32_t consumed = 0;
                uint32_t pkg_len = parse_pkg_length(p, &consumed);
                p += pkg_len;
                continue;
            }
            if (ext == 0x86) { // IndexField (has PkgLength)
                parse_index_field(p, end, scope);
                uint32_t consumed = 0;
                uint32_t pkg_len = parse_pkg_length(p, &consumed);
                p += pkg_len;
                continue;
            }
            if (ext == 0x01) { // Mutex (NO PkgLength per ACPI spec)
                p = parse_mutex(p, end, scope);
                continue;
            }
            if (ext == 0x02) { // Event (NO PkgLength per ACPI spec)
                p = parse_event(p, end, scope);
                continue;
            }
            if (ext == 0x84) { // PowerRes (has PkgLength)
                parse_power_res(p, end, scope);
                uint32_t consumed = 0;
                uint32_t pkg_len = parse_pkg_length(p, &consumed);
                p += pkg_len;
                continue;
            }
            if (ext == 0x83) { // Processor (has PkgLength)
                parse_processor(p, end, scope);
                uint32_t consumed = 0;
                uint32_t pkg_len = parse_pkg_length(p, &consumed);
                p += pkg_len;
                continue;
            }
            if (ext == 0x85) { // ThermalZone (has PkgLength)
                parse_thermal_zone(p, end, scope);
                uint32_t consumed = 0;
                uint32_t pkg_len = parse_pkg_length(p, &consumed);
                p += pkg_len;
                continue;
            }
            // Unknown 0x5B ext op — skip (already consumed 2 bytes)
            continue;
        } else if (op == 0x14) { // MethodOp (has PkgLength)
            parse_method(p, end, scope);
            uint32_t consumed = 0;
            uint32_t pkg_len = parse_pkg_length(p, &consumed);
            p += pkg_len;
            continue;
        } else if (op == 0x10) { // ScopeOp (has PkgLength)
            parse_scope(p, end, scope);
            uint32_t consumed = 0;
            uint32_t pkg_len = parse_pkg_length(p, &consumed);
            p += pkg_len;
            continue;
        } else if (op == 0x08) { // NameOp
            p = parse_nameop(p, end, scope);
            continue;
        } else if (op == 0x06) { // AliasOp
            p = parse_alias(p, end, scope);
            continue;
        } else if (op == 0xA0 || op == 0xA2) { // IfOp / WhileOp (has PkgLength)
            uint32_t consumed = 0;
            uint32_t pkg_len = parse_pkg_length(p, &consumed);
            const uint8_t *block_end = p + pkg_len;
            if (block_end > end) block_end = end;
            // Body includes predicate + TermList; recurse to find declarations
            parse_block(p + consumed, block_end, scope);
            p = block_end;
            continue;
        } else if (op == 0xA1) { // ElseOp (has PkgLength)
            uint32_t consumed = 0;
            uint32_t pkg_len = parse_pkg_length(p, &consumed);
            const uint8_t *block_end = p + pkg_len;
            if (block_end > end) block_end = end;
            parse_block(p + consumed, block_end, scope);
            p = block_end;
            continue;
        }
        // Unknown opcode — skip single byte (already consumed by p++)
    }
}

static void parse_dsdt(uint64_t dsdt_phys) {
    if (!dsdt_phys) {
        return;
    }
    struct acpi_sdt_header *h = (struct acpi_sdt_header *)(uintptr_t)vmm_phys_to_virt(dsdt_phys);
    const uint8_t *aml = (const uint8_t *)h + sizeof(struct acpi_sdt_header);
    const uint8_t *end = (const uint8_t *)h + h->length;
    parse_block(aml, end, ns_root());
}

static void parse_ssdts(void) {
    uint32_t count = acpi_table_count();
    for (uint32_t i = 0; i < count; i++) {
        struct acpi_sdt_header *h = acpi_get_table(i);
        if (!h) continue;
        if (h->signature[0]=='S' && h->signature[1]=='S' && h->signature[2]=='D' && h->signature[3]=='T') {
            const uint8_t *aml = (const uint8_t *)h + sizeof(struct acpi_sdt_header);
            const uint8_t *end = (const uint8_t *)h + h->length;
            parse_block(aml, end, ns_root());
        }
    }
}

void aml_parse_tables(void) {
    acpi_ns_init();
    parse_dsdt(fadt_get_dsdt_phys());
    parse_ssdts();
    kprint("AML: namespace populated\n");
}
