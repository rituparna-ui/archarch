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
#include "pmm.h"
#include "mmu.h"

/* Assembly: drop_to_el0(entry, user_sp) */
extern void drop_to_el0(uintptr_t entry, uintptr_t user_sp);

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
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    int id = current_task;
    if (tasks[id].is_user) {
        /* Drop to EL0 — this never returns normally.
         * When the user calls SYS_EXIT, syscall_handler calls sched_exit
         * which context_switches away. */
        drop_to_el0(tasks[id].user_entry, tasks[id].user_sp);
    } else {
        start_info[id].entry(start_info[id].arg);
    }
    sched_exit();
    for (;;) __asm__ volatile("wfe");
}

void sched_init(void) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
        tasks[i].id = i;
        tasks[i].ticks = 0;
        tasks[i].wait_for_tid = -1;
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
    t->is_user = 0;
    t->user_entry = 0;
    t->user_sp = 0;
    t->ttbr0 = 0;
    t->wait_for_tid = -1;

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

#define USER_STACK_PAGES 4

int sched_create_user(const char *name, const void *code, uint32_t code_size) {
    if (num_tasks >= SCHED_MAX_TASKS) {
        uart_puts("[SCHED] Too many tasks!\n");
        return -1;
    }

    /* Allocate and copy user code */
    uint32_t code_pages = (code_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (code_pages == 0) code_pages = 1;
    uintptr_t code_base = pmm_alloc_pages(code_pages);
    if (!code_base) {
        uart_puts("[SCHED] Cannot allocate user code pages\n");
        return -1;
    }
    const uint8_t *src = (const uint8_t *)code;
    uint8_t *dst = (uint8_t *)code_base;
    for (uint32_t i = 0; i < code_size; i++)
        dst[i] = src[i];
    for (uint32_t i = code_size; i < code_pages * PAGE_SIZE; i++)
        dst[i] = 0;

    /* Flush caches for the copied code */
    for (uint32_t i = 0; i < code_pages * PAGE_SIZE; i += 64)
        __asm__ volatile("dc cvau, %0" : : "r"(code_base + i));
    __asm__ volatile("dsb ish");
    for (uint32_t i = 0; i < code_pages * PAGE_SIZE; i += 64)
        __asm__ volatile("ic ivau, %0" : : "r"(code_base + i));
    __asm__ volatile("dsb ish\n isb\n");

    /* Allocate user stack */
    uintptr_t ustack_base = pmm_alloc_pages(USER_STACK_PAGES);
    if (!ustack_base) {
        uart_puts("[SCHED] Cannot allocate user stack\n");
        pmm_free_pages(code_base, code_pages);
        return -1;
    }
    uintptr_t ustack_top = (ustack_base + USER_STACK_PAGES * PAGE_SIZE) & ~0xFUL;

    /* Create per-process page table */
    uintptr_t pgd = mmu_create_user_pgd(code_base, code_pages,
                                         ustack_base, USER_STACK_PAGES);
    if (!pgd) {
        uart_puts("[SCHED] Cannot create user page table\n");
        pmm_free_pages(code_base, code_pages);
        pmm_free_pages(ustack_base, USER_STACK_PAGES);
        return -1;
    }

    int id = num_tasks++;
    struct task *t = &tasks[id];

    t->state = TASK_READY;
    t->name  = name;
    t->ticks = 0;
    t->is_user = 1;
    t->user_entry = code_base;
    t->user_sp = ustack_top;
    t->ttbr0 = pgd;
    t->wait_for_tid = -1;

    /* Kernel stack — used when this task traps to EL1 */
    uint8_t *kstack_top = &task_stacks[id][SCHED_STACK_SIZE];
    kstack_top = (uint8_t *)((uintptr_t)kstack_top & ~0xFUL);

    t->ctx.sp  = (uint64_t)(uintptr_t)kstack_top;
    t->ctx.x30 = (uint64_t)(uintptr_t)task_wrapper;
    t->ctx.x29 = 0;
    t->ctx.x19 = 0; t->ctx.x20 = 0; t->ctx.x21 = 0; t->ctx.x22 = 0;
    t->ctx.x23 = 0; t->ctx.x24 = 0; t->ctx.x25 = 0; t->ctx.x26 = 0;
    t->ctx.x27 = 0; t->ctx.x28 = 0;

    uart_puts("[SCHED] Created user task ");
    uart_putdec((uint64_t)id);
    uart_puts(": \"");
    uart_puts(name);
    uart_puts("\" code=");
    uart_puthex(code_base);
    uart_puts(" ustack=");
    uart_puthex(ustack_top);
    uart_puts("\n");

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

    /* Switch TTBR0 if the next task has a different page table */
    mmu_switch_ttbr0(tasks[next].ttbr0);

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
        mmu_switch_ttbr0(tasks[next].ttbr0);
        context_switch(&tasks[prev].ctx, &tasks[next].ctx);
    }
}

void sched_exit(void) {
    int me = current_task;

    uart_puts("[SCHED] Task ");
    uart_putdec((uint64_t)me);
    uart_puts(" (\"");
    uart_puts(tasks[me].name);
    uart_puts("\") finished\n");

    tasks[me].state = TASK_FINISHED;

    /* Wake any task that was waiting on us */
    for (int i = 0; i < num_tasks; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_for_tid == me) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_for_tid = -1;
        }
    }

    sched_yield();
}

int sched_wait(int tid) {
    if (tid < 0 || tid >= num_tasks)
        return -1;

    /* Already finished? */
    if (tasks[tid].state == TASK_FINISHED)
        return 0;

    /* Block until the target finishes */
    tasks[current_task].state = TASK_BLOCKED;
    tasks[current_task].wait_for_tid = tid;
    sched_yield();

    /* We've been woken up — target is finished */
    return 0;
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
