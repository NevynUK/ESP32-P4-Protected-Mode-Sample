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
 *   - kernel_main() has to un-route the assist_debug source on every core but
 *     every core.  The CLIC threshold umode.S raises masks its own core only,
 *     and the source is shared between the cores' monitors; with a window on
 *     each core there is no core left that can safely keep it.  See the comment
 *     at the call for what that costs.
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
#include "soc/assist_debug_reg.h"
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

/* One U-mode window per core, each with its own host task, its own arena and
 * its own user context.  The two are independent all the way down: every CSR
 * umode_enter() borrows is per-hart, umode.S picks its assist_debug block from
 * mhartid, and umode.S has no writable data of its own -- its only sections are
 * "ax" and "a" -- so both cores run the same window code re-entrantly.
 *
 * Each host task must stay pinned for the length of its window: umode_enter()
 * snapshots per-hart CSRs before it masks interrupts, so an unpinned host task
 * could be migrated between the snapshot and the restore.  WHICH core each one
 * is pinned to is free.
 *
 * The kernel task is pinned to USER_CORE(0) only to keep the console output in
 * a predictable order.
 */

#define USER_SLOTS 2

#define USER_CORE(n) (n)

#define KERNEL_PERIOD_MS 1000

/* Longest SYS_PUTS message the kernel will copy out of the arena */

#define SYS_PUTS_MAX 128

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* U-mode's memory: one arena per slot.  In .bss, so in DRAM, which is the
 * region ESP-IDF's locked PMP entry 5 grants U-mode read+write on.
 *
 * Separate arrays rather than one two-dimensional one so that each is its own
 * object with its own bounds, which is what user_range_ok() checks against.
 */

static uint8_t g_user0_arena[USER_ARENA_SIZE] __attribute__((aligned(16)));
static uint8_t g_user1_arena[USER_ARENA_SIZE] __attribute__((aligned(16)));

/* Everything one U-mode window owns.
 *
 * The point of gathering it here rather than in file-scope globals is that
 * TWO of these are now live at once, on two cores, running concurrently.  Any
 * mutable state left shared between them would be a cross-core data race --
 * and, in the case of the arena bounds, a hole: without a per-slot arena to
 * check against, user 0 could hand the kernel a pointer into user 1's arena
 * and user_range_ok() would wave it through.
 *
 * The console mutex below is the ONLY thing the two deliberately share.
 */

typedef struct
{
    const char *name;         /* what the kernel calls it in messages       */
    uint32_t id;              /* passed to user_main() in a0                */
    int core;                 /* the core its host task is pinned to        */
    uint8_t *arena;           /* its U-mode memory: stack and data          */
    uint32_t arena_size;

    /* The context umode_enter() runs.  It lives here rather than on the host
     * task's stack because the trap vector reaches it through mscratch and it
     * must outlive every path through the window.
     */

    umode_ctx_t ctx;

    volatile bool running;

    /* Per-slot counters.  Two host tasks on two cores incrementing one
     * counter is a data race with no lock in sight, so each keeps its own.
     */

    uint32_t syscalls;
    uint32_t irqs_in_user;
    bool umode_confirmed;
} user_slot_t;

static user_slot_t g_slots[USER_SLOTS] = {
    {
        .name = "user0",
        .id = 0,
        .core = USER_CORE(0),
        .arena = g_user0_arena,
        .arena_size = sizeof(g_user0_arena),
    },
    {
        .name = "user1",
        .id = 1,
        .core = USER_CORE(1),
        .arena = g_user1_arena,
        .arena_size = sizeof(g_user1_arena),
    },
};

static SemaphoreHandle_t g_console_mux;

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

/* umode.S reads five assist_debug registers with plain loads and stores, and
 * hw_stack_guard.h names only the CORE_0 copies as addresses.  It reaches the
 * current core's copy by adding mhartid * ASSIST_DEBUG_CORE_STRIDE, which is
 * correct only while the per-core blocks are evenly spaced and that spacing is
 * the shift UMODE_CORE_OFF applies.  Neither is something umode.S can check
 * for itself, so check all five here.
 */

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD

#define ASSIST_DEBUG_CORE_STRIDE (ASSIST_DEBUG_CORE_1_INTR_ENA_REG - ASSIST_DEBUG_CORE_0_INTR_ENA_REG)

_Static_assert(ASSIST_DEBUG_CORE_STRIDE == 0x80, "UMODE_CORE_OFF in umode.S shifts mhartid by 7 to reach "
                                                 "the current core's assist_debug block");

#define UMODE_STRIDE_OK(reg) \
  (ASSIST_DEBUG_CORE_1_##reg - ASSIST_DEBUG_CORE_0_##reg == ASSIST_DEBUG_CORE_STRIDE)

_Static_assert(UMODE_STRIDE_OK(INTR_RAW_REG), "assist_debug INTR_RAW is not one stride apart per core");
_Static_assert(UMODE_STRIDE_OK(INTR_CLR_REG), "assist_debug INTR_CLR is not one stride apart per core");
_Static_assert(UMODE_STRIDE_OK(SP_MIN_REG), "assist_debug SP_MIN is not one stride apart per core");
_Static_assert(UMODE_STRIDE_OK(SP_MAX_REG), "assist_debug SP_MAX is not one stride apart per core");
_Static_assert(UMODE_STRIDE_OK(SP_PC_REG), "assist_debug SP_PC is not one stride apart per core");

/* The stride only means anything if there is more than one block to stride
 * between, and this demo wants one window per core, so it needs exactly the
 * two it knows how to address.
 */

_Static_assert(SOC_CPU_CORES_NUM == 2, "the assist_debug stride arithmetic assumes exactly two cores");

#endif

/* One window per core, and USER_CORE(n) == n, so the slot count and the core
 * count have to agree.  If they ever stop agreeing, two slots would land on one
 * core -- which is not itself unsafe, but it is not what this demo claims to
 * show, and the un-route above would then be needlessly wide.
 */

_Static_assert(USER_SLOTS == SOC_CPU_CORES_NUM, "USER_SLOTS and SOC_CPU_CORES_NUM disagree; USER_CORE(n) maps one slot per core");

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
 *   Is [addr, addr + len) entirely inside THIS user's arena?
 *
 *   Every pointer that arrives from U-mode goes through here.  On this
 *   configuration IDF's locked PMP entry 5 would let U-mode hand us a pointer
 *   into kernel DRAM and the load would succeed, so this check -- not the
 *   hardware -- is what stops a user pointer being used to read the kernel.
 *
 *   With more than one user the bounds have to come from the SLOT rather than
 *   from a file-scope arena: entry 5 grants U-mode all of DRAM, so both arenas
 *   are equally reachable from either user, and a check against "the" arena
 *   would let user 0 read and write user 1's memory through the kernel.  The
 *   two users are isolated from each other here, in software, for exactly the
 *   same reason they are isolated from the kernel here rather than in the PMP.
 *
 ****************************************************************************/
static bool user_range_ok(const user_slot_t *u, uint32_t addr, uint32_t len)
{
    const uintptr_t lo = (uintptr_t) u->arena;
    const uintptr_t hi = lo + u->arena_size;

    if (len > u->arena_size)
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
static int user_strlen(const user_slot_t *u, uint32_t addr, uint32_t max)
{
    const uintptr_t hi = (uintptr_t) u->arena + u->arena_size;
    const char *p = (const char *) (uintptr_t) addr;
    uint32_t n;

    if (!user_range_ok(u, addr, 1))
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
static bool syscall_dispatch(user_slot_t *u)
{
    umode_ctx_t *ctx = &u->ctx;
    const uint32_t nr = ctx->x[17];   /* a7 */
    const uint32_t arg0 = ctx->x[10]; /* a0 */
    const uint32_t arg1 = ctx->x[11]; /* a1 */
    uint32_t ret = 0;

    u->syscalls++;

    /* The trap arrived as ECALL_U rather than ECALL_M, which is the system's
     * own proof that the caller really was in U-mode.  Said once per user, so
     * the log carries the proof for BOTH of them independently.
     */

    if (!u->umode_confirmed)
    {
        u->umode_confirmed = true;
        kprintf(
            "KERNEL: %s: first syscall arrived with mcause=%08lx (ECALL from "
            "U-mode) on core %d -- user_main is running unprivileged\n",
            u->name, (unsigned long) ctx->mcause, xPortGetCoreID());
    }

    switch (nr)
    {
        case SYS_NOP:
            break;

        case SYS_WRITE:

            /* The user's own console output: a counted buffer, emitted verbatim */

            if (!user_range_ok(u, arg0, arg1))
            {
                kprintf(
                    "KERNEL: %s: SYS_WRITE rejected: buf=%08lx len=%lu outside "
                    "its arena\n",
                    u->name, (unsigned long) arg0, (unsigned long) arg1);
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

                len = user_strlen(u, arg0, SYS_PUTS_MAX);
                if (len < 0)
                {
                    kprintf(
                        "KERNEL: %s: SYS_PUTS rejected: msg=%08lx is not a "
                        "terminated string in its arena\n",
                        u->name, (unsigned long) arg0);
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
            kprintf("KERNEL: %s: user_main exited, status %lu\n", u->name, (unsigned long) arg0);
            return false;

        default:
            kprintf("KERNEL: %s: unknown syscall %lu from pc=%08lx\n", u->name, (unsigned long) nr, (unsigned long) ctx->pc);
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
static void report_fault(const user_slot_t *u)
{
    const umode_ctx_t *ctx = &u->ctx;
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

    kprintf("KERNEL: %s: user fault: %s\n", u->name, what);
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
static void report_stack_fault(const user_slot_t *u)
{
    const umode_ctx_t *ctx = &u->ctx;

    kprintf("KERNEL: %s: user fault: stack pointer left its arena\n", u->name);
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
 *   a0 carries the slot's id, so the one user_main() in the image can tell
 *   which instance of itself it is.  Both users run that same code, from the
 *   same IRAM, at the same time -- it holds no writable state, so its locals
 *   live on whichever arena stack this context points at and the two
 *   instances never touch each other.
 *
 ****************************************************************************/
static void user_ctx_init(user_slot_t *u)
{
    umode_ctx_t *ctx = &u->ctx;
    uintptr_t stack_top;
    int i;

    for (i = 0; i < 32; i++)
    {
        ctx->x[i] = 0;
    }

    stack_top = (uintptr_t) u->arena + u->arena_size;
    stack_top &= ~(uintptr_t) 15;

    ctx->x[1] = (uint32_t) (uintptr_t) u_exit_stub; /* ra */
    ctx->x[2] = (uint32_t) stack_top;               /* sp */
    ctx->x[10] = u->id;                             /* a0 = user_main's arg */
    ctx->pc = (uint32_t) (uintptr_t) user_main;
    ctx->mcause = 0;
    ctx->mtval = 0;

    /* The range ESP-IDF's hardware stack guard watches while the window is open.
     * umode.S swaps this in for the host task's bounds on the way down and back
     * out again on the way up, so a user that runs its stack off either end of
     * the arena is caught by the same silicon that protects every kernel task --
     * and is reported as a user fault rather than panicking the kernel.
     */

    ctx->u_sp_min = (uint32_t) (uintptr_t) u->arena;
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
 *   One of these runs per slot, on the slot's own core, at the same time.  The
 *   loop below touches nothing outside its own slot except kprintf(), so the
 *   two instances need no lock between them: the window itself is entirely
 *   core-local, from the CSRs umode_enter() borrows to the assist_debug block
 *   it picks out of mhartid.
 *
 ****************************************************************************/
static void user_host_task(void *arg)
{
    user_slot_t *u = (user_slot_t *) arg;

    /* The pin is a correctness requirement, not a preference -- see the
     * comment on USER_CORE above -- so check it rather than assume it.
     */

    configASSERT(xPortGetCoreID() == u->core);

    user_ctx_init(u);

    kprintf("KERNEL: %s: entering U-mode on core %d at pc=%08lx sp=%08lx\n", u->name, xPortGetCoreID(), (unsigned long) u->ctx.pc, (unsigned long) u->ctx.x[2]);

    u->running = true;

    while (u->running)
    {
        const uint32_t cause = umode_enter(&u->ctx);

        /* Checked before the trap cause, because it is independent of it: the
         * user overflowed its stack at some point in the window and then got
         * here by whatever means -- very likely an ordinary syscall.
         */

        if (u->ctx.sp_fired != 0)
        {
            report_stack_fault(u);
            u->running = false;
            continue;
        }

        if ((cause & MCAUSE_INTERRUPT) != 0)
        {
            /* An interrupt landed inside the U-mode window.  umode.S put
             * ESP-IDF's vector back before returning, so it has already been
             * serviced; pick U-mode up exactly where it was.
             */

            u->irqs_in_user++;
            continue;
        }

        if ((cause & MCAUSE_EXCCODE_MASK) == EXC_ECALL_U)
        {
            u->running = syscall_dispatch(u);
            continue;
        }

        report_fault(u);
        u->running = false;
    }

    kprintf(
        "KERNEL: %s: user context retired after %lu syscalls "
        "(%lu interrupts taken in U-mode)\n",
        u->name, (unsigned long) u->syscalls, (unsigned long) u->irqs_in_user);

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
    int i;

    kprintf("\n");
    kprintf("=== ESP32-P4 protected-mode demo: M-mode kernel, U-mode user ===\n");
    kprintf("KERNEL: mstatus=%08lx mtvec=%08lx mintstatus=%08lx\n", (unsigned long) umode_read_mstatus(), (unsigned long) umode_read_mtvec(), (unsigned long) umode_read_mintstatus());
    kprintf("KERNEL: pmpcfg 0=%08lx 1=%08lx 2=%08lx 3=%08lx\n", (unsigned long) umode_read_pmpcfg(0), (unsigned long) umode_read_pmpcfg(1), (unsigned long) umode_read_pmpcfg(2), (unsigned long) umode_read_pmpcfg(3));
    kprintf(
        "KERNEL: IRAM text ends at %08lx; PMP entry 4 grants U-mode R+X "
        "below it\n",
        (unsigned long) iram_end);
    kprintf("KERNEL: user text  %08lx (%s)\n", (unsigned long) user_pc, (user_pc >= SOC_IRAM_LOW && user_pc < iram_end) ? "inside the U-mode execute grant" : "OUTSIDE the grant -- U-mode will fault on the fetch");
    kprintf("KERNEL: %d U-mode windows, one per core, sharing that one user_main:\n", USER_SLOTS);

    for (i = 0; i < USER_SLOTS; i++)
    {
        const user_slot_t *u = &g_slots[i];
        const uintptr_t lo = (uintptr_t) u->arena;

        kprintf("KERNEL:   %s on core %d, arena %08lx..%08lx (%u bytes, stack and data)\n", u->name, u->core, (unsigned long) lo, (unsigned long) (lo + u->arena_size), (unsigned) u->arena_size);
    }

    kprintf(
        "KERNEL: the arenas are separate objects and every syscall pointer is\n"
        "KERNEL:   checked against the calling user's own bounds, so neither\n"
        "KERNEL:   user can reach the other's memory through the kernel\n");

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    kprintf("KERNEL: hardware stack guard follows each window on to its own arena,\n");
    kprintf("KERNEL:   read from the assist_debug block of the core it runs on\n");
    #if SOC_CPU_CORES_NUM > 1
    kprintf(
        "KERNEL: COST: both cores are un-routed from the shared assist_debug\n"
        "KERNEL:   source, so ESP-IDF no longer panics on an M-mode stack\n"
        "KERNEL:   overflow on either core.  The monitors still latch, and each\n"
        "KERNEL:   window still reports its own user's excursions.\n");
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

    /* Take EVERY core out of the assist_debug interrupt routing.
     *
     * ETS_ASSIST_DEBUG_INTR_SOURCE is ONE source in the interrupt matrix, and
     * esp_hw_stack_guard_init() runs on every core, so each core routes it to
     * its own ETS_ASSIST_DEBUG_INUM.  A violation on one core's monitor is
     * therefore delivered to the other core as well -- where the CLIC threshold
     * that umode.S raises for its own window means nothing.  The other core
     * takes it, finds no explanation (the first core clears the latch on the way
     * out of its window) and panics the whole system with "ASSIST_DEBUG is not
     * triggered BUT interrupt occurred!".
     *
     * The peripheral offers no way to separate the two -- on this SoC
     * ASSIST_DEBUG_CORE_n_MONITOR_REG is #defined to
     * ASSIST_DEBUG_CORE_n_INTR_ENA_REG, so the monitor enable and the interrupt
     * enable are the same bits.  Un-routing the source is the only lever.
     *
     * WITH ONE WINDOW this loop skipped the window's own core, so that core kept
     * IDF's stack-guard panic for its M-mode stacks.  WITH A WINDOW ON EVERY
     * CORE there is no core left to keep it: whichever core a violation is
     * attributed to, some other core in a window has the source routed and will
     * panic on it.  So the source comes out everywhere, and the price is stated
     * plainly in the boot report -- ESP-IDF will no longer panic on an M-mode
     * stack overflow on either core.
     *
     * What is NOT lost: the monitors still run and still latch into their own
     * INTR_RAW, and each window still reads its own core's latch on the way out,
     * so a U-mode stack excursion is still caught and still attributed to the
     * user that caused it.  The residue is that a kernel-side latch on a core is
     * cleared by that core's next window entry, so an M-mode overflow there goes
     * unreported rather than merely unpanicked.
     */

    for (int core = 0; core < SOC_CPU_CORES_NUM; core++)
    {
        esp_rom_route_intr_matrix(core, ETS_ASSIST_DEBUG_INTR_SOURCE, ETS_INVALID_INUM);
    }

#endif

    boot_report();

    xTaskCreatePinnedToCore(kernel_task, "kernel", KERNEL_TASK_STACK, NULL, KERNEL_TASK_PRIO, NULL, USER_CORE(0));

    /* One host task per slot, each pinned to its slot's core.  The task name
     * carries the slot name so the two are distinguishable in any FreeRTOS
     * introspection, and the slot is the task argument -- the host task reads
     * everything it needs from it and touches no other slot.
     */

    for (int i = 0; i < USER_SLOTS; i++)
    {
        xTaskCreatePinnedToCore(user_host_task, g_slots[i].name, USER_HOST_STACK, &g_slots[i], USER_HOST_PRIO, NULL, g_slots[i].core);
    }
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
