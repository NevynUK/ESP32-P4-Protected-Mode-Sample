# ESP32-P4: an ESP-IDF application split across M-mode and U-mode

A teaching reference. An ESP-IDF app whose kernel runs in machine mode and
whose user applications run, genuinely, in user mode — two of them, one per
core, in the same binary, talking to the kernel over a syscall interface.

Target: M5Stack Tab5, ESP32-P4 revision **v1.0**. ESP-IDF **v5.5.4**.

This is the follow-on to the experiment in [`probe/`](probe/README.md), which
established that `mret` to U-mode works on this silicon. That was a control;
this is the thing the control was clearing the way for.

---

## How to use this document

It is written to be worked through, not skimmed. The order is deliberate:

1. **[Quick start](#quick-start)** — get it running on hardware first. Nothing
   below means much until you have watched it work.
2. **[Part 1: the concepts](#part-1--the-concepts)** — the four ideas the code
   is built out of.
3. **[Part 2: reading the code](#part-2--reading-the-code)** — a route through
   the source, with what to look for in each file.
4. **[Part 3: five things that do not work the obvious
   way](#part-3--five-things-that-do-not-work-the-obvious-way)** — the real
   content. Each is a bug that was hit, diagnosed and fixed in this tree, and
   each teaches something the datasheet does not tell you.
5. **[Part 4: exercises](#part-4--exercises)** — break it deliberately. Most
   have captured output so you can check your answer.
6. **[Part 5: the limit of the isolation](#part-5--the-limit-of-the-isolation-stated-plainly)**
   — what this does *not* protect, and why.
7. **[Reference](#reference)** — syscall table, files, build, gotchas, glossary.

If you only read one section, read **Part 3**.

## What you will learn

- How RISC-V M-mode and U-mode actually divide a real SoC, and what the
  hardware does when the boundary is crossed.
- How to run unprivileged code under an RTOS that has no concept of user mode,
  without modifying the RTOS.
- How to design a syscall interface, and why the trap handler should do as
  little as possible.
- Which processor state is per-hart, why that decides what can be shared
  between cores, and what has to be duplicated per core.
- How to read `mcause`, `mtval` and `mepc` to work out what a faulting program
  did.
- Why a memory-protection story can be *almost* right and still be worth
  nothing — and how to tell the difference.

## Prerequisites

Assumed: C, basic RISC-V assembly, and FreeRTOS tasks. Everything specific to
RISC-V privilege modes, the CLIC, the PMP and the ESP32-P4's debug assist
peripheral is explained where it is used, and collected in the
[glossary](#glossary).

Hardware is required for the exercises. Reading Parts 1–3 does not need a
board.

## Quick start

```bash
./build.sh          # configure for esp32p4 if needed, then build
./flash.sh          # build, then flash via /dev/cu.usbmodem*
./monitor.sh        # console on /dev/cu.usbserial-*
```

**The two USB paths are not interchangeable.** `/dev/cu.usbmodem*` is the P4's
USB-Serial/JTAG port, used by esptool and OpenOCD. `/dev/cu.usbserial-*` is the
external bridge on UART0, which `sdkconfig.defaults` puts the console on
deliberately — so the board can be reset and reflashed through one port while a
capture is held open on the other.

## What it does

Two U-mode applications and one M-mode task run concurrently. On boot the
kernel prints what the privilege and memory configuration actually **is**,
rather than assuming it:

```
=== ESP32-P4 protected-mode demo: M-mode kernel, U-mode user ===
KERNEL: mstatus=00010088 mtvec=4ff00003 mintstatus=00000000
KERNEL: pmpcfg 0=809d9b9b 1=8d808b8d 2=80000089 3=9b8b8d8b
KERNEL: IRAM text ends at 4ff0fa00; PMP entry 4 grants U-mode R+X below it
KERNEL: user text  4ff01d86 (inside the U-mode execute grant)
KERNEL: 2 U-mode windows, one per core, sharing that one user_main:
KERNEL:   user0 on core 0, arena 4ff150d0..4ff170d0 (8192 bytes, stack and data)
KERNEL:   user1 on core 1, arena 4ff130d0..4ff150d0 (8192 bytes, stack and data)
KERNEL: the arenas are separate objects and every syscall pointer is
KERNEL:   checked against the calling user's own bounds, so neither
KERNEL:   user can reach the other's memory through the kernel
KERNEL: hardware stack guard follows each window on to its own arena,
KERNEL:   read from the assist_debug block of the core it runs on
KERNEL: COST: both cores are un-routed from the shared assist_debug
KERNEL:   source, so ESP-IDF no longer panics on an M-mode stack
KERNEL:   overflow on either core.  The monitors still latch, and each
KERNEL:   window still reports its own user's excursions.
Kernel 1
KERNEL: user0: entering U-mode on core 0 at pc=4ff01d86 sp=4ff170d0
KERNEL: user0: first syscall arrived with mcause=08000008 (ECALL from U-mode) on core 0 -- user_main is running unprivileged
KERNEL: user1: entering U-mode on core 1 at pc=4ff01d86 sp=4ff150d0
KERNEL: user1: first syscall arrived with mcause=08000008 (ECALL from U-mode) on core 1 -- user_main is running unprivileged
```

Then all five cadences interleave indefinitely:

```
Kernel 2
Kernel 3
User 0.1
User 1.1
Kernel 4
syscall 0.1
syscall 1.1
Kernel 5
User 0.2
User 1.2
```

| line | emitted by | privilege | core | cadence | path |
|---|---|---|---|---|---|
| `Kernel x` | `kernel_task` | M | 0 | 1 s | `printf` |
| `User 0.y` | `user_main(0)` | **U** | 0 | 2 s | `SYS_WRITE` |
| `syscall 0.z` | `user_main(0)` | **U** | 0 | 3 s | `SYS_PUTS` — the kernel prints the message it was handed |
| `User 1.y` | `user_main(1)` | **U** | 1 | 2 s | `SYS_WRITE` |
| `syscall 1.z` | `user_main(1)` | **U** | 1 | 3 s | `SYS_PUTS` |

`user1` is staggered half a tick behind `user0` so the two interleave in the log
rather than landing on the same second and reading as one.

**`mcause=08000008` is the load-bearing line in that boot report.** Exception
code 8 is `ECALL_U` — an `ecall` taken *from user mode*. Code 11 would be
`ECALL_M`. The kernel is not claiming the user is unprivileged; the hardware is
telling it so, and it says so once per user so the log carries the proof for
both independently.

The cadences are independent because the user applications are asleep on a real
`vTaskDelay` between syscalls, so the scheduler runs the kernel task normally
while a U-mode window is parked.

---

# Part 1 — The concepts

## 1.1 Two privilege modes, one binary

The ESP32-P4's cores are RV32 with M and U privilege modes. ESP-IDF and
FreeRTOS live entirely in M-mode. This app carves out U-mode for the
application code:

| | M-mode (the kernel) | U-mode (the user) |
|---|---|---|
| CSR access | yes | **illegal instruction** |
| `mret`, `wfi`, other privileged instructions | yes | **illegal instruction** |
| FreeRTOS calls | yes | impossible — they take locks, locks use CSRs |
| Memory | all of it | whatever the PMP grants (see [Part 5](#part-5--the-limit-of-the-isolation-stated-plainly)) |
| Way to reach the other side | `mret` | `ecall` — the only one |

The privilege boundary is real and you can prove it in one line of user code —
see [Exercise 1](#exercise-1-execute-a-privileged-instruction).

## 1.2 U-mode is not a task; it is a coroutine hosted by one

FreeRTOS has no notion of a user mode, and this tree does not modify FreeRTOS.
So U-mode is not a task. A perfectly ordinary FreeRTOS task **hosts** it:

```
user_host_task (M-mode, FreeRTOS, pinned to one core)
    |
    +-- umode_enter(&slot->ctx) ------------.
    |     save kernel regs, mtvec, mtvt     |
    |     mask interrupts (CLIC threshold)   |
    |     hand the stack guard over          |
    |     mret, MPP=U                        v
    |                                    user_main(id)   (U-mode)
    |                                        |
    |     <--- umode_trap_entry <---------- ecall / fault / interrupt
    |     save user regs into ctx
    |     hand the stack guard back
    |     restore kernel vectors + threshold
    |     mret back to M-mode
    +-- returns mcause
    |
    +-- syscall_dispatch()  <- vTaskDelay, printf, all legal here
    +-- loop
```

The single most important design decision in this tree: **the trap vector does
no dispatching at all.** It records the trap and hands control back to ordinary
task context.

That is why `SYS_DELAY_MS` can be a real `vTaskDelay()` and `SYS_WRITE` can go
through the console driver — neither would be legal inside a trap handler, and
both are legal in a task. A trap handler that tried to service syscalls itself
would be unable to block, unable to log, and unable to use most of the RTOS.

Every way out of U-mode comes back through the same call: a syscall, a fault,
or an interrupt. The host loop tells them apart from `mcause`.

## 1.3 The syscall interface

`a7` carries the number, `a0`/`a1` the arguments, `a0` the result — the RISC-V
norm. The kernel writes the result into the saved `a0` and steps the saved `pc`
past the `ecall` before resuming, so the user resumes at the instruction after
the trap with its return value in place.

| # | call | arguments | on error |
|---|---|---|---|
| 0 | `SYS_NOP` | — | — |
| 1 | `SYS_WRITE` | buffer, length → bytes written | `SYS_ERR_FAULT`, user continues |
| 2 | `SYS_PUTS` | NUL-terminated message → 0 | `SYS_ERR_FAULT`, user continues |
| 3 | `SYS_DELAY_MS` | milliseconds | — |
| 4 | `SYS_EXIT` | status; does not return | — |

`SYS_PUTS` is message passing: U-mode hands over a string, the kernel validates
it, copies it out of the user's arena and prints it. `SYS_WRITE` is separate on
purpose — it is the counted-length console write that the user's *own* output
goes through, so the two mechanisms stay visibly distinct in the log.

`SYS_DELAY_MS` exists because U-mode cannot call FreeRTOS. Without it a user
would have to spin, which would defeat the point of showing two privilege
levels sharing a core.

Note the error column: a rejected pointer is a *recoverable* syscall error, not
a fatal fault. A privilege violation or a stack excursion retires the context.
Both behaviours are worth seeing — Exercises [1](#exercise-1-execute-a-privileged-instruction)
and [3](#exercise-3-hand-the-kernel-a-pointer-outside-your-arena).

## 1.4 One window per core, and what that forces

There are two U-mode windows, one per core, running at the same time. Getting
there is mostly a lesson in **which processor state is per-hart**.

Per-hart, therefore free: every CSR the window borrows — `mtvec`, `mtvt`,
`mscratch`, `mepc`, `mcause`, `mstatus`, `mintstatus` — plus the CLIC interrupt
threshold, which is memory-mapped but at a *core-local alias* address, and the
PMP, which ESP-IDF programs identically on both cores at boot.

Not per-hart, therefore work:

- **The debug assist peripheral has one register block per core**, and IDF's
  assembler macros name only core 0's unless `SOC_CPU_CORES_NUM` is visible.
  `umode.S` includes `soc/soc_caps.h` to make it visible, and computes
  `mhartid * 0x80` for the five registers it addresses by hand.
- **Both cores' stack monitors drive one interrupt-matrix source.** This is the
  hard one; see [3.5](#35-one-interrupt-source-two-monitors).

`umode.S` itself needs nothing per-core beyond that: its only sections are
`"ax"` and `"a"`, so it holds no writable state and both cores execute the same
window code re-entrantly.

**Each host task must stay pinned** for the length of its window.
`umode_enter()` snapshots per-hart CSRs *before* it masks interrupts, so an
unpinned task could be migrated between the snapshot and the restore and would
put core A's vectors back on core B. `configASSERT(xPortGetCoreID() ==
u->core)` checks the pin rather than assuming it. Which core each window runs
on is free — `USER_CORE(n)` in `kernel_main.c` is the mapping.

Kernel-side state is per-slot for the same reason. `user_slot_t` carries each
window's name, id, core, arena, context, run flag and counters; two host tasks
on two cores sharing one counter would be a data race with no lock in sight.
The console mutex is the only thing the two deliberately share.

---

# Part 2 — Reading the code

All assembly is in `.S` files. There is no inline assembly anywhere, including
for CSR reads — `umode_read_mstatus()` and friends are real functions in
`umode.S`.

Suggested order:

| # | file | privilege | read it for |
|---|---|---|---|
| 1 | `main/syscall.h` | both | the kernel/user ABI. Smallest file, start here |
| 2 | `main/user_main.c` | **U** | what unprivileged code can and cannot do. Note what is *absent*: no `printf`, no library calls, no writable statics |
| 3 | `main/user_syscall.S` | **U** | the `ecall` stubs and the exit trampoline. Twenty lines that are the whole user-to-kernel path |
| 4 | `main/umode.h` | both | the context layout, shared with `umode.S` as byte offsets |
| 5 | `main/kernel_main.c` | M | the kernel: boot report, `user_slot_t`, the host loop, syscall dispatch, pointer validation |
| 6 | `main/umode.S` | M | `umode_enter()`, the private trap vector, the CLIC vector table. Read last; Part 3 is mostly about this file |

Things to look for as you go:

- In `user_main.c`: **no writable statics.** Two cores run this one copy at
  once, so anything static and mutable would be shared with no lock. Every
  variable is a local — on the calling window's own arena stack — or `const`.
  `id`, delivered in the context's `a0`, is how an instance knows which it is.
- In `kernel_main.c`: `user_range_ok()` takes the **slot**, not just an address.
  [Part 5](#part-5--the-limit-of-the-isolation-stated-plainly) explains why that
  is load-bearing rather than tidy.
- In `umode.S`: the ordering comments are not decoration. Several sequences are
  correct only in the order written, and Part 3 explains four of them.

## Why the user application looks the way it does

Four constraints, all consequences of running unprivileged:

- **No library calls.** `printf` reaches the console driver, which takes
  FreeRTOS locks, which execute `csr` instructions — illegal in U-mode. The
  number formatting in `user_main.c` is therefore hand-rolled. It also keeps
  libgcc out: the only helpers the compiler wants are `divu`/`remu`, which the
  P4 has in hardware. (Verified in the disassembly — `user_main` calls nothing
  but `u_append`, `u_append_u32` and the syscall stubs.)

- **Code in IRAM.** IDF's PMP entry 4 grants U-mode read+execute over
  `[SOC_IRAM_LOW, _iram_text_end)`. `IRAM_ATTR` puts the user functions inside
  it; the same code in flash would fault on the instruction fetch. The boot
  report prints whether `user_main` actually landed inside the grant.

- **Data in DRAM.** Entry 5 grants U-mode read+write over the DRAM above.
  String constants are `DRAM_ATTR` rather than left in flash rodata — flash
  rodata happens to be U-readable in this configuration, but depending on that
  would tie the app to a PMP entry it has no reason to need.

- **No writable statics**, as above, because there is more than one instance.

`gp` and `tp` are deliberately **not** loaded from the user context: U-mode
inherits the kernel's, so gp-relative addressing would work if the compiler
chose it. In the current build it does not — the constants are reached
PC-relative (`addi a2,a2,-1336`), and no user function references `gp` at all.
Either way both registers are saved on the kernel frame and restored on the way
out, so a user that clobbers them cannot hurt the kernel.

---

# Part 3 — Five things that do not work the obvious way

Each of these was a real bug in this tree. They are the reason the code is
shaped the way it is, and they are the most transferable content here.

## 3.1 The hardware stack guard fires on the stack switch

`CONFIG_ESP_SYSTEM_HW_STACK_GUARD` arms the ESP32-P4's debug assist SP monitor
on the bounds of whichever FreeRTOS task is current. U-mode runs on its own
stack in its arena, nowhere near the host task's stack, so the `lw sp` at the
end of `umode_enter` is an out-of-bounds `sp` **by construction**.

Left alone that is fatal, and it was: this is the bug that stopped the demo ever
running. **The failure is worth studying because none of it points at the
cause.** The violation raises an interrupt, but the window is masked, so nothing
is delivered until the return `mret` restores `mstatus.MIE` — at which point the
kernel takes a `Stack protection fault` panic on the first instruction of
`.Lkresume`, reporting the *kernel's* `sp`, which is comfortably inside the
bounds printed beside it. The only field naming the real instruction is the PC
the peripheral latched, which the panic prints as `Detected in task "user_host"
at 0x…`.

Rather than stand the guard down, `umode.S` **hands it over**: it saves the host
task's bounds, points the monitor at the arena for the duration of the window,
and puts the task's bounds back on the way out. A user that runs its stack off
either end of its arena is caught by the same silicon that protects every
kernel task, and reported as a *user* fault.

Four things make it work, and each is a thing that fails if you do it the
obvious way:

- **The swap has to happen in assembly, inside the masked region.** FreeRTOS's
  `rtos_int_exit` re-arms the monitor and re-points the bounds at the current
  task on *every* interrupt exit, so anything done in C around the
  `umode_enter()` call is undone by any tick that lands before the `mret`.
  Between `csrci mstatus, MSTATUS_MIE` and the `mret` nothing can preempt the
  hart, which makes that the only window where the swap holds. IDF supplies the
  assembler macros in `esp_private/hw_stack_guard.h` — the same ones FreeRTOS's
  own `portasm.S` uses.

- **The monitor must be off for the bounds change, not just the `sp` change.**
  Moving `SP_MAX` below the `sp` the core is currently using is itself a
  violation, and the peripheral latches it against the store that did it.

- **The trap vector cannot put the context pointer in `sp`.** The classic entry
  sequence is `csrrw sp, mscratch, sp`, which would take `sp` to a kernel
  address while the monitor is still watching the arena — latching a violation
  on *every syscall*. So the swap goes through `t0` instead and `sp` keeps the
  user's value until the guard has been handed back. It is slightly shorter that
  way too: the user's `sp` gets stored straight out rather than via `mscratch`.

- **A real violation must not reach IDF's panic handler.** It would arrive after
  the window closes and be reported against the kernel — the same misleading
  panic as above, for a fault that is the user's. So the trap vector samples the
  current core's `INTR_RAW`, records the verdict and latched PC in the context,
  and clears the source. The host loop reports it and retires that user; the
  other user and the kernel task carry on.

Two measured details. The upper bound is **inclusive** — the initial `sp` is
exactly `u_sp_max` and does not fire. And `INTR_RAW` is **sticky**, which is
what makes the whole scheme work: an excursion the user puts back before its
next syscall is still there to be read, so a transient dip is caught with `sp`
already back in range and the latched PC pointing at the instruction that did
it. [Exercise 2](#exercise-2-run-your-stack-off-its-arena) shows exactly that.

## 3.2 The CLIC threshold write is not immediately effective

`mstatus.MIE` does **not** mask interrupts while the core is in U-mode. The only
lever is the CLIC threshold, which on the pre-rev3 P4 is a *memory-mapped*
register (`CLIC_INT_THRESH_REG`, `0x20800008`), not a CSR.

IDF's own `rv_utils_restore_intlevel_regval()` documents that a write to it is
not taken into account by the CPU straight away: a subsequent load (or ~8
`nop`s) is what forces it active. Both threshold writes in `umode.S` are
followed by a read-back. Without it the `mret` can reach U-mode before the mask
does, and an interrupt lands in a window that believes it is masked.

Why mask at all? Interrupts *could* be left live — one taken in U-mode enters
the private vector, which returns to the kernel, and IDF's vector is restored
before the `mret` so a level-held source is delivered the instant `MIE` comes
back. The probe ran 600,000 iterations that way without trouble. It is masked
anyway because an **edge-triggered** source is claimed on entry and would be
*lost*. The host loop still handles an interrupt-cause return, as a safety net,
and counts them.

The cost: a user that computes for a long time between syscalls delays the tick
on its core, so the user model here is a cooperative one. In this application
the gap between syscalls is a few microseconds.

## 3.3 `mcause` aliases `mstatus.MPP`

On the pre-rev3 P4 the CLIC packs previous privilege, previous interrupt-enable
and previous interrupt level into `mcause`:

| bits | field |
|---|---|
| 31 | interrupt |
| 29:28 | `mpp` — **write-through aliased to `mstatus.MPP`** |
| 27 | `mpie` |
| 23:16 | `mpil` — `mret` restores `mintstatus.mil` from this |
| 11:0 | exception code |

Because 29:28 and `mstatus.MPP` are the same state, **whichever of the two is
written last decides the privilege the `mret` returns to.** `umode.S` writes
`mstatus` last everywhere, deliberately, and says so at both sites. Get the
order wrong and an `mret` intended for U-mode returns to M-mode — silently, with
the user's registers loaded.

This is also why the exit path leaves through an `mret` rather than a jump: the
hardware restores `mintstatus.mil` from `mcause.mpil` as part of the `mret`, and
a jump would leave the core at the wrong interrupt level.

## 3.4 The context pointer travels in `mscratch`

`mscratch` is how the trap vector finds the context. Nothing in ESP-IDF uses
that CSR on this target, which is what makes it available; the kernel's value is
saved and restored regardless, so the assumption is not load-bearing.

The entry sequence is `csrrw t0, mscratch, t0` — a swap that leaves the context
in `t0` and the *user's* `t0` parked in `mscratch`, to be recovered a few
instructions later. Using `t0` rather than the conventional `sp` is what
[3.1](#31-the-hardware-stack-guard-fires-on-the-stack-switch) requires.

## 3.5 One interrupt source, two monitors

`ETS_ASSIST_DEBUG_INTR_SOURCE` is **one** source in the interrupt matrix, and
`esp_hw_stack_guard_init()` runs on every core, so each core routes it to its own
handler. A violation on *one* core's monitor is therefore delivered to the other
core as well — where the CLIC threshold that the local window raised means
nothing. The other core takes it, finds no explanation (the first core clears the
latch on the way out of its window) and panics the whole system with
`ASSIST_DEBUG is not triggered BUT interrupt occurred!`.

There is no way to separate the monitor from its interrupt. On this SoC
`ASSIST_DEBUG_CORE_n_MONITOR_REG` is `#define`d to
`ASSIST_DEBUG_CORE_n_INTR_ENA_REG` — the same register, the same bits. Enabling
the monitor *is* enabling the interrupt.

Un-routing the source is the only lever, and the matrix is global so one core can
pull it on another's behalf. With a **single** window, the loop skipped the
window's own core, so that core kept IDF's stack-guard panic for its M-mode
stacks. With a window on **every** core there is no core left to keep it —
whichever core a violation is attributed to, some other core is in a window and
will panic on it.

So `kernel_main()` un-routes the source on every core, and the boot report states
the price:

> **ESP-IDF no longer panics on an M-mode stack overflow on either core.**

What survives: the monitors still run and still latch into their own `INTR_RAW`,
and each window still reads its own core's latch on the way out, so a U-mode
stack excursion is still caught and still attributed to the user that caused it.
The residue is that a kernel-side latch on a core is cleared by that core's next
window entry, so an M-mode overflow there goes *unreported* rather than merely
unpanicked.

This is the clearest trade-off in the tree, and a good one to argue about: it is
a real reduction in the safety net, accepted to get a second unprivileged
execution context. A design that only ever wanted one window should keep the
single-window behaviour.

---

# Part 4 — Exercises

Each of these is a few lines added to `user_main.c` or `kernel_main.c`. Outputs
marked **captured** are real console output from running the exercise on
hardware; the rest tell you what to look for.

Add the probe, `./flash.sh`, `./monitor.sh`, then take the probe out again.

## Exercise 1: execute a privileged instruction

*Does the privilege boundary actually exist?* Add to `user_main`'s loop:

```c
if (id == 1 && tick == 6)
{
    __asm__ volatile("csrr t0, mstatus" ::: "t0");
}
```

**Captured:**

```
KERNEL: user1: user fault: illegal instruction
KERNEL:   mcause=08000002 mtval=300022f3 pc=4ff01df4 sp=4ff15080 ra=4ff01dd8
KERNEL: user1: user context retired after 10 syscalls (0 interrupts taken in U-mode)
Kernel 8
Kernel 9
User 0.4
```

Three things to take from that:

- `mcause=08000002` — exception code **2**, illegal instruction. Bits 29:28 are
  zero, so the trap came from U-mode ([3.3](#33-mcause-aliases-mstatusmpp)).
- `mtval=300022f3` is **the offending instruction word**, and you can decode it
  by hand: `csrrs rd=t0, csr=0x300 (mstatus), rs1=x0` encodes as
  `0x300 << 20 | 0 << 15 | 0b010 << 12 | 5 << 7 | 0x73` = `0x300022f3`. The
  hardware handed the kernel the exact instruction the user tried to run.
- `user0` keeps going (`User 0.4`). One user faulting does not disturb the other
  core's window, or the kernel.

Try it with `mret`, `wfi`, or a write to `pmpcfg0` and compare `mtval`.

## Exercise 2: run your stack off its arena

*Does the handed-over stack guard work?* Add:

```c
if (id == 1 && tick == 6)
{
    __asm__ volatile("li   t0, -16384\n\t"
                     "add  sp, sp, t0\n\t"
                     "sub  sp, sp, t0\n\t" ::: "t0", "memory");
}
```

That moves `sp` 16 KB clear of the arena and straight back **without touching
memory**, so nothing is corrupted — the point is only to move `sp`.

**Captured:**

```
KERNEL: user1: user fault: stack pointer left its arena
KERNEL:   sp=4ff15080 allowed=4ff130d0..4ff150d0 detected at pc=4ff01de2
KERNEL: user1: user context retired after 10 syscalls (0 interrupts taken in U-mode)
User 0.4
syscall 0.3
```

Note `sp` is reported back **inside** the allowed range. The excursion was
transient, and it was caught anyway, because `INTR_RAW` is sticky
([3.1](#31-the-hardware-stack-guard-fires-on-the-stack-switch)). `detected at
pc` is the instruction the peripheral latched — the `add` that did it. The
bounds printed are **user1's own** arena, read from core 1's register block.

## Exercise 3: hand the kernel a pointer outside your arena

*Is pointer validation doing anything?* Add:

```c
if (id == 0 && tick == 4)
{
    u_syscall2(SYS_WRITE, 0x4ff00000u, 8);
}
```

**Captured:**

```
KERNEL: user0: SYS_WRITE rejected: buf=4ff00000 len=8 outside its arena
User 0.2
User 1.2
```

The important part is what does *not* happen: `user0` is not retired. A rejected
pointer is a recoverable syscall error — `SYS_ERR_FAULT` in `a0` — because a user
handing over a bad pointer is a bug in the user, not a privilege violation.
Contrast with Exercises 1 and 2, which both retire the context.

## Exercise 4: reach into the other user's arena

Take the address the boot report prints for the *other* user's arena and pass it
to `SYS_PUTS`. Expect the same rejection as Exercise 3.

This is the exercise that shows why `user_range_ok()` takes the slot rather than
checking against a single global arena. PMP entry 5 grants U-mode **all** of
DRAM (see [Part 5](#part-5--the-limit-of-the-isolation-stated-plainly)), so both
arenas are equally reachable from either user; the only thing separating them is
this check. To see the check itself rather than a user's view of it, call
`user_range_ok(&g_slots[0], (uint32_t)(uintptr_t) g_slots[1].arena, 1)` in
`kernel_main()` and print the result.

## Exercise 5: change which cores the windows run on

`USER_CORE(n)` in `kernel_main.c` maps slot to core. Try:

- swapping them, so `user0` runs on core 1;
- putting both on one core.

Both should work — the per-core machinery is resolved at run time from
`mhartid`. With both slots on one core you no longer have one window per core,
so the `USER_SLOTS == SOC_CPU_CORES_NUM` assertion is no longer describing what
you built, and the un-route in `kernel_main()` is wider than it needs to be.
Watch the two windows time-slice on a single core instead of running
concurrently.

## Exercise 6: return from `user_main`

Delete the `for (;;)` so `user_main` falls off its end. The context is created
with `ra` pointing at `u_exit_stub`, so this becomes a `SYS_EXIT` rather than a
wild branch. Expect `user_main exited, status …` and a clean retirement.

## Exercise 7: leave interrupts unmasked

Comment out the threshold raise in `umode_enter` and see how far it gets.
Predict first — [3.2](#32-the-clic-threshold-write-is-not-immediately-effective)
explains what does and does not break, and why the failure mode is a *lost*
edge-triggered interrupt rather than an immediate crash. Watch the host loop's
interrupt-in-U-mode counter in the retirement message.

---

# Part 5 — The limit of the isolation, stated plainly

**Read this before citing the project as a protected-mode example.**

The **privilege** boundary is real. U-mode code that executes a `csr`
instruction, an `mret`, or anything else reserved to M-mode takes an
illegal-instruction trap and lands in the kernel's fault handler
([Exercise 1](#exercise-1-execute-a-privileged-instruction) proves it). `ecall`
is the only way across.

The **memory** boundary is not what a production protected build would have, and
that is ESP-IDF's doing rather than a shortcut taken here.
`esp_cpu_configure_region_protection()` programs all sixteen PMP entries **with
the lock bit set**, very early, before any application code runs. PMP lock bits
cannot be cleared without Smepmp, which the ESP32-P4 does not implement. The
entries are final for the rest of the boot, and a locked entry applies to U-mode
as well as M-mode:

| entry | range | permissions |
|---|---|---|
| 4 | `[SOC_IRAM_LOW, _iram_text_end)` | R+X, locked |
| 5 | `[_iram_text_end, SOC_DRAM_HIGH)` | R+W, locked |

Those two grants are exactly what let `user_main` run at all — but entry 5 also
means U-mode can read and write **all** of kernel DRAM, and both arenas, not
just its own. So:

- the kernel validates every pointer arriving from a syscall against **the
  calling slot's** arena (`user_range_ok()`), which is good practice regardless,
  but here it is doing work the hardware would otherwise do;
- the isolation between the two users is entirely software, for the same reason;
- the stack guard does **not** close the gap and should not be mistaken for it.
  It watches `sp`, not accesses. A user that leaves `sp` alone and writes through
  a wild pointer is caught by neither, which is what `user_range_ok()` is for.

Closing the gap means stopping the entries being locked in the first place. The
NuttX `BUILD_PROTECTED` port for this board does exactly that: it drops IDF's
`cpu_region_protect.c` from the build and recompiles the same source with `PMP_L`
defined to zero (`CONFIG_ESPRESSIF_KERNEL_OWNS_PMP`), then re-describes the
regions for a kernel/user split. The same trick would work here — define
`esp_cpu_configure_region_protection()` in this component so the linker prefers
it over IDF's, and reprogram the entries in `kernel_main()`. **That is not done
in this tree**, and it is the single biggest thing a reader should understand
before drawing conclusions about what this demonstrates.

---

# Reference

## The files

| file | privilege | what it is |
|---|---|---|
| `main/kernel_main.c` | M | the kernel. Boot report, the slot table, the host loop, the syscall dispatcher, user-pointer validation |
| `main/umode.S` | M | `umode_enter()`, the private trap vector, the CLIC vector table |
| `main/user_main.c` | **U** | the user application. No library calls, no writable statics, compiled into IRAM |
| `main/user_syscall.S` | **U** | the `ecall` stubs and the exit trampoline |
| `main/umode.h` | both | the context layout, shared with `umode.S` as byte offsets |
| `main/syscall.h` | both | the kernel/user ABI |
| `probe/` | — | the earlier `mret`-to-U-mode experiment ([README](probe/README.md)) |

## Build and run

```bash
./build.sh          # configure for esp32p4 if needed, then build
./flash.sh          # build, then flash via /dev/cu.usbmodem*
./monitor.sh        # console on /dev/cu.usbserial-*
./clean.sh          # drop build/;  --full also drops sdkconfig
```

The scripts find ESP-IDF themselves — `IDF_VERSION=5.5.3 ./build.sh` picks a
different one — so there is no need to activate a toolchain beforehand. They do
not use the `useidf-*` alias because the activation script refuses to run from a
shell script and the `idf.py` it provides is a shell alias a non-interactive
shell will not expand; `scripts/idf-env.sh` asks it for its environment with
`-e` instead and calls `tools/idf.py` through the venv's python.

Both ports are optional arguments: `./flash.sh /dev/cu.usbmodem101`.

## Gotchas worth keeping

- **Chip revision.** IDF defaults to a minimum of v3.1 and the bootloader
  refuses to flash on v1.0 silicon (`requires chip revision in range [v3.1 -
  v3.99]`). `CONFIG_ESP32P4_REV_MIN_*` for the <3.0 family is gated behind
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`; both are in `sdkconfig.defaults`.

- **Watchdogs are off**, inherited from the probe, so that a hang persists and
  can be inspected with an observe-only JTAG halt rather than being reset away.

- **Non-standard CLIC CSRs.** `mtvt` is `0x307` and `mintstatus` is `0x346`
  rather than the ratified `0xfb1`.

- **`mtvt` must be 256-byte aligned and `mtvec.base` 64-byte aligned**, and
  `mtvec`'s low bits carry CLIC mode 3. Both are handled by `.balign` directives
  in `umode.S`; check them in the map file if the vector ever misbehaves.

- **The context layout is duplicated** between the C struct in `umode.h` and the
  `UCTX_*` offsets `umode.S` uses. `kernel_main.c` `_Static_assert`s every field,
  because the failure mode otherwise is a wild store inside a trap vector.

- **`soc_caps.h` must be included in `umode.S`** for IDF's `_CUR_CORE` macros to
  dispatch on `mhartid` at all. Without it they silently collapse to their
  core-0 variants — and the failure mode is not a crash but a *silently
  unreported* stack violation, because reading the wrong core's `INTR_RAW`
  returns zero. IDF's own `portasm.S` includes it for the same reason.

- **The assist_debug per-core stride is assumed to be `0x80`.**
  `kernel_main.c` `_Static_assert`s it for all five registers `umode.S`
  addresses by hand, and `umode.S` carries an `#error` if it changes.

## Glossary

| term | meaning |
|---|---|
| **hart** | hardware thread — one RISC-V core. The P4 has two |
| **M-mode** | machine mode. Full privilege. ESP-IDF and FreeRTOS run here |
| **U-mode** | user mode. Lowest privilege. No CSRs, no privileged instructions |
| **`ecall`** | the instruction that traps from a lower privilege to a higher one. The only way from U to M |
| **`mret`** | return from a trap. Also how the kernel *enters* U-mode, by setting `MPP=U` first |
| **`mcause`** | why the trap happened. On this SoC it also packs previous privilege, interrupt-enable and interrupt level |
| **`mtval`** | trap value — for an illegal instruction, the instruction word itself |
| **`mepc`** | the PC the trap interrupted, and where `mret` returns to |
| **`mtvec` / `mtvt`** | trap vector base / CLIC vector table base. Per-hart |
| **`mscratch`** | a scratch CSR, unused by ESP-IDF here, used to carry the context pointer into the trap vector |
| **CLIC** | the P4's core-local interrupt controller. Vectored, with an interrupt *level* rather than a simple enable |
| **CLIC threshold** | the only thing that masks interrupts in U-mode, since `mstatus.MIE` does not apply there |
| **PMP** | physical memory protection. Region-based permissions that apply to U-mode, and to M-mode when locked |
| **Smepmp** | the RISC-V extension that would allow clearing PMP lock bits. **Not** implemented on the P4 |
| **assist_debug** | the ESP32-P4 debug assist peripheral. Its SP monitor is what ESP-IDF's hardware stack guard uses |
| **window** | one entry into and return from U-mode: `umode_enter()` to the trap that ends it |
| **arena** | one user's memory — its stack and data. One per slot |
| **slot** | `user_slot_t`: everything one U-mode window owns |

## Status

Runs on hardware. Verified on an M5Stack Tab5 (ESP32-P4 v1.0) against ESP-IDF
v5.5.4, console captured over UART0:

- the boot report confirms `user_main` lands inside the U-mode execute grant;
- **both** windows enter U-mode and report `mcause=08000008` — `ECALL from
  U-mode` — from core 0 and core 1 respectively, which is the hardware's own
  statement that each is genuinely unprivileged;
- both sustain equal syscall rates indefinitely with no faults and no panics,
  while the kernel task keeps its own cadence;
- the console excerpts in [What it does](#what-it-does) and in
  [Part 4](#part-4--exercises) are captures, not illustrations.

Getting to a single working window took one fix, described in
[3.1](#31-the-hardware-stack-guard-fires-on-the-stack-switch): IDF's hardware
stack guard fired on the U-mode stack switch and panicked the kernel on the way
back out, before any user output could be printed.

Getting to one window per core took two: making `umode.S` core-agnostic
([1.4](#14-one-window-per-core-and-what-that-forces)), and accepting the
un-route trade-off in [3.5](#35-one-interrupt-source-two-monitors). Per-slot
arenas and per-slot pointer validation came with it, and the isolation between
the two users was verified in both directions.

No probes are in the tree. Builds clean for `esp32p4`, and the placement
assumptions are verified in the ELF: user text and the trap machinery sit below
`_iram_text_end`, the arenas and user constants sit above it in DRAM, the vector
table is 256-byte aligned and the trap entry 64-byte aligned.
