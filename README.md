# ESP32-P4: An ESP-IDF Application Split Across M-mode and U-mode

A teaching reference for running genuinely unprivileged code on an ESP32-P4,
under an RTOS that has no concept of user mode.

An ESP-IDF app whose kernel runs in machine mode and whose user applications
run in user mode — eight of them, none pinned to a core, in the same binary,
talking to the kernel over a syscall interface. The privilege boundary is real:
a `csr` instruction in user code is an illegal instruction, and `ecall` is the
only way across.

Target: M5Stack Tab5, ESP32-P4 revision **v1.0**. ESP-IDF **v5.5.4**.

```
KERNEL: 8 U-mode windows, NONE of them pinned, sharing that one user_main:
KERNEL: user0: first syscall arrived with mcause=08000008 (ECALL from U-mode) on core 0
User 0.2 on core 1
User 3.2 on core 0
KERNEL: window:core/moves u0:c0/578300 u1:c1/548421 u2:c0/607706 u3:c0/563506 ...
```

`mcause=08000008` is exception code 8, `ECALL_U` — the *hardware* stating that
the caller was unprivileged. The `on core c` came back through a syscall,
because U-mode cannot read `mhartid` without taking an illegal instruction.
Each of those eight windows changed core several hundred thousand times in
40 seconds, without a fault.

This is the follow-on to the experiment in [`probe/`](probe/README.md), which
established that `mret` to U-mode works on this silicon.

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

## Documentation

Written to be worked through in order. If you only read one thing, read
[Hard-Won Lessons](Documentation/hard-won-lessons.md) — six bugs that were hit,
diagnosed and fixed in this tree, each teaching something no datasheet says.

| # | Document | What It Covers |
|---|---|---|
| 1 | [The Demo](Documentation/the-demo.md) | What actually runs, the boot report, and why the user tasks are deliberately busy |
| 2 | [Concepts](Documentation/concepts.md) | Privilege modes, U-mode as a hosted coroutine, the syscall ABI, and which state is per-hart |
| 3 | [Reading the Code](Documentation/reading-the-code.md) | A route through the six source files, and why the user application looks the way it does |
| 4 | [Adding a Syscall](Documentation/adding-a-syscall.md) | Three edits, the rules that are easy to get wrong, and the limits — argument count, return width, numbering |
| 5 | [Hard-Won Lessons](Documentation/hard-won-lessons.md) | The six things that do not work the obvious way |
| 6 | [Exercises](Documentation/exercises.md) | Break it deliberately; most have captured output to check against |
| 7 | [The Limit of the Isolation](Documentation/isolation-limits.md) | What the hardware enforces, what is still software, and how to read the PMP |
| 8 | [Reference](Documentation/reference.md) | Files, build, gotchas, further reading, glossary |

### The Six Lessons

| # | Lesson |
|---|---|
| 3.1 | [The hardware stack guard fires on the stack switch](Documentation/hard-won-lessons.md#31-the-hardware-stack-guard-fires-on-the-stack-switch) |
| 3.2 | [The CLIC threshold write is not immediately effective](Documentation/hard-won-lessons.md#32-the-clic-threshold-write-is-not-immediately-effective) |
| 3.3 | [`mcause` aliases `mstatus.MPP`](Documentation/hard-won-lessons.md#33-mcause-aliases-mstatusmpp) |
| 3.4 | [The context pointer travels in `mscratch`](Documentation/hard-won-lessons.md#34-the-context-pointer-travels-in-mscratch) |
| 3.5 | [One interrupt source, two monitors](Documentation/hard-won-lessons.md#35-one-interrupt-source-two-monitors) |
| 3.6 | [Per-hart state must be snapshotted behind the mask](Documentation/hard-won-lessons.md#36-per-hart-state-must-be-snapshotted-behind-the-mask) |

### The Exercises

| # | Exercise | Shows |
|---|---|---|
| 1 | [Execute a privileged instruction](Documentation/exercises.md#exercise-1-execute-a-privileged-instruction) | The privilege boundary is real; `mtval` holds the offending instruction |
| 2 | [Run your stack off its arena](Documentation/exercises.md#exercise-2-run-your-stack-off-its-arena) | The handed-over stack guard catches it, against the right arena |
| 3 | [Hand the kernel a pointer outside your arena](Documentation/exercises.md#exercise-3-hand-the-kernel-a-pointer-outside-your-arena) | Pointer validation, and that it is recoverable rather than fatal |
| 4 | [Reach into another user's arena](Documentation/exercises.md#exercise-4-reach-into-the-other-users-arena) | Why the bounds check is per-slot |
| 5 | [Pin the windows again](Documentation/exercises.md#exercise-5-pin-the-windows-again) | Affinity is now a choice; the migration counters prove it |
| 6 | [Return from `user_main`](Documentation/exercises.md#exercise-6-return-from-user_main) | The exit trampoline |
| 7 | [Leave interrupts unmasked](Documentation/exercises.md#exercise-7-leave-interrupts-unmasked) | Why the window masks, and what is lost if it does not |
| 8 | [Add a syscall of your own](Documentation/exercises.md#exercise-8-add-a-syscall-of-your-own) | The whole interface, including the pointer rules |
| 9 | [Scribble on the kernel's slot table](Documentation/exercises.md#exercise-9-scribble-on-the-kernels-slot-table) | The one boundary the PMP enforces, not the kernel |
| 10 | [Reach into the kernel's memory](Documentation/exercises.md#exercise-10-reach-into-the-kernels-memory) | The kernel/user split, and what it still does not cover |

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
- How to take the PMP over from ESP-IDF and describe a kernel/user memory
  split the hardware enforces — and why IDF's own configuration makes that
  impossible until you stop it running.
- Why a memory-protection story can be *almost* right and still be worth
  nothing — and how to tell the difference.

## Prerequisites

Assumed: C, basic RISC-V assembly, and FreeRTOS tasks. Everything specific to
RISC-V privilege modes, the CLIC, the PMP and the ESP32-P4's debug assist
peripheral is explained where it is used, and collected in the
[glossary](Documentation/reference.md#glossary).

Hardware is required for the exercises. Reading Parts 1–3 does not need a
board.

## Status

Runs on hardware. Verified on an M5Stack Tab5 (ESP32-P4 v1.0) against ESP-IDF
v5.5.4, console captured over UART0:

- the boot report confirms `user_main` lands inside the U-mode execute grant;
- **both** windows enter U-mode and report `mcause=08000008` — `ECALL from
  U-mode` — from core 0 and core 1 respectively, which is the hardware's own
  statement that each is genuinely unprivileged;
- both sustain equal syscall rates indefinitely with no faults and no panics,
  while the kernel task keeps its own cadence;
- the console excerpts in [What it does](Documentation/the-demo.md#what-it-does) and in
  [Part 4](Documentation/exercises.md#part-4--exercises) are captures, not illustrations.

Getting to a single working window took one fix, described in
[3.1](Documentation/hard-won-lessons.md#31-the-hardware-stack-guard-fires-on-the-stack-switch): IDF's hardware
stack guard fired on the U-mode stack switch and panicked the kernel on the way
back out, before any user output could be printed.

Getting to more than one window took two: making `umode.S` core-agnostic
([1.4](Documentation/concepts.md#14-eight-windows-two-cores-no-pins)) and accepting the un-route
trade-off in [3.5](Documentation/hard-won-lessons.md#35-one-interrupt-source-two-monitors). Per-slot arenas and
per-slot pointer validation came with it, and the isolation between users was
verified in both directions.

Getting to eight *unpinned* windows took one more: the prologue ordering in
[3.6](Documentation/hard-won-lessons.md#36-per-hart-state-must-be-snapshotted-behind-the-mask). Measured over
40 s, each of the eight changed core between 450 000 and 618 000 times with no
fault, which is the demo and the evidence being the same thing.

No probes are in the tree. Builds clean for `esp32p4`, and the placement
assumptions are verified in the ELF: user text and the trap machinery sit below
`_iram_text_end`, the arenas and user constants sit above it in DRAM, the vector
table is 256-byte aligned and the trap entry 64-byte aligned.
