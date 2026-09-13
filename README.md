# ESP32-P4: An ESP-IDF Application Split Across M-mode and U-mode

A teaching reference. An ESP-IDF app whose kernel runs in machine mode and
whose user applications run, genuinely, in user mode — eight of them, none
pinned to a core, in the same binary, talking to the kernel over a syscall
interface.

Target: M5Stack Tab5, ESP32-P4 revision **v1.0**. ESP-IDF **v5.5.4**.

This is the follow-on to the experiment in [`probe/`](probe/README.md), which
established that `mret` to U-mode works on this silicon. That was a control;
this is the thing the control was clearing the way for.

---

## How to Use This Document

It is written to be worked through, not skimmed. The order is deliberate:

1. **[Quick Start](#quick-start)** — get it running on hardware first. Nothing
   below means much until you have watched it work.
2. **[Part 1: The Concepts](#part-1--the-concepts)** — the four ideas the code
   is built out of.
3. **[Part 2: Reading the Code](#part-2--reading-the-code)** — a route through
   the source, with what to look for in each file.
4. **[Part 3: Six Things That Do Not Work the Obvious
   Way](#part-3--six-things-that-do-not-work-the-obvious-way)** — the real
   content. Each is a bug that was hit, diagnosed and fixed in this tree, and
   each teaches something the datasheet does not tell you.
5. **[Part 4: Exercises](#part-4--exercises)** — break it deliberately. Most
   have captured output so you can check your answer.
6. **[Part 5: The Limit of the Isolation](#part-5--the-limit-of-the-isolation-stated-plainly)**
   — what this does *not* protect, and why.
7. **[Reference](#reference)** — syscall table, files, build, gotchas, further
   reading, glossary.

If you only read one section, read **Part 3**.

## What You Will Learn

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

## Quick Start

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

## What It Does

Eight U-mode applications and one M-mode task run concurrently on two cores.
**Nothing is pinned:** every task is created with `tskNO_AFFINITY`, so which
core a U-mode window runs on is the scheduler's choice and changes constantly.

On boot the kernel prints what the privilege and memory configuration actually
**is**, rather than assuming it:

```
=== ESP32-P4 protected-mode demo: M-mode kernel, U-mode user ===
KERNEL: mstatus=00010088 mtvec=4ff00003 mintstatus=00000000
KERNEL: pmpcfg 0=809d9b9b 1=8d808b8d 2=80000089 3=9b8b8d8b
KERNEL: IRAM text ends at 4ff0fa00; PMP entry 4 grants U-mode R+X below it
KERNEL: user text  4ff01d86 (inside the U-mode execute grant)
KERNEL: 8 U-mode windows, NONE of them pinned, sharing that one user_main:
KERNEL:   user0 arena 4ff13690..4ff15690 (8192 bytes, stack and data)
KERNEL:   user1 arena 4ff15690..4ff17690 (8192 bytes, stack and data)
...
KERNEL:   user7 arena 4ff21690..4ff23690 (8192 bytes, stack and data)
KERNEL: the arenas are separate objects and every syscall pointer is
KERNEL:   checked against the calling user's own bounds, so no user
KERNEL:   can reach another's memory through the kernel
KERNEL: hardware stack guard follows each window on to its own arena,
KERNEL:   read from the assist_debug block of the core it runs on
Kernel 1 (core 0)
KERNEL: user0: first syscall arrived with mcause=08000008 (ECALL from U-mode) on core 0 -- user_main is running unprivileged
```

Then the windows run, and move:

```
User 0.2 on core 1
User 1.2 on core 1
User 2.2 on core 1
User 3.2 on core 0
User 4.2 on core 1
Kernel 7 (core 0)
KERNEL: window:core/moves u0:c0/578300 u1:c1/548421 u2:c0/607706 u3:c0/563506 u4:c0/557972 u5:c0/551322 u6:c1/546088 u7:c0/618362
```

| Line | Emitted by | Privilege | Says |
|---|---|---|---|
| `Kernel x (core c)` | `kernel_task` | M | An ordinary FreeRTOS task, also unpinned |
| `User n.m on core c` | `user_main(n)` | **U** | Via `SYS_WRITE`, with the core from `SYS_GETCORE` |
| `syscall n.m` | `user_main(n)` | **U** | Via `SYS_PUTS` — the kernel prints the message it was handed |
| `window:core/moves` | `kernel_task` | M | Where each window is now, and how often it has changed core |

**Two numbers in that output are the whole point.**

`mcause=08000008` is exception code 8, `ECALL_U` — an `ecall` taken *from user
mode*. Code 11 would be `ECALL_M`. The kernel is not claiming the user is
unprivileged; the hardware is telling it so, once per user, so the log carries
the proof for all eight independently.

The `on core c` in each user's own line came from **`SYS_GETCORE`**. U-mode
cannot read `mhartid` — it is a CSR, and a `csr` instruction from U-mode is an
illegal instruction ([Exercise 1](#exercise-1-execute-a-privileged-instruction))
— so the only way a user can know where it is running is to ask the kernel.
That number is therefore evidence of two things at once: the syscall interface
works, and the window is not where it was last time.

Over 40 s on hardware, each of the eight windows changed core between 450 000
and 618 000 times, with no faults.

## Why the Users Are Busy

The user loop is a burst of `SYS_YIELD` calls followed by a sleep, which is not
the obvious shape for a demo. Both halves are there for a reason, and both were
arrived at by getting it wrong first.

A user that only sleeps **never moves**. Only core 0 walks ESP-IDF's
delayed-task list ([gotchas](#gotchas-worth-keeping)), so a sleeping task is
woken by core 0 every time and runs there. Measured with a sleep-only loop:
three unpinned windows, thirty seconds, **zero** core changes between them.
Entirely correct, and it makes unpinned tasks look pinned.

Being unpinned is not enough either. Two runnable tasks on two cores is a
*stable* assignment — the scheduler has no reason to move anything. What moves
tasks is **oversubscription**, so the burst keeps each user runnable long enough
to overlap the others, and eight of them over two cores are then permanently
oversubscribed. Spreading the users evenly across the period made things worse,
not better: evenly spread means the bursts never overlap.

| Configuration | Core changes per user, 30–40 s |
|---|---|
| 3 windows, sleep only | 0 |
| 3 windows, short burst, evenly staggered | 0 |
| 3 windows, short burst, overlapping | ~26 |
| 3 windows, long burst | ~145 000 |
| 8 windows, short burst | ~450 000–618 000 |

---

# Part 1 — The Concepts

## 1.1 Two Privilege Modes, One Binary

The ESP32-P4's cores are RV32 with M and U privilege modes. ESP-IDF and
FreeRTOS live entirely in M-mode. This app carves out U-mode for the
application code:

|  | M-mode (the Kernel) | U-mode (the User) |
|---|---|---|
| CSR access | Yes | **Illegal instruction** |
| `mret`, `wfi`, other privileged instructions | Yes | **Illegal instruction** |
| FreeRTOS calls | Yes | Impossible — they take locks, locks use CSRs |
| Memory | All of it | Whatever the PMP grants (see [Part 5](#part-5--the-limit-of-the-isolation-stated-plainly)) |
| Way to reach the other side | `mret` | `ecall` — the only one |

The privilege boundary is real and you can prove it in one line of user code —
see [Exercise 1](#exercise-1-execute-a-privileged-instruction).

## 1.2 U-mode Is Not a Task; It Is a Coroutine Hosted by One

FreeRTOS has no notion of a user mode, and this tree does not modify FreeRTOS.
So U-mode is not a task. A perfectly ordinary FreeRTOS task **hosts** it:

```
user_host_task (M-mode, FreeRTOS, unpinned — one per window)
    |
    +-- umode_enter(&slot->ctx) -----------.
    |     save kernel regs and mstatus     |
    |     mask interrupts (MIE, then CLIC) |
    |     snapshot the per-hart CSRs       |
    |     hand the stack guard over        |
    |     mret, MPP=U                      v
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

## 1.3 The Syscall Interface

`a7` carries the number, `a0`/`a1` the arguments, `a0` the result — the RISC-V
norm. The kernel writes the result into the saved `a0` and steps the saved `pc`
past the `ecall` before resuming, so the user resumes at the instruction after
the trap with its return value in place.

| # | Call | Arguments | On Error |
|---|---|---|---|
| 0 | `SYS_NOP` | — | — |
| 1 | `SYS_WRITE` | Buffer, length → bytes written | `SYS_ERR_FAULT`, user continues |
| 2 | `SYS_PUTS` | NUL-terminated message → 0 | `SYS_ERR_FAULT`, user continues |
| 3 | `SYS_DELAY_MS` | Milliseconds | — |
| 4 | `SYS_EXIT` | Status; does not return | — |
| 5 | `SYS_GETCORE` | — → the core this window is running on | — |
| 6 | `SYS_YIELD` | — → 0; gives up the core without blocking | — |

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

## 1.4 Eight Windows, Two Cores, No Pins

There are eight U-mode windows and two cores, and no window is tied to either.
Getting there is mostly a lesson in **which processor state is per-hart**.

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

**A window cannot span two cores**, and it does not need a pin to guarantee
that. `umode_enter()` takes `mstatus.MIE` down *before* it snapshots any
per-hart CSR, so from there to the closing `mret` the hart cannot be preempted
and every hart-specific value is read and restored on the same core. That
ordering is the entire reason this demo is allowed to leave everything
unpinned, and it is worth reading [3.6](#36-per-hart-state-must-be-snapshotted-behind-the-mask)
before changing anything in that prologue.

Between windows the scheduler may put a host task anywhere, and in this demo it
constantly does. Nothing in the kernel side objects, because kernel-side state
is per-slot: `user_slot_t` carries each window's name, id, arena, context, run
flag and counters, so eight host tasks on two cores share no mutable state. The
console mutex is the only thing they deliberately share.

---

# Part 2 — Reading the Code

All assembly is in `.S` files. There is no inline assembly anywhere, including
for CSR reads — `umode_read_mstatus()` and friends are real functions in
`umode.S`.

Suggested order:

| # | File | Privilege | Read It for |
|---|---|---|---|
| 1 | `main/syscall.h` | Both | The kernel/user ABI. Smallest file, start here |
| 2 | `main/user_main.c` | **U** | What unprivileged code can and cannot do. Note what is *absent*: no `printf`, no library calls, no writable statics |
| 3 | `main/user_syscall.S` | **U** | The `ecall` stubs and the exit trampoline. Twenty lines that are the whole user-to-kernel path |
| 4 | `main/umode.h` | Both | The context layout, shared with `umode.S` as byte offsets |
| 5 | `main/kernel_main.c` | M | The kernel: boot report, `user_slot_t`, the host loop, syscall dispatch, pointer validation |
| 6 | `main/umode.S` | M | `umode_enter()`, the private trap vector, the CLIC vector table. Read last; Part 3 is mostly about this file |

Things to look for as you go:

- In `user_main.c`: **no writable statics.** Eight windows run this one copy
  at once, across two cores, so anything static and mutable would be shared
  with no lock. Every variable is a local — on the calling window's own arena
  stack — or `const`. `id`, delivered in the context's `a0`, is how an instance
  knows which it is.
- In `kernel_main.c`: `user_range_ok()` takes the **slot**, not just an address.
  [Part 5](#part-5--the-limit-of-the-isolation-stated-plainly) explains why that
  is load-bearing rather than tidy.
- In `umode.S`: the ordering comments are not decoration. Several sequences are
  correct only in the order written, and Part 3 explains four of them.

## Why the User Application Looks the Way It Does

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

- **No writable statics**, as above, because eight instances share the code.

`gp` and `tp` are deliberately **not** loaded from the user context: U-mode
inherits the kernel's, so gp-relative addressing would work if the compiler
chose it. In the current build it does not — the constants are reached
PC-relative (`addi a2,a2,-1336`), and no user function references `gp` at all.
Either way both registers are saved on the kernel frame and restored on the way
out, so a user that clobbers them cannot hurt the kernel.

---

# Part 3 — Six Things That Do Not Work the Obvious Way

Each of these was a real bug in this tree. They are the reason the code is
shaped the way it is, and they are the most transferable content here.

## 3.1 The Hardware Stack Guard Fires on the Stack Switch

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

## 3.2 The CLIC Threshold Write Is Not Immediately Effective

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

## 3.3 `mcause` Aliases `mstatus.MPP`

On the pre-rev3 P4 the CLIC packs previous privilege, previous interrupt-enable
and previous interrupt level into `mcause`:

| Bits | Field |
|---|---|
| 31 | Interrupt |
| 29:28 | `mpp` — **write-through aliased to `mstatus.MPP`** |
| 27 | `mpie` |
| 23:16 | `mpil` — `mret` restores `mintstatus.mil` from this |
| 11:0 | Exception code |

Because 29:28 and `mstatus.MPP` are the same state, **whichever of the two is
written last decides the privilege the `mret` returns to.** `umode.S` writes
`mstatus` last everywhere, deliberately, and says so at both sites. Get the
order wrong and an `mret` intended for U-mode returns to M-mode — silently, with
the user's registers loaded.

This is also why the exit path leaves through an `mret` rather than a jump: the
hardware restores `mintstatus.mil` from `mcause.mpil` as part of the `mret`, and
a jump would leave the core at the wrong interrupt level.

## 3.4 The Context Pointer Travels in `mscratch`

`mscratch` is how the trap vector finds the context. Nothing in ESP-IDF uses
that CSR on this target, which is what makes it available; the kernel's value is
saved and restored regardless, so the assumption is not load-bearing.

The entry sequence is `csrrw t0, mscratch, t0` — a swap that leaves the context
in `t0` and the *user's* `t0` parked in `mscratch`, to be recovered a few
instructions later. Using `t0` rather than the conventional `sp` is what
[3.1](#31-the-hardware-stack-guard-fires-on-the-stack-switch) requires.

## 3.5 One Interrupt Source, Two Monitors

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

## 3.6 Per-Hart State Must Be Snapshotted Behind the Mask

`umode_enter()` borrows the kernel's `mtvec`, `mtvt`, `mscratch` and
`mintstatus` for the length of the window and puts them back on the way out.
All four are **per-hart**. The obvious order is to save them, then mask
interrupts, then go — and that order is wrong, because the save is exposed to
preemption. A scheduler tick landing between the save and the mask can move
the task to the other core, and the trap vector will then faithfully restore
one hart's values into a different one.

`mintstatus` is the one that would bite hardest: its `mil` field is the live
CLIC interrupt level, and the exit path folds it into `mcause.mpil` for the
`mret` to restore ([3.3](#33-mcause-aliases-mstatusmpp)). `mtvec` and `mtvt`
survive by luck, because ESP-IDF programs both cores identically, and IDF does
not use `mscratch` on this target at all.

The fix is an ordering, not a mechanism. `mstatus` is read first, because the
exit path needs the caller's `MIE` bit and the `csrci` is about to clear it —
and that one read *is* safe early, since `MIE` is set on whichever hart a
running task is on, so the value does not depend on where it was sampled.
Everything else is read below the mask. The whole of the exposed prologue is
now the stack frame, the callee-saved registers going to the task's **own**
stack, and one `csrr`:

```
4ff0173c:  sw     s11,56(sp)
4ff01740:  csrr   t0,mstatus
4ff01744:  sw     t0,156(a0)
4ff01748:  csrci  mstatus,8      <- everything hart-specific happens below here
```

The exit path stops rebuilding `mstatus` from the snapshot for the same
reason, and for one more. It takes the base from a live `csrr` on the hart it
is returning to, using only the `MIE` bit of the saved copy. `mstatus` carries
the coprocessor dirty bits `FS` and `VS`, and this SoC has an FPU: writing a
pre-window copy back would tell the kernel the state was still clean when
U-mode may have dirtied it. That is a bug even when nothing migrates.

**How often does it actually happen?** Often enough to matter, and it took some
work to find out. The exposed prologue is four instructions, so for a long time
it looked unreachable — millions of windows with no migration ever landing in
it. Two things turned out to be true:

| Load | Migrations Landing in the Prologue |
|---|---|
| One runnable task per core | none, over millions of windows |
| Cores oversubscribed | 474 in 45 s |

With one runnable task per core the placement is stable and preemption simply
puts a task back where it was. Oversubscribe the cores — one U-mode window
pinned, another floating, one spare runnable task — and the same four
instructions are hit hundreds of times a minute.

**And what does it cost when it happens?** Today, nothing observable. A probe
that forces the race shows the corruption arriving reliably — every mid-call
migration under the old ordering captures the wrong hart's state — and the
system carries on regardless, because `mtvec` and `mtvt` match across cores,
`mscratch` is unused, and `mintstatus.mil` is zero in task context. Those are
three ESP-IDF implementation details, not three guarantees, which is the whole
argument for fixing the order rather than relying on them.

It is worth being clear that this is the one entry in Part 3 that never
produced a crash. It earns its place because the failure it prevents is silent:
the wrong value is restored, everything keeps running, and nothing anywhere
says so.

---

# Part 4 — Exercises

Each of these is a few lines added to `user_main.c` or `kernel_main.c`. Outputs
marked **captured** are real console output from running the exercise on
hardware; the rest tell you what to look for.

Add the probe, `./flash.sh`, `./monitor.sh`, then take the probe out again.

## Exercise 1: Execute a Privileged Instruction

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

## Exercise 2: Run Your Stack Off Its Arena

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
bounds printed are **user1's own** arena, read from the assist_debug block of
whichever core that window happened to be running on — this capture is from an
earlier two-window build, where it was core 1.

## Exercise 3: Hand the Kernel a Pointer Outside Your Arena

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

## Exercise 4: Reach into the Other User's Arena

Take the address the boot report prints for the *other* user's arena and pass it
to `SYS_PUTS`. Expect the same rejection as Exercise 3.

This is the exercise that shows why `user_range_ok()` takes the slot rather than
checking against a single global arena. PMP entry 5 grants U-mode **all** of
DRAM (see [Part 5](#part-5--the-limit-of-the-isolation-stated-plainly)), so both
arenas are equally reachable from either user; the only thing separating them is
this check. To see the check itself rather than a user's view of it, call
`user_range_ok(&g_slots[0], (uint32_t)(uintptr_t) g_slots[1].arena, 1)` in
`kernel_main()` and print the result.

## Exercise 5: Pin the Windows Again

Nothing is pinned by default. In `kernel_main()`, change the host tasks'
`tskNO_AFFINITY` to a real core and watch what changes:

- pin every window to core 0, and the two cores stop sharing the work;
- pin half to core 0 and half to core 1, and each half stops moving;
- pin some and leave others floating — a mix works, and the floating ones still
  migrate around the pinned ones.

All of these work, because the per-core machinery is resolved at run time from
`mhartid` and a window is bounded by its own core's interrupt mask rather than
by an affinity. The interesting part is the migration counter in the
`window:core/moves` line: it goes to zero for whatever you pin, and stays high
for whatever you do not.

Then try the opposite experiment — delete the `SYS_YIELD` burst from
`user_main` so the users only sleep, leave everything unpinned, and watch the
counters go to **zero anyway**. That is not a bug; it is
[why the users are busy](#why-the-users-are-busy).

## Exercise 6: Return from `user_main`

Delete the `for (;;)` so `user_main` falls off its end. The context is created
with `ra` pointing at `u_exit_stub`, so this becomes a `SYS_EXIT` rather than a
wild branch. Expect `user_main exited, status …` and a clean retirement.

## Exercise 7: Leave Interrupts Unmasked

Comment out the threshold raise in `umode_enter` and see how far it gets.
Predict first — [3.2](#32-the-clic-threshold-write-is-not-immediately-effective)
explains what does and does not break, and why the failure mode is a *lost*
edge-triggered interrupt rather than an immediate crash. Watch the host loop's
interrupt-in-U-mode counter in the retirement message.

---

# Part 5 — The Limit of the Isolation, Stated Plainly

**Read this before citing the project as a protected-mode example.**

The **privilege** boundary is real. U-mode code that executes a `csr`
instruction, an `mret`, or anything else reserved to M-mode takes an
illegal-instruction trap and lands in the kernel's fault handler
([Exercise 1](#exercise-1-execute-a-privileged-instruction) proves it). `ecall`
is the only way across.

The **memory** boundary is not what a production protected build would have, and
that is ESP-IDF's doing rather than a shortcut taken here.
`esp_cpu_configure_region_protection()` runs very early, before any application
code runs, and sets **the lock bit on every entry it programs** — its `NONE`,
`R`, `RW`, `RX` and `RWX` constants all carry `PMP_L`. PMP lock bits cannot be
cleared without Smepmp, which the ESP32-P4 does not implement, so those entries
are final for the rest of the boot. A locked entry applies to U-mode *and* to
M-mode.

Two of them are what this project rests on:

| Entry | Range | Permissions |
|---|---|---|
| 4 | `[SOC_IRAM_LOW, _iram_text_end)` | R+X, locked |
| 5 | `[_iram_text_end, SOC_DRAM_HIGH)` | R+W, locked |

Those two grants are exactly what let `user_main` run at all — but entry 5 also
means U-mode can read and write **all** of kernel DRAM, and both arenas, not
just its own. So:

- the kernel validates every pointer arriving from a syscall against **the
  calling slot's** arena (`user_range_ok()`), which is good practice regardless,
  but here it is doing work the hardware would otherwise do;
- the isolation between the eight users is entirely software, for the same
  reason — nothing in the hardware separates one arena from another;
- the stack guard does **not** close the gap and should not be mistaken for it.
  It watches `sp`, not accesses. A user that leaves `sp` alone and writes through
  a wild pointer is caught by neither, which is what `user_range_ok()` is for.

### Reading the PMP Configuration

The boot report prints the four `pmpcfg` words rather than describing them, so
you can check the above rather than take it on trust:

```
KERNEL: pmpcfg 0=809d9b9b 1=8d808b8d 2=80000089 3=9b8b8d8b
```

Each word packs four entries, one byte each, entry 0 in the low byte:

| Bit | Field | Values |
|---|---|---|
| 7 | `L` | Lock. Once set, the entry cannot be changed, and it applies to M-mode too |
| 6:5 | — | Reserved, zero |
| 4:3 | `A` | Address matching: `0`=OFF, `1`=TOR, `2`=NA4, `3`=NAPOT |
| 2 | `X` | Execute |
| 1 | `W` | Write |
| 0 | `R` | Read |

**`TOR` means "top of range", and it takes its base from the *previous* entry's
`pmpaddr`.** That is why entry 3 exists at all: it is set to `SOC_IRAM_LOW` with
`A=OFF`, granting nothing, purely to supply entry 4's lower bound. Entry 4's
range is `[pmpaddr3, pmpaddr4)` and entry 5's is `[pmpaddr4, pmpaddr5)` — which
is how the two rows in the table above get their boundaries.

Decoding the words above gives this board's actual configuration:

| Entry | Byte | L | A | Perms |  |
|---|---|---|---|---|---|
| 0 | `9b` | 1 | NAPOT | RW |  |
| 1 | `9b` | 1 | NAPOT | RW |  |
| 2 | `9d` | 1 | NAPOT | RX |  |
| 3 | `80` | 1 | OFF | — | Base for entry 4 |
| 4 | `8d` | 1 | TOR | RX | **The U-mode execute grant** |
| 5 | `8b` | 1 | TOR | RW | **The U-mode data grant — all of DRAM** |
| 6 | `80` | 1 | OFF | — | Base for entry 7 |
| 7 | `8d` | 1 | TOR | RX |  |
| 8 | `89` | 1 | TOR | R |  |
| 9 | `00` | **0** | OFF | — | Never programmed |
| 10 | `00` | **0** | OFF | — | Never programmed |
| 11 | `80` | 1 | OFF | — | Base for entry 12 |
| 12 | `8b` | 1 | TOR | RW |  |
| 13 | `8d` | 1 | TOR | RX |  |
| 14 | `8b` | 1 | TOR | RW |  |
| 15 | `9b` | 1 | NAPOT | RW |  |

So "all sixteen locked" would be too strong: **entries 9 and 10 are unlocked and
unprogrammed** on this configuration. IDF only sets them in its external-RAM
branch, and this build takes the other one — visible in entry 8 being `R` rather
than `RW`. IDF's own comment notes the spare entries "can be used to provide
more granular access".

**They do not help here, and the reason is worth understanding.** PMP entries are
statically prioritised: **the lowest-numbered entry that matches any byte of an
access decides it**, and no higher-numbered entry is consulted. Entry 5 already
matches every DRAM address, and 5 < 9, so a stricter rule installed at entry 9
or 10 would be shadowed and never evaluated. Spare high entries can *grant*
access to ranges that no lower entry matches — which is what IDF means — but they
cannot *narrow* a grant a lower entry has already made.

That is the structural reason this gap cannot be closed by adding entries. It can
only be closed by changing entries 4 and 5, and those are locked.

Note also that the report prints `pmpcfg` but not `pmpaddr`, so it gives you
permissions and matching modes but not the boundaries. Those come from
`components/esp_hw_support/port/esp32p4/cpu_region_protect.c` in your IDF
checkout, which is the file to read alongside this table.

### Closing the Gap

Closing it means stopping the entries being locked in the first place. The
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

## The Files

| File | Privilege | What It Is |
|---|---|---|
| `main/kernel_main.c` | M | The kernel. Boot report, the slot table, the host loop, the syscall dispatcher, user-pointer validation |
| `main/umode.S` | M | `umode_enter()`, the private trap vector, the CLIC vector table |
| `main/user_main.c` | **U** | The user application. No library calls, no writable statics, compiled into IRAM |
| `main/user_syscall.S` | **U** | The `ecall` stubs and the exit trampoline |
| `main/umode.h` | Both | The context layout, shared with `umode.S` as byte offsets |
| `main/syscall.h` | Both | The kernel/user ABI |
| `probe/` | — | The earlier `mret`-to-U-mode experiment ([README](probe/README.md)) |

## Build and Run

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

## Gotchas Worth Keeping

- **Chip revision.** IDF defaults to a minimum of v3.1 and the bootloader
  refuses to flash on v1.0 silicon (`requires chip revision in range [v3.1 -
  v3.99]`). `CONFIG_ESP32P4_REV_MIN_*` for the <3.0 family is gated behind
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`; both are in `sdkconfig.defaults`.

- **Watchdogs are off**, inherited from the probe, so that a hang persists and
  can be inspected with an observe-only JTAG halt rather than being reset away.

- **Non-standard CLIC CSRs.** `mtvt` is `0x307` and `mintstatus` is `0x346`
  rather than the `0xfb1` the CLIC specification gives it. Note that CLIC is a
  *proposal*, not a ratified extension — see
  [further reading](#further-reading) — so "non-standard" here means the P4
  disagrees with an unratified document, in one CSR number out of the two it
  takes from it.

- **`mtvt` must be 256-byte aligned and `mtvec.base` 64-byte aligned**, and
  `mtvec`'s low bits carry CLIC mode 3. Both are handled by `.balign` directives
  in `umode.S`; check them in the map file if the vector ever misbehaves.

- **The context layout is duplicated** between the C struct in `umode.h` and the
  `UCTX_*` offsets `umode.S` uses. `kernel_main.c` `_Static_assert`s every field,
  because the failure mode otherwise is a wild store inside a trap vector.

- **Only core 0 runs the full FreeRTOS tick.** ESP-IDF's
  `xTaskIncrementTick()` opens with
  `configASSERT( portGET_CORE_ID() == 0 )`, so core 0 alone walks the
  delayed-task list *and* applies the time-slice rotation. Two consequences
  bite anyone writing per-core code here: a task that blocks is almost always
  woken onto core 0, so a sleeping workload quietly herds itself onto one core;
  and whatever is running on core 1 is not rotated off by a tick. Both look
  like bugs in your own code when you first meet them.

- **`app_main` runs at priority 1.** Creating a task above that which never
  blocks preempts the rest of your initialisation permanently, and any task you
  meant to create afterwards simply never exists. The symptom looks exactly
  like scheduler starvation — a task that produces no output and no
  counters — and the giveaway is that its *creation-time* log line is missing
  too. Raise your own priority for the duration of the setup if you create
  tasks that spin.

- **`soc_caps.h` must be included in `umode.S`** for IDF's `_CUR_CORE` macros to
  dispatch on `mhartid` at all. Without it they silently collapse to their
  core-0 variants — and the failure mode is not a crash but a *silently
  unreported* stack violation, because reading the wrong core's `INTR_RAW`
  returns zero. IDF's own `portasm.S` includes it for the same reason.

- **The assist_debug per-core stride is assumed to be `0x80`.**
  `kernel_main.c` `_Static_assert`s it for all five registers `umode.S`
  addresses by hand, and `umode.S` carries an `#error` if it changes.

## Further Reading

Annotated with what in *this* project each one explains, because a spec index is
not much use to someone holding a specific question.

### RISC-V

| Document | What to Look up in It |
|---|---|
| [RISC-V International — specifications index](https://riscv.org/technical/specifications/) | The ratified specs, and which are ratified at all. Start here to check whether something is standard |
| [`riscv/riscv-isa-manual`](https://github.com/riscv/riscv-isa-manual) — source | The ISA and **privileged** manuals. The privileged manual is the reference for `mret`, `mstatus.MPP`/`MPIE`, `mcause`, `mtval`, `mepc`, `mscratch`, `ecall` and the PMP — every mechanism in [Part 1](#part-1--the-concepts) and [3.3](#33-mcause-aliases-mstatusmpp) |
| [Ratified PDFs](https://github.com/riscv/riscv-isa-manual/releases) | Built copies of the above, if you would rather not render AsciiDoc |
| [`github.com/riscv`](https://github.com/riscv) | Every other spec repo, for anything not in the two manuals |

The PMP, including the `L` lock bit, the `A` matching modes, the
lowest-numbered-match-wins priority rule and why clearing a lock needs
**Smepmp**, is in the privileged manual's physical-memory-protection chapter.
That is the background to [Part 5](#part-5--the-limit-of-the-isolation-stated-plainly)
and to [reading this board's own PMP state](#reading-the-pmp-configuration), and
worth reading before deciding this project's memory isolation is weaker than it
had to be.

### CLIC

| Document | What to Look up in It |
|---|---|
| [`riscv/riscv-fast-interrupt`](https://github.com/riscv/riscv-fast-interrupt) | The CLIC proposal: `mtvt`, `mintstatus.mil`, `mcause.mpil`, interrupt levels, and the vectored-entry model the private vector in `umode.S` implements |
| [Built PDF releases](https://github.com/riscv/riscv-fast-interrupt/releases) | Rendered copies (v0.20 at the time of writing) |
| [Spec source](https://github.com/riscv/riscv-fast-interrupt/tree/master/src) | The AsciiDoc, if you want to diff versions |

**Read the status line on that repo before treating it as authority.** It is
titled a *proposal* for a Core-Local Interrupt Controller, and it is not a
ratified RISC-V extension. That is the honest explanation for a good deal of
what [Part 3](#part-3--six-things-that-do-not-work-the-obvious-way) documents:
the P4 implements a moving target, so `mintstatus` sits at `0x346` rather than
`0xfb1`, the interrupt threshold is a memory-mapped register rather than a CSR,
and `mcause` carries fields ([3.3](#33-mcause-aliases-mstatusmpp)) that no
ratified document describes. When silicon and this document disagree, the
silicon wins and the TRM below is the tie-breaker.

### ESP32-P4

| Document | What to Look up in It |
|---|---|
| [ESP32-P4 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-p4_technical_reference_manual_en.pdf) | The authority when the RISC-V documents and the hardware disagree. The interrupt matrix and its per-core routing ([3.5](#35-one-interrupt-source-two-monitors)), the CLIC's memory-mapped registers, and the debug assist peripheral's per-core register blocks and SP-monitor semantics ([3.1](#31-the-hardware-stack-guard-fires-on-the-stack-switch)) |
| [ESP32-P4 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf) | Part numbering, revisions, and the memory map the PMP entries in [Part 5](#part-5--the-limit-of-the-isolation-stated-plainly) are cut from |
| [ESP-IDF v5.5.4 — ESP32-P4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/index.html) | The exact IDF version this tree is built against |

Two ESP-IDF sources are quoted directly in the code and are worth opening
alongside it, in your local IDF checkout rather than online, so the version
matches:

- `components/esp_system/port/include/private/esp_private/hw_stack_guard.h` —
  the stack-guard macros `umode.S` uses, and the `SOC_CPU_CORES_NUM` gate that
  decides whether they dispatch on `mhartid`
  ([1.4](#14-eight-windows-two-cores-no-pins));
- `components/freertos/FreeRTOS-Kernel/portable/riscv/portasm.S` — how the port
  itself arms the guard on every interrupt exit, which is what
  [3.1](#31-the-hardware-stack-guard-fires-on-the-stack-switch) has to work
  around.

## Glossary

| Term | Meaning |
|---|---|
| **hart** | Hardware thread — one RISC-V core. The P4 has two |
| **M-mode** | Machine mode. Full privilege. ESP-IDF and FreeRTOS run here |
| **U-mode** | User mode. Lowest privilege. No CSRs, no privileged instructions |
| **`ecall`** | The instruction that traps from a lower privilege to a higher one. The only way from U to M |
| **`mret`** | Return from a trap. Also how the kernel *enters* U-mode, by setting `MPP=U` first |
| **`mcause`** | Why the trap happened. On this SoC it also packs previous privilege, interrupt-enable and interrupt level |
| **`mtval`** | Trap value — for an illegal instruction, the instruction word itself |
| **`mepc`** | The PC the trap interrupted, and where `mret` returns to |
| **`mtvec` / `mtvt`** | Trap vector base / CLIC vector table base. Per-hart |
| **`mscratch`** | A scratch CSR, unused by ESP-IDF here, used to carry the context pointer into the trap vector |
| **CLIC** | The P4's core-local interrupt controller. Vectored, with an interrupt *level* rather than a simple enable |
| **CLIC threshold** | The only thing that masks interrupts in U-mode, since `mstatus.MIE` does not apply there |
| **PMP** | Physical memory protection. Region-based permissions that apply to U-mode, and to M-mode when locked |
| **Smepmp** | The RISC-V extension that would allow clearing PMP lock bits. **Not** implemented on the P4 |
| **assist_debug** | The ESP32-P4 debug assist peripheral. Its SP monitor is what ESP-IDF's hardware stack guard uses |
| **window** | One entry into and return from U-mode: `umode_enter()` to the trap that ends it |
| **arena** | One user's memory — its stack and data. One per slot |
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

Getting to more than one window took two: making `umode.S` core-agnostic
([1.4](#14-eight-windows-two-cores-no-pins)) and accepting the un-route
trade-off in [3.5](#35-one-interrupt-source-two-monitors). Per-slot arenas and
per-slot pointer validation came with it, and the isolation between users was
verified in both directions.

Getting to eight *unpinned* windows took one more: the prologue ordering in
[3.6](#36-per-hart-state-must-be-snapshotted-behind-the-mask). Measured over
40 s, each of the eight changed core between 450 000 and 618 000 times with no
fault, which is the demo and the evidence being the same thing.

No probes are in the tree. Builds clean for `esp32p4`, and the placement
assumptions are verified in the ELF: user text and the trap machinery sit below
`_iram_text_end`, the arenas and user constants sit above it in DRAM, the vector
table is 256-byte aligned and the trap entry 64-byte aligned.
