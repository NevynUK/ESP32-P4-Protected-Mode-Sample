/*
 * umode.S -- the M-mode side of the U-mode window.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Three things live here:
 *
 *   umode_enter()      drops the core to U-mode running the supplied context,
 *                      and returns to its caller when the user traps;
 *   umode_trap_entry   the private trap vector that is installed for exactly
 *                      the duration of that window;
 *   umode_read_*()     CSR reads for the kernel's boot report.
 *
 * The design is a coroutine, not a scheduler.  umode_enter() is an ordinary C
 * call made from a FreeRTOS task; it parks the kernel's callee-saved registers
 * and trap vectors, mrets to U-mode, and every path back -- syscall, fault or
 * interrupt -- returns through the same call.  Blocking work therefore happens
 * in the caller, in M-mode, in task context, where FreeRTOS APIs are legal.
 * The trap vector itself does no dispatching at all.
 *
 * Three ESP32-P4 specifics drive the shape of the code:
 *
 *  - The core is a CLIC, not a CLINT.  mtvec carries mode 3 in its low bits
 *    and mtvt (CSR 0x307, non-standard on the pre-rev3 part) points at a
 *    vector table.  Both are saved and restored around the window so IDF's own
 *    vectoring is untouched outside it.
 *
 *  - mcause[29:28] is write-through aliased to mstatus.MPP, so of the two,
 *    whichever is written LAST decides the privilege an mret returns to.
 *    mstatus is written last, deliberately, everywhere below.
 *
 *  - ESP-IDF's hardware stack guard watches sp against the bounds of the
 *    current FreeRTOS task, and the window deliberately leaves that stack.
 *    umode_enter() hands the guard over to the user's stack and the trap
 *    vector hands it back; see the two blocks marked
 *    CONFIG_ESP_SYSTEM_HW_STACK_GUARD.  This is also why the trap vector
 *    swaps the context pointer through t0 rather than through sp, which is
 *    the one thing below that looks gratuitous and is not.
 *
 * Register conventions worth knowing before reading either function:
 *
 *   a0        the context pointer: umode_enter()'s argument until the user's
 *             registers are loaded, and again through the whole of the trap
 *             vector from `mv a0, t0` onwards.  Where a helper macro needs a0
 *             for something else it is parked in t3 and put straight back.
 *   t0        the context pointer for the first half of the trap vector,
 *             before a0 has been saved and is free to take over.
 *   t0, t1    scratch throughout.  In umode_enter they are the LAST two user
 *             registers loaded, because the guard hand-over needs them after
 *             every other user register is already in place.
 *   mscratch  carries the context pointer into the trap vector.  IDF does not
 *             use this CSR on this target; the kernel's value is saved and
 *             restored regardless.
 */

#include "umode.h"

/* ESP-IDF's hardware stack guard.  This header is assembler-clean and supplies
 * both the SET_BOUNDS/MONITOR_START/MONITOR_STOP macros that FreeRTOS's own
 * portasm.S uses and the ASSIST_DEBUG_* register addresses the code below
 * reads directly.  It is reachable from an application component without any
 * CMake change because esp_system/port/CMakeLists.txt puts include/private on
 * the PUBLIC side of its target_include_directories() list.
 *
 * soc_caps.h comes FIRST, and it is load-bearing.  The _CUR_CORE macros in
 * hw_stack_guard.h carry a real "which core am I?" dispatch on mhartid, but it
 * is behind `#if SOC_CPU_CORES_NUM > 1` -- and hal/assist_debug_ll.h, the only
 * thing hw_stack_guard.h includes, pulls soc_caps.h in on the C side of its own
 * __ASSEMBLER__ guard.  Without the include below the constant is undefined
 * here, the dispatch is preprocessed away, and every _CUR_CORE macro collapses
 * to its _CPU0 variant.  IDF's own portasm.S includes soc_caps.h for exactly
 * this reason, which is also why FreeRTOS keeps BOTH cores' bounds up to date
 * and why the hand-over below can read the current core's back out again.
 *
 * soc_caps.h is safe in assembly: it has no #includes of its own and defines
 * nothing but constants.
 */

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
#  include "esp_private/hw_stack_guard.h"
#endif

/* Non-standard CLIC CSR numbers on the pre-rev3 ESP32-P4: mtvt is 0x307 and
 * mintstatus is 0x346 rather than the ratified 0xfb1.
 */

#define CSR_MTVT        0x307
#define CSR_MINTSTATUS  0x346

/* Kernel save area on umode_enter()'s own stack frame */

#define KFRAME_SIZE     64
#define KF_RA            0
#define KF_GP            4
#define KF_TP            8
#define KF_S0           12
#define KF_S1           16
#define KF_S2           20
#define KF_S3           24
#define KF_S4           28
#define KF_S5           32
#define KF_S6           36
#define KF_S7           40
#define KF_S8           44
#define KF_S9           48
#define KF_S10          52
#define KF_S11          56

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD

/* The assist_debug registers this file reads directly -- INTR_RAW, INTR_CLR,
 * SP_MIN, SP_MAX and SP_PC -- come in one block per core, and hw_stack_guard.h
 * names only the CORE_0 copies as addresses (its own per-core handling is
 * inside the macros, which is no help to a plain lw/sw).  The two blocks are a
 * fixed stride apart, so the current core's copy is the CORE_0 address plus
 * mhartid * that stride.  kernel_main.c _Static_asserts the stride against
 * IDF's headers for all five registers, so a change there is a build error
 * rather than a wrong address at run time.
 */

#define ASSIST_DEBUG_CORE_STRIDE \
  (ASSIST_DEBUG_CORE_1_INTR_ENA_REG - ASSIST_DEBUG_CORE_0_INTR_ENA_REG)

/* UMODE_CORE_OFF reg -- put mhartid * ASSIST_DEBUG_CORE_STRIDE in \reg, ready
 * to be added to any CORE_0 address below.  Costs one csrr, so it is done once
 * per block rather than once per access, and \reg has to stay live across the
 * block: both call sites use t2, which no ESP_HW_STACK_GUARD macro touches.
 */

  .macro UMODE_CORE_OFF reg
  csrr  \reg, mhartid
#if ASSIST_DEBUG_CORE_STRIDE == 0x80
  slli  \reg, \reg, 7
#else
#  error "ASSIST_DEBUG_CORE_STRIDE is no longer 0x80; fix the shift above"
#endif
  .endm

/* UMODE_ASSIST_ADDR reg, core0_reg, off -- the current core's copy of
 * \core0_reg into \reg, given an \off already loaded by UMODE_CORE_OFF.
 */

  .macro UMODE_ASSIST_ADDR reg, core0_reg, off
  li    \reg, \core0_reg
  add   \reg, \reg, \off
  .endm

#endif /* CONFIG_ESP_SYSTEM_HW_STACK_GUARD */

/* Everything here runs in the trap path, so it goes in IRAM rather than in
 * flash where an evicted cache line would put a fetch inside the window.
 */

  .section .iram1.umode, "ax", @progbits
  .option push
  .option norvc

/****************************************************************************
 * Name: umode_enter
 *
 * Description:
 *   uint32_t umode_enter(umode_ctx_t *ctx)
 *
 *   Runs ctx in U-mode until it traps.  Returns the mcause of the trap that
 *   brought control back; ctx holds the user's registers, pc and mtval.
 *
 *   The caller must have filled in ctx->u_sp_min and ctx->u_sp_max -- the
 *   range the hardware stack guard is to watch while the window is open.  On
 *   return, ctx->sp_fired says whether the user's sp left it and ctx->sp_pc
 *   says where; the caller is expected to check sp_fired before it acts on
 *   the trap cause, because the two are independent of each other.
 *
 * Input Parameters:
 *   a0 - the context to run
 *
 * Returned Value:
 *   ctx->mcause -- the cause of the trap that ended the window.
 *
 ****************************************************************************/

  .balign 4
  .global umode_enter
  .type   umode_enter, @function
umode_enter:
  addi  sp, sp, -KFRAME_SIZE
  sw    ra,  KF_RA(sp)
  sw    gp,  KF_GP(sp)
  sw    tp,  KF_TP(sp)
  sw    s0,  KF_S0(sp)
  sw    s1,  KF_S1(sp)
  sw    s2,  KF_S2(sp)
  sw    s3,  KF_S3(sp)
  sw    s4,  KF_S4(sp)
  sw    s5,  KF_S5(sp)
  sw    s6,  KF_S6(sp)
  sw    s7,  KF_S7(sp)
  sw    s8,  KF_S8(sp)
  sw    s9,  KF_S9(sp)
  sw    s10, KF_S10(sp)
  sw    s11, KF_S11(sp)

  /* Park the kernel's trap state in the context.  This has to happen before
   * the private vector goes in: from the csrw mtvec below until the mret, a
   * trap would enter umode_trap_entry with kernel registers live and no saved
   * state to put back.
   */

  csrr  t0, mtvec
  sw    t0, UCTX_K_MTVEC(a0)
  csrr  t0, CSR_MTVT
  sw    t0, UCTX_K_MTVT(a0)
  csrr  t0, mscratch
  sw    t0, UCTX_K_MSCRATCH(a0)
  csrr  t0, CSR_MINTSTATUS
  sw    t0, UCTX_K_MINTSTATUS(a0)
  csrr  t0, mstatus
  sw    t0, UCTX_K_MSTATUS(a0)
  sw    sp, UCTX_K_SP(a0)

  /* Close that window: no M-mode interrupt from here to the mret.  The
   * caller's MIE is in the saved mstatus and comes back through the return
   * path's mret, which restores it from mstatus.MPIE.
   */

  csrci mstatus, MSTATUS_MIE

  /* Keep interrupts out of the U-mode window entirely, by raising the CLIC
   * threshold.  Two reasons this is done rather than letting them land:
   *
   *  - mstatus.MIE does NOT mask interrupts while the core is in U-mode, so
   *    the threshold is the only lever available;
   *
   *  - an interrupt taken in U-mode enters the private vector below, which
   *    hands control back to the kernel without servicing it.  A level-held
   *    source would simply re-assert once ESP-IDF's vector is back, but an
   *    edge-triggered one is claimed on entry and would be LOST.  Masking
   *    removes the question.
   *
   * The cost is that a user which computes for a long time between syscalls
   * delays the tick, so the user model here is a cooperative one.  In this
   * application the gap between syscalls is a few microseconds.
   *
   * Safe as a read-modify-write because MIE was cleared above, and the
   * register is core-local -- nothing else can be running on this hart.
   *
   * The read-back is not redundant.  IDF's own rv_utils_restore_intlevel_regval()
   * documents that a write to this register is not taken into account by the
   * CPU straight away: a subsequent load (or ~8 nops) is what forces it to
   * become active.  Without it the mret could reach U-mode before the mask
   * does.
   */

  li    t0, UMODE_CLIC_THRESH_REG
  lw    t1, 0(t0)
  sw    t1, UCTX_K_THRESH(a0)
  li    t1, UMODE_CLIC_THRESH_MASK
  sw    t1, 0(t0)
  lw    t1, 0(t0)                /* read back: see note below */

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD

  /* Hand IDF's hardware stack guard over from the host task's stack to the
   * user's, for the duration of the window.
   *
   * The assist_debug SP monitor watches sp against the bounds of whichever
   * FreeRTOS task is current -- here, user_host's stack.  The user runs on its
   * own stack in the arena, well clear of it (about 160 KB away as this builds
   * today), so the `lw sp, UCTX_X(2)(a0)` at the end of this function is an
   * out-of-bounds sp by construction.  Left
   * alone, the monitor latches a violation there; the interrupt cannot be
   * delivered while the window is masked, so it lands on the first instruction
   * after the return mret and the kernel dies in .Lkresume with a "Stack
   * protection fault" whose reported sp is the kernel's and therefore looks
   * perfectly healthy.
   *
   * All of this has to be done HERE, in assembly, rather than around the call
   * in the host task: FreeRTOS's rtos_int_exit re-arms the monitor and
   * re-points the bounds at the current task on every interrupt exit, so
   * anything done in C before the call is undone by any tick that lands before
   * the mret.  Between the csrci above and the mret nothing can preempt this
   * hart, so this is the only sequence that holds.
   */

  /* Off first.  The monitor has to be down for the bounds change as well as
   * for the sp change: moving SP_MAX below the sp the core is currently using
   * is itself a violation, and the peripheral latches it against the store
   * instruction that did it.
   */

  ESP_HW_STACK_GUARD_MONITOR_STOP_CUR_CORE t0 t1

  /* Which core's block of assist_debug registers the plain loads and stores
   * below are to use.  Held in t2 for the rest of this block; a0 is the
   * context and t0/t1/t3 are taken by the accesses themselves.
   */

  UMODE_CORE_OFF t2

  /* Clear any latched violation, so that what the trap vector reads on the way
   * out belongs to this window and nothing else.
   */

  UMODE_ASSIST_ADDR t0, ASSIST_DEBUG_CORE_0_INTR_CLR_REG, t2
  li    t1, ASSIST_DEBUG_SP_SPILL_BITS
  sw    t1, 0(t0)

  /* Park the host task's bounds.  Read from the peripheral rather than passed
   * in, because FreeRTOS is the only thing that knows them and it writes them
   * straight here on every context switch -- for whichever core it is running
   * on, since its portasm.S gets the real _CUR_CORE dispatch.
   */

  UMODE_ASSIST_ADDR t0, ASSIST_DEBUG_CORE_0_SP_MIN_REG, t2
  lw    t1, 0(t0)
  sw    t1, UCTX_K_SP_MIN(a0)
  UMODE_ASSIST_ADDR t0, ASSIST_DEBUG_CORE_0_SP_MAX_REG, t2
  lw    t1, 0(t0)
  sw    t1, UCTX_K_SP_MAX(a0)

  /* Install the user's, from the range the caller put in the context.  IDF's
   * SET_BOUNDS macro takes them in a0 and a1, so the context pointer steps
   * aside into t3 and comes straight back.
   */

  mv    t3, a0
  lw    a0, UCTX_U_SP_MIN(t3)
  lw    a1, UCTX_U_SP_MAX(t3)
  ESP_HW_STACK_GUARD_SET_BOUNDS_CUR_CORE t0
  mv    a0, t3

  /* The monitor stays OFF until sp actually holds the user's stack, which is
   * the last thing this function does before the mret.  It is restarted there.
   */

#endif

  /* Install the private vector.  mtvt must be 256-byte aligned, mtvec.base
   * 64-byte aligned, and mtvec's low bits select CLIC mode 3.
   */

  la    t0, umode_mtvt_table
  csrw  CSR_MTVT, t0
  la    t0, umode_trap_entry
  ori   t0, t0, 3
  csrw  mtvec, t0

  /* mscratch is how the trap vector finds the context.  IDF does not use this
   * CSR for anything, and the kernel's value is restored on the way out.
   */

  csrw  mscratch, a0

  /* Where U-mode resumes */

  lw    t0, UCTX_PC(a0)
  csrw  mepc, t0

  /* Enter at CLIC level 0 claiming no interrupt in service.  Written before
   * mstatus because of the MPP aliasing described at the top of the file.
   */

  csrw  mcause, zero

  /* The kernel's mstatus with MPP forced to U and MPIE set: mret drops to
   * U-mode.  Built from the saved value rather than a constant so that FS and
   * the rest of the caller's state survive the window.
   */

  lw    t0, UCTX_K_MSTATUS(a0)
  li    t1, ~(MSTATUS_MPP | MSTATUS_MIE)
  and   t0, t0, t1
  ori   t0, t0, MSTATUS_MPIE
  csrw  mstatus, t0

  /* The user's registers.  gp and tp are deliberately NOT loaded: the user
   * inherits the kernel's, so that gp-relative addressing and IDF's per-core
   * tp both work in U-mode.  They are saved on the kernel frame above and
   * restored on the way out, so a user that clobbers them cannot hurt us.
   *
   * sp and a0 come last -- a0 is the base register for all of this.
   */

  lw    ra,  UCTX_X(1)(a0)
  lw    t2,  UCTX_X(7)(a0)
  lw    s0,  UCTX_X(8)(a0)
  lw    s1,  UCTX_X(9)(a0)
  lw    a1,  UCTX_X(11)(a0)
  lw    a2,  UCTX_X(12)(a0)
  lw    a3,  UCTX_X(13)(a0)
  lw    a4,  UCTX_X(14)(a0)
  lw    a5,  UCTX_X(15)(a0)
  lw    a6,  UCTX_X(16)(a0)
  lw    a7,  UCTX_X(17)(a0)
  lw    s2,  UCTX_X(18)(a0)
  lw    s3,  UCTX_X(19)(a0)
  lw    s4,  UCTX_X(20)(a0)
  lw    s5,  UCTX_X(21)(a0)
  lw    s6,  UCTX_X(22)(a0)
  lw    s7,  UCTX_X(23)(a0)
  lw    s8,  UCTX_X(24)(a0)
  lw    s9,  UCTX_X(25)(a0)
  lw    s10, UCTX_X(26)(a0)
  lw    s11, UCTX_X(27)(a0)
  lw    t3,  UCTX_X(28)(a0)
  lw    t4,  UCTX_X(29)(a0)
  lw    t5,  UCTX_X(30)(a0)
  lw    t6,  UCTX_X(31)(a0)
  lw    sp,  UCTX_X(2)(a0)

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD

  /* sp is the user's now and is inside the bounds installed above, so the
   * monitor can come back on.  t0 and t1 are the scratch it needs, which is
   * why they are the last two user registers loaded below.
   */

  ESP_HW_STACK_GUARD_MONITOR_START_CUR_CORE t0 t1

#endif

  lw    t0,  UCTX_X(5)(a0)
  lw    t1,  UCTX_X(6)(a0)
  lw    a0,  UCTX_X(10)(a0)
  mret
  .size umode_enter, .-umode_enter

/****************************************************************************
 * Name: umode_trap_entry
 *
 * Description:
 *   The private trap vector, live only between umode_enter()'s mret and the
 *   return below.  In CLIC mode exceptions and non-vectored interrupts enter
 *   at mtvec.base, so this must be 64-byte aligned.
 *
 *   It records the trap and hands control straight back to the kernel.  No
 *   dispatch happens here: the caller of umode_enter() is an ordinary task and
 *   is a far better place to do work that might block.
 *
 *   In order, it: saves the user's registers and trap CSRs into the context;
 *   takes the hardware stack guard back off the user's stack, recording its
 *   verdict on the way; restores the kernel's CLIC threshold, vectors and sp;
 *   re-arms the guard on the host task's stack; and mrets to .Lkresume, which
 *   unwinds umode_enter()'s frame and returns to its caller.
 *
 *   The mret at the end is an mret rather than a jump so that the hardware
 *   restores mintstatus.mil from mcause.mpil.
 *
 ****************************************************************************/

  .balign 64
  .global umode_trap_entry
  .type   umode_trap_entry, @function
umode_trap_entry:

  /* The classic swap, but through t0 rather than sp.
   *
   * sp has to keep the USER's value for the whole of the save below, because
   * ESP-IDF's hardware stack guard is still watching the user's stack at this
   * point (umode_enter handed it over) and would latch a violation the instant
   * sp took a kernel address -- on every syscall, not just a real overflow.
   * Parking the context pointer in t0 instead costs nothing and lets the
   * user's sp be stored straight out rather than via mscratch.
   */

  csrrw t0, mscratch, t0         /* t0 = the context; mscratch = user's t0 */

  sw    ra,  UCTX_X(1)(t0)
  sw    sp,  UCTX_X(2)(t0)
  sw    gp,  UCTX_X(3)(t0)
  sw    tp,  UCTX_X(4)(t0)
  sw    t1,  UCTX_X(6)(t0)
  sw    t2,  UCTX_X(7)(t0)
  sw    s0,  UCTX_X(8)(t0)
  sw    s1,  UCTX_X(9)(t0)
  sw    a0,  UCTX_X(10)(t0)
  sw    a1,  UCTX_X(11)(t0)
  sw    a2,  UCTX_X(12)(t0)
  sw    a3,  UCTX_X(13)(t0)
  sw    a4,  UCTX_X(14)(t0)
  sw    a5,  UCTX_X(15)(t0)
  sw    a6,  UCTX_X(16)(t0)
  sw    a7,  UCTX_X(17)(t0)
  sw    s2,  UCTX_X(18)(t0)
  sw    s3,  UCTX_X(19)(t0)
  sw    s4,  UCTX_X(20)(t0)
  sw    s5,  UCTX_X(21)(t0)
  sw    s6,  UCTX_X(22)(t0)
  sw    s7,  UCTX_X(23)(t0)
  sw    s8,  UCTX_X(24)(t0)
  sw    s9,  UCTX_X(25)(t0)
  sw    s10, UCTX_X(26)(t0)
  sw    s11, UCTX_X(27)(t0)
  sw    t3,  UCTX_X(28)(t0)
  sw    t4,  UCTX_X(29)(t0)
  sw    t5,  UCTX_X(30)(t0)
  sw    t6,  UCTX_X(31)(t0)

  /* The user's t0, parked in mscratch by the swap above */

  csrr  t1, mscratch
  sw    t1, UCTX_X(5)(t0)

  csrr  t1, mepc
  sw    t1, UCTX_PC(t0)
  csrr  t1, mcause
  sw    t1, UCTX_MCAUSE(t0)
  csrr  t1, mtval
  sw    t1, UCTX_MTVAL(t0)

  mv    a0, t0                   /* a0 = ctx, and a0 is already saved */

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD

  /* Take the guard off the user's stack, before sp goes back to the kernel's
   * a few instructions below.  Everything from here to the mret is scratch:
   * every user register is already in the context.
   */

  ESP_HW_STACK_GUARD_MONITOR_STOP_CUR_CORE t0 t1

  /* Which core's assist_debug block to read, as on the way in.  Set before the
   * beqz below, because the far side of that branch needs it too.
   */

  UMODE_CORE_OFF t2

  /* Did it fire?  If the user's sp left the arena the monitor latched it and
   * raised a level interrupt that the window's threshold has been holding off.
   * Left pending, it would be delivered the moment the mret below restores MIE
   * -- and the kernel would take a "Stack protection fault" panic for a fault
   * that is the USER's.  So record the verdict for the host loop and clear the
   * source.  A window in which the user behaved leaves sp_fired zero.
   *
   * INTR_RAW is sticky, which is what makes this work at all: an excursion the
   * user put back before trapping is still here to be read.
   *
   * The threshold raised above only masks THIS core.  Both cores' monitors
   * drive one interrupt-matrix source, so the same violation is delivered to
   * every OTHER core as well, and any of them would panic long before this
   * code runs; kernel_main() un-routes the source on all of them once at
   * start-up to prevent that, and this block does not work without it.  There
   * is no finer lever -- on this SoC ASSIST_DEBUG_CORE_n_MONITOR_REG is
   * #defined to ASSIST_DEBUG_CORE_n_INTR_ENA_REG, so the monitor enable and
   * the interrupt enable are the same bits and cannot be separated.
   */

  UMODE_ASSIST_ADDR t0, ASSIST_DEBUG_CORE_0_INTR_RAW_REG, t2
  lw    t1, 0(t0)
  andi  t1, t1, ASSIST_DEBUG_SP_SPILL_BITS
  sw    t1, UCTX_SP_FIRED(a0)
  beqz  t1, .Lsp_ok

  UMODE_ASSIST_ADDR t0, ASSIST_DEBUG_CORE_0_SP_PC_REG, t2
  lw    t1, 0(t0)
  sw    t1, UCTX_SP_PC(a0)
  UMODE_ASSIST_ADDR t0, ASSIST_DEBUG_CORE_0_INTR_CLR_REG, t2
  li    t1, ASSIST_DEBUG_SP_SPILL_BITS
  sw    t1, 0(t0)

.Lsp_ok:                         /* joined from the beqz above when it did not fire */

  /* Put the host task's bounds back, so that the monitor is watching the right
   * stack by the time sp becomes the kernel's again and the monitor is
   * restarted -- both a few instructions below, in that order.
   */

  mv    t3, a0
  lw    a0, UCTX_K_SP_MIN(t3)
  lw    a1, UCTX_K_SP_MAX(t3)
  ESP_HW_STACK_GUARD_SET_BOUNDS_CUR_CORE t0
  mv    a0, t3

#endif

  /* Unmask.  Still safe as a read-modify-write: the trap cleared mstatus.MIE,
   * so nothing can preempt this hart until the mret below.
   */

  li    t0, UMODE_CLIC_THRESH_REG
  lw    t1, UCTX_K_THRESH(a0)
  sw    t1, 0(t0)
  lw    t1, 0(t0)                /* read back before MIE comes back */

  /* IDF's vectoring goes back in BEFORE the mret below, so that any interrupt
   * that arrived during the window is delivered to IDF's handler the instant
   * MIE comes back.
   */

  lw    t0, UCTX_K_MTVEC(a0)
  csrw  mtvec, t0
  lw    t0, UCTX_K_MTVT(a0)
  csrw  CSR_MTVT, t0
  lw    t0, UCTX_K_MSCRATCH(a0)
  csrw  mscratch, t0
  lw    sp, UCTX_K_SP(a0)

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD

  /* The kernel's sp is back and the host task's bounds are back with it, so
   * the guard can be re-armed.  Before the mret below, not after: that mret
   * restores mstatus.MIE, and the first thing an interrupt exit would do is
   * re-arm the monitor itself -- from a state we had left disabled.
   */

  ESP_HW_STACK_GUARD_MONITOR_START_CUR_CORE t0 t1

#endif

  /* Leave through an mret rather than a jump, so that mintstatus.mil is put
   * back from mcause.mpil by the hardware.
   */

  la    t0, .Lkresume
  csrw  mepc, t0

  lw    t1, UCTX_K_MSTATUS(a0)   /* the kernel's mstatus, saved on entry */

  /* mcause: return to M, no interrupt in service, mpie = the kernel's MIE,
   * mpil = the CLIC level the kernel was resting at.
   */

  li    t0, MCAUSE_MPP_M
  andi  t2, t1, MSTATUS_MIE
  slli  t2, t2, 24               /* mstatus.MIE(3) -> mcause.mpie(27) */
  or    t0, t0, t2
  lw    t2, UCTX_K_MINTSTATUS(a0)
  srli  t2, t2, 8                /* mintstatus.mil[31:24] -> mpil[23:16] */
  li    t3, MCAUSE_MPIL_MASK
  and   t2, t2, t3
  or    t0, t0, t2
  csrw  mcause, t0

  /* mstatus: the kernel's, with MPP = M and MPIE = the kernel's MIE.  Written
   * last so it, not mcause, decides the privilege.
   */

  li    t0, ~(MSTATUS_MPP | MSTATUS_MPIE | MSTATUS_MIE)
  and   t0, t1, t0
  li    t2, MSTATUS_MPP          /* MPP = 3 = M */
  or    t0, t0, t2
  andi  t2, t1, MSTATUS_MIE
  slli  t2, t2, 4                /* MIE(3) -> MPIE(7) */
  or    t0, t0, t2
  csrw  mstatus, t0
  mret

.Lkresume:

  /* Back in M-mode on the kernel stack, with the kernel's vectors, its MIE and
   * its CLIC level.  a0 is still the context.
   */

  lw    a0,  UCTX_MCAUSE(a0)     /* the return value */
  lw    ra,  KF_RA(sp)
  lw    gp,  KF_GP(sp)
  lw    tp,  KF_TP(sp)
  lw    s0,  KF_S0(sp)
  lw    s1,  KF_S1(sp)
  lw    s2,  KF_S2(sp)
  lw    s3,  KF_S3(sp)
  lw    s4,  KF_S4(sp)
  lw    s5,  KF_S5(sp)
  lw    s6,  KF_S6(sp)
  lw    s7,  KF_S7(sp)
  lw    s8,  KF_S8(sp)
  lw    s9,  KF_S9(sp)
  lw    s10, KF_S10(sp)
  lw    s11, KF_S11(sp)
  addi  sp, sp, KFRAME_SIZE
  ret
  .size umode_trap_entry, .-umode_trap_entry

/****************************************************************************
 * Name: umode_read_*
 *
 * Description:
 *   CSR reads for the kernel's boot report.  They are here rather than as
 *   inline asm so that every instruction in this project is in a .S file.
 *
 ****************************************************************************/

  .balign 4
  .global umode_read_mstatus
  .type   umode_read_mstatus, @function
umode_read_mstatus:
  csrr  a0, mstatus
  ret
  .size umode_read_mstatus, .-umode_read_mstatus

  .balign 4
  .global umode_read_mtvec
  .type   umode_read_mtvec, @function
umode_read_mtvec:
  csrr  a0, mtvec
  ret
  .size umode_read_mtvec, .-umode_read_mtvec

  .balign 4
  .global umode_read_mintstatus
  .type   umode_read_mintstatus, @function
umode_read_mintstatus:
  csrr  a0, CSR_MINTSTATUS
  ret
  .size umode_read_mintstatus, .-umode_read_mintstatus

/* uint32_t umode_read_pmpcfg(uint32_t idx) -- idx 0..3, anything else reads
 * pmpcfg0.  A switch rather than an indexed read because the CSR number is an
 * instruction field, not an operand.
 */

  .balign 4
  .global umode_read_pmpcfg
  .type   umode_read_pmpcfg, @function
umode_read_pmpcfg:
  li    t0, 1
  beq   a0, t0, 1f
  li    t0, 2
  beq   a0, t0, 2f
  li    t0, 3
  beq   a0, t0, 3f
  csrr  a0, pmpcfg0
  ret
1:
  csrr  a0, pmpcfg1
  ret
2:
  csrr  a0, pmpcfg2
  ret
3:
  csrr  a0, pmpcfg3
  ret
  .size umode_read_pmpcfg, .-umode_read_pmpcfg

  .option pop

/****************************************************************************
 * The CLIC vector table for the window.  Every entry lands on the trap entry
 * above: an interrupt taken in U-mode is simply another way back to the
 * kernel, which re-enters U-mode once the interrupt has been serviced.
 *
 * mtvt must be 256-byte aligned.  The table is read as data by the trap
 * hardware, which IRAM's read+execute grant covers.
 ****************************************************************************/

  .section .iram1.umode_mtvt, "a", @progbits
  .balign 256
  .global umode_mtvt_table
umode_mtvt_table:
  .rept 64
  .word umode_trap_entry
  .endr
  .size umode_mtvt_table, .-umode_mtvt_table
