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
it. [Exercise 2](exercises.md#exercise-2-run-your-stack-off-its-arena) shows exactly that.

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
