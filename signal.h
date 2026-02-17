/*
 * Signal support.
 *
 * Signals are delivered when returning to EL0 (after syscall or IRQ).
 * The kernel modifies the saved user state to jump to the signal handler.
 * After the handler returns, sigreturn() restores the original state.
 */
#ifndef SIGNAL_H
#define SIGNAL_H

#include "types.h"

/* Signal numbers (Linux-compatible subset) */
#define SIGHUP      1
#define SIGINT      2
#define SIGKILL     9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGCHLD    17

#define NSIG       32

/* Special handler values */
#define SIG_DFL    0    /* Default action */
#define SIG_IGN    1    /* Ignore */

/*
 * Signal frame pushed on user stack before calling handler.
 * sigreturn() pops this to restore the original state.
 *
 * Layout on user stack (grows down):
 *   [sp + 0]   = x0 (original)
 *   [sp + 8]   = x1
 *   ...
 *   [sp + 240] = x30
 *   [sp + 248] = original SP_EL0
 *   [sp + 256] = original ELR_EL1 (return PC)
 *   [sp + 264] = original SPSR_EL1
 *   [sp + 272] = signal number
 *   [sp + 280] = sigreturn trampoline (svc SIGRETURN)
 */
#define SIGFRAME_SIZE 288

/* Per-process signal state */
struct signal_state {
    uint64_t handlers[NSIG];    /* Handler addresses (0=default, 1=ignore) */
    uint32_t pending;           /* Bitmask of pending signals */
};

/*
 * Initialize signal state for a new process.
 */
void signal_init(struct signal_state *ss);

/*
 * Send a signal to a task.
 */
int signal_send(int tid, int sig);

/*
 * Check and deliver pending signals before returning to EL0.
 * Called from syscall/IRQ return path.
 * regs: saved register frame (same layout as syscall frame).
 * Returns 1 if a signal was delivered (regs modified), 0 if not.
 */
int signal_deliver(uint64_t *regs);

/*
 * Handle sigreturn — restore original state from signal frame.
 * regs: current saved register frame.
 */
void signal_return(uint64_t *regs);

/*
 * Set a signal handler. Returns the previous handler.
 */
uint64_t signal_set_handler(int sig, uint64_t handler);

#endif
