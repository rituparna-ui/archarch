/*
 * User-space syscall wrappers — Unix-style with file descriptors.
 *
 * Syscall numbers:
 *   0 = read(fd, buf, count)
 *   1 = write(fd, buf, count)
 *   2 = open(path, flags)
 *   3 = close(fd)
 *   4 = getpid()
 *   5 = exit(code)
 *   6 = yield()
 *   7 = exec(name, namelen) → child tid
 *   8 = wait(tid)
 *   9 = sbrk(size)
 *  10 = listdir(buf, buflen)
 */
#ifndef USYS_H
#define USYS_H

typedef unsigned long uint64_t;
typedef unsigned int  uint32_t;
typedef long          int64_t;

#define STDIN  0
#define STDOUT 1
#define STDERR 2

#define O_RDONLY 0

/* Generic 3-arg syscall helper */
static inline int64_t _syscall3(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x8 __asm__("x8") = num;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return (int64_t)x0;
}

static inline int64_t _syscall2(uint64_t num, uint64_t a0, uint64_t a1) {
    return _syscall3(num, a0, a1, 0);
}

static inline int64_t _syscall1(uint64_t num, uint64_t a0) {
    return _syscall3(num, a0, 0, 0);
}

static inline int64_t _syscall0(uint64_t num) {
    return _syscall3(num, 0, 0, 0);
}

/* File descriptor syscalls */
static inline int64_t read(int fd, void *buf, uint64_t count) {
    return _syscall3(0, (uint64_t)fd, (uint64_t)buf, count);
}

static inline int64_t write(int fd, const void *buf, uint64_t count) {
    return _syscall3(1, (uint64_t)fd, (uint64_t)buf, count);
}

static inline int64_t open(const char *path, int flags) {
    return _syscall2(2, (uint64_t)path, (uint64_t)flags);
}

static inline int64_t close(int fd) {
    return _syscall1(3, (uint64_t)fd);
}

/* Process syscalls */
static inline int64_t getpid(void) { return _syscall0(4); }

static inline void exit(int code) {
    _syscall1(5, (uint64_t)code);
    __builtin_unreachable();
}

static inline void yield(void) { _syscall0(6); }

static inline int64_t exec(const char *name, uint64_t len) {
    return _syscall2(7, (uint64_t)name, len);
}

static inline int64_t wait(int tid) { return _syscall1(8, (uint64_t)tid); }

static inline int64_t sbrk(uint64_t size) { return _syscall1(9, size); }

static inline int64_t fork(void) { return _syscall0(11); }

static inline int64_t listdir(char *buf, uint64_t buflen) {
    return _syscall2(10, (uint64_t)buf, buflen);
}

/* Convenience helpers */
static inline uint64_t strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline void print(const char *s) {
    write(STDOUT, s, strlen(s));
}

static inline int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Backward compat aliases */
#define sys_write(buf, len)     write(STDOUT, buf, len)
#define sys_read(buf, max)      read(STDIN, buf, max)
#define sys_getpid()            getpid()
#define sys_exit(c)             exit(c)
#define sys_yield()             yield()
#define sys_exec(n, l)          exec(n, l)
#define sys_wait(t)             wait(t)
#define sys_listdir(b, l)       listdir(b, l)

#endif
