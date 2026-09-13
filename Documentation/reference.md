# Reference

## The Files

| File | Privilege | What It Is |
|---|---|---|
| `main/kernel_main.c` | M | The kernel. Boot report, the slot table, the host loop, the syscall dispatcher, user-pointer validation |
| `main/umode.S` | M | `umode_enter()`, the private trap vector, the CLIC vector table |
| `main/user_main.c` | **U** | The user application. No library calls, no writable statics, compiled into IRAM |
| `main/user_syscall.S` | **U** | The `ecall` stubs and the exit trampoline |
| `main/umode.h` | Both | The context layout, shared with `umode.S` as byte offsets |
| `main/syscall.h` | Both | The kernel/user ABI |
| `probe/` | — | The earlier `mret`-to-U-mode experiment ([README](../probe/README.md)) |

## Build and Run

```bash
./build.sh          # configure for esp32p4 if needed, then build
./flash.sh          # build, then flash via /dev/cu.usbmodem*
./monitor.sh        # console on /dev/cu.usbserial-*
./clean.sh          # drop build/;  --full also drops sdkconfig
```

The scripts find ESP-IDF themselves — `IDF_VERSION=5.5.3 ./build.sh` picks a
different one — so there is no need to activate a toolchain beforehand. They do
not use the `useidf-*` alias because the activation script refuses to run from a
shell script and the `idf.py` it provides is a shell alias a non-interactive
shell will not expand; `scripts/idf-env.sh` asks it for its environment with
`-e` instead and calls `tools/idf.py` through the venv's python.

Both ports are optional arguments: `./flash.sh /dev/cu.usbmodem101`.

## Gotchas Worth Keeping

- **Chip revision.** IDF defaults to a minimum of v3.1 and the bootloader
  refuses to flash on v1.0 silicon (`requires chip revision in range [v3.1 -
  v3.99]`). `CONFIG_ESP32P4_REV_MIN_*` for the <3.0 family is gated behind
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`; both are in `sdkconfig.defaults`.

- **Watchdogs are off**, inherited from the probe, so that a hang persists and
  can be inspected with an observe-only JTAG halt rather than being reset away.

- **Non-standard CLIC CSRs.** `mtvt` is `0x307` and `mintstatus` is `0x346`
  rather than the `0xfb1` the CLIC specification gives it. Note that CLIC is a
  *proposal*, not a ratified extension — see
  [further reading](#further-reading) — so "non-standard" here means the P4
  disagrees with an unratified document, in one CSR number out of the two it
  takes from it.

- **`mtvt` must be 256-byte aligned and `mtvec.base` 64-byte aligned**, and
  `mtvec`'s low bits carry CLIC mode 3. Both are handled by `.balign` directives
  in `umode.S`; check them in the map file if the vector ever misbehaves.

- **The context layout is duplicated** between the C struct in `umode.h` and the
  `UCTX_*` offsets `umode.S` uses. `kernel_main.c` `_Static_assert`s every field,
  because the failure mode otherwise is a wild store inside a trap vector.

- **`CONFIG_SPIRAM=y` on its own does not boot.** `SPIRAM_SPEED_200M` is gated
  behind `CONFIG_IDF_EXPERIMENTAL_FEATURES` and the two have to be set
  together; without it the second-stage bootloader panics on a SPIMEM2
  register with a Store/AMO access fault. The flash size matters too — it was
  set to 2 MB here for a long time while the part is 16 MB, which the ROM
  reported on every boot and which nothing acted on. With
  `IDF_EXPERIMENTAL_FEATURES`, `SPIRAM_MODE_HEX`, `SPIRAM_SPEED_200M` and
  `ESPTOOLPY_FLASHSIZE_16MB` the 32 MB comes up cleanly.

- **A failed PSRAM bring-up needs a power cycle.** The bootloader fault keeps
  happening afterwards *with PSRAM disabled again*, from a clean rebuild, with
  a byte-identical `sdkconfig`, in a bootloader containing none of this
  project's code. PSRAM VDD comes from the MPLL LDO domain and the
  half-configured state survives an EN reset, so reflashing and resetting over
  RTS both achieve nothing. Pull the USB lead out and put it back in.

- **Only core 0 runs the full FreeRTOS tick.** ESP-IDF's
  `xTaskIncrementTick()` opens with
  `configASSERT( portGET_CORE_ID() == 0 )`, so core 0 alone walks the
  delayed-task list *and* applies the time-slice rotation. Two consequences
  bite anyone writing per-core code here: a task that blocks is almost always
  woken onto core 0, so a sleeping workload quietly herds itself onto one core;
  and whatever is running on core 1 is not rotated off by a tick. Both look
  like bugs in your own code when you first meet them.

- **`app_main` runs at priority 1.** Creating a task above that which never
  blocks preempts the rest of your initialisation permanently, and any task you
  meant to create afterwards simply never exists. The symptom looks exactly
  like scheduler starvation — a task that produces no output and no
  counters — and the giveaway is that its *creation-time* log line is missing
  too. Raise your own priority for the duration of the setup if you create
  tasks that spin.

- **`soc_caps.h` must be included in `umode.S`** for IDF's `_CUR_CORE` macros to
  dispatch on `mhartid` at all. Without it they silently collapse to their
  core-0 variants — and the failure mode is not a crash but a *silently
  unreported* stack violation, because reading the wrong core's `INTR_RAW`
  returns zero. IDF's own `portasm.S` includes it for the same reason.

- **The assist_debug per-core stride is assumed to be `0x80`.**
  `kernel_main.c` `_Static_assert`s it for all five registers `umode.S`
  addresses by hand, and `umode.S` carries an `#error` if it changes.

## Further Reading

Annotated with what in *this* project each one explains, because a spec index is
not much use to someone holding a specific question.

### RISC-V

| Document | What to Look up in It |
|---|---|
| [RISC-V International — specifications index](https://riscv.org/technical/specifications/) | The ratified specs, and which are ratified at all. Start here to check whether something is standard |
| [`riscv/riscv-isa-manual`](https://github.com/riscv/riscv-isa-manual) — source | The ISA and **privileged** manuals. The privileged manual is the reference for `mret`, `mstatus.MPP`/`MPIE`, `mcause`, `mtval`, `mepc`, `mscratch`, `ecall` and the PMP — every mechanism in [Part 1](concepts.md#part-1--the-concepts) and [3.3](hard-won-lessons.md#33-mcause-aliases-mstatusmpp) |
| [Ratified PDFs](https://github.com/riscv/riscv-isa-manual/releases) | Built copies of the above, if you would rather not render AsciiDoc |
| [`github.com/riscv`](https://github.com/riscv) | Every other spec repo, for anything not in the two manuals |

The PMP, including the `L` lock bit, the `A` matching modes, the
lowest-numbered-match-wins priority rule and why clearing a lock needs
**Smepmp**, is in the privileged manual's physical-memory-protection chapter.
That is the background to [Part 5](isolation-limits.md#part-5--the-limit-of-the-isolation-stated-plainly)
and to [reading this board's own PMP state](isolation-limits.md#reading-the-pmp-configuration), and
worth reading before deciding this project's memory isolation is weaker than it
had to be.

### CLIC

| Document | What to Look up in It |
|---|---|
| [`riscv/riscv-fast-interrupt`](https://github.com/riscv/riscv-fast-interrupt) | The CLIC proposal: `mtvt`, `mintstatus.mil`, `mcause.mpil`, interrupt levels, and the vectored-entry model the private vector in `umode.S` implements |
| [Built PDF releases](https://github.com/riscv/riscv-fast-interrupt/releases) | Rendered copies (v0.20 at the time of writing) |
| [Spec source](https://github.com/riscv/riscv-fast-interrupt/tree/master/src) | The AsciiDoc, if you want to diff versions |

**Read the status line on that repo before treating it as authority.** It is
titled a *proposal* for a Core-Local Interrupt Controller, and it is not a
ratified RISC-V extension. That is the honest explanation for a good deal of
what [Part 3](hard-won-lessons.md#part-3--six-things-that-do-not-work-the-obvious-way) documents:
the P4 implements a moving target, so `mintstatus` sits at `0x346` rather than
`0xfb1`, the interrupt threshold is a memory-mapped register rather than a CSR,
and `mcause` carries fields ([3.3](hard-won-lessons.md#33-mcause-aliases-mstatusmpp)) that no
ratified document describes. When silicon and this document disagree, the
silicon wins and the TRM below is the tie-breaker.

### ESP32-P4

| Document | What to Look up in It |
|---|---|
| [ESP32-P4 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-p4_technical_reference_manual_en.pdf) | The authority when the RISC-V documents and the hardware disagree. The interrupt matrix and its per-core routing ([3.5](hard-won-lessons.md#35-one-interrupt-source-two-monitors)), the CLIC's memory-mapped registers, and the debug assist peripheral's per-core register blocks and SP-monitor semantics ([3.1](hard-won-lessons.md#31-the-hardware-stack-guard-fires-on-the-stack-switch)) |
| [ESP32-P4 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf) | Part numbering, revisions, and the memory map the PMP entries in [Part 5](isolation-limits.md#part-5--the-limit-of-the-isolation-stated-plainly) are cut from |
| [ESP-IDF v5.5.4 — ESP32-P4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/index.html) | The exact IDF version this tree is built against |

Two ESP-IDF sources are quoted directly in the code and are worth opening
alongside it, in your local IDF checkout rather than online, so the version
matches:

- `components/esp_system/port/include/private/esp_private/hw_stack_guard.h` —
  the stack-guard macros `umode.S` uses, and the `SOC_CPU_CORES_NUM` gate that
  decides whether they dispatch on `mhartid`
  ([1.4](concepts.md#14-eight-windows-two-cores-no-pins));
- `components/freertos/FreeRTOS-Kernel/portable/riscv/portasm.S` — how the port
  itself arms the guard on every interrupt exit, which is what
  [3.1](hard-won-lessons.md#31-the-hardware-stack-guard-fires-on-the-stack-switch) has to work
  around.

## Glossary

| Term | Meaning |
|---|---|
| **hart** | Hardware thread — one RISC-V core. The P4 has two |
| **M-mode** | Machine mode. Full privilege. ESP-IDF and FreeRTOS run here |
| **U-mode** | User mode. Lowest privilege. No CSRs, no privileged instructions |
| **`ecall`** | The instruction that traps from a lower privilege to a higher one. The only way from U to M |
| **`mret`** | Return from a trap. Also how the kernel *enters* U-mode, by setting `MPP=U` first |
| **`mcause`** | Why the trap happened. On this SoC it also packs previous privilege, interrupt-enable and interrupt level |
| **`mtval`** | Trap value — for an illegal instruction, the instruction word itself |
| **`mepc`** | The PC the trap interrupted, and where `mret` returns to |
| **`mtvec` / `mtvt`** | Trap vector base / CLIC vector table base. Per-hart |
| **`mscratch`** | A scratch CSR, unused by ESP-IDF here, used to carry the context pointer into the trap vector |
| **CLIC** | The P4's core-local interrupt controller. Vectored, with an interrupt *level* rather than a simple enable |
| **CLIC threshold** | The only thing that masks interrupts in U-mode, since `mstatus.MIE` does not apply there |
| **PMP** | Physical memory protection. Region-based permissions that apply to U-mode, and to M-mode when locked |
| **Smepmp** | The RISC-V extension that would allow clearing PMP lock bits. **Not** implemented on the P4 |
| **assist_debug** | The ESP32-P4 debug assist peripheral. Its SP monitor is what ESP-IDF's hardware stack guard uses |
| **window** | One entry into and return from U-mode: `umode_enter()` to the trap that ends it |
| **arena** | One user's memory — its stack and data. One per slot |
| **slot** | `user_slot_t`: everything one U-mode window owns |
