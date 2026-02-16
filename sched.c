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
#include "fd.h"
#include "kva.h"

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
    t->code_pa = 0;
    t->code_pages = 0;
    t->stack_pa = 0;
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
    uint8_t *dst = (uint8_t *)phys_to_virt(code_base);
    for (uint32_t i = 0; i < code_size; i++)
        dst[i] = src[i];
    for (uint32_t i = code_size; i < code_pages * PAGE_SIZE; i++)
        dst[i] = 0;

    /* Flush caches for the copied code (use VA for cache ops) */
    uintptr_t code_va = phys_to_virt(code_base);
    for (uint32_t i = 0; i < code_pages * PAGE_SIZE; i += 64)
        __asm__ volatile("dc cvau, %0" : : "r"(code_va + i));
    __asm__ volatile("dsb ish");
    for (uint32_t i = 0; i < code_pages * PAGE_SIZE; i += 64)
        __asm__ volatile("ic ivau, %0" : : "r"(code_va + i));
    __asm__ volatile("dsb ish\n isb\n");

    /* Allocate user stack with guard page.
     * We allocate USER_STACK_PAGES + 1 pages, but only map the top
     * USER_STACK_PAGES. The bottom page is the guard (unmapped). */
    uintptr_t ustack_alloc = pmm_alloc_pages(USER_STACK_PAGES + 1);
    if (!ustack_alloc) {
        uart_puts("[SCHED] Cannot allocate user stack\n");
        pmm_free_pages(code_base, code_pages);
        return -1;
    }
    uintptr_t guard_page = ustack_alloc;
    uintptr_t ustack_base = ustack_alloc + PAGE_SIZE;  /* skip guard */
    uintptr_t ustack_top = (ustack_base + USER_STACK_PAGES * PAGE_SIZE) & ~0xFUL;

    /* Create per-process page table — maps code + stack, guard is unmapped */
    uintptr_t pgd = mmu_create_user_pgd(code_base, code_pages,
                                         ustack_base, USER_STACK_PAGES);
    if (!pgd) {
        uart_puts("[SCHED] Cannot create user page table\n");
        pmm_free_pages(code_base, code_pages);
        pmm_free_pages(ustack_alloc, USER_STACK_PAGES + 1);
        return -1;
    }

    int id = num_tasks++;
    struct task *t = &tasks[id];

    t->state = TASK_READY;
    t->name  = name;
    t->ticks = 0;
    t->is_user = 1;
    t->user_entry = USER_VA_CODE;
    t->user_sp = USER_VA_STACK;
    t->ttbr0 = pgd;
    t->code_pa = code_base;
    t->code_pages = code_pages;
    t->stack_pa = ustack_base;
    t->wait_for_tid = -1;
    fd_table_init(&t->fdt);

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

    /* Close all open file descriptors */
    fd_table_destroy(&tasks[me].fdt);

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

/*
 * Fork the current user task.
 *
 * parent_regs: the saved syscall frame on the parent's kernel stack.
 * Layout: [x0..x30, SP_EL0, ELR_EL1, SPSR_EL1] (272 bytes, 34 uint64_t's)
 *
 * We:
 *   1. Allocate new code + stack pages, copy parent's content
 *   2. Create new page table
 *   3. Allocate a new task slot with its own kernel stack
 *   4. Build a fake syscall return frame on the child's kernel stack
 *      so when the child is scheduled, it returns from the SVC with x0=0
 *   5. Set up the child's task_context so context_switch lands in
 *      a trampoline that does the eret from the fake frame
 *
 * Returns child tid to parent, child will see 0 when it runs.
 */

/* Trampoline: the child's first context_switch returns here.
 * We restore the syscall frame and eret to EL0. */
static void fork_child_return(void);

/* Assembly helper defined below */
extern void fork_child_trampoline(void);

int sched_fork(uint64_t *parent_regs) {
    struct task *parent = &tasks[current_task];

    if (!parent->is_user) {
        uart_puts("[FORK] Cannot fork non-user task\n");
        return -1;
    }
    if (num_tasks >= SCHED_MAX_TASKS) {
        uart_puts("[FORK] Too many tasks\n");
        return -1;
    }

    /* 1. Copy user code pages */
    uint32_t cpages = parent->code_pages;
    uintptr_t child_code = pmm_alloc_pages(cpages);
    if (!child_code) return -1;

    uint8_t *csrc = (uint8_t *)phys_to_virt(parent->code_pa);
    uint8_t *cdst = (uint8_t *)phys_to_virt(child_code);
    for (uint32_t i = 0; i < cpages * PAGE_SIZE; i++)
        cdst[i] = csrc[i];

    /* Flush icache for copied code */
    uintptr_t cva = phys_to_virt(child_code);
    for (uint32_t i = 0; i < cpages * PAGE_SIZE; i += 64)
        __asm__ volatile("dc cvau, %0" : : "r"(cva + i));
    __asm__ volatile("dsb ish");
    for (uint32_t i = 0; i < cpages * PAGE_SIZE; i += 64)
        __asm__ volatile("ic ivau, %0" : : "r"(cva + i));
    __asm__ volatile("dsb ish\n isb\n");

    /* 2. Copy user stack pages (with guard) */
    uintptr_t child_salloc = pmm_alloc_pages(USER_STACK_PAGES + 1);
    if (!child_salloc) { pmm_free_pages(child_code, cpages); return -1; }
    uintptr_t child_stack = child_salloc + PAGE_SIZE;  /* skip guard */

    uint8_t *ssrc = (uint8_t *)phys_to_virt(parent->stack_pa);
    uint8_t *sdst = (uint8_t *)phys_to_virt(child_stack);
    for (uint32_t i = 0; i < USER_STACK_PAGES * PAGE_SIZE; i++)
        sdst[i] = ssrc[i];

    /* 3. Create child page table */
    uintptr_t pgd = mmu_create_user_pgd(child_code, cpages,
                                          child_stack, USER_STACK_PAGES);
    if (!pgd) {
        pmm_free_pages(child_code, cpages);
        pmm_free_pages(child_salloc, USER_STACK_PAGES + 1);
        return -1;
    }

    /* 4. Create child task */
    int child_id = num_tasks++;
    struct task *child = &tasks[child_id];

    child->state = TASK_READY;
    child->name = parent->name;
    child->ticks = 0;
    child->is_user = 1;
    child->user_entry = USER_VA_CODE;
    child->user_sp = USER_VA_STACK;
    child->ttbr0 = pgd;
    child->code_pa = child_code;
    child->code_pages = cpages;
    child->stack_pa = child_stack;
    child->wait_for_tid = -1;

    /* Duplicate file descriptor table */
    fd_table_dup(&child->fdt, &parent->fdt);

    /*
     * 5. Build the child's kernel stack so it returns from the syscall.
     *
     * When the scheduler picks the child, it does context_switch which
     * restores callee-saved regs and returns via x30. We set x30 to
     * fork_child_return which restores the syscall frame and erets.
     *
     * The child's kernel stack layout (top to bottom):
     *   [top - 272] = syscall frame (copy of parent's, with x0=0)
     *   [top - 272] = SP for the eret trampoline
     */
    uint8_t *kstack_top = &task_stacks[child_id][SCHED_STACK_SIZE];
    kstack_top = (uint8_t *)((uintptr_t)kstack_top & ~0xFUL);

    /* Copy parent's syscall frame to child's kernel stack */
    uint64_t *child_frame = (uint64_t *)(kstack_top - 272);
    for (int i = 0; i < 34; i++)
        child_frame[i] = parent_regs[i];

    /* Child gets x0 = 0 (fork return value) */
    child_frame[0] = 0;

    /* Set up task_context so context_switch jumps to our trampoline */
    child->ctx.sp = (uint64_t)(uintptr_t)child_frame;  /* SP points to the frame */
    child->ctx.x30 = (uint64_t)(uintptr_t)fork_child_return;
    child->ctx.x29 = 0;
    child->ctx.x19 = 0; child->ctx.x20 = 0; child->ctx.x21 = 0;
    child->ctx.x22 = 0; child->ctx.x23 = 0; child->ctx.x24 = 0;
    child->ctx.x25 = 0; child->ctx.x26 = 0; child->ctx.x27 = 0;
    child->ctx.x28 = 0;

    uart_puts("[FORK] ");
    uart_putdec((uint64_t)current_task);
    uart_puts(" -> ");
    uart_putdec((uint64_t)child_id);
    uart_puts("\n");

    return child_id;
}

/*
 * Child trampoline: called via context_switch x30.
 * SP points to the syscall frame. Restore and eret to EL0.
 */
static void fork_child_return(void) {
    /* Re-enable IRQs (we're coming from context_switch inside IRQ/syscall) */
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /* SP already points to the syscall frame.
     * Do the same restore sequence as sync_el0_entry. */
    __asm__ volatile(
        /* Restore SP_EL0, ELR_EL1, SPSR_EL1 */
        "ldr x0, [sp, #248]\n"
        "ldp x1, x2, [sp, #256]\n"
        "msr sp_el0, x0\n"
        "msr elr_el1, x1\n"
        "msr spsr_el1, x2\n"

        /* Restore x0-x30 */
        "ldp x0,  x1,  [sp, #0]\n"
        "ldp x2,  x3,  [sp, #16]\n"
        "ldp x4,  x5,  [sp, #32]\n"
        "ldp x6,  x7,  [sp, #48]\n"
        "ldp x8,  x9,  [sp, #64]\n"
        "ldp x10, x11, [sp, #80]\n"
        "ldp x12, x13, [sp, #96]\n"
        "ldp x14, x15, [sp, #112]\n"
        "ldp x16, x17, [sp, #128]\n"
        "ldp x18, x19, [sp, #144]\n"
        "ldp x20, x21, [sp, #160]\n"
        "ldp x22, x23, [sp, #176]\n"
        "ldp x24, x25, [sp, #192]\n"
        "ldp x26, x27, [sp, #208]\n"
        "ldp x28, x29, [sp, #224]\n"
        "ldr x30,       [sp, #240]\n"

        "add sp, sp, #272\n"
        "eret\n"
    );
    __builtin_unreachable();
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
