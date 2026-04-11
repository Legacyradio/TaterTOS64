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
