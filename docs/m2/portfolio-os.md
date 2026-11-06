# Portfolio OS v3.0 Kernel, Folios, Memory, and IPC Architecture

3DO M2 runs Portfolio OS v3.0, an object-oriented, message-passing microkernel built around typed 32-bit handles called **Items** and dynamically linked system libraries called **Folios**.

In this repository, the Portfolio OS kernel, memory manager, scheduler, synchronization primitives, and Folio dispatch tables are implemented across [`runtime/`](../../runtime/) and [`shims/`](../../shims/).

## Item Architecture and `ItemNode` Header

Every kernel-managed resource (Task, Thread, MsgPort, Message, Semaphore, IOReq, Device, Folio, File, Bitmap, View, Audio Instrument, Audio Sample, Audio Attachment, Cue, Timer) is referenced by a 32-bit `Item` handle (`int32`). Negative `Item` values represent Portfolio OS `Err` codes.

When a guest function calls `LookupItem(item)` (`1:34`), `CheckItem(item, ftype, ntype)` (`1:29`), or `OpenItem` (`1:162`), the kernel returns a 32-bit guest pointer to an in-memory structure whose first `36` bytes (`0x24`) are a big-endian `ItemNode`:

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `+0x00` | `4` | `u32` | `n_Next` | Next node pointer in kernel list |
| `+0x04` | `4` | `u32` | `n_Prev` | Previous node pointer in kernel list |
| `+0x08` | `1` | `u8` | `n_SubsysType` | Subsystem / Folio ID (`KERNELNODE = 1`, `GRAPHICSNODE = 3`, `FILENODE = 4`, `AUDIONODE = 4/10`, etc.) |
| `+0x09` | `1` | `u8` | `n_Type` | Subsystem node type (e.g., `TASKNODE = 1`, `MSGPORTNODE = 2`, `SEMANODE = 4`, `IOREQNODE = 5`, `DEVICENODE = 6`, `TIMERNODE = 7`, `MESSAGENODE = 8`) |
| `+0x0A` | `1` | `u8` | `n_Priority` | Node priority (`0..255`) |
| `+0x0B` | `1` | `u8` | `n_Flags` | Node flags |
| `+0x0C` | `4` | `i32` | `n_Size` | Total byte size of the structure |
| `+0x10` | `4` | `u32` | `n_Name` | Guest pointer to NUL-terminated ASCII item name (`0` if unnamed) |
| `+0x14` | `1` | `u8` | `n_Version` | Version byte |
| `+0x15` | `1` | `u8` | `n_Revision` | Revision byte |
| `+0x16` | `1` | `u8` | `n_Reserved0` | Reserved |
| `+0x17` | `1` | `u8` | `n_ItemFlags` | Item table flags (`ITEM_WAS_OPENED`, etc.) |
| `+0x18` | `4` | `Item` | `n_Item` | Self `Item` handle |
| `+0x1C` | `4` | `Item` | `n_Owner` | Owning Task `Item` |
| `+0x20` | `4` | `u32` | `n_Reserved1` | Subsystem extension pointer |

## Modules and Folios

A module is a relocatable executable or shared library containing `.imp3do` import and export records, while a folio is a system module that exposes an API vector table. Because `.imp3do` module indices are local to each binary, the transpiler maps them to the project's host folio IDs (`1..9`; see [Selector Dispatch Architecture](../recompilation/shims.md#selector-dispatch-architecture)).

### Imported Kernel Ordinals (`driving/game`, Folio 1)

| Category | Ordinals (`1:N`) |
|---|---|
| **Memory Allocation** | `#2 AllocMem`, `#3 FreeMem`, `#16 GetMemInfo`, `#17 ScavengeMem`, `#35 AllocMemMasked`, `#57 AllocMemDebug`, `#59 FreeMemDebug` |
| **Items & Folios** | `#28 UniversalInsertNode`, `#29 CheckItem`, `#34 LookupItem`, `#36 FindNamedNode`, `#37 PrepList`, `#80 CreateItemVA`, `#119 CreateItem`, `#133 FindAndOpenItem`, `#159 CloseItem`, `#162 OpenItem`, `#163 FindItem`, `#164 DeleteItem`, `#176 FindNamedItem` |
| **Tasks & Threads** | `#84 CreateThreadVA`, `#555 CreateThread` |
| **Signals** | `#143 FreeSignal`, `#144 AllocSignal`, `#165 SendSignal`, `#166 WaitSignal` |
| **Messages & Ports** | `#130 WaitPort`, `#146 GetMsg`, `#147 ReplyMsg`, `#150 SendMsg`, `#151 SendSmallMsg`, `#152 GetThisMsg`, `#168 CreateBufferedMsg`, `#171 CreateMsg`, `#172 CreateMsgPort`, `#175 CreateSmallMsg` |
| **Semaphores & Locks** | `#160 LockSemaphore`, `#161 UnlockSemaphore`, `#174 CreateSemaphore`, `#205 CreateSpinLock`, `#206 DeleteSpinLock`, `#566 ObtainSpinLock`, `#567 ReleaseSpinLock` |
| **Device I/O, Cache & Time** | `#4 CheckIO`, `#21 SampleSystemTimeTT`, `#27 ConvertTimerTicksToTimeVal`, `#38 GetCacheInfo`, `#51 FlushDCache`, `#52 WriteBackDCache`, `#67 CreateDeviceStackList`, `#68 DeleteDeviceStackList`, `#69 OpenDeviceStack`, `#70 CloseDeviceStack`, `#129 WaitIO`, `#132 DoIO`, `#136 FlushDCacheAll`, `#141 AbortIO`, `#142 SendIO`, `#170 CreateIOReq`, `#558 GetTEReadPointer`, `#561 SetTEWritePointer` |
| **Lists & Nodes** | `#40 RemNode`, `#42 AddTail`, `#43 RemTail`, `#44 AddHead`, `#45 RemHead` |
| **C Library & String** | `#8 exit`, `#24 vcprintf`, `#31 memmove`, `#33 memset`, `#309 printf`, `#310 clib_PrintError`, `#311 PrintfSysErr`, `#312 strlen`, `#315 strcpy`, `#316 strncpy`, `#317 strcat`, `#320 strcmp`, `#322 toupper`, `#324 sprintf` |
| **Diab Millicode & VA** | `#89..#91 __tagCall_0..2`, `#267 __va_arg`, `#325 __itof`, `#326 __utof`; register save/restore helpers at `#352..#359`, `#381..#394`, `#417..#430`, `#460..#467`, `#489..#502`, `#525..#538` |

## Portfolio Error Code Encoding (`MakeErr`)

All negative Portfolio `Err` return values use the 32-bit `MakeErr` bitfield layout (`operror.h`):

```text
0x80000000 | (obj << 25) | (subsys_id << 13) | (severity << 11) | (env << 9) | (class << 8) | err_num
```

- **6-bit CHAR4 (`subsys_id`)**: Packs two ASCII characters `(ch1 << 6) | ch0` identifying the Folio or Device:
  - `'Kr'` -> Kernel (`0xd57b9000` base)
  - `'Gf'` -> Graphics (`0xd55b9000` base)
  - `'IF'` -> IFF (`0xd56d5000` / `0xd56d4100` base)
  - `'Au'` -> Audio (`0xd52bf000` base)
  - `'TE'` -> Triangle Engine (`0xd1c53000` base)
  - `'DS'` -> DataStream (`0xe146f000` base)

## Memory Allocation API

`AllocMem` (`1:2`), `FreeMem` (`1:3`), and `AllocMemMasked` (`1:35`) manage dynamic blocks in the guest heap (`0x00400000+`):

- **`MEMTYPE_FILL` (`0x00000100`)**: Fills the allocated block with the low byte `memFlags & 0xFF` (typically `0x00`).
- **`MEMTYPE_TRACKSIZE` (`0x00000200`)**: Prepends a 4-byte header before the returned pointer storing the requested `size` at `((uint32_t *)ptr)[-1]` (8 bytes when an alignment mask is also requested). Calling `FreeMem(ptr, -1)` reads that tracked size.
- **`AllocMemMasked(minSize, memFlags, careBits, stateBits)` (`1:35`)**: Allocates a block whose base effective address `ea` satisfies `(ea & careBits) == (stateBits & careBits)`. Used by `mpegvideo.device` strip and reference buffer allocations to enforce 4 KB page alignment (`careBits = 0xFFF`, `stateBits = 0`).

## Preemptive Multithreading, Signals, Messaging, and Device I/O

### Scheduling

Portfolio uses priority-based scheduling with 15,000 µs round-robin quanta among equal-priority tasks (`src/folios/kernel/task.c`). Spawned threads inherit their creator's task priority unless overridden via tags, while the initial main task starts at priority 200.

In `driving/game`, `StateDriver` runs on the main task while vehicle physics (`CarSimThread`), background track loading (`startuptrackinitthread`), audio streaming, and DataStream workers run as spawned threads that synchronize through signals, message ports, and semaphores.

### Signals and Message Passing

- **Signals (`AllocSignal` / `WaitSignal` / `SendSignal`)**: Each task has a 32-bit `t_AllocatedSigs` mask and a 32-bit `t_SigBits` pending mask. Bits `0x00000001 .. 0x00800000` (`24` bits) are user-allocatable; bits `0x80000000` (`SIGF_MEM`), `0x40000000` (`SIGF_ABORT`), `0x20000000` (`SIGF_IODONE`), and `0x10000000` (`SIGF_DEADTASK`) are system signals.
- **Message Lifecycle (`MsgPort` & `Message`)**:
  - `CreateMsgPort(name, pri, signal)` (`1:172`) allocates a port with a wakeup signal (`mp_Signal` at `+0x28`).
  - `SendMsg(port, msg, dataPtr, dataSize)` (`1:150`) and `SendSmallMsg(port, msg, val1, val2)` (`1:151`) store payload words at `msg + 0x2C` (`msg_DataPtr` / `msg_Val1`) and `msg + 0x30` (`msg_DataSize` / `msg_Val2`), append `msg` to `port`'s queue, and signal `port`'s owner task.
  - `WaitPort(port, msg)` (`1:130`) blocks until `port` (or the specific `msg`) arrives and dequeues it.
  - `ReplyMsg(msg, result, dataPtr, dataSize)` (`1:147`) writes `result` to `msg + 0x28` (`msg_Result`), moves `msg` to its `msg_ReplyPort` (`+0x24`), and wakes the sender.

### Device Stacks and `IOReq` Layout

Guest code opens hardware and virtual devices with `OpenDeviceStack` (`1:69`), allocates an `IOReq` (`1:170`), and submits an `IOInfo` descriptor via `DoIO` (`1:132`) or `SendIO` (`1:142`) + `WaitIO` (`1:129`):

| `IOInfo` Offset | Field | Description |
|---|---|---|
| `+0x00` | `ioi_Command` | Device command code (`u32`) |
| `+0x04` | `ioi_Flags` | Request flags (`u8`) |
| `+0x08` | `ioi_CmdOptions` | Command-specific options word or item handle (`u32`) |
| `+0x0C` | `ioi_UserData` | Caller user data word (`u32`) |
| `+0x10`, `+0x14` | `ioi_Send.iob_Buffer`, `iob_Len` | Input buffer pointer and byte length |
| `+0x18`, `+0x1C` | `ioi_Recv.iob_Buffer`, `iob_Len` | Output buffer pointer and byte length |
| `+0x20` | `ioi_Offset` | Device sector or stream offset (`i32`) |

Upon completion, the kernel populates the `IOReq` result fields:
- `ior + 0x28 .. 0x4B`: Embedded copy of the submitted `IOInfo`.
- `ior + 0x4C` (`io_Actual`, `i32`): Bytes transferred.
- `ior + 0x50` (`io_Flags`, `u32`): `IO_DONE` (`0x01`), `IO_QUICK` (`0x02`), plus device-specific flags (such as bit 31 `0x80000000` indicating a valid PTS on `mpegvideo.device`).
- `ior + 0x54` (`io_Error`, `Err`): `0` on success or a negative Portfolio `Err`.

### Supported Device Command Classes

1. **`timer` (`CMDC_TIMER = 0x004`)**: Supports both `TIMER_UNIT_USEC` and `TIMER_UNIT_VBL` (`60 Hz`) across `TIMERCMD_GETTIME`, `TIMERCMD_SETTIME`, `TIMERCMD_DELAY`, `TIMERCMD_DELAYUNTIL`, and `TIMERCMD_METRONOME`.
2. **`mp` (`CMDC_MP = 0x106`, `MP_CMD_DISPATCH = 0x01060003`)**: Executes slave CPU callback packets `(func_ea, arg_ea, stack_ea, stack_size)` used by vehicle simulation (`CarSimFunction` at `0xa8d8`) and Mercury lighting/culling batches (`SlaveFunction`).
3. **`triangleengine` (`CMDC_TE = 0x00D`)**: Executes Bulldog Triangle Engine command lists and framebuffer/Z-buffer clears (see [`triangle-engine.md`](triangle-engine.md)).
4. **`mpegvideo` (`CMDC_MPEGV = 0x007`)**: Decodes MPEG-1 FMV streams and still frames (see [`mpeg-fmv.md`](mpeg-fmv.md)).
5. **CD-ROM / File Device (`CMD_STATUS`, `CMD_BLOCKREAD`)**: Reads 2048-byte OperaFS sectors for `DataAcqThread` and `SoundPlayer` (see [`operafs.md`](operafs.md)).
