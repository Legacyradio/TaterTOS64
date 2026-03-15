// TaterTOS64v3 init — canonical PID 1 for live/runtime bootstrapping

#include "../libc/libc.h"

static long try_spawn_first(const char *const *paths, uint32_t count, const char **chosen) {
    for (uint32_t i = 0; i < count; i++) {
        long pid = fry_spawn(paths[i]);
        if (pid >= 0) {
            if (chosen) *chosen = paths[i];
            return pid;
        }
    }
    if (chosen) *chosen = count ? paths[count - 1] : 0;
    return -1;
}

static int path_exists(const char *path) {
    long fd = fry_open(path, 0);
    if (fd < 0) return 0;
    fry_close((int)fd);
    return 1;
}

static void maybe_run_autotest(const char *name, const char *trigger_path,
                               const char *const *paths, uint32_t count) {
    if (!path_exists(trigger_path)) return;
    const char *path = 0;
    long pid = try_spawn_first(paths, count, &path);
    if (pid < 0) {
        printf("init: autotest %s requested but unavailable path=%s\n",
               name, path ? path : "(null)");
        return;
    }
    printf("init: autotest %s path=%s pid=%ld\n", name, path ? path : "(null)", pid);
    fry_wait((uint32_t)pid);
    printf("init: autotest %s completed pid=%ld\n", name, pid);
}

int main(void) {
    static const char *vmtest_paths[] = {
        "/apps/VMTEST.FRY",
        "/VMTEST.FRY",
        "/fry/VMTEST.FRY",
        "/FRY/VMTEST.FRY",
        "/EFI/fry/VMTEST.FRY",
        "/EFI/FRY/VMTEST.FRY",
        "/EFI/BOOT/VMTEST.FRY"
    };
    static const char *vmfault_paths[] = {
        "/apps/VMFAULT.FRY",
        "/VMFAULT.FRY",
        "/fry/VMFAULT.FRY",
        "/FRY/VMFAULT.FRY",
        "/EFI/fry/VMFAULT.FRY",
        "/EFI/FRY/VMFAULT.FRY",
        "/EFI/BOOT/VMFAULT.FRY"
    };
    static const char *gui_paths[] = {
        "/system/GUI.FRY",
        "/GUI.FRY",
        "/fry/GUI.FRY",
        "/FRY/GUI.FRY",
        "/EFI/fry/GUI.FRY",
        "/EFI/FRY/GUI.FRY",
        "/EFI/BOOT/GUI.FRY"
    };
    static const char *shell_paths[] = {
        "/apps/SHELL.TOT",
        "/SHELL.TOT",
        "/fry/SHELL.TOT",
        "/FRY/SHELL.TOT",
        "/EFI/fry/SHELL.TOT",
        "/EFI/FRY/SHELL.TOT",
        "/EFI/BOOT/SHELL.TOT"
    };
    int gui_failures = 0;
    printf("init: pid=%ld\n", fry_getpid());
    maybe_run_autotest("vmtest", "/VMTEST.RUN",
                       vmtest_paths, (uint32_t)(sizeof(vmtest_paths) / sizeof(vmtest_paths[0])));
    maybe_run_autotest("vmfault", "/VMFAULT.RUN",
                       vmfault_paths, (uint32_t)(sizeof(vmfault_paths) / sizeof(vmfault_paths[0])));
    for (;;) {
        const char *path = 0;
        long pid = try_spawn_first(gui_paths,
                                   (uint32_t)(sizeof(gui_paths) / sizeof(gui_paths[0])),
                                   &path);
        if (pid >= 0) {
            printf("init: launched gui path=%s pid=%ld\n", path ? path : "(null)", pid);
            fry_wait((uint32_t)pid);
            printf("init: gui exited pid=%ld\n", pid);
            gui_failures++;
            if (gui_failures < 3) {
                fry_sleep(250);
                continue;
            }
        } else {
            gui_failures++;
            printf("init: gui launch unavailable path=%s\n", path ? path : "/system/GUI.FRY");
        }

        path = 0;
        pid = try_spawn_first(shell_paths,
                              (uint32_t)(sizeof(shell_paths) / sizeof(shell_paths[0])),
                              &path);
        if (pid >= 0) {
            printf("init: launched shell path=%s pid=%ld\n", path ? path : "(null)", pid);
            fry_wait((uint32_t)pid);
            printf("init: shell exited pid=%ld\n", pid);
            gui_failures = 0;
            fry_sleep(250);
            continue;
        }

        printf("init: no usable userspace targets; sleeping\n");
        fry_sleep(1000);
    }
}
