/*
 * Simple preemptive round-robin scheduler.
 *
 * Each task gets its own stack and a saved CPU context.
 * The timer IRQ triggers context switches between tasks.
 * Tasks can also yield voluntarily.
 *
 * Max 8 tasks, each with a 4KB stack.
 */
#ifndef SCHED_H
#define SCHED_H

#include "types.h"
#include "fd.h"
#include "signal.h"

#define SCHED_MAX_TASKS  8
#define SCHED_STACK_SIZE 8192   /* 8KB — needs room for IRQ/syscall frames */

/* Task states */
#define TASK_UNUSED   0
#define TASK_READY    1
#define TASK_RUNNING  2
#define TASK_BLOCKED  3
#define TASK_FINISHED 4

/* Saved CPU context (matches the layout pushed in context_switch.S) */
struct task_context {
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;   /* frame pointer */
    uint64_t x30;   /* link register (return address) */
    uint64_t sp;
};

struct task {
    struct task_context ctx;
    int                 state;
    int                 id;
    const char         *name;
    uint64_t            ticks;  /* number of timer ticks this task has run */
    int                 is_user; /* 1 if this is an EL0 user task */
    uintptr_t           user_entry;  /* EL0 entry point VA */
    uintptr_t           user_sp;     /* EL0 stack pointer VA */
    uintptr_t           ttbr0;       /* Per-process page table PA (0 = kernel) */
    uintptr_t           code_pa;     /* Physical address of user code pages */
    uint32_t            code_pages;  /* Number of code pages */
    uintptr_t           stack_pa;    /* Physical address of user stack pages */
    uintptr_t           heap_break;  /* Current user heap break VA */
    int                 wait_for_tid; /* tid this task is waiting on (-1 = none) */
    int                 parent_tid;   /* parent task id (-1 = none) */
    struct fd_table     fdt;          /* per-process file descriptor table */
    struct signal_state sig;          /* per-process signal state */
};

/*
 * Initialize the scheduler. Must be called before creating tasks.
 * The calling context becomes task 0 ("idle").
 */
void sched_init(void);

/*
 * Create a new task. entry(arg) will be called when the task runs.
 * Returns the task ID, or -1 on failure.
 */
int sched_create(const char *name, void (*entry)(void *), void *arg);

/*
 * Create a new EL0 user task.
 * code/code_size: user program binary (will be copied to allocated pages)
 * Returns the task ID, or -1 on failure.
 */
int sched_create_user(const char *name, const void *code, uint32_t code_size);

/*
 * Yield the CPU to the next ready task (voluntary context switch).
 */
void sched_yield(void);

/*
 * Called from the timer IRQ handler to signal a preemption.
 * The actual context switch happens in the IRQ return path
 * (see irq_handler). We just pick the next task here.
 */
void sched_tick(void);

/*
 * Get the saved context pointer for a task (for IRQ-level switching).
 * Returns pointer to the task's saved SP (which points to the
 * exception frame on the task's stack).
 */
uint64_t *sched_current_sp_ptr(void);
uint64_t *sched_pick_next_sp_ptr(void);

/*
 * Mark the current task as finished and switch away.
 */
void sched_exit(void);

/*
 * Block current task until task `tid` finishes.
 * Returns 0 on success, -1 if tid is invalid.
 */
int sched_wait(int tid);

/*
 * Fork the current user task.
 * regs: pointer to the parent's saved syscall register frame.
 * Returns child tid to parent (via regs[0]), 0 to child.
 * Returns -1 on failure.
 */
int sched_fork(uint64_t *parent_regs);

/*
 * Get the current task ID.
 */
int sched_current_id(void);

/*
 * Get task info for display.
 */
struct task *sched_get_task(int id);
int sched_task_count(void);

#endif
