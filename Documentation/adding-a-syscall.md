# Adding a Syscall

Three edits, in three files. `SYS_GETCORE` is the whole worked example, because
it is about six lines end to end.

**1. Give it a number** — `main/syscall.h`:

```c
#define SYS_GETCORE    5    /* -> the core this window is running on        */
```

**2. Implement it** — the `switch` in `syscall_dispatch()`, `main/kernel_main.c`:

```c
case SYS_GETCORE:
    ret = (uint32_t) xPortGetCoreID();
    break;
```

This runs in the host task, in M-mode, with the window closed — so blocking is
legal here and `SYS_DELAY_MS` really is a `vTaskDelay()`. That is the whole
reason the trap vector does no dispatching ([1.2](concepts.md#12-u-mode-is-not-a-task-it-is-a-coroutine-hosted-by-one)).

**3. Call it** — from `user_main.c`:

```c
pos = u_append_u32(msg, pos, u_syscall0(SYS_GETCORE));
```

There is no third step for registration. There is no dispatch table.

## Three Rules That Are Easy to Get Wrong

**Validate every pointer against the calling slot.**
`user_range_ok(u, addr, len)` — `u`, not a global arena. The PMP keeps U-mode
out of the kernel's memory, but all eight arenas sit in one pool it grants as a
single range ([Part 5](isolation-limits.md#part-5--the-limit-of-the-isolation-stated-plainly)),
so this check is the only thing keeping the users out of each other's memory. Use `user_strlen(u, addr, max)` for strings.

**`break` and `return false` are the recoverable/fatal decision.** `break` falls
through to the tail, which steps `pc` past the `ecall` and writes `ret` into the
user's `a0`; the user carries on. `return false` retires the context. A bad
pointer should be recoverable — `ret = SYS_ERR_FAULT; break;` — because a user
passing rubbish is a bug in the user, not a privilege violation. Only `SYS_EXIT`
and real faults are fatal.

**You cannot return a string.** The return value is one register. Anything
larger has to be copied into a buffer the caller owns, whose pointer and length
you validated — `SYS_PUTS` in reverse.

## Limits

| Limit | Value | If you need more |
|---|---|---|
| Arguments | **2** (`u_syscall0/1/2`) | Write `u_syscall3` in `user_syscall.S` and read `ctx->x[12]` in the dispatcher |
| Arguments, ABI ceiling | **7** (`a0`–`a6`) | `a7` carries the number, so past `a6` you are out of registers — pass a struct in the arena instead |
| Return value | one `uint32_t`, in `a0` | An out-pointer into the caller's arena |
| Syscall number | `a7`, any `uint32_t` | — |
| `SYS_PUTS` message | `SYS_PUTS_MAX`, 128 bytes | Raise it, or use `SYS_WRITE`, which is counted and unbounded |

**On the argument count:** the stubs take the number in `a0` and shift the
arguments down one register, so `u_syscall2(nr, x, y)` arrives as `a7=nr`,
`a0=x`, `a1=y`. A stub for seven arguments has to move `a7` out of the way
*before* overwriting it with the number, via a scratch register — the naive
`mv a7, a0` first would destroy the last argument.

**On the numbering:** there is no ID table and no registration. The numbers are
`#define`s and the dispatcher is a `switch`, so a number and its implementation
cannot drift apart — an unhandled number falls to `default` and returns
`SYS_ERR_BADNR` without retiring the user. They happen to be contiguous from 0;
nothing requires that.

**On the error codes:** `SYS_ERR_BADNR` and `SYS_ERR_FAULT` are `(uint32_t)-1`
and `-2`, so they sit at the top of the value space. A call that could
legitimately return `0xFFFFFFFE` would be indistinguishable from a failure.
None currently can, but a new one might, and the fix is an out-parameter rather
than a cleverer sentinel.

**One thing not to assume:** which core you are on. Nothing is pinned, so
`xPortGetCoreID()` is valid for this call only, and the same window may service
its next syscall on the other core.
