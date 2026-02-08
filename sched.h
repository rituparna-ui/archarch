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

#define SCHED_MAX_TASKS  8
#define SCHED_STACK_SIZE 4096

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
 * Get the current task ID.
 */
int sched_current_id(void);

/*
 * Get task info for display.
 */
struct task *sched_get_task(int id);
int sched_task_count(void);

#endif
