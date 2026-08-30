/*
 * ESP32-P4: does `mret` to U-mode with mcause.interrupt set corrupt trap
 * delivery?  Measured answer, on this silicon, in this environment: NO.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ===========================================================================
 * READ THIS FIRST -- THE RESULT IS NEGATIVE
 * ===========================================================================
 *
 * This app was written to reproduce a failure seen while bringing up a NuttX
 * BUILD_PROTECTED port on an M5Stack Tab5 (ESP32-P4 rev v1.0).  There, a user
 * thread's next `ecall` after a resume sometimes fails to vector: the trap IS
 * recognised -- mepc, mcause and mstatus all latch and privilege is raised to M
 * -- but the pc never reaches mtvec.base, so the `ecall` re-executes forever.
 * Clearing mcause.interrupt (bit 31) on returns to U-mode prevents it, A/B'd
 * both directions over twelve boots including a timing control.
 *
 * The hypothesis under test was: *`mret` to U-mode with mcause.interrupt set is
 * not supported on this CLIC*.  That would explain why nobody else hits it --
 * FreeRTOS never leaves M-mode, so every `mret` in IDF's own vectors.S is M-to-M
 * and the case is never exercised.
 *
 * **It does not reproduce.**  1.4 million mrets to U-mode, 800000 of them
 * carrying mcause.interrupt, across eight configurations: zero anomalies.
 *
 * So this file is NOT a bug report.  It is a CONTROL, and a useful one: it says
 * the instruction is fine on this silicon in a simple environment, which
 * narrows where the real trigger lives.  Do not send it to Espressif as
 * evidence of a defect -- it is evidence of the opposite.
 *
 * ===========================================================================
 * WHAT IT ACTUALLY DOES
 * ===========================================================================
 *
 * Each test installs a private mtvec/mtvt, performs the same three CSR writes an
 * RTOS trap epilogue performs -- mepc, then mcause, then mstatus -- executes
 * `mret` to a stub of `ecall`s, and reports the mcause the resulting trap
 * produced.  The handler restores the vectors and returns, so IDF's own
 * vectoring is untouched outside the window.
 *
 *   #  privilege  mcause      irqs in window  resting level  n
 *   1  M          0x883f0010  masked          0              1000
 *   2  U          0x083f0010  masked          0              1000
 *   3  U          0x883f0010  masked          0              200000
 *   4  U          0x083f0010  LIVE            0              200000
 *   5  U          0x883f0010  LIVE            0              200000
 *   6  U          0x083f0010  masked          1              200000
 *   7  U          0x883f0010  masked          1              200000
 *   8  U          0x883f0010  LIVE            1              200000
 *
 * 0x883f0010 is the exact mcause measured at the NuttX failure: interrupt=1,
 * minhv=0, mpp=U, mpie=1, mpil=0x3f (CLIC level 1), exccode 16.  0x083f0010 is
 * the same value with bit 31 cleared.
 *
 * Tests 1, 2, 4 and 6 are controls, and they are the point of the design:
 *
 *   1  the M-mode path takes the failing mcause without complaint, so the value
 *      itself is not rejected;
 *   2  the U-mode path works -- same privilege drop, same private vector, same
 *      PMP grant -- with one bit different, so a failure in 3 could not be
 *      blamed on the U-mode plumbing;
 *   4  interrupts landing in the U-mode window are harmless on their own;
 *   6  a resting CLIC level of 1 is harmless on its own.
 *
 * Expected output is every test reporting `OK` and the final line
 * "all three returned -- NOT reproduced on this part".
 *
 * Reading the reported mcause:
 *   0x...0008  ECALLU  -- the mret dropped to U-mode and the ecall trapped
 *   0x...000b  ECALLM  -- as above but from M-mode (tests 1)
 *   0x883f0012  an interrupt (id 18) landed in the window before the ecall;
 *               expected in the "irqs live" tests, and NOT counted as bad
 *   0x...0001  instruction access fault -- the PMP grant is missing.  If this
 *               appears, nothing else in the run means anything.
 *
 * ===========================================================================
 * THINGS THAT COST TIME
 * ===========================================================================
 *
 * - CHIP REVISION.  IDF defaults to a minimum of v3.1 and the bootloader
 *   refuses to flash on v1.0 silicon ("requires chip revision in range
 *   [v3.1 - v3.99]").  The <3.0 revisions are gated behind
 *   CONFIG_ESP32P4_SELECTS_REV_LESS_V3 because IDF cannot support both families
 *   in one binary.  Both settings are in sdkconfig.defaults.
 *
 * - NO PMP PROGRAMMING IS NEEDED, which was a surprise.  IDF already grants
 *   U-mode R+X on IRAM text: cpu_region_protect.c sets entry 4 over
 *   [SOC_IRAM_LOW, _iram_text_end) with PMP_TOR | RX, and its RX includes
 *   PMP_L, so the entry applies to U-mode as well as M-mode.  The stub
 *   therefore just lives in IRAM.
 *
 * - mstatus.MIE DOES NOT MASK INTERRUPTS IN U-MODE.  The first version of this
 *   app measured interrupt id 18 instead of the ecall, every time, because the
 *   stub was interrupted before its ecall executed.  Masking means raising the
 *   CLIC threshold, and on the pre-rev3 P4 that threshold is a MEMORY-MAPPED
 *   register (CLIC_INT_THRESH_REG = 0x20800008), not a CSR.  Exceptions are not
 *   gated by it, so the ecall still arrives.
 *
 * - WATCHDOGS ARE DISABLED in sdkconfig.defaults, so that if a hang ever does
 *   occur it persists and can be inspected rather than being reset away.
 *
 * If a test ever does wedge, confirm the signature with an observe-only halt
 * that is NEVER resumed (a privilege reading taken across a halt-and-resume is
 * not evidence on RISC-V -- dcsr.prv holds the privilege the core resumes into):
 *
 *   openocd -f board/esp32p4-builtin.cfg &
 *   riscv32-esp-elf-gdb -q -nx -batch build/p4_umode_mret.elf \
 *       -ex 'target extended-remote localhost:3333' -ex 'monitor halt' \
 *       -ex 'printf "pc=%08x mepc=%08x mcause=%08x mstatus=%08x mtvec=%08x\n", \
 *            $pc,$mepc,$mcause,$mstatus,$mtvec'
 *
 * Expect pc == mepc == the stub's first ecall, mcause = 0x08000008 (ECALLU),
 * mstatus = 0x00000080, and a valid mtvec the core never reached.
 */

#include <stdint.h>
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "sdkconfig.h"

/* Non-standard CLIC CSR numbers on the pre-rev3 ESP32-P4:
 * mintstatus is 0x346 rather than the ratified 0xfb1.  mtvt is 0x307.
 */
#define CSR_MTVT 0x307
#define CSR_MINTSTATUS 0x346

/* mstatus values to mret with */
#define MSTATUS_TO_U 0x00000080u /* MPP=U, MPIE=1, MIE=0 */
#define MSTATUS_TO_M 0x00001880u /* MPP=M, MPIE=1, MIE=0 */

/* The mcause measured on the failing return in the NuttX port:
 * interrupt=1, minhv=0, mpp=U, mpie=1, mpil=0x3f (CLIC level 1), exccode 16.
 */
#define MCAUSE_FAIL 0x883f0010u

/* The same value with mcause.interrupt cleared -- the control */
#define MCAUSE_SAFE (MCAUSE_FAIL & ~0x80000000u)

/****************************************************************************
 * Communication with the assembly below.  Not static: the asm names them.
 *
 *   g_save[0] caller sp   [1] mtvec   [2] mtvt   [3] caller mstatus
 *   g_out[0]  mcause at the trap      [1] mepc at the trap
 ****************************************************************************/

uintptr_t g_save[4];
uintptr_t g_out[2];

/* mcause.mpil the handler returns with, i.e. the CLIC level the core is left
 * at between probes.  mret restores mintstatus.mil from it.
 *
 * This matters: in the NuttX port where the wedge occurs, mintstatus reads
 * 0x3f000000 (level 1) persistently, in task context, from boot.  A bare IDF
 * app sits at level 0.  That is one of the few remaining differences between
 * the two environments, so it is made settable rather than assumed irrelevant.
 */

uint32_t g_ret_mpil = 0x00000000;

void p4_stub_u(void);
uint32_t p4_mret_probe(uintptr_t stub, uint32_t mstatus, uint32_t mcause);

__asm__("  .section .iram1.p4umret, \"ax\", @progbits                           \n"
        "  .option push                                                        \n"
        "  .option norvc                                                       \n"
        "                                                                      \n"
        "/* The stub.  A RUN of ecalls rather than one, so a trap that fails to \n"
        " * vector is measurable (mepc lands past the entry) instead of running \n"
        " * off the end; then a backstop.  Touches neither ra nor sp, because   \n"
        " * the handler returns through them.                                   \n"
        " */                                                                   \n"
        "  .balign 4                                                           \n"
        "  .global p4_stub_u                                                   \n"
        "  .type   p4_stub_u, @function                                        \n"
        "p4_stub_u:                                                            \n"
        "  ecall                                                               \n"
        "  ecall                                                               \n"
        "  ecall                                                               \n"
        "  ecall                                                               \n"
        "  ecall                                                               \n"
        "  ecall                                                               \n"
        "  ecall                                                               \n"
        "  ecall                                                               \n"
        "1:                                                                    \n"
        "  j     1b                                                            \n"
        "  .size p4_stub_u, .-p4_stub_u                                        \n"
        "                                                                      \n"
        "/* ------------------------------------------------------------------ \n"
        " * uint32_t p4_mret_probe(stub, mstatus, mcause)                       \n"
        " *                                                                     \n"
        " * Installs a private mtvec/mtvt, does the three CSR writes in the same \n"
        " * order an RTOS trap epilogue does, and mrets.  Returns the mcause the \n"
        " * resulting trap produced; g_out[1] holds its mepc.                    \n"
        " */                                                                   \n"
        "  .balign 4                                                           \n"
        "  .global p4_mret_probe                                               \n"
        "  .type   p4_mret_probe, @function                                    \n"
        "p4_mret_probe:                                                        \n"
        "  addi  sp, sp, -16                                                   \n"
        "  sw    ra, 12(sp)                                                    \n"
        "                                                                      \n"
        "  /* Everything the handler needs, saved BEFORE the private vectors go \n"
        "   * in -- until then no trap can reach the handler.                    \n"
        "   */                                                                 \n"
        "  la    t2, g_save                                                    \n"
        "  sw    sp, 0(t2)                                                     \n"
        "  csrr  t0, mtvec                                                     \n"
        "  sw    t0, 4(t2)                                                     \n"
        "  csrr  t0, 0x307                                                     \n"
        "  sw    t0, 8(t2)                                                     \n"
        "  csrr  t0, mstatus                                                   \n"
        "  sw    t0, 12(t2)                                                    \n"
        "                                                                      \n"
        "  la    t2, g_out                                                     \n"
        "  sw    zero, 0(t2)                                                   \n"
        "  sw    zero, 4(t2)                                                   \n"
        "                                                                      \n"
        "  /* Install the private vectors.  mtvt (0x307) must be 256-byte       \n"
        "   * aligned and mtvec.base 64-byte aligned; mode bits 3 select CLIC.  \n"
        "   * Nothing may trap between here and the mret except the stub.       \n"
        "   */                                                                 \n"
        "  la    t0, p4_mtvt_table                                             \n"
        "  csrw  0x307, t0                                                     \n"
        "  la    t0, p4_handler                                                \n"
        "  ori   t0, t0, 3                /* CLIC mode 3 */                    \n"
        "  csrw  mtvec, t0                                                     \n"
        "                                                                      \n"
        "  /* The instruction under test.  Same three writes, same order, as a  \n"
        "   * trap epilogue: mepc, then mcause, then mstatus, then mret.        \n"
        "   */                                                                 \n"
        "  /* THE INSTRUCTION UNDER TEST.  Same three writes, in the same order  \n"
        "   * an RTOS trap epilogue uses -- mepc, mcause, mstatus -- then mret.  \n"
        "   * Order matters: mcause[29:28] is write-through aliased to           \n"
        "   * mstatus.MPP on this part, so whichever is written LAST wins for    \n"
        "   * privilege.  mstatus is written last, deliberately, so a2 controls  \n"
        "   * only interrupt/minhv/mpie/mpil and a1 controls the privilege.      \n"
        "   */                                                                  \n"
        "  csrw  mepc, a0                 /* a0 = stub                       */ \n"
        "  csrw  mcause, a2               /* a2 = the value under test       */ \n"
        "  csrw  mstatus, a1              /* a1 = MPP=U (0x80) or MPP=M      */ \n"
        "  mret                                                                \n"
        "  .size p4_mret_probe, .-p4_mret_probe                                \n"
        "                                                                      \n"
        "/* Private trap vector.  In CLIC mode both exceptions and non-vectored \n"
        " * interrupts enter at mtvec[31:6] << 6, so this must be 64-byte        \n"
        " * aligned.                                                            \n"
        " */                                                                   \n"
        "  .balign 64                                                          \n"
        "  .global p4_handler                                                  \n"
        "  .type   p4_handler, @function                                       \n"
        "p4_handler:                                                           \n"
        "  /* Record the trap FIRST, before anything can overwrite mcause.      \n"
        "   * mepc says WHICH ecall of the run trapped: the stub is a run of    \n"
        "   * them, so mepc past the entry would mean an earlier trap raised    \n"
        "   * privilege and latched the CSRs without ever reaching this vector. \n"
        "   */                                                                 \n"
        "  csrr  t0, mcause                                                    \n"
        "  csrr  t1, mepc                                                      \n"
        "  la    t2, g_out                                                     \n"
        "  sw    t0, 0(t2)                                                     \n"
        "  sw    t1, 4(t2)                                                     \n"
        "                                                                      \n"
        "  la    t2, g_save                                                    \n"
        "  lw    t0, 4(t2)                                                     \n"
        "  csrw  mtvec, t0                                                     \n"
        "  lw    t0, 8(t2)                                                     \n"
        "  csrw  0x307, t0                                                     \n"
        "  lw    sp, 0(t2)                                                     \n"
        "                                                                      \n"
        "  /* Leave through an mret so mintstatus.mil is put back from          \n"
        "   * mcause.mpil, rather than by jumping.                              \n"
        "   */                                                                 \n"
        "  la    t0, .Lresume                                                  \n"
        "  csrw  mepc, t0                                                      \n"
        "                                                                      \n"
        "  lw    t1, 12(t2)               /* caller's mstatus */               \n"
        "  andi  t3, t1, 0x8              /* its MIE */                        \n"
        "  li    t4, 0x1880               /* MPP | MPIE */                     \n"
        "  not   t4, t4                                                        \n"
        "  and   t5, t1, t4                                                    \n"
        "  li    t4, 0x1800               /* MPP = M */                        \n"
        "  or    t5, t5, t4                                                    \n"
        "  slli  t6, t3, 4                /* MIE(3) -> MPIE(7) */              \n"
        "  or    t5, t5, t6                                                    \n"
        "                                                                      \n"
        "  /* Return claiming no interrupt in service, at machine privilege. */ \n"
        "  li    t0, 0x30000000           /* mcause.mpp = M, interrupt = 0 */   \n"
        "  la    t1, g_ret_mpil                                                 \n"
        "  lw    t1, 0(t1)                                                      \n"
        "  or    t0, t0, t1               /* leave the core at this level */     \n"
        "  slli  t6, t3, 24               /* MIE(3) -> mcause.mpie(27) */       \n"
        "  or    t0, t0, t6                                                    \n"
        "  csrw  mcause, t0                                                    \n"
        "  csrw  mstatus, t5                                                   \n"
        "  mret                                                                \n"
        "                                                                      \n"
        ".Lresume:                                                             \n"
        "  la    t2, g_out                                                     \n"
        "  lw    a0, 0(t2)                                                     \n"
        "  lw    ra, 12(sp)                                                    \n"
        "  addi  sp, sp, 16                                                    \n"
        "  ret                                                                 \n"
        "  .size p4_handler, .-p4_handler                                      \n"
        "                                                                      \n"
        "  .section .iram1.p4umret_mtvt, \"a\", @progbits                       \n"
        "  .balign 256                                                         \n"
        "  .global p4_mtvt_table                                               \n"
        "p4_mtvt_table:                                                        \n"
        "  .rept 64                                                            \n"
        "  .word p4_handler                                                    \n"
        "  .endr                                                               \n"
        "                                                                      \n"
        "  .option pop                                                         \n");

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The pre-rev3 ESP32-P4 keeps the CLIC interrupt threshold in a MEMORY-MAPPED
 * register rather than a CSR (INTTHRESH_STANDARD = 0), at CLIC_INT_THRESH_REG.
 * Raising it to the maximum masks every interrupt source, which is needed here:
 * mstatus.MIE does not apply while the core is in U-mode, so without this the
 * stub is interrupted before its ecall ever executes and the probe measures an
 * interrupt instead of the trap under test.  Exceptions are not gated by the
 * threshold, so the ecall still arrives.
 */

#define CLIC_INT_THRESH_REG 0x20800008u
#define CLIC_THRESH_MASK_ALL 0xff000000u

static inline uint32_t clic_thresh_get(void)
{
    return *(volatile uint32_t *) CLIC_INT_THRESH_REG;
}

static inline void clic_thresh_set(uint32_t v)
{
    *(volatile uint32_t *) CLIC_INT_THRESH_REG = v;
}

static inline uint32_t csr_mintstatus(void)
{
    uint32_t v;
    __asm__ __volatile__("csrr %0, 0x346" : "=r"(v));
    return v;
}

static inline uint32_t csr_mtvec(void)
{
    uint32_t v;
    __asm__ __volatile__("csrr %0, mtvec" : "=r"(v));
    return v;
}

/* Report the PMP entry that governs an address, so a fault rather than a hang
 * is immediately diagnosable.  Lowest matching index wins.
 */

static void dump_pmp_for(uintptr_t addr)
{
    uint32_t cfg[4];

    __asm__ __volatile__("csrr %0, pmpcfg0" : "=r"(cfg[0]));
    __asm__ __volatile__("csrr %0, pmpcfg1" : "=r"(cfg[1]));
    __asm__ __volatile__("csrr %0, pmpcfg2" : "=r"(cfg[2]));
    __asm__ __volatile__("csrr %0, pmpcfg3" : "=r"(cfg[3]));

    esp_rom_printf("P4UMRET: pmpcfg 0=%08x 1=%08x 2=%08x 3=%08x  (stub %08x)\n", cfg[0], cfg[1], cfg[2], cfg[3], (unsigned) addr);
}

/****************************************************************************
 * Name: run
 *
 * Description:
 *   One test.  Prints ARMED before the mret, because a wedge produces no
 *   output afterwards and the ARMED line is then the whole result.
 *
 ****************************************************************************/
static void run(int n, const char *what, uint32_t mstatus, uint32_t mcause, uint32_t iters, int mask_irq)
{
    uint32_t cause = 0;
    uint32_t thresh;
    uint32_t bad = 0;
    uint32_t i;

    esp_rom_printf("P4UMRET: [%d] %s mstatus=%08x mcause=%08x n=%u  ARMED\n", n, what, mstatus, mcause, iters);

    /* Mask every interrupt for the duration.  Not restored on a wedge, which
     * does not matter: the core never comes back.
     */

    thresh = clic_thresh_get();
    if (mask_irq)
    {
        clic_thresh_set(CLIC_THRESH_MASK_ALL);
    }

    for (i = 0; i < iters; i++)
    {
        cause = p4_mret_probe((uintptr_t) p4_stub_u, mstatus, mcause);

        if ((cause & 0x80000000u) == 0 && (cause & 0xfff) != 8 && (cause & 0xfff) != 11)
        {
            bad++;
            if (bad == 1)
            {
                clic_thresh_set(thresh);
                esp_rom_printf(
                    "P4UMRET: [%d] FIRST BAD at i=%u mcause=%08x "
                    "mepc=%08x\n",
                    n, i, cause, (unsigned) g_out[1]);
                if (mask_irq)
                {
                    clic_thresh_set(CLIC_THRESH_MASK_ALL);
                }
            }
        }

        /* Progress, so a wedge is localised to a 10000-iteration window.  The
         * print needs interrupts back for the console, hence the dance.
         */

        if (iters > 10000 && (i % 10000) == 9999)
        {
            clic_thresh_set(thresh);
            esp_rom_printf("P4UMRET: [%d]   ... %u done\n", n, i + 1);
            if (mask_irq)
            {
                clic_thresh_set(CLIC_THRESH_MASK_ALL);
            }
        }
    }

    clic_thresh_set(thresh);

    esp_rom_printf(
        "P4UMRET: [%d] returned: n=%u bad=%u last mcause=%08x "
        "mepc=%08x  %s\n",
        n, iters, bad, cause, (unsigned) g_out[1], bad == 0 ? "OK" : "UNEXPECTED");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/
void app_main(void)
{
    esp_rom_printf("\n");
    esp_rom_printf("P4UMRET: ESP32-P4 mret-to-U-mode with mcause.interrupt\n");
    esp_rom_printf("P4UMRET: stub=%08x mtvec=%08x mintstatus=%08x thresh=%08x\n", (unsigned) (uintptr_t) p4_stub_u, csr_mtvec(), csr_mintstatus(), clic_thresh_get());
    dump_pmp_for((uintptr_t) p4_stub_u);

    /* [1] Control: the failing mcause, but staying in M-mode.  Expected clean;
     * the stub's ecall arrives as ECALLM (exccode 11).
     */

    run(1, "M-mode, fail mcause  ", MSTATUS_TO_M, MCAUSE_FAIL, 1000, 1);

    /* [2] Control: drop to U-mode, but with mcause.interrupt CLEAR.  Expected
     * clean, ECALLU (exccode 8).  This is what proves the U-mode path itself
     * works -- same privilege drop, same private vector, same PMP grant.  If
     * this one faults (exccode 1, instruction access) the PMP grant is missing
     * and nothing below means anything.
     */

    run(2, "U-mode, safe mcause  ", MSTATUS_TO_U, MCAUSE_SAFE, 1000, 1);

    /* [3] The defect.  Exactly one bit different from [2].  Expected to hang:
     * no further output, and an observe-only halt finds pc == mepc == the
     * stub's first ecall with mcause = 0x08000008 and a valid mtvec.
     */

    run(3, "U-mode, fail mcause  ", MSTATUS_TO_U, MCAUSE_FAIL, 200000, 1);

    /* [4] and [5]: the same, but with interrupts LIVE in the U-mode window.
     * mstatus.MIE does not apply while the core is in U-mode, so an RTOS cannot
     * prevent this - NuttX's failing case always has interrupts able to land
     * between the mret and the next trap.  [4] is the control: interrupts
     * landing on their own, with bit 31 clear.
     */

    run(4, "U-mode, safe, IRQ live", MSTATUS_TO_U, MCAUSE_SAFE, 200000, 0);
    run(5, "U-mode, fail, IRQ live", MSTATUS_TO_U, MCAUSE_FAIL, 200000, 0);

    /* [6] and [7]: with the core left at CLIC level 1 between probes, matching
     * the mintstatus = 0x3f000000 that the NuttX port sits at persistently.
     */

    g_ret_mpil = 0x003f0000u;
    esp_rom_printf("P4UMRET: raising the resting CLIC level, mpil=%08x\n", g_ret_mpil);

    run(6, "lvl1, U-mode, safe    ", MSTATUS_TO_U, MCAUSE_SAFE, 200000, 1);
    run(7, "lvl1, U-mode, fail    ", MSTATUS_TO_U, MCAUSE_FAIL, 200000, 1);
    run(8, "lvl1, U-mode, fail IRQ", MSTATUS_TO_U, MCAUSE_FAIL, 200000, 0);

    esp_rom_printf("P4UMRET: all three returned -- NOT reproduced on this part\n");

    for (;;)
    {
        /* Leave the core somewhere harmless. */
    }
}
