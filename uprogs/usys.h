/* Minimal syscall wrappers for user programs */
#ifndef USYS_H
#define USYS_H

typedef unsigned long uint64_t;
typedef unsigned int  uint32_t;
typedef long          int64_t;

static inline int64_t sys_write(const char *buf, uint64_t len) {
    int64_t ret;
    __asm__ volatile(
        "mov x0, %1\n"
        "mov x1, %2\n"
        "mov x8, #0\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(ret) : "r"(buf), "r"(len)
        : "x0", "x1", "x8", "memory"
    );
    return ret;
}

static inline int64_t sys_getpid(void) {
    int64_t ret;
    __asm__ volatile("mov x8, #1\n svc #0\n mov %0, x0" : "=r"(ret) : : "x0", "x8");
    return ret;
}

static inline void sys_exit(int code) {
    __asm__ volatile(
        "mov x0, %0\n mov x8, #2\n svc #0\n"
        : : "r"((uint64_t)code) : "x0", "x8"
    );
    __builtin_unreachable();
}

static inline void sys_yield(void) {
    __asm__ volatile("mov x8, #3\n svc #0\n" : : : "x0", "x8");
}

static inline int64_t sys_exec(const char *name, uint64_t len) {
    int64_t ret;
    __asm__ volatile(
        "mov x0, %1\n mov x1, %2\n mov x8, #5\n svc #0\n mov %0, x0\n"
        : "=r"(ret) : "r"(name), "r"(len) : "x0", "x1", "x8", "memory"
    );
    return ret;
}

static inline int64_t sys_read(char *buf, uint64_t maxlen) {
    int64_t ret;
    __asm__ volatile(
        "mov x0, %1\n mov x1, %2\n mov x8, #6\n svc #0\n mov %0, x0\n"
        : "=r"(ret) : "r"(buf), "r"(maxlen) : "x0", "x1", "x8", "memory"
    );
    return ret;
}

static inline int64_t sys_wait(int tid) {
    int64_t ret;
    __asm__ volatile(
        "mov x0, %1\n mov x8, #7\n svc #0\n mov %0, x0\n"
        : "=r"(ret) : "r"((uint64_t)tid) : "x0", "x8"
    );
    return ret;
}

static inline int64_t sys_listdir(char *buf, uint64_t buflen) {
    int64_t ret;
    __asm__ volatile(
        "mov x0, %1\n mov x1, %2\n mov x8, #8\n svc #0\n mov %0, x0\n"
        : "=r"(ret) : "r"(buf), "r"(buflen) : "x0", "x1", "x8", "memory"
    );
    return ret;
}

/* Helpers */
static inline void print(const char *s) {
    uint64_t len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static inline int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline uint64_t strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

#endif
