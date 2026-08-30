/*
 * umode.h -- kernel-side interface to the U-mode execution window.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The ESP32-P4 core is an RV32 with M and U privilege modes.  ESP-IDF and
 * FreeRTOS live entirely in M-mode; this header describes the machinery that
 * drops out of M-mode into U-mode to run the user application, and that brings
 * control back to the kernel when the user traps.
 *
 * The context block below is shared with umode.S, so the byte offsets are
 * spelled out as macros and checked against the C struct with _Static_asserts
 * in kernel_main.c.  Change one and the assert catches the other.
 *
 * It carries four groups of fields, and it is worth knowing which is which
 * before changing any of them:
 *
 *   x[]/pc/mcause/mtval   the user's machine state.  Loaded into the core by
 *                         umode_enter(), written back by the trap vector.
 *   k_*                   kernel state parked for the length of the window
 *                         and restored on the way out.  Written by
 *                         umode_enter(); nothing else should touch it.
 *   u_sp_min/u_sp_max     configuration, written by the CALLER before
 *                         umode_enter() is called.
 *   sp_fired/sp_pc        results, written by the trap vector for the caller
 *                         to read after umode_enter() returns.
 */

#ifndef __UMODE_H
#define __UMODE_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Offsets into umode_ctx_t.  UCTX_X(n) is RISC-V register x<n>; x0 is not
 * stored but the slot is kept so the index is the register number.
 */

#define UCTX_X(n)            ((n) * 4)

#define UCTX_PC              128    /* user pc (mepc)                       */
#define UCTX_MCAUSE          132    /* mcause of the trap that came back    */
#define UCTX_MTVAL           136    /* mtval of that trap                   */

/* Kernel state parked across the U-mode window */

#define UCTX_K_SP            140
#define UCTX_K_MTVEC         144
#define UCTX_K_MTVT          148
#define UCTX_K_MSCRATCH      152
#define UCTX_K_MSTATUS       156
#define UCTX_K_MINTSTATUS    160
#define UCTX_K_THRESH        164
#define UCTX_K_SP_MIN        168    /* the host task's stack-guard bounds    */
#define UCTX_K_SP_MAX        172

/* The stack ESP-IDF's hardware stack guard watches while the window is open.
 * Filled in by the caller, not by umode_enter().
 */

#define UCTX_U_SP_MIN        176
#define UCTX_U_SP_MAX        180

/* The guard's verdict on the user's stack, written by the trap vector */

#define UCTX_SP_FIRED        184    /* nonzero if the user left its stack    */
#define UCTX_SP_PC           188    /* the pc assist_debug latched, if so    */

#define UCTX_SIZE            192

/* The pre-rev3 ESP32-P4 keeps the CLIC interrupt threshold in a MEMORY-MAPPED
 * register rather than a CSR (INTTHRESH_STANDARD = 0).  Raising it to the
 * maximum masks every interrupt source; exceptions are not gated by it, so
 * `ecall` still arrives.  This is the only way to keep interrupts out of the
 * U-mode window: mstatus.MIE does not apply while the core is in U-mode.
 *
 * Spelled out rather than taken from soc/clic_reg.h because umode.S needs it
 * and that header is not assembler-clean.  kernel_main.c static_asserts the
 * two against each other, so a change in IDF is caught at compile time.
 */

#define UMODE_CLIC_THRESH_REG   0x20800008
#define UMODE_CLIC_THRESH_MASK  0xff000000

/* mstatus fields */

#define MSTATUS_MIE          0x00000008
#define MSTATUS_MPIE         0x00000080
#define MSTATUS_MPP          0x00001800   /* 3 = M, 0 = U */

/* mcause fields.  The pre-rev3 ESP32-P4 CLIC packs the previous privilege,
 * previous interrupt-enable and previous interrupt level into mcause:
 *
 *   [31]    interrupt
 *   [29:28] mpp    -- write-through aliased to mstatus.MPP, so whichever of
 *                     the two is written LAST wins for the privilege an mret
 *                     returns to
 *   [27]    mpie
 *   [23:16] mpil   -- mret restores mintstatus.mil from this
 *   [11:0]  exccode
 */

#define MCAUSE_INTERRUPT     0x80000000
#define MCAUSE_MPP_M         0x30000000
#define MCAUSE_MPIE          0x08000000
#define MCAUSE_MPIL_MASK     0x00ff0000
#define MCAUSE_EXCCODE_MASK  0x00000fff

/* Exception codes we care about */

#define EXC_INSN_MISALIGNED  0
#define EXC_INSN_ACCESS      1
#define EXC_ILLEGAL_INSN     2
#define EXC_BREAKPOINT       3
#define EXC_LOAD_MISALIGNED  4
#define EXC_LOAD_ACCESS      5
#define EXC_STORE_MISALIGNED 6
#define EXC_STORE_ACCESS     7
#define EXC_ECALL_U          8    /* the syscall path                       */
#define EXC_ECALL_M          11

#ifndef __ASSEMBLER__

#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One user thread's machine state.  umode_enter() loads the x[] array and pc
 * into the core and drops to U-mode; the trap vector writes them back when the
 * user traps and returns to the kernel.
 */

typedef struct
{
  uint32_t x[32];              /* x[0] unused, x[1]..x[31]                  */
  uint32_t pc;                 /* where U-mode resumes                      */
  uint32_t mcause;             /* filled in by the trap vector              */
  uint32_t mtval;              /* filled in by the trap vector              */

  /* Everything below belongs to the kernel and is written by umode_enter() */

  uint32_t k_sp;
  uint32_t k_mtvec;
  uint32_t k_mtvt;
  uint32_t k_mscratch;
  uint32_t k_mstatus;
  uint32_t k_mintstatus;
  uint32_t k_thresh;
  uint32_t k_sp_min;           /* the host task's stack-guard bounds        */
  uint32_t k_sp_max;

  /* The stack ESP-IDF's hardware stack guard watches while the window is
   * open.  Written by the CALLER before umode_enter(), not by umode_enter().
   * The guard is otherwise armed on the host task's stack, which the U-mode
   * stack switch leaves by construction.
   */

  uint32_t u_sp_min;
  uint32_t u_sp_max;

  /* The guard's verdict, written by the trap vector on the way out.  Nonzero
   * sp_fired means the user's sp left [u_sp_min, u_sp_max] at some point in
   * the window; sp_pc is where.
   */

  uint32_t sp_fired;
  uint32_t sp_pc;
} umode_ctx_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* umode_enter -- run the user context until it traps.
 *
 * Saves the kernel's callee-saved registers and trap vectors, installs the
 * private vector in umode.S, and mrets to U-mode at ctx->pc.  Returns when the
 * user takes any trap at all -- syscall, fault or interrupt -- with the user's
 * registers written back into ctx.  The return value is ctx->mcause.
 *
 * Must be called from a FreeRTOS task, in M-mode, with a valid ctx.
 *
 * The caller must have set ctx->u_sp_min and ctx->u_sp_max: the range the
 * hardware stack guard watches for the length of the window.  umode_enter()
 * installs whatever is there, zeros included.
 *
 * On return, check ctx->sp_fired BEFORE acting on the return value.  A guard
 * violation is independent of the trap that ended the window -- the user
 * leaves its stack at one instruction and reaches the kernel at a later,
 * unrelated one -- so a nonzero sp_fired can accompany a perfectly ordinary
 * syscall cause.  ctx->sp_pc is the instruction the hardware latched.
 */

uint32_t umode_enter(umode_ctx_t *ctx);

/* CSR reads.  These live in umode.S rather than as inline asm so that every
 * instruction in this project is in a .S file.
 */

uint32_t umode_read_mstatus(void);
uint32_t umode_read_mtvec(void);
uint32_t umode_read_mintstatus(void);
uint32_t umode_read_pmpcfg(uint32_t idx);

#endif /* __ASSEMBLER__ */
#endif /* __UMODE_H */
