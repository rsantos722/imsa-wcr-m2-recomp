# Guest Runtime, Memory Layout, and Task Model

The runtime library ([`runtime/`](../../runtime/)) provides the execution environment for statically recompiled PowerPC code. It manages the big-endian guest address space, loads and verifies ELF sections from the game data directory, resolves indirect function calls, and runs the multi-threaded 3DO M2 Portfolio OS kernel scheduler.

## Runtime Ownership Architecture

[`runtime/src/recomp_entry.c`](../../runtime/src/recomp_entry.c) owns the primary guest execution state (`g_ctx` and `g_guest_kernel_ctx`):

| Resource | Implementation | Description |
| :--- | :--- | :--- |
| **`PPCContext` (`g_ctx`)** | [`recomp_entry.c`](../../runtime/src/recomp_entry.c) | CPU register file (`gpr[32]`, `fpr[32]`, `cr`, `lr`, `ctr`, `xer`) and base memory pointer (`mem`). |
| **Guest RAM** | `RECOMP_GUEST_MEM` | 256 MiB zero-initialized contiguous byte buffer shared across all guest tasks. |
| **Kernel Context (`m2k_context`)** | [`guest_kernel.c`](../../runtime/src/guest_kernel.c) | Thread-safe object pools for tasks, signals, message ports, I/O requests, and semaphores. |
| **Module Registry** | [`guest_module.c`](../../runtime/src/guest_module.c) | Tracks loaded and dynamically spawned modules (`LaunchMe.m2`, `driving/game`, `shooter2d`). |
| **Frame Snapshot** | `0x003F0000` | State telemetry buffer queried via `m2_guest_frame_query()`. |

## Guest Virtual Address Space Map

Guest pointers are 32-bit unsigned big-endian addresses (`uint32_t`) indexed directly into `c->mem`. Out-of-bounds accesses and hardware MMIO polling ranges (such as `0x20202028`) are intercepted via shadow tables and bounds checks.

```text
Guest VA Range              Region / Content
--------------------------------------------------------------------------------
0x00000000 - 0x000FFFFF     Reserved / Null-pointer guard page
0x00100000 - 0x0017606B     driving/game .text (483,436 bytes)
0x00176070 - 0x00181297     driving/game .data (45,608 bytes)
0x001812A0 - 0x0018B197     driving/game .bss  (40,696 bytes, zero-initialized)
0x0018B198 - 0x0019B198     driving/game initial main thread stack (r1 = 0x0019B198)
0x00200000 - 0x002BFFFF     Spawned guest task stacks (64 KiB per slot, 12 slots)
0x002C0000 - 0x002D23E7     shooter2d/shooter2d .text (74,728 bytes)
0x002D23F0 - 0x002D2593     shooter2d/shooter2d .data (420 bytes)
0x002D25A0 - 0x002DEFD7     shooter2d/shooter2d .bss  (51,768 bytes)
0x002E0000 - 0x002E06B7     LaunchMe.m2 (imsa_shell) .text (1,720 bytes)
0x002E06C0 - 0x002E06C7     LaunchMe.m2 (imsa_shell) .data (8 bytes)
0x002E06D0                   LaunchMe.m2 (imsa_shell) empty .bss
0x00300000 - 0x003EFFFF     Kernel, EventBroker, IFF, GState, and Font scratch buffers
0x003F0000 - 0x003F001F     Telemetry carve: Frame counter, primary state, loop PC
0x003F1000 - 0x003F1FFF     driving/game synthetic folio import-slot table
0x003F2000 - 0x003F2FFF     shooter2d/shooter2d synthetic folio import-slot table
0x003F3000 - 0x003F3FFF     LaunchMe.m2 boot tag staging area
0x003F4000 - 0x003F4FFF     LaunchMe.m2 synthetic folio import-slot table
0x00400000 - 0x0FFFFFFF     Guest dynamic heap (GK_HEAP_BASE to 256 MiB host window)
```

## Startup Image Loading and Relocation Fixup

Generated C holds translated code but no copies of the disc's sections. [`guest_mem_init()`](../../runtime/src/recomp_entry.c) loads the original binary sections from the user's extracted disc directory at startup:

1. **Locate Data Root**: Resolves the game data path from `--game-data` or `IMSA_DATA_DIR`.
2. **Hash Verification**: Verifies the SHA-256 digests of `LaunchMe.m2`, `driving/game`, and `shooter2d/shooter2d` against [`launcher/disc-manifest.json`](../../launcher/disc-manifest.json).
3. **Memory Mapping**: Copies raw `.text` (used for inline constant pools and jump tables) and `.data` sections into `c->mem`, and zeroes `.bss`.
4. **Relocation Application**: Applies `R_ADDR32` fixup tables (`{offset, abs_guest_va}` pairs) to resolve static pointers in `.data` and `.text`.
5. **Folio Import Slots**: Populates synthetic descriptors in `0x003F1000+` so guest instructions that read imported folio globals see valid structures.

## Boot Sequence and Module Control

The [boot page](../game/boot.md) covers the shell and child handoff in detail.

- **Standard Disc Boot (`m2_boot_disc_shell`)**: Registers all three modules in `m2_module_registry` and starts `LaunchMe.m2` (`imsa_shell`) at `0x002E0000`. The shell changes directory to `"driving"`, opens `"game"`, spawns `driving/game` on a child task thread, and waits on a synchronization port. When `driving/game` exits with status `1` (the [Skid Pad shooter trigger](../game/tracks.md#skid-pad-trigger-volumes)), the shell starts `shooter2d/shooter2d`.
- **Direct Boot Override (`M2_BOOT_APP`)**: Setting `M2_BOOT_APP=game` boots `driving/game` (`FN_main__1ef6c` at `0x0011EF6C`) directly; `M2_BOOT_APP=shooter2d` boots the companion game (`FN_shooter2d_main__002a0` at `0x002C02A0`) directly.

## Indirect Function Dispatch (`recomp_lookup`)

When guest code branches through a function pointer (`mtctr; bctrl`) or computed return (`blrl`), the runtime maps the 32-bit guest address to a lifted C function pointer (`RecompFn`):

1. **Binary Search Dispatch**: Each module exports a sorted table `kDispatch[]` of `{uint32_t guest_va, RecompFn fn}` entries searched in $O(\log N)$ time by `recomp_lookup()`.
2. **Cross-Module Resolution**: If the target is not in the current module, `m2_cross_module_lookup()` searches the remaining registered modules (`game`, `shooter2d`, `launchme`).
3. **Text-Relative Fallback**: Some callback slots in `.bss` hold raw `.text` section offsets rather than relocated virtual addresses; if `va < text_size` and the direct lookup misses, the runtime retries at `va + text_base`.

## Multi-Threaded Task and Kernel Scheduler

Each spawned guest task runs on a native POSIX thread (`pthread`) coordinated through the shared `m2k_context` scheduler:

| Guest Thread | Entry VA | Responsibilities |
| :--- | :--- | :--- |
| `imsa_shell` | `0x002E0000` | Root disc launcher; manages child module execution and handoff. |
| `main` / `StateDriver` | `0x0011EF6C` / `0x0012FA4C` | Main game thread: `main` calls `StateDriver`, which runs menus, HUD, rendering, and the state machine; paced by 59.94 Hz VBL. |
| `CarSimThread` | `0x0010B054` | Vehicle physics simulation; paced by a 16,666 µs periodic metronome timer. |
| `startuptrackinitthread_1` / `_2` | `0x0012E604` / `0x0012E65C` | Background track geometry and texture loading during the loading screen. |
| `DataStreamThread` / `DataAcqThread` | `0x001692A0` / `0x0016A388` | CD-ROM streaming buffers and FMV/audio data acquisition. |
| `MPADecoderThread` | `0x00163CF8` | Background MPEG audio decompression worker. |

`m2k_context` manages pools for up to `M2K_MAX_TASKS = 12` concurrent tasks, each given a 64 KiB stack at `0x00200000 + (slot + 1) * 0x10000`. Higher-priority tasks preempt lower-priority tasks at kernel synchronization points (`WaitSignal`, `SendMsg`, `LockSemaphore`), while equal-priority tasks share time in 15 ms round-robin quantums (`t_Quantum`).
