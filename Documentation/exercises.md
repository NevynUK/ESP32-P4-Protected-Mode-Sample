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
  zero, so the trap came from U-mode ([3.3](hard-won-lessons.md#33-mcause-aliases-mstatusmpp)).
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
([3.1](hard-won-lessons.md#31-the-hardware-stack-guard-fires-on-the-stack-switch)). `detected at
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
DRAM (see [Part 5](isolation-limits.md#part-5--the-limit-of-the-isolation-stated-plainly)), so both
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
[why the users are busy](the-demo.md#why-the-users-are-busy).

## Exercise 6: Return from `user_main`

Delete the `for (;;)` so `user_main` falls off its end. The context is created
with `ra` pointing at `u_exit_stub`, so this becomes a `SYS_EXIT` rather than a
wild branch. Expect `user_main exited, status …` and a clean retirement.

## Exercise 7: Leave Interrupts Unmasked

Comment out the threshold raise in `umode_enter` and see how far it gets.
Predict first — [3.2](hard-won-lessons.md#32-the-clic-threshold-write-is-not-immediately-effective)
explains what does and does not break, and why the failure mode is a *lost*
edge-triggered interrupt rather than an immediate crash. Watch the host loop's
interrupt-in-U-mode counter in the retirement message.

## Exercise 8: Add a Syscall of Your Own

Follow [Adding a Syscall](adding-a-syscall.md#adding-a-syscall) and add `SYS_TICKS`, returning
`xTaskGetTickCount()`. Three edits, no registration, and the user can then
print elapsed time without a clock of its own.

Then add a deliberately awkward one: `SYS_READ`, which copies a kernel-chosen
string *into* a buffer the user supplies. That forces you to meet both of the
rules that matter — validating the caller's pointer and length against its own
slot before writing a byte, and deciding whether a bad pointer retires the user
or just returns `SYS_ERR_FAULT`. Get the first one wrong and you have handed
every user a way to write into any other user's arena, which is precisely the
hole [Part 5](isolation-limits.md#part-5--the-limit-of-the-isolation-stated-plainly) is about.

## Exercise 9: Scribble on the Kernel's Slot Table

*Is any of this enforced by hardware rather than by a check the kernel makes?*
The boot report prints where the slot table lives. Add to `user_main`'s loop:

```c
if (id == 0 && tick == 18)
{
    *(volatile uint32_t *) 0x30100044u = 0xdeadbeefu;   /* the slot table */
}
```

**Captured:**

```
KERNEL: user0: user fault: store access fault
KERNEL:   mcause=08000007 mtval=30100044 pc=4ff01ed0 sp=4ff15000 ra=4ff01ebc
KERNEL: user0: user context retired after 360028 syscalls
User 1.3 on core 0
User 2.3 on core 1
```

Three things to take from that:

- `mcause=08000007` — exception code **7**, a store access fault. Bits 29:28 are
  zero, so it came from U-mode ([3.3](hard-won-lessons.md#33-mcause-aliases-mstatusmpp)).
- `mtval` is **exactly** the slot table address. The hardware names the thing it
  refused, and nothing in the kernel had to check anything.
- The other seven windows carry on. No panic.

Now contrast it with [Exercise 3](#exercise-3-hand-the-kernel-a-pointer-outside-your-arena),
where a bad pointer is caught by `user_range_ok()` — a *software* check, which
only works because the pointer was handed to the kernel. Here the user never
asked permission; it simply stored, and the PMP refused. That difference is the
whole of [the isolation limits](isolation-limits.md#what-u-mode-cannot-reach).

Then try the same store against one of the other users' arenas, whose addresses
the boot report also prints. It succeeds silently. That is the gap.
