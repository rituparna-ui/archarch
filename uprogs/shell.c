/*
 * shell — interactive command interpreter with pipe support.
 *
 * Supports:
 *   cmd1 | cmd2 | cmd3    — pipe chains
 *   Built-in commands: help, ls, cat, echo, wc, ps, exit
 *   External programs: hello.bin, fib.bin, etc.
 *
 * Pipe implementation:
 *   For "cmd1 | cmd2", the shell forks for each command,
 *   connects them with pipes via dup2, and waits for all to finish.
 */
#include "usys.h"

static char line[256];
static char dirbuf[1024];
static char filebuf[4096];

/* ---- String helpers ---- */

static int memcmp(const void *a, const void *b, uint64_t n) {
    const char *p = a, *q = b;
    for (uint64_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}

static int strncmp(const char *a, const char *b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static void print_num(int64_t n) {
    if (n == 0) { print("0"); return; }
    if (n < 0) { print("-"); n = -n; }
    char buf[20]; int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) write(STDOUT, &buf[i], 1);
}

/* ---- Built-in command implementations ---- */
/* These write to STDOUT so they work with pipes */

static void builtin_help(void) {
    print("Commands:\n");
    print("  help          — show this message\n");
    print("  ls            — list files on disk\n");
    print("  cat <file>    — print file contents\n");
    print("  echo <text>   — print text\n");
    print("  wc            — count lines/words/bytes from stdin\n");
    print("  ps            — show shell PID\n");
    print("  exit          — exit the shell\n");
    print("  <file.bin>    — run a program\n");
    print("  cmd1 | cmd2   — pipe output of cmd1 into cmd2\n");
}

static void builtin_ls(void) {
    int64_t n = listdir(dirbuf, sizeof(dirbuf));
    if (n > 0) write(STDOUT, dirbuf, (uint64_t)n);
}

static void builtin_cat(const char *filename) {
    int64_t fd = open(filename, O_RDONLY);
    if (fd < 0) {
        print("cat: cannot open '");
        print(filename);
        print("'\n");
        return;
    }
    int64_t n;
    while ((n = read((int)fd, filebuf, sizeof(filebuf) - 1)) > 0)
        write(STDOUT, filebuf, (uint64_t)n);
    close((int)fd);
}

static void builtin_echo(const char *text) {
    write(STDOUT, text, strlen(text));
    print("\n");
}

static void builtin_wc(void) {
    /* Count lines, words, bytes from stdin */
    int64_t lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[256];
    int64_t n;

    while ((n = read(STDIN, buf, sizeof(buf))) > 0) {
        for (int64_t i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }

    print("  ");
    print_num(lines);
    print("  ");
    print_num(words);
    print("  ");
    print_num(bytes);
    print("\n");
}

static void builtin_ps(void) {
    print("Shell PID: ");
    print_num(getpid());
    print("\n");
}

/* Trim trailing spaces in-place */
static void trim_end(char *s) {
    int len = 0;
    while (s[len]) len++;
    while (len > 0 && s[len-1] == ' ') len--;
    s[len] = '\0';
}

/*
 * Execute a single command (no pipes).
 * If it's a built-in, run it directly.
 * If it's an external program, exec it.
 * Called in a forked child when part of a pipe chain.
 */
static void run_single_cmd(const char *cmd) {
    cmd = skip_spaces(cmd);
    if (*cmd == '\0') return;

    if (streq(cmd, "help"))    { builtin_help(); return; }
    if (streq(cmd, "ls"))      { builtin_ls(); return; }
    if (streq(cmd, "ps"))      { builtin_ps(); return; }
    if (streq(cmd, "wc"))      { builtin_wc(); return; }

    /* Commands with arguments */
    if (strncmp(cmd, "cat ", 4) == 0) {
        builtin_cat(skip_spaces(cmd + 4));
        return;
    }
    if (strncmp(cmd, "echo ", 5) == 0) {
        builtin_echo(cmd + 5);
        return;
    }

    /* External program */
    int64_t tid = exec(cmd, strlen(cmd));
    if (tid < 0) {
        print("Error: '");
        print(cmd);
        print("' not found\n");
        return;
    }
    wait((int)tid);
}

/*
 * Find the next '|' in the string. Returns pointer to '|' or NULL.
 */
static char *find_pipe(char *s) {
    while (*s) {
        if (*s == '|') return s;
        s++;
    }
    return 0;
}

/*
 * Execute a command line, handling pipes.
 *
 * For "cmd1 | cmd2 | cmd3":
 *   1. Split into segments at '|'
 *   2. For each segment, fork a child
 *   3. Connect adjacent children with pipes
 *   4. Wait for all children
 */
static void execute_line(char *cmdline) {
    /* Check if there's a pipe */
    if (!find_pipe(cmdline)) {
        /* No pipe — run directly (don't fork for simple commands) */
        const char *cmd = skip_spaces(cmdline);
        if (*cmd == '\0') return;

        /* Handle exit specially — must not fork */
        if (streq(cmd, "exit")) {
            print("Goodbye!\n");
            exit(0);
        }

        run_single_cmd(cmd);
        return;
    }

    /* Parse pipe segments */
    char *segments[8];
    int nseg = 0;
    char *p = cmdline;

    while (p && nseg < 8) {
        segments[nseg] = p;
        char *pipe_pos = find_pipe(p);
        if (pipe_pos) {
            *pipe_pos = '\0';
            p = pipe_pos + 1;
        } else {
            p = 0;
        }
        /* Trim whitespace from segment */
        trim_end(segments[nseg]);
        nseg++;
    }

    /* Execute pipe chain */
    int prev_read_fd = -1;  /* Read end of previous pipe */
    int child_pids[8];

    for (int i = 0; i < nseg; i++) {
        int pipefd[2] = {-1, -1};

        /* Create pipe between this segment and the next (except for last) */
        if (i < nseg - 1) {
            if (pipe(pipefd) < 0) {
                print("pipe failed\n");
                return;
            }
        }

        int64_t pid = fork();
        if (pid == 0) {
            /* Child process */

            /* Connect stdin to previous pipe's read end */
            if (prev_read_fd >= 0) {
                dup2(prev_read_fd, STDIN);
                close(prev_read_fd);
            }

            /* Connect stdout to this pipe's write end */
            if (pipefd[1] >= 0) {
                dup2(pipefd[1], STDOUT);
                close(pipefd[1]);
            }

            /* Close the read end of our pipe (child doesn't need it) */
            if (pipefd[0] >= 0)
                close(pipefd[0]);

            /* Run the command */
            run_single_cmd(segments[i]);
            exit(0);
        }

        child_pids[i] = (int)pid;

        /* Parent: close pipe ends we don't need */
        if (prev_read_fd >= 0)
            close(prev_read_fd);
        if (pipefd[1] >= 0)
            close(pipefd[1]);

        /* Save read end for next segment */
        prev_read_fd = pipefd[0];
    }

    /* Close last read end if any */
    if (prev_read_fd >= 0)
        close(prev_read_fd);

    /* Wait for all children */
    for (int i = 0; i < nseg; i++)
        wait(child_pids[i]);
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
    print("\n");
    print("  =============================\n");
    print("  |   VirtIO-OS Shell v3.0    |\n");
    print("  |     (now with pipes!)     |\n");
    print("  =============================\n");
    print("\n");
    print("Type 'help' for commands.\n\n");

    for (;;) {
        print("> ");
        int64_t n = readline(line, sizeof(line));
        if (n <= 0) continue;
        execute_line(line);
    }
}
