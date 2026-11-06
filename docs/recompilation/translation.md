# PowerPC 602 to C11 Static Translation

The static recompiler ([`recompiler/transpile.py`](../../recompiler/transpile.py)) translates big-endian 3DO M2 relocatable ELF binaries (`ET_REL`) into portable C11 source shards ahead of time. Each PowerPC function is lifted into a native C function compiled directly by the host C compiler (Clang or GCC).

## Input Binary Architecture

The three executable modules on the prototype disc—`driving/game`, `shooter2d/shooter2d`, and `LaunchMe.m2`—were compiled with Diab Data `dcc` 3.7a for the PowerPC 602:

| Property | Value |
| :--- | :--- |
| **Container Format** | 32-bit Big-Endian ELF (`ELFCLASS32`, `ELFDATA2MSB`) |
| **ELF Object Type** | `ET_REL` (`e_type = 1`, relocatable object module) |
| **Machine Architecture** | `0x0011` (`e_machine = 17`, 3DO M2 PowerPC) |
| **Target ABI** | Diab Data PowerPC EABI (32-bit single-precision hardware FPU) |
| **Primary Sections** | `.text`, `.data`, `.bss`, `.symtab`, `.strtab`, `.rela.text`, `.rela.data`, `.hdr3do`, `.imp3do` |

In `driving/game` (1,831,772 bytes total with symbols), `.text` is 483,436 bytes (120,859 words), `.data` is 45,608 bytes, and `.bss` is 40,696 bytes. The translator lifts 1,365 functions covering 110,105 instructions across 129 distinct PowerPC mnemonics.

## Guest Virtual Address Layout

Because M2 executables are relocatable (`ET_REL`) modules without fixed load addresses, the translator assigns fixed virtual address bases at configure time:

$$\begin{aligned}
\text{text\_base} &= \text{profile base address} \\
\text{data\_base} &= \operatorname{align16}(\text{text\_base} + \operatorname{sizeof}(\text{.text})) \\
\text{bss\_base} &= \operatorname{align16}(\text{data\_base} + \operatorname{sizeof}(\text{.data}))
\end{aligned}$$

| Image Target | Profile | `text_base` | `data_base` | `bss_base` | Import Table Base |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `driving/game` | `driving-game` | `0x00100000` | `0x00176070` | `0x001812A0` | `0x003F1000` |
| `shooter2d/shooter2d` | `generic` | `0x002C0000` | `0x002D23F0` | `0x002D25A0` | `0x003F2000` |
| `LaunchMe.m2` | `generic` | `0x002E0000` | `0x002E06C0` | `0x002E06D0` | `0x003F4000` |

## Function Discovery Passes

[`recompiler/transpile.py`](../../recompiler/transpile.py) identifies function boundaries from `.symtab` and runs three recovery passes for assembly helpers and compiler thunks:

1. **Named Symbols**: Emits `STT_FUNC` symbols with `st_size > 0` as `FN_<sanitized_name>__<offset>(PPCContext* c)`.
2. **Zero-Size Leaf Routines (`ZERO_SIZE_SPANS`)**: Hand-written assembly routines in `.symtab` often have `st_size == 0` (such as `sinf`, `cosf`, `sqrtf`, `GetBusClock`, and `SetTEWritePointer`). The translator bounds each span at the next symbol boundary.
3. **Diab Varargs Thunks (`DECVASTUB`)**: Diab `dcc` emits 12-byte varargs entry stubs (`DECVASTUB_<N>`) immediately before varargs functions to spill `r3`–`r10` onto the stack. The translator promotes these into callable entry functions.
4. **Unmarked Gap Helpers**: Scans remaining gaps between symbolised functions for branch targets (`b`, `bl`, `bc`), traces reachability to return instructions (`blr`, `bctr`), and emits `FN_gap_<offset>__<offset>()` while leaving inline float and jump-table data pools untouched.

## Relocation Resolution

The M2 ELF toolchain uses custom relocation types in `.rela.text` and `.rela.data`, which are resolved before instruction lifting:

| Relocation Type | Name | Formula / Meaning |
| :--- | :--- | :--- |
| `164` | `R_LO16` | Low 16 bits of target address: `(target + addend) & 0xFFFF`. |
| `165` | `R_HI16` | High 16 bits without sign adjustment: `((target + addend) >> 16) & 0xFFFF`. |
| `166` | `R_HA16` | High 16 bits adjusted for signed 16-bit addition: `((target + addend + 0x8000) >> 16) & 0xFFFF`. |
| `186` | `R_ADDR32` | 32-bit absolute address word (`target + addend`), used in jump tables and pointer arrays. |
| `196`–`198` | `R_*_FOLIO` | References to imported Portfolio OS folio data symbols, resolved to synthetic slots in `0x003F1000+`. |
| `202` | `R_IMPREL24` | 24-bit branch encoding a Portfolio OS folio selector: `(module_index << 16) | ordinal`. |

Generated comments show each instruction's unrelocated section offset, while the C statement uses the resolved guest address. For example, from `M_SetupDynLit`:

```c
/* 00028: lis r12, 5 */
c->gpr[12] = 0u + ((uint32_t)(int32_t)(int16_t)21 << 16);
/* 00030: addi r12, r12, 0x3568 */
c->gpr[12] = c->gpr[12] + (uint32_t)13672;
```

Unrelocated `.text+0x53568` (`(5 << 16) + 0x3568`) resolves to guest VA `0x153568` (`(21 << 16) + 13672`) after adding `text_base` (`0x100000`).

## CPU Context and Instruction Lifting

Every lifted function takes a pointer to the task's [`PPCContext`](../../runtime/include/ppc_ctx.h) (`void FN_calc_forces__3fbe4(PPCContext *c)`), accessing guest memory through inline big-endian helpers (`rd8`, `rd16`, `rd32`, `wr8`, `wr16`, `wr32`, `rdf32`, `wrf32`, and `rdf64_as_f32` / `wrf32_as_f64` for `lfd` / `stfd`).

- **Integer and Bitwise Operations**: Standard arithmetic lowers to C expressions; carry-tracking instructions (`addc`, `adde`, `subfe`) use 64-bit unsigned sums to update `XER[CA]`, and rotate-and-mask instructions (`rlwinm`, `rlwimi`, `rlwnm`) lower to `rotl32()` and bitmasks.
- **Condition Register**: Signed (`cmpw`) and unsigned (`cmplw`) comparisons update condition fields (`crf0`–`crf7`) with `LT`, `GT`, `EQ`, and `SO` bits.
- **Control Flow**: Intra-function branches lower to `goto L<offset>;`, direct calls (`bl`) invoke `FN_<target>__<offset>(c);`, switch tables (`bctr` over `.data` jump tables) emit native `switch (c->ctr)` blocks, and indirect calls (`bctrl`, `blrl`) dispatch through `recomp_lookup()`.

## Floating-Point Implementation

The PowerPC 602 has a 32-bit single-precision FPU with hardware fused multiply-add instructions:

- **Compiler Flags**: Generated shards are built with `-ffp-contract=off -fno-fast-math -fno-unsafe-math-optimizations` so the host compiler does not fuse separate multiply and add instructions.
- **Single-Precision Rounding**: Basic operations (`PPC_FADD`, `PPC_FMUL`, etc.) pass results through `volatile float` temporaries to enforce 32-bit IEEE-754 rounding after each instruction, while PowerPC FMA instructions (`fmadds`, `fmsubs`, `fnmadds`, `fnmsubs`) map to `ppc_fma_single(a, c, b)`, a correctly rounded helper in `runtime/include/ppc_ctx.h` that does not depend on the host C runtime's `fmaf`.
- **Reciprocal Square-Root Estimate (`frsqrte`)**: Uses `ppc_frsqrte()`, satisfying the PowerPC relative error bound ($|e| \le 1/32$) and IEEE special-value rules ($\pm 0$, $\text{NaN}$, $\pm \infty$).
- **Gradual Underflow**: On x86_64, `MXCSR` clears Flush-To-Zero (`FTZ`) and Denormals-Are-Zero (`DAZ`) so subnormal numbers behave identically to the PowerPC 602.

## Generated Shards

At configure time, `cmake/ImsaGenerate.cmake` writes the lifted output to `<build>/generated/`:

| File Pattern | Purpose |
| :--- | :--- |
| `<prefix>_decls.h` | Function prototypes, section base addresses, and relocation externs. |
| `<prefix>_text_00.c` .. `<prefix>_text_NN.c` | Lifted C11 functions split into shards of 120 functions per file for parallel builds. |
| `<prefix>_data.c` | Section sizes, file offsets, SHA-256 digests, and `R_ADDR32` relocation arrays (contains no copyrighted asset bytes). |
| `<prefix>_dispatch.c` | Sorted `kDispatch[]` binary-search table mapping guest VAs to function pointers. |
| `recomp_report.json` | Translation summary with mnemonic counts, discovered functions, and jump tables. |
