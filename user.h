/*
 * Userspace syscall stubs (EL0 side).
 *
 * These are called from EL0 user tasks. Each stub loads the
 * syscall number into x8, arguments into x0-x5, and executes
 * SVC #0 to trap into EL1.
 *
 * The kernel's synchronous exception handler dispatches to
 * syscall_handler() and places the return value in x0 before
 * eret back to EL0.
 */
#ifndef USER_H
#define USER_H

#include "types.h"
#include "syscall.h"

static inline uint64_t syscall0(uint64_t nr) {
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline uint64_t syscall1(uint64_t nr, uint64_t a0) {
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline uint64_t syscall2(uint64_t nr, uint64_t a0, uint64_t a1) {
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

/* ---- Friendly wrappers ---- */

/* Write a buffer to the console */
static inline uint64_t user_write(const char *buf, uint64_t len) {
    return syscall2(SYS_WRITE, (uint64_t)buf, len);
}

/* Write a null-terminated string */
static inline void user_puts(const char *s) {
    uint64_t len = 0;
    while (s[len]) len++;
    user_write(s, len);
}

/* Get milliseconds since boot */
static inline uint64_t user_gettime(void) {
    return syscall0(SYS_GETTIME);
}

/* Yield CPU to next task */
static inline void user_yield(void) {
    syscall0(SYS_YIELD);
}

/* Exit current task */
static inline void user_exit(uint64_t code) {
    syscall1(SYS_EXIT, code);
}

/* Get current task ID */
static inline uint64_t user_getpid(void) {
    return syscall0(SYS_GETPID);
}

/* Sleep for approximately ms milliseconds */
static inline void user_sleep(uint64_t ms) {
    syscall1(SYS_SLEEP, ms);
}

/* Simple decimal print for userspace */
static inline void user_putdec(uint64_t val) {
    char buf[20];
    int i = 0;
    if (val == 0) {
        user_write("0", 1);
        return;
    }
    while (val > 0) {
        buf[i++] = '0' + (char)(val % 10);
        val /= 10;
    }
    /* Reverse */
    char rev[20];
    for (int j = 0; j < i; j++)
        rev[j] = buf[i - 1 - j];
    user_write(rev, (uint64_t)i);
}

#endif
