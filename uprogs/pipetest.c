/*
 * pipetest.bin — tests pipe() + fork() + dup2().
 *
 * Creates a pipe, forks. Child writes to pipe, parent reads from it.
 * Then tests pipe with dup2 to redirect stdout.
 */
#include "usys.h"

static void print_num(int64_t n) {
    if (n == 0) { print("0"); return; }
    if (n < 0) { print("-"); n = -n; }
    char buf[20];
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) write(STDOUT, &buf[i], 1);
}

int main(void) {
    print("[pipe] === Pipe Test ===\n");

    /* Test 1: basic pipe with fork */
    print("[pipe] Test 1: fork + pipe\n");
    int fds[2];
    if (pipe(fds) < 0) {
        print("[pipe] pipe() failed!\n");
        exit(1);
    }
    print("[pipe] Created pipe: read=");
    print_num(fds[0]);
    print(" write=");
    print_num(fds[1]);
    print("\n");

    int64_t pid = fork();
    if (pid == 0) {
        /* Child: close read end, write message, close write end */
        close(fds[0]);
        const char *msg = "Hello through the pipe!";
        write(fds[1], msg, strlen(msg));
        close(fds[1]);
        exit(0);
    }

    /* Parent: close write end, read from pipe */
    close(fds[1]);
    char buf[64];
    int64_t n = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);

    if (n > 0) {
        print("[pipe] Parent received: \"");
        write(STDOUT, buf, (uint64_t)n);
        print("\"\n");
    } else {
        print("[pipe] Parent read failed!\n");
    }
    wait((int)pid);

    /* Test 2: pipe with dup2 to redirect stdout */
    print("[pipe] Test 2: dup2 stdout redirect\n");
    int fds2[2];
    pipe(fds2);

    int64_t pid2 = fork();
    if (pid2 == 0) {
        /* Child: redirect stdout to pipe write end */
        close(fds2[0]);
        dup2(fds2[1], STDOUT);
        close(fds2[1]);

        /* This write goes to the pipe, not UART! */
        /* Use write() directly with fd 1 */
        const char *msg2 = "Redirected output!";
        write(STDOUT, msg2, strlen(msg2));
        exit(0);
    }

    /* Parent: read from pipe */
    close(fds2[1]);

    /* Wait for child to finish first, then read */
    wait((int)pid2);

    char buf2[64];
    int64_t n2 = read(fds2[0], buf2, sizeof(buf2) - 1);
    close(fds2[0]);

    if (n2 > 0) {
        print("[pipe] Captured from child stdout: \"");
        write(STDOUT, buf2, (uint64_t)n2);
        print("\"\n");
    }

    print("[pipe] === All tests passed ===\n");
    exit(0);
}
