#ifndef TATER_ACPI_NAMESPACE_H
#define TATER_ACPI_NAMESPACE_H

#include <stdint.h>

#define ACPI_NODE_ROOT        0
#define ACPI_NODE_DEVICE      1
#define ACPI_NODE_METHOD      2
#define ACPI_NODE_INTEGER     3
#define ACPI_NODE_STRING      4
#define ACPI_NODE_BUFFER      5
#define ACPI_NODE_PACKAGE     6
#define ACPI_NODE_SCOPE       7
#define ACPI_NODE_OP_REGION   8
#define ACPI_NODE_FIELD       9
#define ACPI_NODE_MUTEX       10
#define ACPI_NODE_EVENT       11
#define ACPI_NODE_POWER_RES   12
#define ACPI_NODE_PROCESSOR   13
#define ACPI_NODE_THERMAL     14
#define ACPI_NODE_ALIAS       15
#define ACPI_NODE_UNKNOWN     0xFF

struct acpi_node {
    char name[4];
    uint8_t type;
    struct acpi_node *parent;
    struct acpi_node *first_child;
    struct acpi_node *next_sibling;
    void *object;
    union {
        struct {
            uint8_t *bytecode;
            uint32_t bc_len;
            uint8_t arg_count;
            uint8_t flags;
        } method;
        struct {
            uint8_t space;
            uint64_t offset;
            uint32_t length;
        } op_region;
        struct {
            struct acpi_node *region;
            uint32_t bit_offset;
            uint32_t bit_length;
            uint8_t access_type;
            uint8_t lock_rule;
            uint8_t update_rule;
            uint8_t field_type; // 0=region,1=bank,2=index
            struct acpi_node *bank_reg;
            uint32_t bank_value;
            struct acpi_node *index_reg;
            struct acpi_node *data_reg;
        } field;
        struct {
            uint8_t proc_id;
            uint32_t pblk_addr;
            uint8_t pblk_len;
        } processor;
        struct {
            uint8_t system_level;
            uint16_t resource_order;
        } power_res;
        struct {
            uint8_t sync_level;
        } mutex;
        struct acpi_node *alias;
    } u;
};

void acpi_ns_init(void);
struct acpi_node *ns_root(void);
struct acpi_node *ns_lookup(struct acpi_node *scope, const char *path);
struct acpi_node *ns_create(struct acpi_node *scope, const char name[4], uint8_t type);
struct acpi_node *ns_find_child(struct acpi_node *parent, const char name[4]);
int ns_hid_match(struct acpi_node *node, const char *hid_str);
struct acpi_node *ns_find_device(const char *hid_str);
struct acpi_node *ns_find_device_by_adr(uint32_t adr);
void ns_walk(void (*cb)(struct acpi_node *node, void *ctx), void *ctx);
void ns_build_path(struct acpi_node *n, char *buf, uint32_t max);
uint32_t ns_node_count(void);

#endif
