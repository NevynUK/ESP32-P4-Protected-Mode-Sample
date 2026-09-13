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
| Memory | All of it | Whatever the PMP grants (see [Part 5](isolation-limits.md#part-5--the-limit-of-the-isolation-stated-plainly)) |
| Way to reach the other side | `mret` | `ecall` — the only one |

The privilege boundary is real and you can prove it in one line of user code —
see [Exercise 1](exercises.md#exercise-1-execute-a-privileged-instruction).

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
Both behaviours are worth seeing — Exercises [1](exercises.md#exercise-1-execute-a-privileged-instruction)
and [3](exercises.md#exercise-3-hand-the-kernel-a-pointer-outside-your-arena).

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
  hard one; see [3.5](hard-won-lessons.md#35-one-interrupt-source-two-monitors).

`umode.S` itself needs nothing per-core beyond that: its only sections are
`"ax"` and `"a"`, so it holds no writable state and both cores execute the same
window code re-entrantly.

**A window cannot span two cores**, and it does not need a pin to guarantee
that. `umode_enter()` takes `mstatus.MIE` down *before* it snapshots any
per-hart CSR, so from there to the closing `mret` the hart cannot be preempted
and every hart-specific value is read and restored on the same core. That
ordering is the entire reason this demo is allowed to leave everything
unpinned, and it is worth reading [3.6](hard-won-lessons.md#36-per-hart-state-must-be-snapshotted-behind-the-mask)
before changing anything in that prologue.

Between windows the scheduler may put a host task anywhere, and in this demo it
constantly does. Nothing in the kernel side objects, because kernel-side state
is per-slot: `user_slot_t` carries each window's name, id, arena, context, run
flag and counters, so eight host tasks on two cores share no mutable state. The
console mutex is the only thing they deliberately share.
