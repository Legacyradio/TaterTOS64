/*
 * selftest.c — Kernel boot-time self-test (Phase 0 ABI discipline)
 *
 * Validates core ABI invariants at boot before any userspace runs.
 * Called once from after_vmm() when TATER_SELFTEST is enabled.
 *
 * On failure: dumps the failing check name, expected/got values to serial,
 * then panics. On success: prints pass count to serial and continues boot.
 *
 * All output goes through early_serial_puts (bypasses kprint filter).
 */

#include <stdint.h>
#include <errno.h>
#include <fry_limits.h>
#include <fry_random.h>
#include <fry_time.h>
#include "proc/process.h"
#include "../boot/early_serial.h"
#include "entropy/entropy.h"
#include "../drivers/timer/rtc.h"
#include "../drivers/timer/hpet.h"
#include <fry_seek.h>
#include <fry_input.h>
#include "fs/vfs.h"

void kernel_panic(const char *msg);

static uint32_t st_pass;
static uint32_t st_fail;

static void st_put_dec(uint64_t v) {
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    if (v == 0) { early_serial_puts("0"); return; }
    while (v > 0) {
        buf[--pos] = '0' + (char)(v % 10);
        v /= 10;
    }
    early_serial_puts(&buf[pos]);
}

static void st_check(const char *name, uint64_t got, uint64_t want) {
    if (got == want) {
        st_pass++;
        return;
    }
    st_fail++;
    early_serial_puts("SELFTEST FAIL: ");
    early_serial_puts(name);
    early_serial_puts("  got=0x");
    early_serial_puthex64(got);
    early_serial_puts("  want=0x");
    early_serial_puthex64(want);
    early_serial_puts("\n");
    kernel_panic("kernel self-test failed");
}

void kernel_selftest(void) {
    st_pass = 0;
    st_fail = 0;
    early_serial_puts("selftest: running\n");

    /* errno.h values match Linux ABI numbering */
    st_check("EPERM",  EPERM,   1);
    st_check("ENOENT", ENOENT,  2);
    st_check("EBADF",  EBADF,   9);
    st_check("ENOMEM", ENOMEM, 12);
    st_check("EFAULT", EFAULT, 14);
    st_check("EINVAL", EINVAL, 22);
    st_check("ENOSYS", ENOSYS, 38);

    /* Struct/constant sizes */
    st_check("FRY_FD_MAX",        FRY_FD_MAX,        64);
    st_check("FRY_PROC_MAX",      FRY_PROC_MAX,      256);
    st_check("FRY_VMREG_MAX",     FRY_VMREG_MAX,     256);
    st_check("FRY_SHM_MAX",       FRY_SHM_MAX,       128);
    st_check("FRY_VM_SHARED_MAX", FRY_VM_SHARED_MAX, 128);
    st_check("FRY_PATH_MAX",      FRY_PATH_MAX,      128);
    st_check("PROC_MAX",          PROC_MAX,          FRY_PROC_MAX);
    st_check("PROC_OUTBUF",       PROC_OUTBUF,       512);
    st_check("PROC_INBUF",        PROC_INBUF,        512);
    st_check("PROC_VMREG_MAX",    PROC_VMREG_MAX,    FRY_VMREG_MAX);

    /* Negative errno encoding: -(errno) must produce correct uint64_t */
    st_check("-EINVAL", (uint64_t)-EINVAL, 0xFFFFFFFFFFFFFFEAULL);
    st_check("-EBADF",  (uint64_t)-EBADF,  0xFFFFFFFFFFFFFFF7ULL);

    /* Phase 3: new errno values */
    st_check("EPIPE",   EPIPE,   32);
    st_check("ESPIPE",  ESPIPE,  29);
    st_check("E2BIG",   E2BIG,    7);
    st_check("ECHILD",  ECHILD,  10);
    st_check("-EPIPE",  (uint64_t)-EPIPE,  0xFFFFFFFFFFFFFFE0ULL);

    /* Phase 3: new limits */
    st_check("FRY_PIPE_MAX",    FRY_PIPE_MAX,    128);
    st_check("FRY_PIPE_BUFSZ",  FRY_PIPE_BUFSZ,  4096);
    st_check("FRY_ARGV_MAX",    FRY_ARGV_MAX,     32);
    st_check("FRY_ENV_MAX",     FRY_ENV_MAX,      32);
    st_check("FRY_ARGS_BUFSZ",  FRY_ARGS_BUFSZ,  2048);
    st_check("FRY_POLL_MAX",    FRY_POLL_MAX,     64);

    /* Phase 3: fd_kind enum */
    st_check("FD_NONE",       FD_NONE,       0);
    st_check("FD_FILE",       FD_FILE,       1);
    st_check("FD_PIPE_READ",  FD_PIPE_READ,  2);
    st_check("FD_PIPE_WRITE", FD_PIPE_WRITE, 3);

    /* Phase 3: pipe pool is zeroed at boot */
    st_check("pipe0_unused", g_pipes[0].used, 0);

    /* Phase 4: new errno values */
    st_check("ENOTSOCK",     ENOTSOCK,      88);
    st_check("ECONNREFUSED", ECONNREFUSED, 111);
    st_check("ENOTCONN",     ENOTCONN,     107);
    st_check("EADDRINUSE",   EADDRINUSE,    98);
    st_check("EINPROGRESS",  EINPROGRESS,  115);

    /* Phase 4: new limits */
    st_check("FRY_SOCK_MAX",       FRY_SOCK_MAX,       16);
    st_check("FRY_SOCK_UDP_RXMAX", FRY_SOCK_UDP_RXMAX,  4);
    st_check("FRY_SOCK_UDP_PKTSZ", FRY_SOCK_UDP_PKTSZ, 512);

    /* Phase 4: fd_kind enum — socket kind */
    st_check("FD_SOCKET", FD_SOCKET, 4);

    /* Phase 4: socket pool is zeroed at boot */
    st_check("sock0_unused", g_sockets[0].used, 0);

    /* Phase 5: entropy subsystem */
    st_check("entropy_ready", entropy_ready(), 1);
    st_check("FRY_RANDOM_MAX", FRY_RANDOM_MAX, 256);
    st_check("FRY_GRND_NONBLOCK", FRY_GRND_NONBLOCK, 1);
    /* Verify getrandom produces bytes (non-zero check on 8 bytes) */
    {
        uint8_t rnd[8] = {0,0,0,0,0,0,0,0};
        int rc = entropy_getbytes(rnd, 8);
        st_check("entropy_getbytes_rc", (uint64_t)rc, 0);
        uint64_t sum = 0;
        for (int i = 0; i < 8; i++) sum += rnd[i];
        /* Probability of 8 random bytes all being zero: 1/2^64.  Safe to check. */
        st_check("entropy_nonzero", sum != 0, 1);
    }
    /* Verify two sequential draws differ */
    {
        uint64_t a = 0, b = 0;
        entropy_getbytes(&a, sizeof(a));
        entropy_getbytes(&b, sizeof(b));
        st_check("entropy_unique", a != b, 1);
    }

    /* Phase 5: /dev entropy and runtime device nodes */
    {
        struct vfs_file *urnd = vfs_open("/dev/urandom");
        st_check("devfs_urandom_open", urnd != 0, 1);
        if (urnd) {
            uint8_t rnd[8] = {0,0,0,0,0,0,0,0};
            int rd = vfs_read(urnd, rnd, sizeof(rnd));
            st_check("devfs_urandom_read", rd, sizeof(rnd));
            uint64_t sum = 0;
            for (int i = 0; i < 8; i++) sum += rnd[i];
            st_check("devfs_urandom_nonzero", sum != 0, 1);
            vfs_close(urnd);
        }
        struct vfs_file *zero = vfs_open("/dev/zero");
        st_check("devfs_zero_open", zero != 0, 1);
        if (zero) {
            uint8_t z[8] = {1,1,1,1,1,1,1,1};
            int rd = vfs_read(zero, z, sizeof(z));
            st_check("devfs_zero_read", rd, sizeof(z));
            uint64_t sum = 0;
            for (int i = 0; i < 8; i++) sum += z[i];
            st_check("devfs_zero_all_zero", sum, 0);
            vfs_close(zero);
        }
        struct vfs_file *nul = vfs_open("/dev/null");
        st_check("devfs_null_open", nul != 0, 1);
        if (nul) {
            uint8_t b = 0;
            st_check("devfs_null_read_eof", vfs_read(nul, &b, 1), 0);
            vfs_close(nul);
        }
    }

    /* Phase 5: Tater Bridge synthetic ABI files */
    {
        char buf[64];
        struct vfs_file *over = vfs_open("/proc/sys/vm/overcommit_memory");
        st_check("tbridgefs_overcommit_open", over != 0, 1);
        if (over) {
            int rd = vfs_read(over, buf, sizeof(buf));
            st_check("tbridgefs_overcommit_read", rd, 2);
            st_check("tbridgefs_overcommit_value", rd >= 2 && buf[0] == '1' && buf[1] == '\n', 1);
            vfs_close(over);
        }
    }

    {
        char buf[64];
        struct vfs_file *cg = vfs_open("/proc/self/cgroup");
        st_check("tbridgefs_cgroup_open", cg != 0, 1);
        if (cg) {
            int rd = vfs_read(cg, buf, sizeof(buf));
            st_check("tbridgefs_cgroup_read", rd, 5);
            st_check("tbridgefs_cgroup_value", rd >= 5 && buf[0] == '0' && buf[1] == ':' && buf[2] == ':' && buf[3] == '/' && buf[4] == '\n', 1);
            vfs_close(cg);
        }
    }

    {
        char buf[64];
        struct vfs_file *thp = vfs_open("/sys/kernel/mm/transparent_hugepage/enabled");
        st_check("tbridgefs_thp_open", thp != 0, 1);
        if (thp) {
            int rd = vfs_read(thp, buf, sizeof(buf));
            st_check("tbridgefs_thp_read_nonempty", rd > 0, 1);
            vfs_close(thp);
        }
    }

    {
        const char mark[] = "claude\n";
        struct vfs_file *tm = vfs_open("/sys/kernel/debug/tracing/trace_marker");
        st_check("tbridgefs_trace_marker_open", tm != 0, 1);
        if (tm) {
            st_check("tbridgefs_trace_marker_write", vfs_write(tm, mark, sizeof(mark) - 1), (int)(sizeof(mark) - 1));
            vfs_close(tm);
        }
    }

    {
        char buf[128];
        struct vfs_file *maps = vfs_open("/proc/self/maps");
        st_check("tbridgefs_maps_open", maps != 0, 1);
        if (maps) {
            /* maps now advertises the user [stack] VMA so glibc
             * pthread_getattr_np can resolve the main thread's stack
             * (empty maps -> WTF::Thread stack-origin 0 -> abort). */
            int n = vfs_read(maps, buf, sizeof(buf) - 1);
            st_check("tbridgefs_maps_read_nonempty", n > 0 ? 1 : 0, 1);
            int has_stack = 0;
            if (n > 0) {
                buf[n] = 0;
                for (int i = 0; i + 7 <= n; i++) {
                    if (buf[i] == '[' && buf[i+1] == 's' && buf[i+2] == 't' &&
                        buf[i+3] == 'a' && buf[i+4] == 'c' && buf[i+5] == 'k' &&
                        buf[i+6] == ']') { has_stack = 1; break; }
                }
            }
            st_check("tbridgefs_maps_has_stack", has_stack, 1);
            vfs_close(maps);
        }
    }

    {
        char buf[64];
        struct vfs_file *mma = vfs_open("/proc/sys/vm/mmap_min_addr");
        st_check("tbridgefs_mmap_min_open", mma != 0, 1);
        if (mma) {
            int rd = vfs_read(mma, buf, sizeof(buf));
            st_check("tbridgefs_mmap_min_read", rd > 0, 1);
            vfs_close(mma);
        }
    }

    {
        char buf[64];
        struct vfs_file *cpu = vfs_open("/sys/devices/system/cpu/online");
        st_check("tbridgefs_cpu_online_open", cpu != 0, 1);
        if (cpu) {
            int rd = vfs_read(cpu, buf, sizeof(buf));
            st_check("tbridgefs_cpu_online_read", rd > 0, 1);
            vfs_close(cpu);
        }
    }

    {
        char buf[128];
        struct vfs_file *ps = vfs_open("/proc/stat");
        st_check("tbridgefs_proc_stat_open", ps != 0, 1);
        if (ps) {
            int rd = vfs_read(ps, buf, sizeof(buf));
            st_check("tbridgefs_proc_stat_read", rd > 0, 1);
            vfs_close(ps);
        }
    }

    {
        char buf[64];
        struct vfs_file *lim = vfs_open("/sys/fs/cgroup/memory.max");
        st_check("tbridgefs_cgroup_memory_max_open", lim != 0, 1);
        if (lim) {
            int rd = vfs_read(lim, buf, sizeof(buf));
            st_check("tbridgefs_cgroup_memory_max_read", rd > 0, 1);
            vfs_close(lim);
        }
    }

    /* Phase 5: time constants */
    st_check("CLOCK_MONOTONIC", FRY_CLOCK_MONOTONIC, 0);
    st_check("CLOCK_REALTIME",  FRY_CLOCK_REALTIME,  1);
    st_check("CLOCK_BOOTTIME",  FRY_CLOCK_BOOTTIME,  2);

    /* Phase 5: HPET nanosecond API */
    {
        int64_t sec = 0, nsec = 0;
        hpet_get_ns(&sec, &nsec);
        /* Boot just happened; sec should be small (< 300) and nsec in range */
        st_check("hpet_ns_sec_ok", sec < 300, 1);
        st_check("hpet_ns_nsec_range", nsec >= 0 && nsec < 1000000000LL, 1);
    }

    /* Phase 5: RTC sanity (year should be >= 2024) */
    {
        int64_t epoch = rtc_boot_epoch_sec();
        /* epoch for 2024-01-01 = 1704067200.  QEMU RTC defaults to ~current date. */
        st_check("rtc_epoch_sane", epoch > 1704067200LL, 1);
    }

    /* Phase 6: seek constants */
    st_check("SEEK_SET", FRY_SEEK_SET, 0);
    st_check("SEEK_CUR", FRY_SEEK_CUR, 1);
    st_check("SEEK_END", FRY_SEEK_END, 2);

    /* Phase 6: fs_ops struct has seek/truncate/rename pointers
       (compile-time check — offset is non-zero if field exists) */
    {
        struct fs_ops dummy;
        for (uint32_t i = 0; i < sizeof(dummy); i++) ((uint8_t *)&dummy)[i] = 0;
        st_check("fs_ops_seek_null",     (uint64_t)(uintptr_t)dummy.seek,     0);
        st_check("fs_ops_truncate_null",  (uint64_t)(uintptr_t)dummy.truncate, 0);
        st_check("fs_ops_rename_null",    (uint64_t)(uintptr_t)dummy.rename,   0);
    }

    /* Phase 6: vfs_stat struct layout */
    st_check("vfs_stat_size", sizeof(struct vfs_stat) >= 12, 1);

    /* Phase 7: input event constants */
    st_check("FRY_MOD_LSHIFT",  FRY_MOD_LSHIFT,  0x01);
    st_check("FRY_MOD_RSHIFT",  FRY_MOD_RSHIFT,  0x02);
    st_check("FRY_MOD_LCTRL",   FRY_MOD_LCTRL,   0x04);
    st_check("FRY_MOD_RCTRL",   FRY_MOD_RCTRL,   0x08);
    st_check("FRY_MOD_LALT",    FRY_MOD_LALT,    0x10);
    st_check("FRY_MOD_RALT",    FRY_MOD_RALT,    0x20);
    st_check("FRY_MOD_CAPSLOCK",FRY_MOD_CAPSLOCK, 0x40);
    st_check("FRY_KEY_PRESSED",  FRY_KEY_PRESSED,  0x00);
    st_check("FRY_KEY_RELEASED", FRY_KEY_RELEASED, 0x01);
    st_check("FRY_VK_ESCAPE",   FRY_VK_ESCAPE,   0x100);
    st_check("FRY_VK_UP",       FRY_VK_UP,       0x110);
    st_check("FRY_CLIPBOARD_MAX", FRY_CLIPBOARD_MAX, 4096);

    /* Phase 7: fry_key_event struct size (should be 8 bytes) */
    st_check("key_event_size", sizeof(struct fry_key_event), 8);

    early_serial_puts("selftest: ");
    st_put_dec(st_pass);
    early_serial_puts(" passed, 0 failed\n");
}
