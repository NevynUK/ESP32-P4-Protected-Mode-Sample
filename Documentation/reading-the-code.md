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
  [Part 5](isolation-limits.md#part-5--the-limit-of-the-isolation-stated-plainly) explains why that
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
