# Kernel imports

`default.xbe` imports 112 functions and variables from `xboxkrnl.exe`, all by
ordinal. The loader patches each 4-byte slot of the thunk table at
`0x003BC220` with the kernel export's address; code calls them as
`call dword ptr [slot]`, and reads data exports through the slot as a pointer.
Ghidra also created a few `jmp [slot]` stubs in `.text` (for example
`MmFreeContiguousMemory` at `0x0002ABA1`), classed `xboxkrnl` in
[functions.csv](functions.csv).

For a static recompiler this table is the whole OS boundary below XAPI: every
row is a host function (or host variable) the runtime has to provide. Most are
called only from XAPI, D3D8 and DSOUND, so once those libraries are replaced
with high-level host implementations the set the game still reaches shrinks a
lot. Measure that from call sites before implementing the long tail.

Data exports (the slot holds a pointer to the variable, not code):
`LaunchDataPage`, `ExEventObjectType`, `PsThreadObjectType`, `XboxHardwareInfo`,
`HalDiskCachePartitionCount`, `XboxHDKey`, `XboxSignatureKey`, `KeTickCount`.

| Group | Count |
|---|---|
| Ke*/Kf* (scheduler, IRQL, DPCs, timers, interrupts, FPU state) | 26 |
| Nt* (files, VM, objects, events) | 21 |
| Mm* (contiguous / system memory, protection) | 14 |
| Rtl* (critical sections, strings, time, exceptions) | 11 |
| Xbox*/Xc*/Xe* (keys, SHA-1, section loading) | 8 |
| Hal* (shutdown, interrupts, SMC, firmware return) | 7 |
| Io* (driver model: used by XAPI's USB/MU stack) | 7 |
| Ex* (pool, non-volatile settings) | 6 |
| Ob*/Ps* (object refs, system threads) | 5 |
| Av* (video encoder, used by D3D) | 4 |
| Misc (`LaunchDataPage`, `FscSetCacheSize`, `DbgPrint`) | 3 |

Interrupt, DPC and IRP imports (`KeConnectInterrupt`, `IoStartPacket`, ...)
exist because D3D8, DSOUND and the USB driver in `XPP` run as in-process
drivers. A recompiler that replaces those libraries does not need them.

## Thunk table

Slot order is the order in the file.

| Slot | Ordinal | Name | Kind |
|---|---|---|---|
| `0x003bc220` | 184 | `NtAllocateVirtualMemory` | function |
| `0x003bc224` | 199 | `NtFreeVirtualMemory` | function |
| `0x003bc228` | 217 | `NtQueryVirtualMemory` | function |
| `0x003bc22c` | 181 | `MmQueryStatistics` | function |
| `0x003bc230` | 178 | `MmPersistContiguousMemory` | function |
| `0x003bc234` | 165 | `MmAllocateContiguousMemory` | function |
| `0x003bc238` | 164 | `LaunchDataPage` | data export |
| `0x003bc23c` | 171 | `MmFreeContiguousMemory` | function |
| `0x003bc240` | 301 | `RtlNtStatusToDosError` | function |
| `0x003bc244` | 49 | `HalReturnToFirmware` | function |
| `0x003bc248` | 187 | `NtClose` | function |
| `0x003bc24c` | 215 | `NtQuerySymbolicLinkObject` | function |
| `0x003bc250` | 203 | `NtOpenSymbolicLinkObject` | function |
| `0x003bc254` | 16 | `ExEventObjectType` | data export |
| `0x003bc258` | 289 | `RtlInitAnsiString` | function |
| `0x003bc25c` | 225 | `NtSetEvent` | function |
| `0x003bc260` | 99 | `KeDelayExecutionThread` | function |
| `0x003bc264` | 226 | `NtSetInformationFile` | function |
| `0x003bc268` | 202 | `NtOpenFile` | function |
| `0x003bc26c` | 37 | `FscSetCacheSize` | function |
| `0x003bc270` | 207 | `NtQueryDirectoryFile` | function |
| `0x003bc274` | 161 | `KfLowerIrql` | function |
| `0x003bc278` | 129 | `KeRaiseIrqlToDpcLevel` | function |
| `0x003bc27c` | 65 | `IoCreateDevice` | function |
| `0x003bc280` | 14 | `ExAllocatePool` | function |
| `0x003bc284` | 145 | `KeSetEvent` | function |
| `0x003bc288` | 159 | `KeWaitForSingleObject` | function |
| `0x003bc28c` | 113 | `KeInitializeTimerEx` | function |
| `0x003bc290` | 107 | `KeInitializeDpc` | function |
| `0x003bc294` | 74 | `IoInvalidDeviceRequest` | function |
| `0x003bc298` | 149 | `KeSetTimer` | function |
| `0x003bc29c` | 97 | `KeCancelTimer` | function |
| `0x003bc2a0` | 250 | `ObfDereferenceObject` | function |
| `0x003bc2a4` | 246 | `ObReferenceObjectByHandle` | function |
| `0x003bc2a8` | 166 | `MmAllocateContiguousMemoryEx` | function |
| `0x003bc2ac` | 180 | `MmQueryAllocationSize` | function |
| `0x003bc2b0` | 182 | `MmSetAddressProtect` | function |
| `0x003bc2b4` | 179 | `MmQueryAddressProtect` | function |
| `0x003bc2b8` | 190 | `NtCreateFile` | function |
| `0x003bc2bc` | 236 | `NtWriteFile` | function |
| `0x003bc2c0` | 219 | `NtReadFile` | function |
| `0x003bc2c4` | 211 | `NtQueryInformationFile` | function |
| `0x003bc2c8` | 172 | `MmFreeSystemMemory` | function |
| `0x003bc2cc` | 167 | `MmAllocateSystemMemory` | function |
| `0x003bc2d0` | 17 | `ExFreePool` | function |
| `0x003bc2d4` | 83 | `IoStartPacket` | function |
| `0x003bc2d8` | 87 | `IofCompleteRequest` | function |
| `0x003bc2dc` | 81 | `IoStartNextPacket` | function |
| `0x003bc2e0` | 359 | `IoMarkIrpMustComplete` | function |
| `0x003bc2e4` | 358 | `HalIsResetOrShutdownPending` | function |
| `0x003bc2e8` | 125 | `KeQueryInterruptTime` | function |
| `0x003bc2ec` | 322 | `XboxHardwareInfo` | data export |
| `0x003bc2f0` | 360 | `HalInitiateShutdown` | function |
| `0x003bc2f4` | 294 | `RtlLeaveCriticalSection` | function |
| `0x003bc2f8` | 277 | `RtlEnterCriticalSection` | function |
| `0x003bc2fc` | 269 | `RtlCompareMemoryUlong` | function |
| `0x003bc300` | 291 | `RtlInitializeCriticalSection` | function |
| `0x003bc304` | 302 | `RtlRaiseException` | function |
| `0x003bc308` | 218 | `NtQueryVolumeInformationFile` | function |
| `0x003bc30c` | 67 | `IoCreateSymbolicLink` | function |
| `0x003bc310` | 128 | `KeQuerySystemTime` | function |
| `0x003bc314` | 143 | `KeSetBasePriorityThread` | function |
| `0x003bc318` | 259 | `PsThreadObjectType` | data export |
| `0x003bc31c` | 224 | `NtResumeThread` | function |
| `0x003bc320` | 258 | `PsTerminateSystemThread` | function |
| `0x003bc324` | 255 | `PsCreateSystemThreadEx` | function |
| `0x003bc328` | 44 | `HalGetInterruptVector` | function |
| `0x003bc32c` | 160 | `KfRaiseIrql` | function |
| `0x003bc330` | 47 | `HalRegisterShutdownNotification` | function |
| `0x003bc334` | 98 | `KeConnectInterrupt` | function |
| `0x003bc338` | 109 | `KeInitializeInterrupt` | function |
| `0x003bc33c` | 151 | `KeStallExecutionProcessor` | function |
| `0x003bc340` | 24 | `ExQueryNonVolatileSetting` | function |
| `0x003bc344` | 356 | `HalBootSMCVideoMode` | function |
| `0x003bc348` | 279 | `RtlEqualString` | function |
| `0x003bc34c` | 327 | `XeLoadSection` | function |
| `0x003bc350` | 328 | `XeUnloadSection` | function |
| `0x003bc354` | 40 | `HalDiskCachePartitionCount` | data export |
| `0x003bc358` | 200 | `NtFsControlFile` | function |
| `0x003bc35c` | 173 | `MmGetPhysicalAddress` | function |
| `0x003bc360` | 175 | `MmLockUnlockBufferPages` | function |
| `0x003bc364` | 119 | `KeInsertQueueDpc` | function |
| `0x003bc368` | 176 | `MmLockUnlockPhysicalPage` | function |
| `0x003bc36c` | 196 | `NtDeviceIoControlFile` | function |
| `0x003bc370` | 312 | `RtlUnwind` | function |
| `0x003bc374` | 95 | `KeBugCheck` | function |
| `0x003bc378` | 336 | `XcSHAUpdate` | function |
| `0x003bc37c` | 335 | `XcSHAInit` | function |
| `0x003bc380` | 337 | `XcSHAFinal` | function |
| `0x003bc384` | 323 | `XboxHDKey` | data export |
| `0x003bc388` | 325 | `XboxSignatureKey` | data export |
| `0x003bc38c` | 305 | `RtlTimeToTimeFields` | function |
| `0x003bc390` | 156 | `KeTickCount` | data export |
| `0x003bc394` | 304 | `RtlTimeFieldsToTime` | function |
| `0x003bc398` | 126 | `KeQueryPerformanceCounter` | function |
| `0x003bc39c` | 127 | `KeQueryPerformanceFrequency` | function |
| `0x003bc3a0` | 233 | `NtWaitForSingleObject` | function |
| `0x003bc3a4` | 198 | `NtFlushBuffersFile` | function |
| `0x003bc3a8` | 232 | `NtUserIoApcDispatcher` | function |
| `0x003bc3ac` | 1 | `AvGetSavedDataAddress` | function |
| `0x003bc3b0` | 2 | `AvSendTVEncoderOption` | function |
| `0x003bc3b4` | 3 | `AvSetDisplayMode` | function |
| `0x003bc3b8` | 4 | `AvSetSavedDataAddress` | function |
| `0x003bc3bc` | 8 | `DbgPrint` | function |
| `0x003bc3c0` | 100 | `KeDisconnectInterrupt` | function |
| `0x003bc3c4` | 168 | `MmClaimGpuInstanceMemory` | function |
| `0x003bc3c8` | 23 | `ExQueryPoolBlockSize` | function |
| `0x003bc3cc` | 15 | `ExAllocatePoolWithTag` | function |
| `0x003bc3d0` | 137 | `KeRemoveQueueDpc` | function |
| `0x003bc3d4` | 153 | `KeSynchronizeExecution` | function |
| `0x003bc3d8` | 142 | `KeSaveFloatingPointState` | function |
| `0x003bc3dc` | 139 | `KeRestoreFloatingPointState` | function |
