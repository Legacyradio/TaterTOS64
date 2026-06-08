#include "tbridgefs.h"

#include <errno.h>
#include <stdint.h>

#include "../../drivers/smp/smp.h"
#include "../proc/process.h"
#include "vfs.h"

enum tbridgefs_kind {
    TBRIDGEFS_PROC = 1,
    TBRIDGEFS_SYS = 2,
    TBRIDGEFS_USR = 3
};

enum tbridgefs_node {
    TBRIDGEFS_NODE_NONE = 0,
    TBRIDGEFS_NODE_DIR,
    TBRIDGEFS_NODE_OVERCOMMIT,
    TBRIDGEFS_NODE_CGROUP,
    TBRIDGEFS_NODE_THP_ENABLED,
    TBRIDGEFS_NODE_TRACE_MARKER,
    TBRIDGEFS_NODE_MAPS,
    TBRIDGEFS_NODE_MMAP_MIN_ADDR,
    TBRIDGEFS_NODE_PROC_STAT,
    TBRIDGEFS_NODE_CPU_ONLINE,
    TBRIDGEFS_NODE_CGROUP_CPU_MAX,
    TBRIDGEFS_NODE_CGROUP_MEMORY_MAX,
    TBRIDGEFS_NODE_CGROUP_MEMORY_HIGH,
    TBRIDGEFS_NODE_ZONEINFO_UTC,
    TBRIDGEFS_NODE_PROC_EXE,
    TBRIDGEFS_NODE_PROC_AUXV,
    TBRIDGEFS_NODE_PROC_STATUS,
    TBRIDGEFS_NODE_PROC_VERSION,
    TBRIDGEFS_NODE_PROC_CMDLINE,
    TBRIDGEFS_NODE_PROC_STATM,
    TBRIDGEFS_NODE_PROC_MEM
};

static struct fs_ops procfs_ops;
static struct fs_ops sysfs_ops;
static struct fs_ops usrfs_ops;

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static uint32_t slen(const char *s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static enum tbridgefs_node lookup_proc(const char *path) {
    if (!path || path[0] == 0) return TBRIDGEFS_NODE_DIR;
    if (streq(path, "sys") || streq(path, "sys/vm") || streq(path, "self"))
        return TBRIDGEFS_NODE_DIR;
    if (streq(path, "sys/vm/overcommit_memory"))
        return TBRIDGEFS_NODE_OVERCOMMIT;
    if (streq(path, "sys/vm/mmap_min_addr"))
        return TBRIDGEFS_NODE_MMAP_MIN_ADDR;
    if (streq(path, "self/cgroup"))
        return TBRIDGEFS_NODE_CGROUP;
    if (streq(path, "self/maps"))
        return TBRIDGEFS_NODE_MAPS;
    if (streq(path, "self/exe"))
        return TBRIDGEFS_NODE_PROC_EXE;
    if (streq(path, "self/auxv"))
        return TBRIDGEFS_NODE_PROC_AUXV;
    if (streq(path, "self/status"))
        return TBRIDGEFS_NODE_PROC_STATUS;
    if (streq(path, "self/cmdline"))
        return TBRIDGEFS_NODE_PROC_CMDLINE;
    if (streq(path, "self/statm"))
        return TBRIDGEFS_NODE_PROC_STATM;
    if (streq(path, "version"))
        return TBRIDGEFS_NODE_PROC_VERSION;
    if (streq(path, "self/mem") || streq(path, "3/mem"))
        return TBRIDGEFS_NODE_PROC_MEM;
    if (streq(path, "stat"))
        return TBRIDGEFS_NODE_PROC_STAT;
    return TBRIDGEFS_NODE_NONE;
}

static enum tbridgefs_node lookup_sys(const char *path) {
    if (!path || path[0] == 0) return TBRIDGEFS_NODE_DIR;
    if (streq(path, "kernel") ||
        streq(path, "kernel/mm") ||
        streq(path, "kernel/mm/transparent_hugepage") ||
        streq(path, "kernel/debug") ||
        streq(path, "kernel/debug/tracing") ||
        streq(path, "devices") ||
        streq(path, "devices/system") ||
        streq(path, "devices/system/cpu") ||
        streq(path, "fs") ||
        streq(path, "fs/cgroup"))
        return TBRIDGEFS_NODE_DIR;
    if (streq(path, "kernel/mm/transparent_hugepage/enabled"))
        return TBRIDGEFS_NODE_THP_ENABLED;
    if (streq(path, "kernel/debug/tracing/trace_marker"))
        return TBRIDGEFS_NODE_TRACE_MARKER;
    if (streq(path, "devices/system/cpu/online"))
        return TBRIDGEFS_NODE_CPU_ONLINE;
    if (streq(path, "fs/cgroup/cpu.max") || streq(path, "fs/cgroup//cpu.max"))
        return TBRIDGEFS_NODE_CGROUP_CPU_MAX;
    if (streq(path, "fs/cgroup/memory.max") || streq(path, "fs/cgroup//memory.max"))
        return TBRIDGEFS_NODE_CGROUP_MEMORY_MAX;
    if (streq(path, "fs/cgroup/memory.high") || streq(path, "fs/cgroup//memory.high"))
        return TBRIDGEFS_NODE_CGROUP_MEMORY_HIGH;
    return TBRIDGEFS_NODE_NONE;
}

static enum tbridgefs_node lookup_usr(const char *path) {
    if (!path || path[0] == 0) return TBRIDGEFS_NODE_DIR;
    if (streq(path, "UTC"))
        return TBRIDGEFS_NODE_ZONEINFO_UTC;
    return TBRIDGEFS_NODE_NONE;
}

static char g_maps_buf[4096];
static uint32_t g_maps_len;

static uint8_t g_auxv_buf[512];
static uint32_t g_auxv_len;

static char g_cpu_online_buf[32];
static char g_proc_stat_buf[512];
static char g_status_buf[2048];
static char g_statm_buf[96];

static uint32_t dec_append(char *dst, uint32_t pos, uint32_t cap, uint64_t v) {
    char tmp[24];
    uint32_t n = 0;
    if (cap == 0 || pos >= cap) return pos;
    if (v == 0) {
        if (pos + 1 < cap) dst[pos++] = '0';
        return pos;
    }
    while (v && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10ULL));
        v /= 10ULL;
    }
    while (n && pos + 1 < cap) dst[pos++] = tmp[--n];
    return pos;
}

static uint32_t str_append(char *dst, uint32_t pos, uint32_t cap, const char *src) {
    if (!dst || !src || cap == 0) return pos;
    while (*src && pos + 1 < cap) dst[pos++] = *src++;
    if (pos < cap) dst[pos] = '\0';
    return pos;
}

static void cpu_online_regenerate(void) {
    uint32_t ncpu = smp_cpu_count();
    uint32_t pos = 0;
    if (ncpu == 0) ncpu = 1;
    pos = dec_append(g_cpu_online_buf, pos, sizeof(g_cpu_online_buf), 0);
    if (ncpu > 1) {
        if (pos + 1 < sizeof(g_cpu_online_buf)) g_cpu_online_buf[pos++] = '-';
        pos = dec_append(g_cpu_online_buf, pos, sizeof(g_cpu_online_buf), ncpu - 1);
    }
    if (pos + 1 < sizeof(g_cpu_online_buf)) g_cpu_online_buf[pos++] = '\n';
    if (pos >= sizeof(g_cpu_online_buf)) pos = sizeof(g_cpu_online_buf) - 1;
    g_cpu_online_buf[pos] = '\0';
}

static uint64_t cpu_mask_for_online(void) {
    uint32_t ncpu = smp_cpu_count();
    uint64_t mask = 0;
    if (ncpu == 0) ncpu = 1;
    if (ncpu > 64) ncpu = 64;
    for (uint32_t i = 0; i < ncpu; i++) mask |= (1ULL << i);
    return mask;
}

static uint32_t hex16_append(char *dst, uint32_t pos, uint32_t cap, uint64_t v) {
    static const char hexdigits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        if (pos + 1 < cap) dst[pos++] = hexdigits[(v >> (uint32_t)shift) & 0xfULL];
    }
    if (pos < cap) dst[pos] = '\0';
    return pos;
}

static uint32_t cpu_list_append(char *dst, uint32_t pos, uint32_t cap) {
    uint32_t ncpu = smp_cpu_count();
    if (ncpu == 0) ncpu = 1;
    pos = dec_append(dst, pos, cap, 0);
    if (ncpu > 1) {
        if (pos + 1 < cap) dst[pos++] = '-';
        pos = dec_append(dst, pos, cap, ncpu - 1);
    }
    return pos;
}

static void proc_stat_regenerate(void) {
    uint32_t ncpu = smp_cpu_count();
    uint32_t pos = 0;
    if (ncpu == 0) ncpu = 1;
    pos = str_append(g_proc_stat_buf, pos, sizeof(g_proc_stat_buf),
                     "cpu  1 0 1 100 0 0 0 0 0 0\n");
    for (uint32_t i = 0; i < ncpu && pos + 48 < sizeof(g_proc_stat_buf); i++) {
        pos = str_append(g_proc_stat_buf, pos, sizeof(g_proc_stat_buf), "cpu");
        pos = dec_append(g_proc_stat_buf, pos, sizeof(g_proc_stat_buf), i);
        pos = str_append(g_proc_stat_buf, pos, sizeof(g_proc_stat_buf),
                         " 1 0 1 100 0 0 0 0 0 0\n");
    }
    if (pos >= sizeof(g_proc_stat_buf)) pos = sizeof(g_proc_stat_buf) - 1;
    g_proc_stat_buf[pos] = '\0';
}

static void status_regenerate(void) {
    uint32_t pos = 0;
    pos = str_append(g_status_buf, pos, sizeof(g_status_buf),
        "Name:\tclaude\nUmask:\t0022\nState:\tR (running)\nTgid:\t3\nNgid:\t0\nPid:\t3\nPPid:\t1\nTracerPid:\t0\n"
        "Uid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nFDSize:\t64\nGroups:\t\nNStgid:\t3\nNSpid:\t3\nNSpgid:\t0\nNSsid:\t0\n"
        "VmPeak:\t0 kB\nVmSize:\t0 kB\nVmLck:\t0 kB\nVmPin:\t0 kB\nVmHWM:\t0 kB\nVmRSS:\t0 kB\n"
        "RssAnon:\t0 kB\nRssFile:\t0 kB\nRssShmem:\t0 kB\nVmData:\t0 kB\nVmStk:\t0 kB\nVmExe:\t0 kB\nVmLib:\t0 kB\nVmPTE:\t0 kB\nVmSwap:\t0 kB\n"
        "HugePages:\t0 kB\nShdPnd:\t00000000\nSigPnd:\t00000000\nSigBlk:\t00000000\nSigIgn:\t00000000\nSigCgt:\t00000000\n"
        "CapInh:\t0000000000000000\nCapPrm:\t000001ffffffffff\nCapEff:\t000001ffffffffff\nCapBnd:\t000001ffffffffff\nCapAmb:\t0000000000000000\n"
        "NoNewPrivs:\t0\nSeccomp:\t0\nSeccomp_filters:\t0\nSpeculation_Store_Bypass:\tnot vulnerable\nSpeculationIndirectBranch:\tnot affected\nCpus_allowed:\t");
    pos = hex16_append(g_status_buf, pos, sizeof(g_status_buf), cpu_mask_for_online());
    pos = str_append(g_status_buf, pos, sizeof(g_status_buf), "\nCpus_allowed_list:\t");
    pos = cpu_list_append(g_status_buf, pos, sizeof(g_status_buf));
    pos = str_append(g_status_buf, pos, sizeof(g_status_buf),
        "\nMems_allowed:\t1\nMems_allowed_list:\t0\nvoluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n");
    if (pos >= sizeof(g_status_buf)) pos = sizeof(g_status_buf) - 1;
    g_status_buf[pos] = '\0';
}

static void statm_regenerate(void) {
    struct fry_process *cur = proc_current();
    struct fry_process_shared *shared = cur ? cur->shared : 0;
    uint64_t size_pages = 0;
    uint64_t resident_pages = 0;
    uint64_t shared_pages = 0;
    uint64_t text_pages = 0;
    uint64_t data_pages = 0;
    uint32_t pos = 0;

    if (shared) {
        for (int i = 0; i < PROC_VMREG_MAX; i++) {
            const struct fry_vm_region *r = &shared->vm_regions[i];
            uint64_t pages;
            if (!r->used || r->length == 0) continue;
            pages = (r->length + 4095ULL) >> 12;
            size_pages += pages;
            if (r->committed) resident_pages += pages;
            if ((r->flags & 0x01u) != 0) shared_pages += pages;
            if (r->prot & 0x04u) {
                text_pages += pages;
            } else if (r->prot & 0x02u) {
                data_pages += pages;
            }
        }
    }

    /* Linux /proc/[pid]/statm:
     * size resident shared text lib data dt, all in pages. */
    pos = dec_append(g_statm_buf, pos, sizeof(g_statm_buf), size_pages);
    if (pos + 1 < sizeof(g_statm_buf)) g_statm_buf[pos++] = ' ';
    pos = dec_append(g_statm_buf, pos, sizeof(g_statm_buf), resident_pages);
    if (pos + 1 < sizeof(g_statm_buf)) g_statm_buf[pos++] = ' ';
    pos = dec_append(g_statm_buf, pos, sizeof(g_statm_buf), shared_pages);
    if (pos + 1 < sizeof(g_statm_buf)) g_statm_buf[pos++] = ' ';
    pos = dec_append(g_statm_buf, pos, sizeof(g_statm_buf), text_pages);
    if (pos + 1 < sizeof(g_statm_buf)) g_statm_buf[pos++] = ' ';
    pos = dec_append(g_statm_buf, pos, sizeof(g_statm_buf), 0);
    if (pos + 1 < sizeof(g_statm_buf)) g_statm_buf[pos++] = ' ';
    pos = dec_append(g_statm_buf, pos, sizeof(g_statm_buf), data_pages);
    if (pos + 1 < sizeof(g_statm_buf)) g_statm_buf[pos++] = ' ';
    pos = dec_append(g_statm_buf, pos, sizeof(g_statm_buf), 0);
    if (pos + 1 < sizeof(g_statm_buf)) g_statm_buf[pos++] = '\n';
    if (pos >= sizeof(g_statm_buf)) pos = sizeof(g_statm_buf) - 1;
    g_statm_buf[pos] = '\0';
}

static void auxv_regenerate(void) {
    /* Build a minimal ELF auxv for /proc/self/auxv.
     * Format: pairs of uint64_t {type, value}, AT_NULL terminated.
     * JSC/Bun reads this for CPU feature detection during init. */
    uint32_t off = 0;
#define EMIT_UX(t, v) do { \
    uint64_t _t = (uint64_t)(t);                     \
    uint64_t _v = (uint64_t)(v);                     \
    if (off + 16 > sizeof(g_auxv_buf)) break;        \
    g_auxv_buf[off+0] = (uint8_t)(_t);               \
    g_auxv_buf[off+1] = (uint8_t)(_t>>8);            \
    g_auxv_buf[off+2] = (uint8_t)(_t>>16);           \
    g_auxv_buf[off+3] = (uint8_t)(_t>>24);           \
    g_auxv_buf[off+4] = (uint8_t)(_t>>32);           \
    g_auxv_buf[off+5] = (uint8_t)(_t>>40);           \
    g_auxv_buf[off+6] = (uint8_t)(_t>>48);           \
    g_auxv_buf[off+7] = (uint8_t)(_t>>56);           \
    g_auxv_buf[off+8] = (uint8_t)(_v);               \
    g_auxv_buf[off+9] = (uint8_t)(_v>>8);            \
    g_auxv_buf[off+10] = (uint8_t)(_v>>16);          \
    g_auxv_buf[off+11] = (uint8_t)(_v>>24);          \
    g_auxv_buf[off+12] = (uint8_t)(_v>>32);          \
    g_auxv_buf[off+13] = (uint8_t)(_v>>40);          \
    g_auxv_buf[off+14] = (uint8_t)(_v>>48);          \
    g_auxv_buf[off+15] = (uint8_t)(_v>>56);          \
    off += 16;                                        \
} while(0)
    EMIT_UX(6, 4096);       /* AT_PAGESZ */
    EMIT_UX(16, 0x178bfbff); /* AT_HWCAP */
    EMIT_UX(26, 2);          /* AT_HWCAP2 */
    EMIT_UX(17, 100);        /* AT_CLKTCK */
    EMIT_UX(33, 0);          /* AT_SYSINFO_EHDR */
    EMIT_UX(3, 0x200040);    /* AT_PHDR — approximate, Bun's ELF phdr offset */
    EMIT_UX(5, 9);           /* AT_PHNUM */
    EMIT_UX(25, 0);          /* AT_RANDOM — no random seed for now */
#undef EMIT_UX
    g_auxv_len = off;
}

static void maps_regenerate(void) {
    struct fry_process *cur = proc_current();
    g_maps_len = 0;

    if (!cur || !cur->shared) {
        const char *fallback = "7fffff800000-800000000000 rw-p 00000000 00:00 0 "
                               "                         [stack]\n";
        for (uint32_t i = 0; fallback[i]; i++) g_maps_buf[i] = fallback[i];
        for (g_maps_len = 0; fallback[g_maps_len]; g_maps_len++);
        g_maps_buf[g_maps_len] = '\0';
        return;
    }

    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        const struct fry_vm_region *r = &cur->shared->vm_regions[i];
        if (!r->used) continue;
        if (r->base == 0 && r->length == 0) continue;
        if (g_maps_len + 96 > sizeof(g_maps_buf)) break;

        char tmp[96];
        int pos = 0;

        /* base hex */
        {
            uint64_t v = r->base;
            int started = 0;
            for (int shift = 60; shift >= 0; shift -= 4) {
                uint8_t nib = (uint8_t)((v >> shift) & 0xF);
                if (nib || started || shift == 0) {
                    tmp[pos++] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
                    started = 1;
                }
            }
        }
        tmp[pos++] = '-';
        {
            uint64_t v = r->base + r->length;
            int started = 0;
            for (int shift = 60; shift >= 0; shift -= 4) {
                uint8_t nib = (uint8_t)((v >> shift) & 0xF);
                if (nib || started || shift == 0) {
                    tmp[pos++] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
                    started = 1;
                }
            }
        }
        tmp[pos++] = ' ';
        tmp[pos++] = (r->prot & 0x01u) ? 'r' : '-';
        tmp[pos++] = (r->prot & 0x02u) ? 'w' : '-';
        tmp[pos++] = (r->prot & 0x04u) ? 'x' : '-';
        tmp[pos++] = ((r->flags & 0x01u) == 0) ? 'p' : 's';
        tmp[pos++] = ' ';
        /* offset: 00000000 */
        for (int j = 0; j < 8; j++) tmp[pos++] = '0';
        tmp[pos++] = ' ';
        tmp[pos++] = '0'; tmp[pos++] = '0';
        tmp[pos++] = ':';
        tmp[pos++] = '0'; tmp[pos++] = '0';
        tmp[pos++] = ' ';
        tmp[pos++] = '0';
        tmp[pos++] = ' ';
        while (pos < 73) tmp[pos++] = ' ';
        tmp[pos++] = '\n';
        tmp[pos] = '\0';

        uint32_t add = (uint32_t)pos;
        if (g_maps_len + add > sizeof(g_maps_buf) - 1) break;
        for (uint32_t j = 0; j < add; j++)
            g_maps_buf[g_maps_len + j] = tmp[j];
        g_maps_len += add;
    }

    {
        const char *stack = "7fffff800000-800000000000 rw-p 00000000 00:00 0 "
                            "                         [stack]\n";
        uint32_t sl = 0;
        for (; stack[sl]; sl++);
        if (g_maps_len + sl <= sizeof(g_maps_buf)) {
            for (uint32_t j = 0; j < sl; j++)
                g_maps_buf[g_maps_len + j] = stack[j];
            g_maps_len += sl;
        }
    }
    g_maps_buf[g_maps_len] = '\0';
}

static const char *node_text(enum tbridgefs_node node) {
    switch (node) {
    case TBRIDGEFS_NODE_OVERCOMMIT:
        return "1\n";
    case TBRIDGEFS_NODE_CGROUP:
        return "0::/\n";
    case TBRIDGEFS_NODE_THP_ENABLED:
        return "always [madvise] never\n";
    case TBRIDGEFS_NODE_TRACE_MARKER:
        return "";
    case TBRIDGEFS_NODE_MAPS:
        maps_regenerate();
        return g_maps_buf;
    case TBRIDGEFS_NODE_MMAP_MIN_ADDR:
        return "65536\n";
    case TBRIDGEFS_NODE_PROC_STAT:
        proc_stat_regenerate();
        return g_proc_stat_buf;
    case TBRIDGEFS_NODE_CPU_ONLINE:
        cpu_online_regenerate();
        return g_cpu_online_buf;
    case TBRIDGEFS_NODE_CGROUP_CPU_MAX:
        return "max 100000\n";
    case TBRIDGEFS_NODE_CGROUP_MEMORY_MAX:
    case TBRIDGEFS_NODE_CGROUP_MEMORY_HIGH:
        return "max\n";
    case TBRIDGEFS_NODE_ZONEINFO_UTC:
        return "UTC0\n";
    case TBRIDGEFS_NODE_PROC_EXE:
        return "/nvme/RCLAUDE.LXE\n";
    case TBRIDGEFS_NODE_PROC_STATUS:
        status_regenerate();
        return g_status_buf;
    case TBRIDGEFS_NODE_PROC_VERSION:
        return "TaterTOS64v3 (taterbox) 1.0.0 #1 SMP x86_64\n";
    case TBRIDGEFS_NODE_PROC_CMDLINE:
        return "claude\0-p\0say\0hello\0from\0TaterTOS\0in\010\0words\0";
    case TBRIDGEFS_NODE_PROC_STATM:
        statm_regenerate();
        return g_statm_buf;
    case TBRIDGEFS_NODE_PROC_MEM:
        return "";  /* openable, returns 0 bytes — Bun just needs the fd */
    default:
        return 0;
    }
}

static enum tbridgefs_node lookup_node(void *fs_data, const char *path) {
    uintptr_t kind = (uintptr_t)fs_data;
    if (kind == TBRIDGEFS_PROC) return lookup_proc(path);
    if (kind == TBRIDGEFS_SYS) return lookup_sys(path);
    if (kind == TBRIDGEFS_USR) return lookup_usr(path);
    return TBRIDGEFS_NODE_NONE;
}

static uint32_t file_pos(const struct vfs_file *f) {
    return ((uint32_t)f->private[4]) |
           ((uint32_t)f->private[5] << 8) |
           ((uint32_t)f->private[6] << 16) |
           ((uint32_t)f->private[7] << 24);
}

static void set_file_pos(struct vfs_file *f, uint32_t pos) {
    f->private[4] = (uint8_t)(pos & 0xffu);
    f->private[5] = (uint8_t)((pos >> 8) & 0xffu);
    f->private[6] = (uint8_t)((pos >> 16) & 0xffu);
    f->private[7] = (uint8_t)((pos >> 24) & 0xffu);
}

static int tbridgefs_open(void *fs_data, const char *path, struct vfs_file *out) {
    enum tbridgefs_node node;
    const char *text;
    if (!out) return -EINVAL;
    node = lookup_node(fs_data, path);
    if (node == TBRIDGEFS_NODE_NONE) return -ENOENT;
    if (node == TBRIDGEFS_NODE_DIR) return -EISDIR;
    if (node == TBRIDGEFS_NODE_PROC_AUXV) {
        auxv_regenerate();
        out->private[0] = (uint8_t)node;
        set_file_pos(out, 0);
        out->size = g_auxv_len;
        return 0;
    }
    text = node_text(node);
    if (!text) return -ENOENT;
    out->private[0] = (uint8_t)node;
    set_file_pos(out, 0);
    out->size = slen(text);
    return 0;
}

static int tbridgefs_read(struct vfs_file *f, void *buf, uint32_t len) {
    enum tbridgefs_node node;
    const void *data;
    uint32_t size;
    uint32_t pos;
    uint32_t n;
    uint8_t *dst = (uint8_t *)buf;
    if (!f || !buf) return -EINVAL;
    if (len == 0) return 0;
    node = (enum tbridgefs_node)f->private[0];
    if (node == TBRIDGEFS_NODE_PROC_AUXV) {
        auxv_regenerate();
        data = g_auxv_buf;
        size = g_auxv_len;
    } else {
        data = node_text(node);
        if (!data) return -EBADF;
        size = slen((const char *)data);
    }
    pos = file_pos(f);
    if (pos >= size) return 0;
    n = size - pos;
    if (n > len) n = len;
    for (uint32_t i = 0; i < n; i++) dst[i] = ((const uint8_t *)data)[pos + i];
    set_file_pos(f, pos + n);
    return (int)n;
}

static int tbridgefs_write(struct vfs_file *f, const void *buf, uint32_t len) {
    enum tbridgefs_node node;
    (void)buf;
    if (!f) return -EINVAL;
    node = (enum tbridgefs_node)f->private[0];
    if (node == TBRIDGEFS_NODE_TRACE_MARKER) return (int)len;
    return -EBADF;
}

static int tbridgefs_close(struct vfs_file *f) {
    (void)f;
    return 0;
}

static int add_dirent(const char *name, uint64_t size, uint32_t attr,
                      int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                      void *ctx) {
    return cb ? cb(name, size, attr, ctx) : -EINVAL;
}

static int proc_readdir(void *fs_data, const char *path,
                        int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                        void *ctx) {
    (void)fs_data;
    if (!cb) return -EINVAL;
    if (!path || path[0] == 0) {
        if (add_dirent("self", 0, 0x10u, cb, ctx)) return 1;
        if (add_dirent("sys", 0, 0x10u, cb, ctx)) return 1;
        if (add_dirent("version", slen(node_text(TBRIDGEFS_NODE_PROC_VERSION)), 0, cb, ctx)) return 1;
        return add_dirent("stat", slen(node_text(TBRIDGEFS_NODE_PROC_STAT)), 0, cb, ctx);
    }
    if (streq(path, "self")) {
        if (add_dirent("cgroup", slen(node_text(TBRIDGEFS_NODE_CGROUP)), 0, cb, ctx)) return 1;
        if (add_dirent("maps", 0, 0, cb, ctx)) return 1;
        if (add_dirent("exe", 0, 0, cb, ctx)) return 1;
        if (add_dirent("auxv", 0, 0, cb, ctx)) return 1;
        if (add_dirent("status", slen(node_text(TBRIDGEFS_NODE_PROC_STATUS)), 0, cb, ctx)) return 1;
        if (add_dirent("cmdline", 0, 0, cb, ctx)) return 1;
        return add_dirent("statm", slen(node_text(TBRIDGEFS_NODE_PROC_STATM)), 0, cb, ctx);
    }
    if (streq(path, "sys"))
        return add_dirent("vm", 0, 0x10u, cb, ctx);
    if (streq(path, "sys/vm")) {
        if (add_dirent("overcommit_memory", slen(node_text(TBRIDGEFS_NODE_OVERCOMMIT)), 0, cb, ctx)) return 1;
        return add_dirent("mmap_min_addr", slen(node_text(TBRIDGEFS_NODE_MMAP_MIN_ADDR)), 0, cb, ctx);
    }
    return -ENOTDIR;
}

static int sys_readdir(void *fs_data, const char *path,
                       int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                       void *ctx) {
    (void)fs_data;
    if (!cb) return -EINVAL;
    if (!path || path[0] == 0) {
        if (add_dirent("kernel", 0, 0x10u, cb, ctx)) return 1;
        if (add_dirent("devices", 0, 0x10u, cb, ctx)) return 1;
        return add_dirent("fs", 0, 0x10u, cb, ctx);
    }
    if (streq(path, "kernel")) {
        if (add_dirent("debug", 0, 0x10u, cb, ctx)) return 1;
        return add_dirent("mm", 0, 0x10u, cb, ctx);
    }
    if (streq(path, "kernel/mm"))
        return add_dirent("transparent_hugepage", 0, 0x10u, cb, ctx);
    if (streq(path, "kernel/mm/transparent_hugepage"))
        return add_dirent("enabled", slen(node_text(TBRIDGEFS_NODE_THP_ENABLED)), 0, cb, ctx);
    if (streq(path, "kernel/debug"))
        return add_dirent("tracing", 0, 0x10u, cb, ctx);
    if (streq(path, "kernel/debug/tracing"))
        return add_dirent("trace_marker", 0, 0, cb, ctx);
    if (streq(path, "devices"))
        return add_dirent("system", 0, 0x10u, cb, ctx);
    if (streq(path, "devices/system"))
        return add_dirent("cpu", 0, 0x10u, cb, ctx);
    if (streq(path, "devices/system/cpu"))
        return add_dirent("online", slen(node_text(TBRIDGEFS_NODE_CPU_ONLINE)), 0, cb, ctx);
    if (streq(path, "fs"))
        return add_dirent("cgroup", 0, 0x10u, cb, ctx);
    if (streq(path, "fs/cgroup")) {
        if (add_dirent("cpu.max", slen(node_text(TBRIDGEFS_NODE_CGROUP_CPU_MAX)), 0, cb, ctx)) return 1;
        if (add_dirent("memory.max", slen(node_text(TBRIDGEFS_NODE_CGROUP_MEMORY_MAX)), 0, cb, ctx)) return 1;
        return add_dirent("memory.high", slen(node_text(TBRIDGEFS_NODE_CGROUP_MEMORY_HIGH)), 0, cb, ctx);
    }
    return -ENOTDIR;
}

static int usr_readdir(void *fs_data, const char *path,
                       int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                       void *ctx) {
    (void)fs_data;
    if (!cb) return -EINVAL;
    if (!path || path[0] == 0)
        return add_dirent("UTC", slen(node_text(TBRIDGEFS_NODE_ZONEINFO_UTC)), 0, cb, ctx);
    return -ENOTDIR;
}

static int tbridgefs_stat(void *fs_data, const char *path, struct vfs_stat *out) {
    enum tbridgefs_node node;
    const char *text;
    if (!out) return -EINVAL;
    node = lookup_node(fs_data, path);
    if (node == TBRIDGEFS_NODE_NONE) return -ENOENT;
    if (node == TBRIDGEFS_NODE_DIR) {
        out->size = 0;
        out->attr = 0x10u;
        return 0;
    }
    text = node_text(node);
    out->size = slen(text);
    out->attr = 0;
    return 0;
}

static int64_t tbridgefs_seek(struct vfs_file *f, int64_t offset, int whence) {
    int64_t base;
    int64_t next;
    if (!f) return -EINVAL;
    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = (int64_t)file_pos(f);
    } else if (whence == 2) {
        base = (int64_t)f->size;
    } else {
        return -EINVAL;
    }
    next = base + offset;
    if (next < 0) return -EINVAL;
    if ((uint64_t)next > 0xffffffffULL) return -EINVAL;
    set_file_pos(f, (uint32_t)next);
    return next;
}

static void init_ops(struct fs_ops *ops,
                     int (*readdir)(void *, const char *,
                                    int (*)(const char *, uint64_t, uint32_t, void *),
                                    void *)) {
    ops->open = tbridgefs_open;
    ops->read = tbridgefs_read;
    ops->write = tbridgefs_write;
    ops->close = tbridgefs_close;
    ops->readdir = readdir;
    ops->stat = tbridgefs_stat;
    ops->create = 0;
    ops->mkdir = 0;
    ops->unlink = 0;
    ops->seek = tbridgefs_seek;
    ops->truncate = 0;
    ops->rename = 0;
}

int tbridgefs_mount_proc(void) {
    init_ops(&procfs_ops, proc_readdir);
    return vfs_mount("/proc", &procfs_ops, (void *)(uintptr_t)TBRIDGEFS_PROC);
}

int tbridgefs_mount_sys(void) {
    init_ops(&sysfs_ops, sys_readdir);
    return vfs_mount("/sys", &sysfs_ops, (void *)(uintptr_t)TBRIDGEFS_SYS);
}

int tbridgefs_mount_usr(void) {
    init_ops(&usrfs_ops, usr_readdir);
    return vfs_mount("/usr/share/zoneinfo", &usrfs_ops, (void *)(uintptr_t)TBRIDGEFS_USR);
}
