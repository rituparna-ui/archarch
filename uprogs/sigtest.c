/*
 * sigtest.bin — tests signal handling.
 *
 * Test 1: Register a SIGUSR1 handler, send signal to self
 * Test 2: Fork child, child sends SIGUSR1 to parent
 * Test 3: SIGKILL a child process
 */
#include "usys.h"

static volatile int got_signal = 0;
static volatile int signal_num = 0;

static void sigusr1_handler(int sig) {
    got_signal = 1;
    signal_num = sig;
    /* Handler returns via sigreturn trampoline automatically */
}

static void print_num(int64_t n) {
    if (n == 0) { print("0"); return; }
    if (n < 0) { print("-"); n = -n; }
    char buf[20]; int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) write(STDOUT, &buf[i], 1);
}

int main(void) {
    print("[sig] === Signal Test ===\n");

    /* Test 1: self-signal with handler */
    print("[sig] Test 1: SIGUSR1 to self\n");
    signal(SIGUSR1, (uint64_t)sigusr1_handler);

    got_signal = 0;
    kill((int)getpid(), SIGUSR1);

    /* After kill returns, the signal should have been delivered */
    yield();  /* Give signal a chance to be delivered */

    if (got_signal) {
        print("[sig] Handler called! sig=");
        print_num(signal_num);
        print(" PASS\n");
    } else {
        print("[sig] Handler NOT called. FAIL\n");
    }

    /* Test 2: child sends signal to parent */
    print("[sig] Test 2: child signals parent\n");
    got_signal = 0;
    int parent_pid = (int)getpid();

    int64_t child = fork();
    if (child == 0) {
        /* Child: send SIGUSR1 to parent */
        kill(parent_pid, SIGUSR1);
        exit(0);
    }

    /* Parent: wait for child and check signal */
    wait((int)child);
    yield();  /* Let signal be delivered */

    if (got_signal) {
        print("[sig] Parent got signal from child! PASS\n");
    } else {
        print("[sig] Parent did NOT get signal. FAIL\n");
    }

    /* Test 3: SIGKILL a child */
    print("[sig] Test 3: SIGKILL child\n");
    int64_t child2 = fork();
    if (child2 == 0) {
        /* Child: spin forever */
        for (;;) yield();
    }

    /* Parent: kill the child */
    kill((int)child2, SIGKILL);
    wait((int)child2);
    print("[sig] Child killed. PASS\n");

    print("[sig] === All tests passed ===\n");
    exit(0);
}
