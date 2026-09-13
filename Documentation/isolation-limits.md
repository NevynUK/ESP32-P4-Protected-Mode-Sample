# Part 5 — The Limit of the Isolation, Stated Plainly

**Read this before citing the project as a protected-mode example.**

The **privilege** boundary is real. U-mode code that executes a `csr`
instruction, an `mret`, or anything else reserved to M-mode takes an
illegal-instruction trap and lands in the kernel's fault handler
([Exercise 1](exercises.md#exercise-1-execute-a-privileged-instruction) proves it). `ecall`
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
- **with one exception: the slot table.** It lives in TCM, which no PMP entry
  covers at all, so U-mode cannot reach it however it tries — see
  [What U-mode Cannot Reach](#what-u-mode-cannot-reach) below;
- the stack guard does **not** close the gap and should not be mistaken for it.
  It watches `sp`, not accesses. A user that leaves `sp` alone and writes through
  a wild pointer is caught by neither, which is what `user_range_ok()` is for.

### Reading the PMP Configuration

The boot report decodes all sixteen entries on the board rather than describing
them here, so you can check the above rather than take it on trust:

```
KERNEL: PMP entries:
KERNEL:    0 L NAPOT RW- 20000000..30000000
KERNEL:    3 L OFF   --- matches nothing
KERNEL:    4 L TOR   R-X 4ff00000..4ff0fb00
KERNEL:    5 L TOR   RW- 4ff0fb00..4ffc0000
KERNEL:    9 - OFF   --- matches nothing
KERNEL:   13 L TOR   --- matches nothing
```

`L` is the lock bit, then the matching mode, then the permissions. Two entries
in that list are doing nothing at all: 9 and 10 are unlocked and unprogrammed,
and **13 is a TOR entry whose base and limit are the same address**, so it
matches an empty range. One of the sixteen is wasted.

The raw `pmpcfg` words are printed too. Each packs four entries, one byte each,
entry 0 in the low byte:

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

`components/esp_hw_support/port/esp32p4/cpu_region_protect.c` in your IDF
checkout is the file to read alongside this table — it is what programs every
entry above.

### What U-mode Cannot Reach

Subtracting the covered ranges from the memory map gives the other half of the
picture, and the boot report does that too. **PMP is default-deny for U-mode and
default-allow for M-mode**, so an address no entry matches is unreachable from
U-mode and perfectly reachable from the kernel:

```
KERNEL: no PMP entry matches these, so U-mode cannot reach them at
KERNEL:   all while the kernel still can:
KERNEL:   internal SRAM  -- fully covered, U-mode reaches all of it
KERNEL:   PSRAM window   48000000..4c000000 (65536 KiB)
KERNEL:   flash window   40030000..44000000 (65344 KiB)
KERNEL:   TCM            30100000..30102000 (8 KiB)
KERNEL:   RTC / LP RAM   -- fully covered, U-mode reaches all of it
KERNEL:   peripherals    -- fully covered, U-mode reaches all of it
```

Those ranges are the only memory on this board that U-mode cannot touch **by
hardware**, and they cost nothing: no unlocking, no reconfiguration, no spare
PMP entry. They are simply what IDF never described.

**TCM is the usable one.** 8 KiB of internal memory, tightly coupled to the
CPU — wired into the pipeline rather than reached through the system bus, so
access is fast and deterministic — and shared between both cores rather than a
per-core alias like the CLIC. That last point matters here: an unpinned host
task has to see the same slot wherever it runs.

So the slot table lives there:

```
KERNEL: slot table at 30100044 (1856 bytes) -- in TCM, which no PMP entry
                                               covers, so U-mode cannot reach it
```

`g_slots` holds every window's **saved context**. Before the move, one user
scribbling at random could corrupt another window's saved registers — the
weakest link in an isolation story that is otherwise entirely software. Now it
cannot, and [Exercise 9](exercises.md#exercise-9-scribble-on-the-kernels-slot-table)
demonstrates the fault.

The PSRAM and flash windows are not usable for this. The flash window is not
RAM, and PSRAM is unreachable only because `CONFIG_SPIRAM` is off — enabling it
makes IDF describe that window with a locked R+W grant, which would hand U-mode
access to the PSRAM heap rather than protect anything. (It does not currently
boot on this board in any case; see `sdkconfig.defaults`.)

Keep it in proportion. This is 1856 bytes of hardware-enforced protection in a
system whose arenas — the thing users actually write to — are still shared
DRAM, separated only by `user_range_ok()`. It closes one specific hole, not the
gap.

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
