/*
 * shell.c — TaterTOS System Shell
 */

#include "libc.h"
#include "fry.h"

/* Standard headers for POSIX types */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

static char cwd[256] = "/";

/* -----------------------------------------------------------------------
 * Shell Command Implementations
 * ----------------------------------------------------------------------- */

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("TaterTOS Shell Commands:");
    puts("  ls [path]      List directory");
    puts("  cd <path>      Change directory");
    puts("  cat <file>     Display file content");
    puts("  pwd            Show current directory");
    puts("  echo [str]     Echo text");
    puts("  ps             List processes");
    puts("  kill <pid>     Terminate process");
    puts("  mkdir <path>   Create directory");
    puts("  rm <path>      Delete file");
    puts("  free           Show memory info");
    puts("  df             Show disk info");
    puts("  ifconfig       Show network info");
    puts("  ping <ip>      Ping host");
    puts("  reboot         Restart system");
    puts("  shutdown       Power off");
    puts("  help           Show this help");
    puts("  exit           Exit shell");
}

static void cmd_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : cwd;
    char buf[4096];
    long n = fry_readdir(path, buf, sizeof(buf));
    if (n < 0) {
        printf("ls: %s: error %ld\n", path, -n);
        return;
    }

    struct fry_dirent *de = (struct fry_dirent *)buf;
    while ((char *)de < buf + n) {
        printf("%c %10lu  %s\n", (de->attr & 0x1) ? 'd' : '-', (unsigned long)de->size, de->name);
        de = (struct fry_dirent *)((char *)de + de->rec_len);
    }
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) return;
    if (fry_chdir(argv[1]) < 0) {
        printf("cd: %s: no such directory\n", argv[1]);
        return;
    }
    /* Refresh local CWD from kernel — far more reliable than local path join */
    char gcwd[256];
    if (fry_getcwd(gcwd, sizeof(gcwd)) >= 0) {
        strncpy(cwd, gcwd, sizeof(cwd) - 1);
        cwd[sizeof(cwd) - 1] = 0;
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) return;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("cat: %s: cannot open\n", argv[1]);
        return;
    }

    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, n);
    }
    close(fd);
    putchar('\n');
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    long count = fry_proc_count();
    printf("Active processes: %ld\n", count);
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) return;
    int pid = atoi(argv[1]);
    if (kill(pid, 15) < 0) {
        printf("kill: %d: failed\n", pid);
    }
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) return;
    if (mkdir(argv[1], 0755) < 0) {
        printf("mkdir: %s: failed\n", argv[1]);
    }
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) return;
    if (unlink(argv[1]) < 0) {
        printf("rm: %s: failed\n", argv[1]);
    }
}

static void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    struct fry_storage_info info;
    if (fry_storage_info(&info) >= 0) {
        printf("Storage: %s (%lu sectors)\n", info.nvme_detected ? "NVMe" : "RAM", (unsigned long)info.total_sectors);
        printf("Root: %s\n", info.root_mount);
    }
}

static void cmd_ifconfig(int argc, char **argv) {
    (void)argc; (void)argv;
    struct fry_wifi_status ws;
    if (fry_wifi_status(&ws) >= 0) {
        printf("WiFi: %s (IP: %u.%u.%u.%u)\n", ws.connected ? "Connected" : "Disconnected",
               (ws.ip >> 24) & 0xFF, (ws.ip >> 16) & 0xFF, (ws.ip >> 8) & 0xFF, ws.ip & 0xFF);
    }
}

static void cmd_ping(int argc, char **argv) {
    if (argc < 2) return;
    printf("PING %s...\n", argv[1]);
    /* simplified ping logic */
}

static void cmd_acpi(int argc, char **argv) {
    (void)argc; (void)argv;
    struct fry_acpi_diag d;
    if (fry_acpi_diag(&d) >= 0) {
        printf("ACPI: %u nodes, EC: %s, Batt: %u, BL: %u\n",
               d.ns_nodes, d.ec_ok ? "OK" : "FAIL", d.batt_count, d.bl_found);
    }
}

/* -----------------------------------------------------------------------
 * Main loop
 * ----------------------------------------------------------------------- */

static void execute_command(int argc, char **argv) {
    if (argc == 0) return;
    if (strcmp(argv[0], "help") == 0) cmd_help(argc, argv);
    else if (strcmp(argv[0], "ls") == 0) cmd_ls(argc, argv);
    else if (strcmp(argv[0], "cd") == 0) cmd_cd(argc, argv);
    else if (strcmp(argv[0], "cat") == 0) cmd_cat(argc, argv);
    else if (strcmp(argv[0], "pwd") == 0) puts(cwd);
    else if (strcmp(argv[0], "ps") == 0) cmd_ps(argc, argv);
    else if (strcmp(argv[0], "kill") == 0) cmd_kill(argc, argv);
    else if (strcmp(argv[0], "mkdir") == 0) cmd_mkdir(argc, argv);
    else if (strcmp(argv[0], "rm") == 0) cmd_rm(argc, argv);
    else if (strcmp(argv[0], "df") == 0) cmd_df(argc, argv);
    else if (strcmp(argv[0], "ifconfig") == 0) cmd_ifconfig(argc, argv);
    else if (strcmp(argv[0], "ping") == 0) cmd_ping(argc, argv);
    else if (strcmp(argv[0], "acpi") == 0) cmd_acpi(argc, argv);
    else if (strcmp(argv[0], "reboot") == 0) fry_reboot();
    else if (strcmp(argv[0], "shutdown") == 0) fry_shutdown();
    else if (strcmp(argv[0], "exit") == 0) fry_exit(0);
    else {
        /* try to spawn external binary */
        if (fry_spawn(argv[0]) < 0) {
            printf("shell: %s: command not found\n", argv[0]);
        }
    }
}

int main(void) {
    char line[MAX_LINE];
    char *argv[MAX_ARGS];

    puts("\nTaterTOS64v3 Shell");
    puts("Type 'help' for commands.\n");

    while (1) {
        printf("%s> ", cwd);
        if (!gets_bounded(line, MAX_LINE)) break;

        /* simple tokenization */
        int argc = 0;
        char *p = line;
        while (*p && argc < MAX_ARGS) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        execute_command(argc, argv);
    }

    return 0;
}
