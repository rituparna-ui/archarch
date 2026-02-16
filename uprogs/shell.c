/*
 * shell — interactive command interpreter with file descriptor support.
 *
 * Built-in commands:
 *   help      — show available commands
 *   ls        — list files on disk
 *   cat FILE  — print file contents
 *   run NAME  — load and run a program, wait for it to finish
 *   ps        — show current PID
 *   exit      — exit the shell
 *
 * Any other input is treated as a program name to run.
 */
#include "usys.h"

static char line[128];
static char dirbuf[1024];
static char filebuf[4096];

static void print_banner(void) {
    print("\n");
    print("  =============================\n");
    print("  |   VirtIO-OS Shell v2.0    |\n");
    print("  |   (now with fd support)   |\n");
    print("  =============================\n");
    print("\n");
    print("Type 'help' for commands.\n\n");
}

static void cmd_help(void) {
    print("Commands:\n");
    print("  help       — show this message\n");
    print("  ls         — list files on disk\n");
    print("  cat <file> — print file contents\n");
    print("  run <file> — run a program and wait\n");
    print("  ps         — show shell PID\n");
    print("  exit       — exit the shell\n");
    print("  <file.bin> — shortcut for 'run file.bin'\n");
}

static void cmd_ls(void) {
    int64_t n = listdir(dirbuf, sizeof(dirbuf));
    if (n > 0)
        write(STDOUT, dirbuf, (uint64_t)n);
    else
        print("(empty)\n");
}

static void cmd_cat(const char *filename) {
    int64_t fd = open(filename, O_RDONLY);
    if (fd < 0) {
        print("cat: cannot open '");
        print(filename);
        print("'\n");
        return;
    }

    int64_t n;
    while ((n = read((int)fd, filebuf, sizeof(filebuf) - 1)) > 0) {
        write(STDOUT, filebuf, (uint64_t)n);
    }

    close((int)fd);
}

static void cmd_run(const char *name) {
    int64_t tid = exec(name, strlen(name));
    if (tid < 0) {
        print("Error: cannot load '");
        print(name);
        print("'\n");
        return;
    }
    wait((int)tid);
}

static void cmd_ps(void) {
    print("Shell PID: ");
    char c = '0' + (char)getpid();
    write(STDOUT, &c, 1);
    print("\n");
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static const char *starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    if (*s && *s != ' ') return 0;
    return skip_spaces(s);
}

/* Read a line from stdin (strips trailing newline) */
static int64_t readline(char *buf, uint64_t max) {
    int64_t n = read(STDIN, buf, max);
    if (n > 0 && buf[n-1] == '\n') {
        buf[n-1] = '\0';
        n--;
    }
    return n;
}

int main(void) {
    print_banner();

    for (;;) {
        print("> ");
        int64_t n = readline(line, sizeof(line));
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
            exit(0);
        } else {
            const char *arg;
            if ((arg = starts_with(cmd, "cat")) && *arg) {
                cmd_cat(arg);
            } else if ((arg = starts_with(cmd, "run")) && *arg) {
                cmd_run(arg);
            } else {
                cmd_run(cmd);
            }
        }
    }
}
