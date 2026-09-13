# The Demo

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
KERNEL: slot table at 30100044 (1856 bytes) -- in TCM, which no PMP entry
                                               covers, so U-mode cannot reach it
KERNEL: PMP entries:
KERNEL:    4 L TOR   R-X 4ff00000..4ff0fb00
KERNEL:    5 L TOR   RW- 4ff0fb00..4ffc0000
...
KERNEL: no PMP entry matches these, so U-mode cannot reach them at
KERNEL:   all while the kernel still can:
KERNEL:   TCM            30100000..30102000 (8 KiB)
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
illegal instruction ([Exercise 1](exercises.md#exercise-1-execute-a-privileged-instruction))
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
delayed-task list ([gotchas](reference.md#gotchas-worth-keeping)), so a sleeping task is
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
