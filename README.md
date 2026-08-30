# ESP32-P4: an ESP-IDF application split across M-mode and U-mode

An ESP-IDF app whose kernel runs in machine mode and whose user application
runs, genuinely, in user mode — on the same core, in the same binary, talking
over a syscall interface.

Target: M5Stack Tab5, ESP32-P4 revision **v1.0**. ESP-IDF **v5.5.4**.

This is the follow-on to the experiment in [`probe/`](probe/README.md), which
established that `mret` to U-mode works on this silicon. That was a control;
this is the thing the control was clearing the way for.

## What it does

Three things arrive on the console:

```
Kernel 1
Kernel 2
User 1                     <- U-mode, via SYS_WRITE
Kernel 3
syscall 1                  <- U-mode message, printed by the kernel
Kernel 4
User 2
...
```

| line | emitted by | privilege | cadence | path |
|---|---|---|---|---|
| `Kernel x` | `kernel_task` | M | 1 s | `printf` |
| `User y` | `user_main` | **U** | 2 s | `SYS_WRITE` |
| `syscall z` | `user_main` | **U** | 3 s | `SYS_PUTS` — the kernel prints the message it was handed |

`Kernel x` and the two user cadences are independent: the user application is
asleep on a real `vTaskDelay` between syscalls, so the scheduler runs the
kernel task normally while U-mode is parked.

## The files

| file | privilege | what it is |
|---|---|---|
| `main/kernel_main.c` | M | the kernel. Boot report, the two tasks, the syscall dispatcher, user-pointer validation |
| `main/umode.S` | M | `umode_enter()`, the private trap vector, the CLIC vector table |
| `main/user_main.c` | **U** | the user application. No library calls, compiled into IRAM |
| `main/user_syscall.S` | **U** | the `ecall` stubs and the exit trampoline |
| `main/umode.h` | both | the context layout, shared with `umode.S` as byte offsets |
| `main/syscall.h` | both | the kernel/user ABI |

All assembly is in `.S` files. There is no inline assembly anywhere, including
for CSR reads — `umode_read_mstatus()` and friends are real functions in
`umode.S`.

## How U-mode is entered

FreeRTOS has no notion of a user mode, so U-mode is not a task. It is a
**coroutine hosted by one**:

```
user_host_task (M-mode, FreeRTOS)
    |
    +-- umode_enter(&ctx) ------------------.
    |     save kernel regs, mtvec, mtvt      |
    |     mask interrupts (CLIC threshold)   |
    |     mret, MPP=U                        v
    |                                    user_main()  (U-mode)
    |                                        |
    |     <--- umode_trap_entry <---------- ecall
    |     save user regs into ctx
    |     restore kernel vectors + threshold
    |     mret back to M-mode
    +-- returns mcause
    |
    +-- syscall_dispatch()  <- vTaskDelay, printf, all legal here
    +-- loop
```

The trap vector does no dispatching at all. It records the trap and hands
control back to ordinary task context, which is why `SYS_DELAY_MS` can be a
real `vTaskDelay()` and `SYS_WRITE` can go through the console driver — neither
would be legal inside a trap handler.

Every way out of U-mode comes back through the same call: a syscall, a fault,
or an interrupt. The host loop tells them apart from `mcause`.

## The syscall interface

`a7` carries the number, `a0`/`a1` the arguments, `a0` the result — the RISC-V
norm. The kernel writes the result into the saved `a0` and steps the saved `pc`
past the `ecall` before resuming.

| # | call | arguments |
|---|---|---|
| 0 | `SYS_NOP` | — |
| 1 | `SYS_WRITE` | buffer, length → bytes written |
| 2 | `SYS_PUTS` | NUL-terminated message → 0 |
| 3 | `SYS_DELAY_MS` | milliseconds |
| 4 | `SYS_EXIT` | status; does not return |

`SYS_PUTS` is the message-passing call the system was asked for: U-mode hands
over a string, the kernel validates it, copies it out of the user arena and
prints it. `SYS_WRITE` is separate on purpose — it is the counted-length
console write that the user application's *own* output (`User y`) goes through,
so the two mechanisms stay visibly distinct in the log.

`SYS_DELAY_MS` exists because U-mode cannot call FreeRTOS. Without it the user
application would have to spin, which would defeat the point of showing the two
privilege levels sharing a core.

## Why the user application looks the way it does

Three constraints, all consequences of running unprivileged:

- **No library calls.** `printf` reaches the console driver, which takes
  FreeRTOS locks, which execute `csr` instructions — illegal in U-mode. The
  number formatting in `user_main.c` is therefore hand-rolled. It also keeps
  libgcc out: the only helpers the compiler wants are `divu`/`remu`, which the
  P4 has in hardware. (Verified in the disassembly; `user_main` calls nothing
  but `u_append`, `u_append_u32` and the syscall stubs.)

- **Code in IRAM.** IDF's PMP entry 4 grants U-mode read+execute over
  `[SOC_IRAM_LOW, _iram_text_end)`. `IRAM_ATTR` puts the user functions inside
  it; the same code in flash would fault on the instruction fetch. The boot
  report prints whether `user_main` actually landed inside the grant.

- **Data in DRAM.** Entry 5 grants U-mode read+write over the DRAM above.
  String constants are `DRAM_ATTR` rather than left in flash rodata — flash
  rodata happens to be U-readable in this configuration, but depending on that
  would tie the app to a PMP entry it has no reason to need.

`gp` and `tp` are deliberately **not** loaded from the user context: U-mode
inherits the kernel's, so gp-relative addressing works. (It is used —
`g_user_prefix` is reached as `addi a2,gp,1804`.) They are saved on the kernel
frame and restored on the way out, so a user that clobbers them cannot hurt the
kernel.

## Interrupts, and why the window is masked

`mstatus.MIE` does **not** mask interrupts while the core is in U-mode. The
only lever is the CLIC threshold, which on the pre-rev3 P4 is a *memory-mapped*
register (`CLIC_INT_THRESH_REG`, `0x20800008`), not a CSR. `umode_enter` raises
it for the duration of the window and the trap vector puts it back.

Interrupts could instead be left live — an interrupt taken in U-mode enters the
private vector, which returns to the kernel, and IDF's vector is restored
before the `mret` so a level-held source is delivered the instant `MIE` comes
back. The probe ran 600,000 iterations that way without trouble. It is masked
anyway because an **edge-triggered** source is claimed on entry and would be
lost. The host loop still handles an interrupt-cause return, as a safety net.

The cost is that a user which computes for a long time between syscalls delays
the tick, so the user model here is a cooperative one. In this application the
gap between syscalls is a few microseconds.

Two details that are easy to get wrong and are handled in `umode.S`:

- **The threshold write is not immediately effective.** IDF's own
  `rv_utils_restore_intlevel_regval()` documents that a following load (or ~8
  `nop`s) is what makes it active. Both threshold writes are followed by a
  read-back; without it the `mret` can reach U-mode before the mask does.

- **`mcause[29:28]` is write-through aliased to `mstatus.MPP`**, so whichever
  of the two is written last decides the privilege the `mret` returns to.
  `mstatus` is written last everywhere, deliberately.

`mscratch` carries the context pointer into the trap vector. Nothing in IDF
uses that CSR on this target; it is saved and restored regardless.

## The hardware stack guard follows the window

`CONFIG_ESP_SYSTEM_HW_STACK_GUARD` arms the ESP32-P4's assist_debug SP monitor
on the bounds of whichever FreeRTOS task is current. U-mode runs on its own
stack in the arena, nowhere near the host task's stack, so the `lw sp` at the
end of `umode_enter` is an out-of-bounds `sp` **by construction**.

Left alone that is fatal, and it was: it is the bug that stopped this ever
running. The failure is worth describing, because none of it points at the
cause. The violation raises an interrupt, but the window is masked, so nothing
is delivered until the return `mret` restores `mstatus.MIE` — at which point the
kernel takes a `Stack protection fault` panic on the first instruction of
`.Lkresume`, reporting the *kernel's* `sp`, which is comfortably inside the
bounds it prints beside it. The only field that names the real instruction is
the PC assist_debug latched, which the panic prints as `Detected in task
"user_host" at 0x…`.

Rather than stand the guard down, `umode.S` **hands it over**: it saves the host
task's bounds, points the monitor at `[u_sp_min, u_sp_max]` — the arena — for
the duration of the window, and puts the task's bounds back on the way out. A
user that runs its stack off either end of its arena is caught by the same
silicon that protects every kernel task.

Four things make it work, and each of them is a thing that does not work if you
do it the obvious way:

- **The swap has to happen in assembly, inside the masked region.** FreeRTOS's
  `rtos_int_exit` re-arms the monitor and re-points the bounds at the current
  task on *every* interrupt exit, so anything done in C around the
  `umode_enter()` call is undone by any tick that lands before the `mret`.
  Between `csrci mstatus, MSTATUS_MIE` and the `mret` nothing can preempt the
  hart, which makes that the only window where the swap holds. IDF supplies
  the assembler macros for it in `esp_private/hw_stack_guard.h` — the same ones
  FreeRTOS's own `portasm.S` uses.

- **The trap vector cannot put the context pointer in `sp`.** The classic
  entry sequence is `csrrw sp, mscratch, sp`, which would take `sp` to a kernel
  address while the monitor is still watching the arena — latching a violation
  on *every syscall*. So the swap goes through `t0` instead and `sp` keeps the
  user's value until the guard has been handed back. It is slightly shorter
  that way too: the user's `sp` gets stored straight out rather than via
  `mscratch`.

- **A real violation must not reach IDF's panic handler.** It would arrive
  after the window closes and be reported against the kernel — the same
  misleading panic as above, for a fault that is the user's. So the trap vector
  samples `ASSIST_DEBUG_CORE_0_INTR_RAW`, records the verdict and the latched
  PC in the context, and clears the source. The host loop reports it as
  `user fault: stack pointer left the arena` and retires the user context; the
  kernel task carries on.

- **Core 1 has to be taken out of the routing.**
  `ETS_ASSIST_DEBUG_INTR_SOURCE` is one source in the interrupt matrix and
  `esp_hw_stack_guard_init()` runs on every core, so a violation on *core 0's*
  monitor is delivered to core 1 as well — where the CLIC threshold means
  nothing. Core 1 wins that race every time, finds no explanation (core 0
  clears the latch on the way out) and panics with `ASSIST_DEBUG is not
  triggered BUT interrupt occurred!`. There is no way to separate the monitor
  from its interrupt: on this SoC `ASSIST_DEBUG_CORE_0_MONITOR_REG` is
  `#define`d to `ASSIST_DEBUG_CORE_0_INTR_ENA_REG` — the same register, the
  same bits. So `kernel_main()` un-routes the source on core 1 once, at start,
  and the boot report says so. The cost is that core 1 no longer reports stack
  guard faults of its own; it runs nothing here but ESP-IDF's idle and IPC
  tasks.

Two measured details. The upper bound is inclusive — the initial `sp` is
exactly `u_sp_max` and does not fire. And `INTR_RAW` is sticky, so an excursion
that is put back before the next syscall is still caught: a probe that dipped
`sp` two arenas below the stack and restored it immediately was reported with
`sp` back in range and the latched PC pointing at the `sub sp, sp, t0` that did
it.

## The limit of the isolation, stated plainly

The **privilege** boundary is real. U-mode code that executes a `csr`
instruction, an `mret`, or anything else reserved to M-mode takes an
illegal-instruction trap and lands in the kernel's fault handler. `ecall` is
the only way across.

The **memory** boundary is not what a production protected build would have,
and that is IDF's doing rather than a shortcut taken here.
`esp_cpu_configure_region_protection()` programs all sixteen PMP entries **with
the lock bit set**, very early, before any application code runs. PMP lock bits
cannot be cleared without Smepmp, which the ESP32-P4 does not implement. The
entries are final for the rest of the boot, and a locked entry applies to
U-mode as well as M-mode:

| entry | range | permissions |
|---|---|---|
| 4 | `[SOC_IRAM_LOW, _iram_text_end)` | R+X, locked |
| 5 | `[_iram_text_end, SOC_DRAM_HIGH)` | R+W, locked |

Those two grants are exactly what let `user_main` run at all — but entry 5 also
means U-mode can read and write **all** of kernel DRAM, not just its own arena.
So the kernel validates every pointer arriving from a syscall against the arena
(`user_range_ok()`), which is good practice regardless, but here it is doing
work the hardware would otherwise do.

Closing that gap means stopping the entries being locked in the first place.
The NuttX `BUILD_PROTECTED` port for this board does exactly that: it drops
IDF's `cpu_region_protect.c` from the build and recompiles the same source with
`PMP_L` defined to zero (`CONFIG_ESPRESSIF_KERNEL_OWNS_PMP`), then
re-describes the regions for a kernel/user split. The same trick would work
here — define `esp_cpu_configure_region_protection()` in this component so the
linker prefers it over IDF's, and reprogram the entries in `kernel_main()`.
That is not done in this tree.

## Build and run

```bash
./build.sh          # configure for esp32p4 if needed, then build
./flash.sh          # build, then flash via /dev/cu.usbmodem*
./monitor.sh        # console on /dev/cu.usbserial-*
./clean.sh          # drop build/;  --full also drops sdkconfig
```

The scripts find ESP-IDF themselves — `IDF_VERSION=5.5.3 ./build.sh` picks a
different one — so no `useidf-5-5-4` beforehand. They do not use that alias
because the activation script refuses to run from a shell script and the
`idf.py` it provides is a shell alias a non-interactive shell will not expand;
`scripts/idf-env.sh` asks it for its environment with `-e` instead and calls
`tools/idf.py` through the venv's python.

Both ports are optional arguments: `./flash.sh /dev/cu.usbmodem101`.

**The two USB paths are not interchangeable.** `/dev/cu.usbmodem*` is the P4's
USB-Serial/JTAG port, used by esptool and OpenOCD. `/dev/cu.usbserial-*` is the
external bridge on UART0, which `sdkconfig.defaults` puts the console on
deliberately — so the board can be reset and reflashed through one port while a
capture is held open on the other.

## Gotchas worth keeping

- **Chip revision.** IDF defaults to a minimum of v3.1 and the bootloader
  refuses to flash on v1.0 silicon (`requires chip revision in range [v3.1 -
  v3.99]`). `CONFIG_ESP32P4_REV_MIN_*` for the <3.0 family is gated behind
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`; both are in `sdkconfig.defaults`.

- **Watchdogs are off**, inherited from the probe, so that a hang persists and
  can be inspected with an observe-only JTAG halt rather than being reset away.

- **The hardware stack guard fires on the U-mode stack switch** and has to be
  handed over inside the masked window, not around the call. This was the one
  bug that stopped the demo running; it is described in full above.

- **Non-standard CLIC CSRs.** `mtvt` is `0x307` and `mintstatus` is `0x346`
  rather than the ratified `0xfb1`.

- **`mtvt` must be 256-byte aligned and `mtvec.base` 64-byte aligned**, and
  `mtvec`'s low bits carry CLIC mode 3. Both are asserted by the `.balign`
  directives in `umode.S`; check them in the map file if the vector ever
  misbehaves.

- **The context layout is duplicated** between the C struct in `umode.h` and
  the `UCTX_*` offsets `umode.S` uses. `kernel_main.c` `_Static_assert`s every
  field, because the failure mode otherwise is a wild store inside a trap
  vector.

## Status

Runs on hardware. Measured on an M5Stack Tab5 (ESP32-P4 v1.0) on 2026-08-30
against ESP-IDF v5.5.4: the boot report confirms `user_main` lands inside the
U-mode execute grant, the first trap back arrives as `mcause=08000008` —
`ECALL from U-mode`, previous privilege U — and the three cadences run at
1 s / 2 s / 3 s indefinitely. The console excerpt near the top of this file is
a capture.

Getting there took one fix: IDF's hardware stack guard fired on the U-mode
stack switch and panicked the kernel on the way back out, before any user
output could be printed. The guard now follows the window on to the user's
stack instead; that, and the way it is verified, is the section above.

The verification was a temporary probe in `user_syscall.S` that dipped `sp`
16 KB below the arena and put it straight back. It produced, with no panic and
no reset:

```
KERNEL: user fault: stack pointer left the arena
KERNEL:   sp=4ff14e40 allowed=4ff12e90..4ff14e90 detected at pc=4ff01e70
KERNEL: user context retired after 8 syscalls (0 interrupts taken in U-mode)
Kernel 7
Kernel 8
...
```

The probe is not in the tree.

Builds clean on ESP-IDF v5.5.4 for `esp32p4`, and the placement assumptions are
verified in the ELF: user text and the trap machinery sit below
`_iram_text_end`, the arena and user constants sit above it in DRAM, the vector
table is 256-byte aligned and the trap entry 64-byte aligned.
