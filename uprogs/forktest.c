/*
 * forktest.bin — tests fork() syscall.
 *
 * Parent forks, child prints a message and exits.
 * Parent waits for child, then prints its own message.
 */
#include "usys.h"

static void print_num(int64_t n) {
    if (n < 0) { print("-"); n = -n; }
    char buf[20];
    int i = 0;
    if (n == 0) buf[i++] = '0';
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) write(STDOUT, &buf[i], 1);
}

int main(void) {
    print("[forktest] Parent PID=");
    print_num(getpid());
    print("\n");

    int64_t pid = fork();

    if (pid == 0) {
        /* Child */
        print("[forktest] Child here! PID=");
        print_num(getpid());
        print("\n");

        print("[forktest] Child counting: ");
        for (int i = 1; i <= 5; i++) {
            char c = '0' + i;
            write(STDOUT, &c, 1);
            print(" ");
            yield();
        }
        print("\n");

        print("[forktest] Child exiting\n");
        exit(0);
    } else if (pid > 0) {
        /* Parent */
        print("[forktest] Forked child PID=");
        print_num(pid);
        print("\n");

        print("[forktest] Parent waiting for child...\n");
        wait((int)pid);

        print("[forktest] Child finished! Parent PID=");
        print_num(getpid());
        print(" still alive\n");
    } else {
        print("[forktest] Fork failed!\n");
    }

    print("[forktest] Done\n");
    exit(0);
}
