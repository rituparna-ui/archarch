/*
 * Signal implementation.
 *
 * Signal delivery works by modifying the saved EL0 register frame
 * before eret. The user sees their PC jump to the signal handler
 * with the signal number in x0. The handler returns via sigreturn().
 */
#include "signal.h"
#include "sched.h"
#include "uart.h"

void signal_init(struct signal_state *ss) {
    for (int i = 0; i < NSIG; i++)
        ss->handlers[i] = SIG_DFL;
    ss->pending = 0;
}

int signal_send(int tid, int sig) {
    if (sig < 1 || sig >= NSIG) return -1;

    struct task *t = sched_get_task(tid);
    if (!t || t->state == TASK_UNUSED || t->state == TASK_FINISHED)
        return -1;

    t->sig.pending |= (1U << sig);

    /* If the task is blocked, wake it up so it can handle the signal */
    if (t->state == TASK_BLOCKED) {
        t->state = TASK_READY;
        t->wait_for_tid = -1;
    }

    return 0;
}

/*
 * Default action for a signal.
 */
static void signal_default_action(int sig) {
    switch (sig) {
    case SIGCHLD:
        /* Default: ignore */
        break;
    case SIGKILL:
    case SIGSEGV:
    case SIGTERM:
    case SIGINT:
    case SIGPIPE:
        /* Default: kill the process */
        uart_puts("[SIGNAL] Task ");
        uart_putdec((uint64_t)sched_current_id());
        uart_puts(" killed by signal ");
        uart_putdec((uint64_t)sig);
        uart_puts("\n");
        sched_exit();
        break;
    default:
        /* Default: kill */
        sched_exit();
        break;
    }
}

/*
 * Sigreturn trampoline — 2 instructions placed on the user stack.
 * The handler returns here, which does svc to restore state.
 *
 * mov x8, #16     (SYS_SIGRETURN)
 * svc #0
 */
static const uint32_t sigreturn_trampoline[2] = {
    0xd2800208,  /* mov x8, #16 */
    0xd4000001,  /* svc #0 */
};

int signal_deliver(uint64_t *regs) {
    struct task *t = sched_get_task(sched_current_id());
    if (!t || !t->is_user) return 0;

    uint32_t pending = t->sig.pending;
    if (pending == 0) return 0;

    /* Find the lowest pending signal */
    int sig = 0;
    for (int i = 1; i < NSIG; i++) {
        if (pending & (1U << i)) {
            sig = i;
            break;
        }
    }
    if (sig == 0) return 0;

    /* Clear the pending bit */
    t->sig.pending &= ~(1U << sig);

    /* SIGKILL can't be caught or ignored */
    if (sig == SIGKILL) {
        uart_puts("[SIGNAL] Task ");
        uart_putdec((uint64_t)t->id);
        uart_puts(" SIGKILL\n");
        sched_exit();
        return 1;  /* unreachable */
    }

    uint64_t handler = t->sig.handlers[sig];

    /* SIG_IGN: ignore */
    if (handler == SIG_IGN) return 0;

    /* SIG_DFL: default action */
    if (handler == SIG_DFL) {
        signal_default_action(sig);
        return 0;  /* might not return if killed */
    }

    /*
     * User handler: push signal frame on user stack, redirect PC.
     *
     * regs layout:
     *   [0..30]  = x0-x30
     *   [31]     = SP_EL0
     *   [32]     = ELR_EL1 (return PC)
     *   [33]     = SPSR_EL1
     */
    uint64_t user_sp = regs[31];  /* SP_EL0 */

    /* Make room for signal frame on user stack */
    user_sp -= SIGFRAME_SIZE;
    user_sp &= ~0xFUL;  /* 16-byte align */

    /* Copy current register state to signal frame on user stack */
    uint64_t *frame = (uint64_t *)user_sp;
    for (int i = 0; i < 31; i++)
        frame[i] = regs[i];        /* x0-x30 */
    frame[31] = regs[31];          /* original SP_EL0 */
    frame[32] = regs[32];          /* original ELR (return PC) */
    frame[33] = regs[33];          /* original SPSR */
    frame[34] = (uint64_t)sig;     /* signal number */

    /* Place sigreturn trampoline on the stack */
    uint32_t *tramp = (uint32_t *)&frame[35];
    tramp[0] = sigreturn_trampoline[0];
    tramp[1] = sigreturn_trampoline[1];

    /* Modify saved state to jump to handler */
    regs[0] = (uint64_t)sig;                   /* x0 = signal number */
    regs[30] = user_sp + 35 * 8;               /* x30 (LR) = trampoline address */
    regs[31] = user_sp;                         /* SP_EL0 = new stack */
    regs[32] = handler;                         /* ELR = handler address */
    /* SPSR stays the same (EL0t) */

    return 1;
}

void signal_return(uint64_t *regs) {
    /*
     * Restore the original register state from the signal frame.
     * The frame is at the current SP_EL0.
     */
    uint64_t *frame = (uint64_t *)regs[31];  /* SP_EL0 points to frame */

    for (int i = 0; i < 31; i++)
        regs[i] = frame[i];        /* x0-x30 */
    regs[31] = frame[31];          /* original SP_EL0 */
    regs[32] = frame[32];          /* original ELR */
    regs[33] = frame[33];          /* original SPSR */
}

uint64_t signal_set_handler(int sig, uint64_t handler) {
    if (sig < 1 || sig >= NSIG || sig == SIGKILL)
        return (uint64_t)-1;

    struct task *t = sched_get_task(sched_current_id());
    if (!t) return (uint64_t)-1;

    uint64_t old = t->sig.handlers[sig];
    t->sig.handlers[sig] = handler;
    return old;
}
