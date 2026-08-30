/*
 * kernel_main.c -- the protected-mode (M-mode) side of the system.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ===========================================================================
 * WHAT THIS IS
 * ===========================================================================
 *
 * An ESP-IDF application split across both privilege modes of the ESP32-P4's
 * RISC-V core:
 *
 *   kernel_main()  runs in M-mode.  It is ESP-IDF and FreeRTOS as usual: it
 *                  owns the console, the scheduler and every CSR.
 *
 *   user_main()    runs in U-mode.  It cannot touch a CSR, cannot call into
 *                  FreeRTOS and cannot reach the console.  Its only way out is
 *                  `ecall`.
 *
 * The two talk over the syscall interface in syscall.h.  Three things are on
 * the console when this runs:
 *
 *   "Kernel x"     once a second, from a FreeRTOS task in M-mode
 *   "User y"       every two seconds, from U-mode via SYS_WRITE
 *   "syscall z"    every three seconds, from U-mode via SYS_PUTS -- the
 *                  kernel prints the message the user handed it
 *
 * ===========================================================================
 * HOW U-MODE IS ENTERED
 * ===========================================================================
 *
 * FreeRTOS has no concept of a user mode, so U-mode is not a task.  It is a
 * coroutine hosted by one: user_host_task() below calls umode_enter(), which
 * mrets to U-mode and returns when the user traps.  Blocking work therefore
 * happens here, in task context, where FreeRTOS APIs are legal -- SYS_DELAY_MS
 * is a real vTaskDelay(), so while U-mode is sleeping the scheduler runs
 * everything else, including the kernel task.
 *
 * Every trap comes back the same way, whatever it was:
 *
 *   ecall        a syscall.  Dispatched below, the result written into the
 *                saved a0, the saved pc stepped past the ecall, and U-mode
 *                resumed.
 *   an interrupt landed in the U-mode window.  ESP-IDF's vector is restored
 *                before umode_enter() returns, so the interrupt is serviced
 *                on the way out; the loop simply resumes U-mode where it was.
 *   a fault      reported with mcause/mtval/pc, and the user context retired.
 *
 * There is a fourth outcome that is not a trap cause at all: the hardware
 * stack guard may have fired during the window, whatever the window ended on.
 * user_host_task() checks ctx->sp_fired before it looks at the cause, for the
 * reason given in the next section.
 *
 * ===========================================================================
 * THE HARDWARE STACK GUARD FOLLOWS THE WINDOW
 * ===========================================================================
 *
 * CONFIG_ESP_SYSTEM_HW_STACK_GUARD arms the ESP32-P4's assist_debug SP monitor
 * on the bounds of whichever FreeRTOS task is current.  U-mode runs on its own
 * stack in the arena, nowhere near user_host's stack, so entering the window
 * puts sp out of bounds by construction.
 *
 * Left alone that is fatal, and it is the bug that stopped this ever running:
 * the violation is latched at the sp load in umode_enter, held off by the CLIC
 * threshold for the length of the window, and delivered the instant the return
 * mret restores MIE -- so the kernel panics with a "Stack protection fault"
 * whose reported sp is its own and looks perfectly healthy.
 *
 * umode.S therefore hands the guard OVER rather than standing it down: it
 * parks the host task's bounds, points the monitor at [u_sp_min, u_sp_max] --
 * the arena, set by user_ctx_init() below -- for the duration, and puts the
 * task's bounds back on the way out.  A user that runs its stack off either
 * end of its arena is caught by the same silicon that protects every kernel
 * task, and the trap vector reports it in ctx->sp_fired instead of letting it
 * reach IDF's panic handler.
 *
 * Two consequences land in this file rather than in umode.S:
 *
 *   - user_ctx_init() has to fill in u_sp_min/u_sp_max.  umode_enter() will
 *     happily install whatever is there, including zeros.
 *
 *   - kernel_main() has to un-route the assist_debug source on core 1.  The
 *     CLIC threshold umode.S raises masks core 0 only, and the source is
 *     shared; see the comment at the call.
 *
 * The check is made before the trap cause because the two are independent: the
 * user overflows its stack at one instruction and reaches the kernel at a
 * later, unrelated one -- very likely an ordinary syscall that has nothing
 * wrong with it.
 *
 * ===========================================================================
 * THE LIMIT OF THE ISOLATION HERE, STATED PLAINLY
 * ===========================================================================
 *
 * The privilege boundary is real: U-mode code that executes a csr instruction,
 * an mret, or anything else reserved to M-mode takes an illegal-instruction
 * trap and lands in the handler below.  The syscall interface is the only way
 * across.
 *
 * The MEMORY boundary is not what a production protected build would have, and
 * that is ESP-IDF's doing rather than a shortcut taken here.  IDF's
 * esp_cpu_configure_region_protection() programs all sixteen PMP entries with
 * the lock bit set, very early, before any application code runs.  PMP lock
 * bits cannot be cleared without Smepmp, which the ESP32-P4 does not implement.
 * The entries are therefore final for the rest of the boot, and two of them
 * matter here:
 *
 *   entry 4  [SOC_IRAM_LOW, _iram_text_end)   R+X, locked
 *   entry 5  [_iram_text_end, SOC_DRAM_HIGH)  R+W, locked
 *
 * A locked entry applies to U-mode as well as M-mode, so those two grants are
 * exactly what let user_main() run at all -- but entry 5 also means U-mode can
 * read and write all of kernel DRAM, not just its own arena.  The kernel
 * therefore validates every pointer that arrives from a syscall against the
 * arena (user_range_ok() below) rather than trusting hardware to have done it,
 * which is good practice regardless, but it is a kernel-side check, not a
 * hardware one.
 *
 * Closing that gap means stopping the entries being locked in the first place,
 * by rebuilding IDF's cpu_region_protect.c with PMP_L defined to zero and
 * re-describing the regions for a kernel/user split.  See README.md.
 *
 * The stack guard described above does not close it either, and should not be
 * mistaken for it: it watches sp, not accesses.  A user that leaves sp alone
 * and writes through a wild pointer is caught by neither, which is what
 * user_range_ok() is for.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "soc/clic_reg.h"
#include "soc/interrupts.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"

#include "syscall.h"
#include "umode.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The user application's entire world: its stack grows down from the top and
 * every pointer it hands the kernel has to land inside this.
 */

#define USER_ARENA_SIZE 8192

#define KERNEL_TASK_STACK 3072
#define USER_HOST_STACK 4096
#define KERNEL_TASK_PRIO 5
#define USER_HOST_PRIO 5

/* mtvec is a per-hart CSR and umode_enter() borrows it for the duration of the
 * window, so the host task is pinned.  The kernel task is pinned alongside it
 * only to keep the console output in a predictable order.
 */

#define DEMO_CORE 0

#define KERNEL_PERIOD_MS 1000

/* Longest SYS_PUTS message the kernel will copy out of the arena */

#define SYS_PUTS_MAX 128

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The context umode_enter() runs.  Static rather than on the host task's
 * stack: the trap vector reaches it through mscratch and it must outlive
 * every path through the window.
 */

static umode_ctx_t g_user_ctx;

/* U-mode's memory.  In .bss, so in DRAM, which is the region ESP-IDF's locked
 * PMP entry 5 grants U-mode read+write on.
 */

static uint8_t g_user_arena[USER_ARENA_SIZE] __attribute__((aligned(16)));

static SemaphoreHandle_t g_console_mux;

static volatile bool g_user_running;

/* Counters for the boot-time sanity report */

static uint32_t g_syscall_count;
static uint32_t g_irq_in_user;
static bool g_umode_confirmed;

/* Placed by the linker at the end of IRAM text; the top of the region PMP
 * entry 4 grants U-mode execute on.
 */

extern int _iram_text_end;

/****************************************************************************
 * Layout checks
 *
 * umode.S addresses the context through the UCTX_* macros in umode.h.  If the
 * struct and the macros ever drift, the failure is a wild store in a trap
 * vector; catch it at compile time instead.
 ****************************************************************************/

_Static_assert(offsetof(umode_ctx_t, x) == UCTX_X(0), "umode_ctx_t.x moved");
_Static_assert(offsetof(umode_ctx_t, pc) == UCTX_PC, "umode_ctx_t.pc moved");
_Static_assert(offsetof(umode_ctx_t, mcause) == UCTX_MCAUSE, "umode_ctx_t.mcause moved");
_Static_assert(offsetof(umode_ctx_t, mtval) == UCTX_MTVAL, "umode_ctx_t.mtval moved");
_Static_assert(offsetof(umode_ctx_t, k_sp) == UCTX_K_SP, "umode_ctx_t.k_sp moved");
_Static_assert(offsetof(umode_ctx_t, k_mtvec) == UCTX_K_MTVEC, "umode_ctx_t.k_mtvec moved");
_Static_assert(offsetof(umode_ctx_t, k_mtvt) == UCTX_K_MTVT, "umode_ctx_t.k_mtvt moved");
_Static_assert(offsetof(umode_ctx_t, k_mscratch) == UCTX_K_MSCRATCH, "umode_ctx_t.k_mscratch moved");
_Static_assert(offsetof(umode_ctx_t, k_mstatus) == UCTX_K_MSTATUS, "umode_ctx_t.k_mstatus moved");
_Static_assert(offsetof(umode_ctx_t, k_mintstatus) == UCTX_K_MINTSTATUS, "umode_ctx_t.k_mintstatus moved");
_Static_assert(offsetof(umode_ctx_t, k_thresh) == UCTX_K_THRESH, "umode_ctx_t.k_thresh moved");
_Static_assert(offsetof(umode_ctx_t, k_sp_min) == UCTX_K_SP_MIN, "umode_ctx_t.k_sp_min moved");
_Static_assert(offsetof(umode_ctx_t, k_sp_max) == UCTX_K_SP_MAX, "umode_ctx_t.k_sp_max moved");
_Static_assert(offsetof(umode_ctx_t, u_sp_min) == UCTX_U_SP_MIN, "umode_ctx_t.u_sp_min moved");
_Static_assert(offsetof(umode_ctx_t, u_sp_max) == UCTX_U_SP_MAX, "umode_ctx_t.u_sp_max moved");
_Static_assert(offsetof(umode_ctx_t, sp_fired) == UCTX_SP_FIRED, "umode_ctx_t.sp_fired moved");
_Static_assert(offsetof(umode_ctx_t, sp_pc) == UCTX_SP_PC, "umode_ctx_t.sp_pc moved");
_Static_assert(sizeof(umode_ctx_t) == UCTX_SIZE, "umode_ctx_t size changed");

/* umode.S reaches the assist_debug stack-guard registers through ESP-IDF's
 * _CUR_CORE macros, which collapse to their _CPU0 variants in assembly:
 * SOC_CPU_CORES_NUM is not defined there, so the "which core am I?" branch is
 * preprocessed away.  IDF's own portasm.S has the same property.  That makes
 * the whole guard hand-over correct only while the U-mode window runs on core
 * 0, which is a property of this file, not of umode.S -- so assert it here.
 */

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
_Static_assert(
    DEMO_CORE == 0, "umode.S drives the CORE_0 assist_debug registers "
                    "unconditionally; the U-mode window must run on core 0");
#endif

/* umode.S cannot include soc/clic_reg.h, so it carries its own copy of the
 * CLIC threshold address.  Keep the two honest.
 */

_Static_assert(
    UMODE_CLIC_THRESH_REG == CLIC_INT_THRESH_REG, "UMODE_CLIC_THRESH_REG no longer matches IDF's "
                                                  "CLIC_INT_THRESH_REG");
_Static_assert(UMODE_CLIC_THRESH_MASK == (CLIC_CPU_INT_THRESH_V << CLIC_CPU_INT_THRESH_S), "UMODE_CLIC_THRESH_MASK is not a full CLIC threshold mask");

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kprintf / kwrite
 *
 * Description:
 *   Console output, serialised.  Two tasks print -- the kernel task and the
 *   host task on the user's behalf -- and without the mutex their lines
 *   interleave mid-word.
 *
 ****************************************************************************/
static void kprintf(const char *fmt, ...)
{
    va_list ap;

    xSemaphoreTake(g_console_mux, portMAX_DELAY);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    xSemaphoreGive(g_console_mux);
}

static void kwrite(const char *buf, uint32_t len)
{
    xSemaphoreTake(g_console_mux, portMAX_DELAY);
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    xSemaphoreGive(g_console_mux);
}

/****************************************************************************
 * Name: user_range_ok
 *
 * Description:
 *   Is [addr, addr + len) entirely inside the user arena?
 *
 *   Every pointer that arrives from U-mode goes through here.  On this
 *   configuration IDF's locked PMP entry 5 would let U-mode hand us a pointer
 *   into kernel DRAM and the load would succeed, so this check -- not the
 *   hardware -- is what stops a user pointer being used to read the kernel.
 *
 ****************************************************************************/
static bool user_range_ok(uint32_t addr, uint32_t len)
{
    const uintptr_t lo = (uintptr_t) g_user_arena;
    const uintptr_t hi = lo + sizeof(g_user_arena);

    if (len > sizeof(g_user_arena))
    {
        return false;
    }

    /* addr + len cannot wrap once len is bounded by the arena size and addr is
     * at least lo, but check anyway -- the value came from U-mode.
     */

    if (addr < lo || addr > hi || (addr + len) < addr)
    {
        return false;
    }

    return (addr + len) <= hi;
}

/****************************************************************************
 * Name: user_strlen
 *
 * Description:
 *   Length of a NUL-terminated user string, bounded both by the arena and by
 *   max.  Returns -1 if the string is not terminated inside either.
 *
 ****************************************************************************/
static int user_strlen(uint32_t addr, uint32_t max)
{
    const uintptr_t hi = (uintptr_t) g_user_arena + sizeof(g_user_arena);
    const char *p = (const char *) (uintptr_t) addr;
    uint32_t n;

    if (!user_range_ok(addr, 1))
    {
        return -1;
    }

    for (n = 0; n < max; n++)
    {
        if ((uintptr_t) &p[n] >= hi)
        {
            return -1;
        }

        if (p[n] == '\0')
        {
            return (int) n;
        }
    }

    return -1;
}

/****************************************************************************
 * Name: syscall_dispatch
 *
 * Description:
 *   Service one ecall from U-mode.  Runs in M-mode, in the host task, so
 *   blocking here is legal and is exactly how SYS_DELAY_MS works.
 *
 *   On the normal path this advances the saved pc past the ecall and writes
 *   the result into the saved a0, so umode_enter() resumes the user at the
 *   instruction after the trap with the return value in place.
 *
 * Returned Value:
 *   true to resume U-mode, false to retire the user context.
 *
 ****************************************************************************/
static bool syscall_dispatch(umode_ctx_t *ctx)
{
    const uint32_t nr = ctx->x[17];   /* a7 */
    const uint32_t arg0 = ctx->x[10]; /* a0 */
    const uint32_t arg1 = ctx->x[11]; /* a1 */
    uint32_t ret = 0;

    g_syscall_count++;

    /* The trap arrived as ECALL_U rather than ECALL_M, which is the system's
     * own proof that the caller really was in U-mode.  Say so once.
     */

    if (!g_umode_confirmed)
    {
        g_umode_confirmed = true;
        kprintf(
            "KERNEL: first syscall arrived with mcause=%08lx (ECALL from "
            "U-mode) -- user_main is running unprivileged\n",
            (unsigned long) ctx->mcause);
    }

    switch (nr)
    {
        case SYS_NOP:
            break;

        case SYS_WRITE:

            /* The user's own console output: a counted buffer, emitted verbatim */

            if (!user_range_ok(arg0, arg1))
            {
                kprintf(
                    "KERNEL: SYS_WRITE rejected: buf=%08lx len=%lu outside "
                    "the user arena\n",
                    (unsigned long) arg0, (unsigned long) arg1);
                ret = SYS_ERR_FAULT;
                break;
            }

            kwrite((const char *) (uintptr_t) arg0, arg1);
            ret = arg1;
            break;

        case SYS_PUTS:
            {
                /* The message-passing syscall: U-mode hands the kernel a string and
                 * the kernel prints it.  Copied out of the arena first so that what
                 * is printed cannot change under us.
                 */

                char msg[SYS_PUTS_MAX + 1];
                const char *src;
                int len;
                int i;

                len = user_strlen(arg0, SYS_PUTS_MAX);
                if (len < 0)
                {
                    kprintf(
                        "KERNEL: SYS_PUTS rejected: msg=%08lx is not a "
                        "terminated string in the user arena\n",
                        (unsigned long) arg0);
                    ret = SYS_ERR_FAULT;
                    break;
                }

                src = (const char *) (uintptr_t) arg0;
                for (i = 0; i < len; i++)
                {
                    msg[i] = src[i];
                }

                msg[len] = '\0';

                kprintf("%s\n", msg);
                ret = 0;
            }
            break;

        case SYS_DELAY_MS:

            /* U-mode is parked here, in M-mode, on a real FreeRTOS delay, so the
             * scheduler is free to run the kernel task meanwhile.
             */

            vTaskDelay(pdMS_TO_TICKS(arg0));
            break;

        case SYS_EXIT:
            kprintf("KERNEL: user_main exited, status %lu\n", (unsigned long) arg0);
            return false;

        default:
            kprintf("KERNEL: unknown syscall %lu from pc=%08lx\n", (unsigned long) nr, (unsigned long) ctx->pc);
            ret = SYS_ERR_BADNR;
            break;
    }

    ctx->pc += 4;     /* step past the ecall */
    ctx->x[10] = ret; /* a0 = return value   */
    return true;
}

/****************************************************************************
 * Name: report_fault
 *
 * Description:
 *   Anything that came back from U-mode which was neither a syscall nor an
 *   interrupt.  In a bigger system this is where a user thread would be
 *   killed; here it stops the context and says why.
 *
 ****************************************************************************/
static void report_fault(const umode_ctx_t *ctx)
{
    const uint32_t code = ctx->mcause & MCAUSE_EXCCODE_MASK;
    const char *what;

    switch (code)
    {
        case EXC_INSN_MISALIGNED:
            what = "instruction misaligned";
            break;
        case EXC_INSN_ACCESS:
            what = "instruction access fault";
            break;
        case EXC_ILLEGAL_INSN:
            what = "illegal instruction";
            break;
        case EXC_BREAKPOINT:
            what = "breakpoint";
            break;
        case EXC_LOAD_MISALIGNED:
            what = "load misaligned";
            break;
        case EXC_LOAD_ACCESS:
            what = "load access fault";
            break;
        case EXC_STORE_MISALIGNED:
            what = "store misaligned";
            break;
        case EXC_STORE_ACCESS:
            what = "store access fault";
            break;
        case EXC_ECALL_M:
            what = "ecall from M-mode";
            break;
        default:
            what = "unexpected trap";
            break;
    }

    kprintf("KERNEL: user fault: %s\n", what);
    kprintf("KERNEL:   mcause=%08lx mtval=%08lx pc=%08lx sp=%08lx ra=%08lx\n", (unsigned long) ctx->mcause, (unsigned long) ctx->mtval, (unsigned long) ctx->pc, (unsigned long) ctx->x[2], (unsigned long) ctx->x[1]);
}

/****************************************************************************
 * Name: report_stack_fault
 *
 * Description:
 *   The hardware stack guard fired on the user's stack during the window.
 *   This is the fault the kernel would otherwise have taken itself, several
 *   instructions later and under its own name; umode.S catches the latch on
 *   the way out so it can be attributed to the code that actually caused it.
 *
 ****************************************************************************/
static void report_stack_fault(const umode_ctx_t *ctx)
{
    kprintf("KERNEL: user fault: stack pointer left the arena\n");
    kprintf("KERNEL:   sp=%08lx allowed=%08lx..%08lx detected at pc=%08lx\n", (unsigned long) ctx->x[2], (unsigned long) ctx->u_sp_min, (unsigned long) ctx->u_sp_max, (unsigned long) ctx->sp_pc);
}

/****************************************************************************
 * Name: user_ctx_init
 *
 * Description:
 *   Build the initial U-mode context: entry point, a 16-byte aligned stack at
 *   the top of the arena, and a return address that turns a user_main() which
 *   returns into a SYS_EXIT rather than a wild branch.
 *
 *   gp and tp are deliberately left at zero here; umode.S does not load them,
 *   so U-mode inherits the kernel's and gp-relative addressing works.
 *
 ****************************************************************************/
static void user_ctx_init(umode_ctx_t *ctx)
{
    uintptr_t stack_top;
    int i;

    for (i = 0; i < 32; i++)
    {
        ctx->x[i] = 0;
    }

    stack_top = (uintptr_t) g_user_arena + sizeof(g_user_arena);
    stack_top &= ~(uintptr_t) 15;

    ctx->x[1] = (uint32_t) (uintptr_t) u_exit_stub; /* ra */
    ctx->x[2] = (uint32_t) stack_top;               /* sp */
    ctx->pc = (uint32_t) (uintptr_t) user_main;
    ctx->mcause = 0;
    ctx->mtval = 0;

    /* The range ESP-IDF's hardware stack guard watches while the window is open.
     * umode.S swaps this in for the host task's bounds on the way down and back
     * out again on the way up, so a user that runs its stack off either end of
     * the arena is caught by the same silicon that protects every kernel task --
     * and is reported as a user fault rather than panicking the kernel.
     */

    ctx->u_sp_min = (uint32_t) (uintptr_t) g_user_arena;
    ctx->u_sp_max = (uint32_t) stack_top;
    ctx->sp_fired = 0;
    ctx->sp_pc = 0;
}

/****************************************************************************
 * Name: kernel_task
 *
 * Description:
 *   The M-mode half of the demonstration: "Kernel x" once a second.  An
 *   entirely ordinary FreeRTOS task -- the point is that it keeps running at
 *   its own cadence while the other task is dropping in and out of U-mode.
 *
 ****************************************************************************/
static void kernel_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    uint32_t n = 1;

    (void) arg;

    for (;;)
    {
        kprintf("Kernel %lu\n", (unsigned long) n++);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(KERNEL_PERIOD_MS));
    }
}

/****************************************************************************
 * Name: user_host_task
 *
 * Description:
 *   The U-mode half.  This task is not the user application -- it is the
 *   kernel-side host that runs it.  Each pass through the loop drops to
 *   U-mode, comes back on the next trap, and decides what to do about it.
 *
 ****************************************************************************/
static void user_host_task(void *arg)
{
    (void) arg;

    user_ctx_init(&g_user_ctx);

    kprintf("KERNEL: entering U-mode at pc=%08lx sp=%08lx\n", (unsigned long) g_user_ctx.pc, (unsigned long) g_user_ctx.x[2]);

    g_user_running = true;

    while (g_user_running)
    {
        const uint32_t cause = umode_enter(&g_user_ctx);

        /* Checked before the trap cause, because it is independent of it: the
         * user overflowed its stack at some point in the window and then got
         * here by whatever means -- very likely an ordinary syscall.
         */

        if (g_user_ctx.sp_fired != 0)
        {
            report_stack_fault(&g_user_ctx);
            g_user_running = false;
            continue;
        }

        if ((cause & MCAUSE_INTERRUPT) != 0)
        {
            /* An interrupt landed inside the U-mode window.  umode.S put
             * ESP-IDF's vector back before returning, so it has already been
             * serviced; pick U-mode up exactly where it was.
             */

            g_irq_in_user++;
            continue;
        }

        if ((cause & MCAUSE_EXCCODE_MASK) == EXC_ECALL_U)
        {
            g_user_running = syscall_dispatch(&g_user_ctx);
            continue;
        }

        report_fault(&g_user_ctx);
        g_user_running = false;
    }

    kprintf(
        "KERNEL: user context retired after %lu syscalls "
        "(%lu interrupts taken in U-mode)\n",
        (unsigned long) g_syscall_count, (unsigned long) g_irq_in_user);

    vTaskDelete(NULL);
}

/****************************************************************************
 * Name: boot_report
 *
 * Description:
 *   What the privilege and memory configuration actually is at the moment the
 *   kernel starts, printed rather than assumed.  If U-mode ever fails with an
 *   instruction access fault, this is the first thing to read.
 *
 ****************************************************************************/
static void boot_report(void)
{
    const uintptr_t iram_end = (uintptr_t) &_iram_text_end;
    const uintptr_t user_pc = (uintptr_t) user_main;
    const uintptr_t arena = (uintptr_t) g_user_arena;

    kprintf("\n");
    kprintf("=== ESP32-P4 protected-mode demo: M-mode kernel, U-mode user ===\n");
    kprintf("KERNEL: mstatus=%08lx mtvec=%08lx mintstatus=%08lx\n", (unsigned long) umode_read_mstatus(), (unsigned long) umode_read_mtvec(), (unsigned long) umode_read_mintstatus());
    kprintf("KERNEL: pmpcfg 0=%08lx 1=%08lx 2=%08lx 3=%08lx\n", (unsigned long) umode_read_pmpcfg(0), (unsigned long) umode_read_pmpcfg(1), (unsigned long) umode_read_pmpcfg(2), (unsigned long) umode_read_pmpcfg(3));
    kprintf(
        "KERNEL: IRAM text ends at %08lx; PMP entry 4 grants U-mode R+X "
        "below it\n",
        (unsigned long) iram_end);
    kprintf("KERNEL: user text  %08lx (%s)\n", (unsigned long) user_pc, (user_pc >= SOC_IRAM_LOW && user_pc < iram_end) ? "inside the U-mode execute grant" : "OUTSIDE the grant -- U-mode will fault on the fetch");
    kprintf("KERNEL: user arena %08lx..%08lx (%u bytes, stack and data)\n", (unsigned long) arena, (unsigned long) (arena + USER_ARENA_SIZE), (unsigned) USER_ARENA_SIZE);
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    kprintf("KERNEL: hardware stack guard follows the window on to that range\n");
    #if SOC_CPU_CORES_NUM > 1
    kprintf(
        "KERNEL: core 1 un-routed from the shared assist_debug source, so it\n"
        "KERNEL:   no longer reports stack guard faults of its own\n");
    #endif
#else
    kprintf("KERNEL: hardware stack guard is disabled in this build\n");
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kernel_main
 *
 * Description:
 *   The protected-mode application.  Brings up the console lock, reports the
 *   privilege configuration, and starts the two tasks.
 *
 ****************************************************************************/
void kernel_main(void)
{
    g_console_mux = xSemaphoreCreateMutex();
    configASSERT(g_console_mux != NULL);

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD && SOC_CPU_CORES_NUM > 1

    /* Take core 1 out of the assist_debug interrupt routing.
     *
     * ETS_ASSIST_DEBUG_INTR_SOURCE is ONE source in the interrupt matrix, and
     * esp_hw_stack_guard_init() runs on every core, so each core routes it to
     * its own ETS_ASSIST_DEBUG_INUM.  A violation on CORE 0's monitor is
     * therefore delivered to core 1 as well -- where the CLIC threshold that
     * umode.S raises for the window means nothing.  Core 1 takes it first, finds
     * no explanation (core 0 clears the latch on the way out of the window) and
     * panics the whole system with "ASSIST_DEBUG is not triggered BUT interrupt
     * occurred!".  Measured: core 1 won that race every time.
     *
     * The peripheral offers no way to separate the two -- on this SoC
     * ASSIST_DEBUG_CORE_0_MONITOR_REG is #defined to
     * ASSIST_DEBUG_CORE_0_INTR_ENA_REG, so the monitor enable and the interrupt
     * enable are the same bits.  Un-routing the source on core 1 is the only
     * lever, and it is one core 0 can pull, because the matrix is global.
     *
     * The cost is stated in the boot report: the other core no longer reports
     * stack guard violations of its own.  It runs nothing but ESP-IDF's idle and
     * IPC tasks here.
     *
     * Written as !DEMO_CORE rather than 1 so that it stays "the core the demo is
     * NOT on" if DEMO_CORE ever moves.  Note that umode.S could not follow such
     * a move on its own -- see the _Static_assert on DEMO_CORE above.
     */

    esp_rom_route_intr_matrix(!DEMO_CORE, ETS_ASSIST_DEBUG_INTR_SOURCE, ETS_INVALID_INUM);

#endif

    boot_report();

    xTaskCreatePinnedToCore(kernel_task, "kernel", KERNEL_TASK_STACK, NULL, KERNEL_TASK_PRIO, NULL, DEMO_CORE);

    xTaskCreatePinnedToCore(user_host_task, "user_host", USER_HOST_STACK, NULL, USER_HOST_PRIO, NULL, DEMO_CORE);
}

/****************************************************************************
 * Name: app_main
 *
 * Description:
 *   ESP-IDF's entry point.  It has already brought the SoC up and started the
 *   scheduler by the time this runs; the kernel proper is kernel_main().
 *
 ****************************************************************************/
void app_main(void)
{
    kernel_main();
}
