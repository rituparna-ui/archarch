/*
 * sbrktest.bin — tests sbrk() syscall.
 *
 * Verifies that sbrk allocates usable memory mapped into the process.
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

static void print_hex(uint64_t v) {
    const char *hex = "0123456789abcdef";
    print("0x");
    /* Print 8 hex digits */
    for (int i = 28; i >= 0; i -= 4) {
        char c = hex[(v >> i) & 0xf];
        write(STDOUT, &c, 1);
    }
}

int main(void) {
    print("[sbrktest] PID=");
    print_num(getpid());
    print("\n");

    /* Test 1: get current break */
    int64_t brk0 = sbrk(0);
    print("[test1] current break = ");
    print_hex((uint64_t)brk0);
    print("\n");

    if (brk0 == -1) {
        print("[FAIL] sbrk(0) returned -1\n");
        exit(1);
    }

    /* Test 2: allocate 64 bytes */
    int64_t addr = sbrk(64);
    print("[test2] sbrk(64) = ");
    print_hex((uint64_t)addr);
    print("\n");

    if (addr == -1) {
        print("[FAIL] sbrk(64) returned -1\n");
        exit(1);
    }

    /* Test 3: write to the allocated memory */
    char *p = (char *)addr;
    for (int i = 0; i < 64; i++)
        p[i] = (char)(i + 'A');

    /* Read back and verify */
    int ok = 1;
    for (int i = 0; i < 64; i++) {
        if (p[i] != (char)(i + 'A')) { ok = 0; break; }
    }
    print("[test3] write/read 64 bytes: ");
    print(ok ? "PASS" : "FAIL");
    print("\n");

    /* Test 4: allocate a larger region (8KB = 2 pages) */
    int64_t addr2 = sbrk(8192);
    print("[test4] sbrk(8192) = ");
    print_hex((uint64_t)addr2);
    print("\n");

    if (addr2 == -1) {
        print("[FAIL] sbrk(8192) returned -1\n");
        exit(1);
    }

    /* Fill with pattern and verify */
    char *q = (char *)addr2;
    for (int i = 0; i < 8192; i++)
        q[i] = (char)(i & 0xff);

    ok = 1;
    for (int i = 0; i < 8192; i++) {
        if (q[i] != (char)(i & 0xff)) { ok = 0; break; }
    }
    print("[test4] write/read 8KB: ");
    print(ok ? "PASS" : "FAIL");
    print("\n");

    /* Test 5: verify break advanced correctly */
    int64_t brk_now = sbrk(0);
    print("[test5] break now = ");
    print_hex((uint64_t)brk_now);
    print("\n");

    int64_t expected = brk0 + 64 + 8192;
    print("[test5] expected  = ");
    print_hex((uint64_t)expected);
    print("\n");

    print("[test5] break tracking: ");
    print(brk_now == expected ? "PASS" : "FAIL");
    print("\n");

    /* Test 6: original 64-byte region still intact */
    ok = 1;
    for (int i = 0; i < 64; i++) {
        if (p[i] != (char)(i + 'A')) { ok = 0; break; }
    }
    print("[test6] original region intact: ");
    print(ok ? "PASS" : "FAIL");
    print("\n");

    print("[sbrktest] All tests done\n");
    exit(0);
}
