# The `mret`-to-U-mode probe (kept for reference, not built)

`p4_umode_mret.c` is the standalone experiment this project started as.  It has
its own `app_main`, so it is deliberately left out of `main/CMakeLists.txt`; to
run it again, swap it for `kernel_main.c` in the `SRCS` list.

It is the reason the system in the parent directory works: it established that
`mret` to U-mode behaves on this silicon, and it is where the PMP, CLIC
threshold and chip-revision gotchas in `umode.S` were all first paid for.  Its
original write-up follows verbatim.

---

# ESP32-P4: `mret` to U-mode with `mcause.interrupt` — ESP-IDF probe

A standalone ESP-IDF app that isolates the instruction sequence suspected in an
ESP32-P4 NuttX `BUILD_PROTECTED` bring-up, so it can be exercised without any of
NuttX in the picture.

> **Result: it does NOT reproduce the failure — 1.4 million mrets, zero
> anomalies.** That is the finding. This is a *control*, not a bug report; do
> not send it to Espressif as evidence of a defect, because it is evidence of
> the opposite. See "Result" below.

Target: M5Stack Tab5, ESP32-P4 revision **v1.0**. ESP-IDF **v5.5.4**.

---

## What was suspected

In the NuttX port, a user thread resumed by the trap epilogue makes its next
`ecall` fail to vector: the trap is recognised — `mepc`, `mcause` and `mstatus`
all latch and privilege is raised to M — but the pc never reaches `mtvec.base`,
so the `ecall` re-executes forever. Clearing `mcause.interrupt` (bit 31) on
returns to U-mode prevents it, A/B'd both directions over 12 boots with a
timing control.

The hypothesis this app tests: *`mret` to U-mode with `mcause.interrupt` set is
not supported on this CLIC*. That would explain why nobody else hits it —
FreeRTOS never leaves M-mode, so every `mret` in IDF's own `vectors.S` is M→M.

## What it does

`main/p4_umode_mret.c` installs a private `mtvec`/`mtvt`, performs the same
three CSR writes a trap epilogue does (`mepc`, `mcause`, `mstatus`) and `mret`s
to a stub of `ecall`s, then reports the `mcause` the resulting trap produced.

No PMP programming is needed: IDF already grants U-mode R+X on IRAM text.
`cpu_region_protect.c` sets entry 4 over `[SOC_IRAM_LOW, _iram_text_end)` with
`PMP_TOR | RX`, and its `RX` includes `PMP_L`, so the entry applies to U-mode as
well as M-mode. The stub therefore lives in IRAM and is executable from U-mode.

| # | privilege | `mcause` | interrupts in the window | resting CLIC level |
|---|---|---|---|---|
| 1 | M | `0x883f0010` | masked | 0 |
| 2 | U | `0x083f0010` (bit 31 clear) | masked | 0 |
| 3 | U | `0x883f0010` | masked | 0 |
| 4 | U | `0x083f0010` | **live** | 0 |
| 5 | U | `0x883f0010` | **live** | 0 |
| 6 | U | `0x083f0010` | masked | **1** |
| 7 | U | `0x883f0010` | masked | **1** |
| 8 | U | `0x883f0010` | **live** | **1** |

1 and 2 are the controls: 1 shows M-mode takes the value without complaint,
2 shows the U-mode path itself works (same privilege drop, same private vector,
same PMP grant, one bit different). 4/6/8 are further controls for the
interrupt and level variables.

## Result

All eight pass. **1.4 million `mret`s to U-mode, 200000 of them per failing
configuration, zero anomalies.**

```
P4UMRET: [1] returned: n=1000   bad=0 last mcause=383f000b  OK
P4UMRET: [2] returned: n=1000   bad=0 last mcause=083f0008  OK
P4UMRET: [3] returned: n=200000 bad=0 last mcause=083f0008  OK
P4UMRET: [4] returned: n=200000 bad=0 last mcause=883f0012  OK
P4UMRET: [5] returned: n=200000 bad=0 last mcause=883f0012  OK
P4UMRET: [6] returned: n=200000 bad=0 last mcause=083f0008  OK
P4UMRET: [7] returned: n=200000 bad=0 last mcause=083f0008  OK
P4UMRET: [8] returned: n=200000 bad=0 last mcause=883f0012  OK
```

`mcause=0x...0008` is ECALLU, `0x...000b` is ECALLM, `0x883f0012` is an
interrupt that landed in the U-mode window (id 18) — all expected.

**So `mret` to U-mode with `mcause.interrupt` set is fine on this silicon**, at
least in an environment this simple. The hypothesis is refuted and this app is
not a bug report — it is a control that narrows where the real trigger lives.

## What this rules out, and what it leaves

Ruled out: the instruction on its own, the interrupt bit on its own, an
interrupt landing in the U-mode window, and the resting CLIC level being 1
rather than 0 — in every combination above.

Also ruled out **since this app was written**: the separate user image and its
PMP boundary, which was the leading candidate for a while. Tested back in the
NuttX harness by mret-ing to stubs in kernel SRAM, kernel flash and the user
image with matched iteration counts — all clean (ESP32P4-Kernel.md §36.20).
Note the trap there: the first comparison used 500 iterations for the kernel
stubs against **one** for the user stub, which looked like a positive result
until the counts were matched.

Still different between this app and the NuttX case, and therefore still
candidates:

- NuttX's own `mtvec`, trap frames and `mscratch`/kernel-stack machinery
  interleave with the probe; here the private vector is the only user;
- the failing NuttX stage runs after thousands of syscalls, context switches and
  pthread create/exit cycles.

The current reading (ESP32P4-Kernel.md §36.20.3) is that the trigger needs the
**kernel's ongoing U-mode returns in a real workload**, and that no tight loop
of the instruction reproduces it in either RTOS — which is exactly what this app
demonstrates.

## Gotchas worth keeping

- **Chip revision.** IDF defaults to a minimum of v3.1 and the bootloader
  refuses to flash on v1.0 silicon (`requires chip revision in range
  [v3.1 - v3.99]`). `CONFIG_ESP32P4_REV_MIN_*` for the <3.0 family is gated
  behind `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`; both are in
  `sdkconfig.defaults`.
- **`mstatus.MIE` does not mask interrupts in U-mode.** Without raising the
  CLIC threshold, the stub is interrupted before its `ecall` executes and the
  probe measures an interrupt (`mcause=0x883f0012`) instead of the trap under
  test. The pre-rev3 P4 keeps that threshold in a **memory-mapped** register
  (`CLIC_INT_THRESH_REG` = `0x20800008`), not a CSR.
- Watchdogs are disabled so a hang persists and can be inspected with an
  observe-only JTAG halt rather than resetting.

## Build and run

```bash
source ~/.espressif/tools/activate_idf_v5.5.4.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

Console is UART0 on GPIO37/38, which on this board reaches the external USB
bridge (`/dev/cu.usbserial-*`) — deliberately not the USB-Serial/JTAG port, so
the board can be reset through that one while the console is held open.
