# Part 5 — The Limit of the Isolation, Stated Plainly

**Read this before citing the project as a protected-mode example**, in either
direction. The kernel/user boundary is now enforced by hardware. The
user/user boundary is not.

This document used to say the memory boundary was software throughout, and
explained at length why it could not be otherwise. That is no longer true, and
the reason it changed is worth more than the conclusion: nothing about the
silicon was in the way. What was in the way was ESP-IDF's default PMP
configuration, and it could simply be replaced.

## What the Hardware Enforces

The **privilege** boundary is real. U-mode code that executes a `csr`
instruction, an `mret`, or anything else reserved to M-mode takes an
illegal-instruction trap and lands in the kernel's fault handler
([Exercise 1](exercises.md#exercise-1-execute-a-privileged-instruction) proves
it). `ecall` is the only way across.

The **memory** boundary is now real too. U-mode is granted three ranges and
nothing else:

| Range | Permissions | What It Is |
|---|---|---|
| `[SOC_IRAM_LOW, _iram_text_end)` | R+X | The code U-mode executes |
| The user pool | R+W | `CONFIG_UMODE_ONCHIP_USER_KB` of internal SRAM; the arenas are carved from it |
| `[SOC_EXTRAM_LOW, SOC_EXTRAM_HIGH)` | R+W | All 32 MB of PSRAM |

Everything else matches no PMP entry at all, and **PMP is default-deny for
U-mode and default-allow for M-mode**, so the kernel reaches it freely and
U-mode cannot touch it however it tries. The boot report subtracts one from the
other rather than asserting anything:

```
KERNEL: PMP entries:
KERNEL:    1 - TOR   R-X 4ff00000..4ff11d00
KERNEL:    3 - TOR   RW- 4ff40000..4ff70000
KERNEL:    5 - TOR   RW- 48000000..4c000000
KERNEL: no PMP entry matches these, so U-mode cannot reach them at
KERNEL:   all while the kernel still can:
KERNEL:   internal SRAM  4ff11d00..4ff40000 (184 KiB)
KERNEL:   internal SRAM  4ff70000..4ffc0000 (320 KiB)
KERNEL:   PSRAM window   -- fully covered, U-mode reaches all of it
KERNEL:   flash window   40000000..44000000 (65536 KiB)
KERNEL:   TCM            30100000..30102000 (8 KiB)
KERNEL:   RTC / LP RAM   50108000..50110000 (32 KiB)
KERNEL:   peripherals    50000000..50100000 (1024 KiB)
```

**504 KiB of internal SRAM is off-limits to U-mode** — the kernel's heap, its
task stacks, its data — along with the slot table in TCM, the peripherals, RTC
RAM and the flash window.
[Exercise 10](exercises.md#exercise-10-reach-into-the-kernels-memory)
demonstrates the fault.

## How the Kernel Takes the PMP Over

`CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=n`, and then `pmp_apply()` in
`kernel_main.c` programs all sixteen entries itself.

That option is the whole trick. ESP-IDF's
`esp_cpu_configure_region_protection()` runs very early and **locks** every
entry it programs — and one of them grants U-mode read and write over the whole
of internal DRAM. A locked entry ignores writes to both its configuration and
its address, and PMP lock bits cannot be cleared without **Smepmp**, which the
ESP32-P4 does not implement. So there was no way to build a split on top of it.
Not because of the hardware — because of a default.

With the option off, IDF never calls that function and the PMP is left in its
**reset state**, which is a better starting point than anything that could have
been negotiated: every entry OFF means U-mode can touch *nothing*, and the
kernel hands back only what is needed.

**Nothing the kernel programs is locked, and that is the mechanism rather than
an oversight.** A PMP entry always applies to U-mode; the lock bit only decides
whether it *also* applies to M-mode. Unlocked entries therefore constrain the
user while leaving the kernel's own access untouched — exactly the asymmetry a
kernel/user split needs, and why the kernel can still reach memory the user
cannot.

The PMP is per-hart, so `pmp_apply()` runs on both cores;
`esp_ipc_call_blocking()` carries it to the second one. An unpinned host task
runs wherever the scheduler put it, so a grant made on one core only would be a
trap waiting to spring.

## Reading the PMP Configuration

The boot report decodes all sixteen entries on the board, so you can check the
above rather than take it on trust. `L` is the lock bit — all dashes now — then
the matching mode, then the permissions.

A **TOR** entry spans from the *previous* entry's address to its own, which is
why each range costs two entries and why the odd-numbered ones carry the
permissions. Entry 0 holds `SOC_IRAM_LOW` with its mode OFF, granting nothing
by itself, purely to give entry 1 a lower bound.

The raw `pmpcfg` words are printed too. Each packs four entries, one byte each,
entry 0 in the low byte:

| Bit | Field | Values |
|---|---|---|
| 7 | `L` | Lock. Once set the entry cannot be changed, and it binds M-mode too |
| 6:5 | — | Reserved, zero |
| 4:3 | `A` | Address matching: `0`=OFF, `1`=TOR, `2`=NA4, `3`=NAPOT |
| 2 | `X` | Execute |
| 1 | `W` | Write |
| 0 | `R` | Read |

## What U-mode Cannot Reach

Everything outside the three granted ranges — and the list in the boot report
is generated by subtracting them from the memory map, not written by hand, so
it stays true as the configuration changes.

Two entries in it are worth singling out.

**TCM holds the slot table.** 8 KiB of internal memory tightly coupled to the
CPU — wired into the pipeline rather than reached over the system bus — and
shared between both cores rather than a per-core alias like the CLIC, which
matters because an unpinned host task has to see the same slot wherever it
runs. `g_slots` holds every window's **saved context**, so without this, one
user scribbling at random could corrupt another window's saved registers.

```
KERNEL: slot table at 30100068 (1856 bytes) -- in TCM, which no PMP entry
                                               covers, so U-mode cannot reach it
```

**The peripherals** were reachable from U-mode under IDF's configuration and
are not now. Nothing noticed, because the user application never had any
business there — but it is a reminder that the old default was permissive in
ways this project never asked for.

## What Is Still Software

**The users are not isolated from each other.** All eight arenas are carved out
of the same pool, and the PMP grants that pool read and write as a single
range. It cannot distinguish one arena from another, so:

- a user can read and write another user's arena directly, and nothing stops
  it — [Exercise 4](exercises.md#exercise-4-reach-into-the-other-users-arena)
  still succeeds;
- `user_range_ok()` checks every syscall pointer against **the calling slot's**
  arena, which stops the kernel being *talked into* crossing that line on a
  user's behalf, but does nothing about a user crossing it directly;
- the stack guard does **not** close it either, and should not be mistaken for
  it. It watches `sp`, not accesses. A user that leaves `sp` alone and writes
  through a wild pointer is caught by neither.

## Closing What Remains

The user/user gap is a scheduling problem rather than a hardware limitation.
The PMP can describe one user's arena precisely; it just cannot describe eight
at once. Reprogramming a single entry pair in `umode_enter()` — pointing it at
the arena of the window about to run — would give each user a boundary the
hardware enforces, at the cost of two CSR writes per window.

That is a design rather than a hypothetical: `umode_pmp_program()` already
writes the entries, the window already saves and restores per-hart state around
every entry and exit, and the slot already knows its own bounds.

Three other things are still open.

**The IRAM text grant is whole-region.** U-mode gets read and execute over all
of `[SOC_IRAM_LOW, _iram_text_end)`, not just the user's own functions, so a
user can execute kernel IRAM code — including the trap vector. Narrowing it
needs the user's code in a linker section of its own.

**IDF's PMA entries are no longer programmed.** They came from the same
function, and they cover the *unmapped gaps* in the address space, existing to
fault on accesses to nothing. What is lost is a debugging aid rather than any
cacheability or correctness property of real memory — but it is a real loss,
and it is the price of that option being all-or-nothing.

**The split is not even.** 192 KiB to the user against 504 KiB kept by the
kernel, because the pool is a static array and ESP-IDF needs room to work in.
`CONFIG_UMODE_ONCHIP_USER_KB` moves the line directly.
