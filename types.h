#ifndef TYPES_H
#define TYPES_H

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long       uint64_t;
typedef signed long         int64_t;
typedef signed int          int32_t;
typedef unsigned long       uintptr_t;
typedef unsigned long       size_t;

#define NULL ((void *)0)
#define true 1
#define false 0

/* Memory-mapped I/O helpers */
static inline void mmio_write32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write16(uintptr_t addr, uint16_t val) {
    *(volatile uint16_t *)addr = val;
}

static inline uint16_t mmio_read16(uintptr_t addr) {
    return *(volatile uint16_t *)addr;
}

static inline void mmio_write8(uintptr_t addr, uint8_t val) {
    *(volatile uint8_t *)addr = val;
}

static inline uint8_t mmio_read8(uintptr_t addr) {
    return *(volatile uint8_t *)addr;
}

/* Barriers */
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

#endif
