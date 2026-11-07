# Savestate changelog

One entry per section version (tools/native/statelock.py).

## ACI, APU, CACHE, FILE, KERN, MISC, MM, NV2A, PAGE, PBD, RAM, THRD, TIMR, USB, VM v1 (2026-10-20)

- first version

## FILE v2 (2026-10-21, after ba8ad66cc6)

- NcFile.del: a file deleted through a handle stays until the last handle on it closes (v1 states: a file deleted while open reopens empty)
- upgraded: v1..v1 states load

## MODS v1 (2026-10-21, after 6abc1f8db4)

- first version

## TIMR v2 (2026-10-22, after ed4707c8ae)

- a timer's APC routine, context and thread (NtSetTimerEx's TimerApcRoutine, delivered at expiry)
- upgraded: v1..v1 states load

## FILE v3 (2026-10-23, after e9b90ac2d6)

- NcFile.no_buffering: a handle opened with FILE_NO_INTERMEDIATE_BUFFERING; only its writes pend (v2 states: buffered, writes complete in the call)
- upgraded: v1..v2 states load

## THRD v2 (2026-10-25, after 8768ba9ef1)

- NcThread.tib_base, frame_top: a thread's stack keeps the Xbox kernel's FX save area (0x210 bytes) under its top; the TIB's StackBase (fs:[4]) and the thread's frames sit under it (v1 states: 0, the stack's old layout, kept)
- upgraded: v1..v1 states load

## KERN v2 (2026-10-26, after a8d491bd5d)

- NcKernel.handles_more: the handle table grows from 4096 slots to 16384 (Super Monkey Ball Deluxe keeps ~6100 files open); its slots past 4096 (v1 states: none, all free)
- upgraded: v1..v1 states load

## MM v2 (2026-10-26, after d5616b97b1)

- Contig.protect: a contiguous block's protection, cache bits included (MmAllocateContiguousMemoryEx's ProtectionType, MmSetAddressProtect), for MmQueryAddressProtect (v1 states: 0, read-write)
- upgraded: v1..v1 states load

## NIC v1 (2026-10-26, after a7063de469)

- first version

## PBD v2 (2026-10-29, after d44f6d0fe3)

- HlePbdState.disp_w, disp_h, disp_moded: the device's display and whether the video mode sized it, for the run that loads the state (the title doesn't set its mode again); v1 states: 0, the display from the first frame swapped as before
- upgraded: v1..v1 states load

## KERN v3 (2026-10-29, after 113a34e7e7)

- the kernel pool's first 64 KiB range sits just above the kernel (physical 0x50000), not 512 KiB under the top reserve (Burnout 2 lays out 0x2377000-0x3F00000 at fixed addresses); a state's boot puts it under the state's process object (v1, v2 states, and states saved since by a run that loaded one: where it was)
- upgraded: v1..v2 states load

## THRD v3 (2026-11-03, after 7a32b4d248)

- NcThread.boost, no_boost: NT's wait boost (a thread a signal woke runs at priority + the signal's increment, up to 15, a step less each quantum that ends) and KeSetDisableBoostThread's flag (v2 states: 0, no boost, boosts on)
- upgraded: v1..v2 states load

## FILE v4 (2026-11-03, after 8cb7c31c13)

- NcFile.host, old_path: kept by their place in the hdd folder (Partition1/..., .overwrite/...), so a state loads from a copy of the folder elsewhere or on another system (v1..v3 states: the absolute path of the folder they were made in, taken from its PartitionN or .overwrite on)
- upgraded: v1..v3 states load

## THRD v4 (2026-11-06, after 719a397617)

- a thread keeps up to 64 exception dispatches and unwinds in progress (seh_more holds those past the first 8): Unreal Engine 2 titles rethrow from each guarded function's catch block, XIII 9 deep at boot
- upgraded: v1..v3 states load
