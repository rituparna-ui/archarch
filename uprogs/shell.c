/*
 * shell — interactive command interpreter.
 *
 * Built-in commands:
 *   help     — show available commands
 *   ls       — list files on disk
 *   run NAME — load and run a program, wait for it to finish
 *   ps       — show current PID
 *   exit     — exit the shell
 *
 * Any other input is treated as a program name to run.
 */
#include "usys.h"

static char line[128];
static char dirbuf[1024];

static void print_banner(void) {
    print("\n");
    print("  =============================\n");
    print("  |   VirtIO-OS Shell v1.0    |\n");
    print("  =============================\n");
    print("\n");
    print("Type 'help' for commands, or a .bin filename to run it.\n\n");
}

static void cmd_help(void) {
    print("Commands:\n");
    print("  help       — show this message\n");
    print("  ls         — list files on disk\n");
    print("  run <file> — run a program and wait\n");
    print("  ps         — show shell PID\n");
    print("  exit       — exit the shell\n");
    print("  <file.bin> — shortcut for 'run file.bin'\n");
}

static void cmd_ls(void) {
    int64_t n = sys_listdir(dirbuf, sizeof(dirbuf));
    if (n > 0) {
        sys_write(dirbuf, (uint64_t)n);
    } else {
        print("(empty)\n");
    }
}

static void cmd_run(const char *name) {
    int64_t tid = sys_exec(name, strlen(name));
    if (tid < 0) {
        print("Error: cannot load '");
        print(name);
        print("'\n");
        return;
    }
    /* Wait for child to finish */
    sys_wait((int)tid);
}

static void cmd_ps(void) {
    print("Shell PID: ");
    char c = '0' + (char)sys_getpid();
    sys_write(&c, 1);
    print("\n");
}

/* Skip leading spaces */
static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

/* Check if line starts with prefix, return pointer after prefix+spaces */
static const char *starts_with(const char *line, const char *prefix) {
    while (*prefix) {
        if (*line != *prefix) return 0;
        line++;
        prefix++;
    }
    if (*line && *line != ' ') return 0;
    return skip_spaces(line);
}

int main(void) {
    print_banner();

    for (;;) {
        print("> ");
        int64_t n = sys_read(line, sizeof(line));
        if (n <= 0) continue;

        const char *cmd = skip_spaces(line);
        if (*cmd == '\0') continue;

        if (streq(cmd, "help")) {
            cmd_help();
        } else if (streq(cmd, "ls")) {
            cmd_ls();
        } else if (streq(cmd, "ps")) {
            cmd_ps();
        } else if (streq(cmd, "exit")) {
            print("Goodbye!\n");
            sys_exit(0);
        } else {
            const char *arg = starts_with(cmd, "run");
            if (arg && *arg) {
                cmd_run(arg);
            } else {
                /* Treat the whole line as a program name */
                cmd_run(cmd);
            }
        }
    }
}
