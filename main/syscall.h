/*
 * syscall.h -- the kernel/user ABI.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared by kernel_main.c (which implements the calls) and user_main.c /
 * user_syscall.S (which make them).  This is the ONLY interface the user
 * application has to the rest of the system: it runs in U-mode, so it cannot
 * touch a CSR, cannot call into FreeRTOS, and cannot reach the console driver.
 * Everything it wants done, it asks for here.
 *
 * Calling convention follows the RISC-V norm:
 *
 *   a7      syscall number
 *   a0..a1  arguments
 *   a0      return value
 *
 * The kernel writes the return value into the saved context and advances the
 * saved pc past the ecall before resuming U-mode.
 */

#ifndef __SYSCALL_H
#define __SYSCALL_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SYS_NOP        0    /* no-op; round-trips the trap path             */
#define SYS_WRITE      1    /* a0 = buffer, a1 = length -> bytes written    */
#define SYS_PUTS       2    /* a0 = NUL-terminated message -> 0             */
#define SYS_DELAY_MS   3    /* a0 = milliseconds -> 0                       */
#define SYS_EXIT       4    /* a0 = status; does not return                 */
#define SYS_MAX        5

/* Returned in a0 when the kernel rejects the call */

#define SYS_ERR_BADNR  ((uint32_t)-1)
#define SYS_ERR_FAULT  ((uint32_t)-2)   /* pointer outside the user arena   */

#ifndef __ASSEMBLER__

#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The U-mode side of the trap.  Implemented in user_syscall.S, which lives in
 * IRAM so that it is inside the region U-mode is granted execute on.
 */

uint32_t u_syscall0(uint32_t nr);
uint32_t u_syscall1(uint32_t nr, uint32_t arg0);
uint32_t u_syscall2(uint32_t nr, uint32_t arg0, uint32_t arg1);

/* Where the user context's ra points on entry, so that a user_main() which
 * returns lands on SYS_EXIT rather than in the weeds.  Also in
 * user_syscall.S.
 */

void u_exit_stub(void);

/* The user application itself (user_main.c).  Never called directly by the
 * kernel -- its address is loaded into the user context's pc, and its argument
 * into the context's a0, by user_ctx_init().
 *
 * There is one copy of this code and more than one U-mode window running it at
 * once, on different cores.  It must therefore hold no writable static state:
 * `id` is how each instance tells itself apart, and everything else it needs
 * lives on the user stack its own context points at.
 */

void user_main(uint32_t id);

#endif /* __ASSEMBLER__ */
#endif /* __SYSCALL_H */
