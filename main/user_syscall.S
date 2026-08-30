/*
 * user_syscall.S -- the U-mode side of the trap.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * These are the only instructions the user application executes that are not
 * plain computation.  `ecall` is the one privileged-ish operation U-mode is
 * allowed: it raises an exception, the core switches to M-mode, and the
 * kernel's vector picks it up.  Anything else -- a csr access, an mret, a
 * peripheral the PMP does not grant -- faults instead.
 *
 * Placed in IRAM because that is the region ESP-IDF's PMP entry 4 grants
 * U-mode read+execute on; code in flash would fault on the fetch.
 *
 * Register moves are needed because the C ABI puts the syscall number in a0
 * whereas the ABI to the kernel puts it in a7, with the arguments shifted
 * down into a0/a1.
 */

#include "syscall.h"

  .section .iram1.user, "ax", @progbits
  .option push
  .option norvc

/****************************************************************************
 * Name: u_syscall0 / u_syscall1 / u_syscall2
 *
 * Description:
 *   uint32_t u_syscallN(uint32_t nr, ...)
 *
 *   The kernel resumes U-mode with every register restored from the saved
 *   context and a0 replaced by the return value, so nothing else needs
 *   saving here and `ret` finds an intact ra.
 *
 ****************************************************************************/

  .balign 4
  .global u_syscall0
  .type   u_syscall0, @function
u_syscall0:
  mv    a7, a0
  ecall
  ret
  .size u_syscall0, .-u_syscall0

  .balign 4
  .global u_syscall1
  .type   u_syscall1, @function
u_syscall1:
  mv    a7, a0
  mv    a0, a1
  ecall
  ret
  .size u_syscall1, .-u_syscall1

  .balign 4
  .global u_syscall2
  .type   u_syscall2, @function
u_syscall2:
  mv    a7, a0
  mv    a0, a1
  mv    a1, a2
  ecall
  ret
  .size u_syscall2, .-u_syscall2

/****************************************************************************
 * Name: u_exit_stub
 *
 * Description:
 *   The user context is created with ra pointing here, so a user_main() that
 *   returns asks the kernel to retire it rather than running off into
 *   whatever happens to be at address zero.  The kernel does not resume the
 *   context after SYS_EXIT; the backstop loop is there so that a kernel bug
 *   which did resume it produces a hang at a known pc instead of a wild
 *   branch.
 *
 ****************************************************************************/

  .balign 4
  .global u_exit_stub
  .type   u_exit_stub, @function
u_exit_stub:
  li    a7, SYS_EXIT
  li    a0, 0
  ecall
1:
  j     1b
  .size u_exit_stub, .-u_exit_stub

  .option pop
