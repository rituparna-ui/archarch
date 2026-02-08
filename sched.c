/*
 * Preemptive round-robin scheduler.
 *
 * Preemptive switching works by saving/restoring the stack pointer
 * in the IRQ entry/exit path. When a timer tick fires:
 *   1. IRQ entry saves all registers onto the current task's stack
 *   2. irq_handler calls sched_tick which saves current SP and
 *      loads the next task's SP
 *   3. IRQ exit restores all registers from the (now different) stack
 *   4. eret returns to the new task's interrupted PC
 *
 * For newly created tasks that haven't been interrupted yet, we
 * pre-build a fake IRQ frame on their stack so the first switch
 * to them works correctly.
 *
 * Task 0 is the "idle" task — the original main() context.
 */
#include "sched.h"
#include "uart.h"

/* Task table */
static struct task tasks[SCHED_MAX_TASKS];
static int current_task;
static int num_tasks;

/* Per-task stacks (task 0 uses the main stack) */
static uint8_t task_stacks[SCHED_MAX_TASKS][SCHED_STACK_SIZE]
    __attribute__((aligned(16)));

/*
 * Assembly context switch for voluntary yields.
 * Saves callee-saved regs (x19-x30, sp) into *old, loads from *new.
 */
extern void context_switch(struct task_context *old_ctx,
                           struct task_context *new_ctx);

/* Wrapper that calls the real entry point then marks task finished */
struct task_start_info {
    void (*entry)(void *);
    void *arg;
};

static struct task_start_info start_info[SCHED_MAX_TASKS];

static void task_wrapper(void) {
    /*
     * If we got here via context_switch from inside an IRQ handler
     * (sched_tick), IRQs are masked (PSTATE.I=1). We must re-enable
     * them so the timer can preempt this task.
     */
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    int id = current_task;
    start_info[id].entry(start_info[id].arg);
    sched_exit();
    for (;;) __asm__ volatile("wfe");
}

void sched_init(void) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
        tasks[i].id = i;
        tasks[i].ticks = 0;
    }

    tasks[0].state = TASK_RUNNING;
    tasks[0].name  = "idle";
    current_task = 0;
    num_tasks = 1;

    uart_puts("[SCHED] Initialized (task 0 = idle)\n");
}

int sched_create(const char *name, void (*entry)(void *), void *arg) {
    if (num_tasks >= SCHED_MAX_TASKS) {
        uart_puts("[SCHED] Too many tasks!\n");
        return -1;
    }

    int id = num_tasks++;
    struct task *t = &tasks[id];

    t->state = TASK_READY;
    t->name  = name;
    t->ticks = 0;

    start_info[id].entry = entry;
    start_info[id].arg   = arg;

    uint8_t *stack_top = &task_stacks[id][SCHED_STACK_SIZE];
    stack_top = (uint8_t *)((uintptr_t)stack_top & ~0xFUL);

    t->ctx.sp  = (uint64_t)(uintptr_t)stack_top;
    t->ctx.x30 = (uint64_t)(uintptr_t)task_wrapper;
    t->ctx.x29 = 0;
    t->ctx.x19 = 0;
    t->ctx.x20 = 0;
    t->ctx.x21 = 0;
    t->ctx.x22 = 0;
    t->ctx.x23 = 0;
    t->ctx.x24 = 0;
    t->ctx.x25 = 0;
    t->ctx.x26 = 0;
    t->ctx.x27 = 0;
    t->ctx.x28 = 0;

    uart_puts("[SCHED] Created task ");
    uart_putdec((uint64_t)id);
    uart_puts(": \"");
    uart_puts(name);
    uart_puts("\"\n");

    return id;
}

static int pick_next(void) {
    for (int i = 1; i <= num_tasks; i++) {
        int idx = (current_task + i) % num_tasks;
        if (tasks[idx].state == TASK_READY)
            return idx;
    }
    if (tasks[current_task].state == TASK_RUNNING)
        return current_task;
    return 0;
}

static void switch_to(int next) {
    if (next == current_task)
        return;

    int prev = current_task;

    if (tasks[prev].state == TASK_RUNNING)
        tasks[prev].state = TASK_READY;

    tasks[next].state = TASK_RUNNING;
    current_task = next;

    context_switch(&tasks[prev].ctx, &tasks[next].ctx);
}

void sched_yield(void) {
    int next = pick_next();
    switch_to(next);
}

/*
 * Timer tick handler.
 *
 * Called from irq_handler (C code, inside IRQ context).
 * We use the voluntary context_switch here. This works because:
 *
 * When task A is interrupted by timer:
 *   irq_entry saves A's full state on A's IRQ stack frame
 *   -> irq_handler -> sched_tick -> context_switch
 *      saves callee-saved regs, switches SP to task B
 *   -> context_switch returns into task B's context
 *
 * If task B was also previously preempted, it returns through
 * its own irq_handler -> irq_entry -> eret path, restoring
 * B's full register state. This works because each task has
 * its own stack, and the IRQ frame is on that stack.
 *
 * If task B is new (never preempted), context_switch returns
 * to task_wrapper via x30, starting the task fresh.
 */
void sched_tick(void) {
    tasks[current_task].ticks++;

    int next = pick_next();
    if (next != current_task) {
        int prev = current_task;
        if (tasks[prev].state == TASK_RUNNING)
            tasks[prev].state = TASK_READY;
        tasks[next].state = TASK_RUNNING;
        current_task = next;
        context_switch(&tasks[prev].ctx, &tasks[next].ctx);
    }
}

void sched_exit(void) {
    uart_puts("[SCHED] Task ");
    uart_putdec((uint64_t)current_task);
    uart_puts(" (\"");
    uart_puts(tasks[current_task].name);
    uart_puts("\") finished\n");

    tasks[current_task].state = TASK_FINISHED;
    sched_yield();
}

int sched_current_id(void) {
    return current_task;
}

struct task *sched_get_task(int id) {
    if (id < 0 || id >= num_tasks)
        return NULL;
    return &tasks[id];
}

int sched_task_count(void) {
    return num_tasks;
}
