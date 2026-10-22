# API surface: what the game uses from the XDK

Every place where game code (all of `.text` outside the XAPI and CRT ranges,
including EH funclets and static initializers) calls, jumps to, reads, writes or
takes the address of something in an XDK library, the MSVC runtime or the
kernel. This is what the Windows host runtime must provide, or what must be
recompiled along with the game. Full data, including unnamed targets:
[api-surface.csv](api-surface.csv), from
`tools/ghidra_scripts/export_api_surface.py`.

The counts are static references, so calls made through function pointers
or vtables that point into a library are not included. The game's own
vtables only point at game and CRT code (see [classes.md](classes.md)), so
that gap is small.

## Summary

| Library | Targets | Sites | What it means for the port |
|---|---|---|---|
| D3D8 | 86 | 1,157 | about 60 API functions plus direct writes into D3D's state arrays |
| DSOUND | 16 | 21 | one `IDirectSound`, 3D listener, one stream, effects image, `DirectSoundDoWork` |
| XAPILIB | 51 | 187 | files, time, sleep, threads, launch data, save signing, gamepad and memory unit enumeration |
| D3DX8 | 19 | 61 | 7 matrix helpers (in `.text`) and 12 unnamed functions in the `D3DX` section |
| XGRAPHC | 6 | 23 | texture swizzling |
| Kernel (direct) | 2 | 2 | `MmQueryStatistics`, `RtlInitializeCriticalSection` |
| MSVC runtime | 137 | 8,562 | C++ EH, RTTI, `atexit`, stdio, strings, `_ftol` |

The direct kernel surface is tiny: almost everything goes through XAPI, so a
host XAPI layer covers the OS. The kernel imports in kernel-imports.md are
mostly needed only by the libraries being replaced.

## Findings

- **D3D state is written directly.** Inlined `D3DDevice_SetRenderState` and
  `SetTextureStageState` code in the game writes into D3D8's globals instead of
  calling a function: `D3D_g_RenderState` entries at `0x0037AD4C`..`0x0037ADAC`
  (indices 57 to 81, 199 write sites) and, through
  `D3DDevice_SetTextureStageStateDeferred` (`0x00369120`, 247 calls), into
  `D3D_g_DeferredTextureState` (`0x0037AA68`) and its dirty mask. There are
  also 9 reads of `0x0037AA64`. A host D3D layer has to keep these arrays at
  their guest addresses and read them when it draws, as Cxbx-Reloaded and xemu's
  HLE do, rather than expect every state change as a call.
- **D3D8 usage is moderate and fixed-function heavy:** render states,
  transforms, vertex and pixel shaders (Xbox microcode, created at run time with
  `D3DDevice_CreatePixelShader`; vertex shaders arrive as handles), push-style
  `DrawVerticesUP`, state blocks, fences, `PersistDisplay`. No push-buffer
  recording API (`D3DDevice_BeginPushBuffer`, `RunPushBuffer`) is called.
- **The engine has its own render interface.** `CD3DContext` and
  `CDrawContext` both have 87-slot vtables (classes.md). If most D3D calls come
  from `CD3DContext` methods, reimplementing that class natively may be a
  smaller and cleaner boundary than emulating D3D8. Worth checking next.
- **Audio is a thin slice of DirectSound:** a listener (position, velocity,
  orientation, I3DL2 reverb), distance and Doppler factors, mix-bin headroom, a
  DSP effects image download, one `DirectSoundCreateStream`, and
  `DirectSoundDoWork` once per frame. Buffers are created elsewhere through
  COM-style vtable calls, so the full list needs the DSOUND vtables traced.
- **Input is plain XInput:** `XInitDevices`, `XGetDevices`/`XGetDeviceChanges`,
  `XInputOpen`/`Close`/`GetCapabilities`/`GetState`/`SetState` for gamepads,
  plus memory-unit enumeration (`g_DeviceType_MU`) for saves.
- **Files go through XAPI Win32 wrappers** (`CreateFileA`, `FindNextFileA`,
  `GetFileSize`, `GetFileTime`, `CreateDirectoryA`, `SetFileAttributesA`) and the
  CRT's `fopen`/`fread`/`fseek`/`fprintf`. `fprintf` has 1,003 call sites, nearly
  all debug logging.
- **Saves are signed:** `XCalculateSignatureBegin/Update/End` (HMAC-SHA1 with
  the console's key). A PC port can stub the signature check.
- **The game does use C++ exceptions and RTTI.** `_CxxThrowException` has 160
  call sites in game code, `typeid` 660, `dynamic_cast` 311. The EH machinery
  must work in the recompiled game; see recompiler-notes.md.
- **Time:** `QueryPerformanceCounter/Frequency`, `GetTickCount`, `Sleep`/`SleepEx`.
  Threads: one `CreateThread`, `SetThreadPriority`, `ExitThread` (a streaming or
  loader thread, not yet traced).
- 107 targets are still unnamed (`FUN_...`): 77 CRT internals (math, stdio,
  STL helpers), 13 XAPI file and directory helpers, 12 in `D3DX`, 3 in
  XGraphics, 2 in D3D; the evidence for the 40 that
  were identified by hand is in [library-names.csv](library-names.csv).

## Targets by library

### D3D8

86 targets, 1157 sites.

| Target | Name | Sites | Game functions | How |
|---|---|---|---|---|
| `0x00369120` | `D3DDevice_SetTextureStageStateDeferred` | 247 | 74 | call:247 |
| `0x00369070` | `D3DDevice_SetRenderState_Simple` | 200 | 22 | call:200 |
| `0x0037ad60` | `DAT_0037ad60` | 62 | 14 | write:62 |
| `0x0037ad64` | `DAT_0037ad64` | 62 | 14 | write:62 |
| `0x0036dc60` | `D3DResource_Release` | 39 | 18 | call:39 |
| `0x0036d480` | `D3DDevice_SetVertexShaderConstant` | 37 | 12 | call:37 |
| `0x0036af10` | `D3DDevice_SetTexture` | 36 | 10 | call:36 |
| `0x003690a0` | `D3DDevice_SetRenderState_Deferred` | 31 | 8 | call:31 |
| `0x0036d3a0` | `D3DDevice_SetVertexShader` | 29 | 21 | call:29 |
| `0x0036d590` | `D3DDevice_SetPixelShader` | 26 | 21 | call:26 |
| `0x0037ad54` | `DAT_0037ad54` | 25 | 12 | write:25 |
| `0x0036a130` | `D3DDevice_SetTransform` | 22 | 17 | call:22 |
| `0x0036bfd0` | `D3DVertexBuffer_Lock` | 19 | 5 | call:19 |
| `0x0036df20` | `D3DBaseTexture_GetLevelCount` | 16 | 4 | call:16 |
| `0x0036a0a0` | `D3DDevice_CreateTexture` | 14 | 9 | call:14 |
| `0x0036c670` | `D3DDevice_DrawVertices` | 14 | 5 | call:14 |
| `0x0036a240` | `D3DDevice_GetTransform` | 13 | 8 | call:13 |
| `0x0036cfe0` | `D3DDevice_SetStreamSource` | 13 | 6 | call:13 |
| `0x0036dd60` | `D3DTexture_GetSurfaceLevel` | 12 | 7 | call:12 |
| `0x0036c3f0` | `D3DDevice_DrawVerticesUP` | 11 | 10 | call:11 |
| `0x0036a530` | `D3DDevice_SetRenderTarget` | 11 | 6 | call:11 |
| `0x0036bf70` | `D3DDevice_CreateVertexBuffer` | 11 | 3 | call:11 |
| `0x0036dd50` | `Get2DSurfaceDesc` | 10 | 6 | call:10 |
| `0x0036c710` | `D3DDevice_DrawIndexedVertices` | 9 | 7 | call:9 |
| `0x0037aa64` | `DAT_0037aa64` | 9 | 7 | read:9 |
| `0x0036ddb0` | `D3DTexture_LockRect` | 7 | 6 | call:7 |
| `0x0036d750` | `D3DDevice_SetPixelShaderConstant` | 7 | 4 | call:7 |
| `0x00369610` | `D3DDevice_SetTextureState_TexCoordIndex` | 7 | 3 | call:7 |
| `0x0036dcb0` | `D3DResource_Register` | 7 | 3 | call:7 |
| `0x00369d20` | `D3DDevice_SetRenderState_ZEnable` | 6 | 4 | call:6 |
| `0x0036a0d0` | `D3DDevice_CreateVolumeTexture` | 6 | 1 | call:6 |
| `0x0036de40` | `thunk_FUN_00371d40` | 6 | 1 | call:6 |
| `0x0036de50` | `D3DVolumeTexture_LockBox` | 6 | 1 | call:6 |
| `0x0036aaf0` | `D3DDevice_SetViewport` | 5 | 5 | call:5 |
| `0x0036c020` | `D3DDevice_Clear` | 5 | 5 | call:5 |
| `0x0037ada0` | `DAT_0037ada0` | 5 | 4 | write:5 |
| `0x00369750` | `D3DDevice_SetTextureState_BorderColor` | 4 | 4 | call:4 |
| `0x0036a800` | `D3DDevice_GetBackBuffer` | 4 | 4 | call:4 |
| `0x0036aac0` | `D3DDevice_GetDepthStencilSurface` | 4 | 4 | call:4 |
| `0x0036c530` | `D3DDevice_DrawIndexedVerticesUP` | 4 | 4 | call:4 |
| `0x0036df50` | `D3DSurface_LockRect` | 4 | 4 | call:4 |
| `0x0037ad9c` | `DAT_0037ad9c` | 4 | 4 | write:4 |
| `0x0037adac` | `DAT_0037adac` | 4 | 4 | write:4 |
| `0x0037ad58` | `DAT_0037ad58` | 4 | 3 | write:4 |
| `0x0037ad68` | `DAT_0037ad68` | 4 | 3 | write:4 |
| `0x0037ad70` | `DAT_0037ad70` | 4 | 3 | write:4 |
| `0x00369e10` | `D3DDevice_SetRenderState_StencilFail` | 4 | 1 | call:4 |
| `0x0037ad78` | `DAT_0037ad78` | 4 | 1 | write:4 |
| `0x0037ad7c` | `DAT_0037ad7c` | 4 | 1 | write:4 |
| `0x0037ad80` | `DAT_0037ad80` | 4 | 1 | write:4 |
| `0x0036aaa0` | `D3DDevice_GetRenderTarget` | 3 | 3 | call:3 |
| `0x0036b070` | `D3DDevice_SetIndices` | 3 | 3 | call:3 |
| `0x0036bf30` | `D3DDevice_CreateIndexBuffer` | 3 | 3 | call:3 |
| `0x0036e090` | `D3D_AllocContiguousMemory` | 3 | 3 | call:3 |
| `0x00369200` | `D3DDevice_SetRenderState_CullMode` | 3 | 2 | call:3 |
| `0x00369d90` | `D3DDevice_SetRenderState_StencilEnable` | 3 | 2 | call:3 |
| `0x00369180` | `D3DDevice_SetRenderState_ShadowFunc` | 2 | 2 | call:2 |
| `0x0036d570` | `D3DDevice_DeletePixelShader` | 2 | 2 | call:2 |
| `0x0036dc20` | `D3DResource_AddRef` | 2 | 2 | call:2 |
| `0x0037ad50` | `DAT_0037ad50` | 2 | 2 | write:2 |
| `0x0037ad5c` | `DAT_0037ad5c` | 2 | 2 | write:2 |
| `0x0037ad74` | `DAT_0037ad74` | 2 | 2 | write:2 |
| `0x0037ad4c` | `DAT_0037ad4c` | 2 | 1 | write:2 |
| `0x0037ad84` | `DAT_0037ad84` | 2 | 1 | write:2 |
| `0x0036886a` | `D3DDevice_ApplyStateBlock` | 1 | 1 | call:1 |
| `0x003689f2` | `D3DDevice_CaptureStateBlock` | 1 | 1 | call:1 |
| `0x00368c9f` | `D3DDevice_DeleteStateBlock` | 1 | 1 | call:1 |
| `0x00368d30` | `D3DDevice_CreateStateBlock` | 1 | 1 | call:1 |
| `0x00368e60` | `FUN_00368e60` | 1 | 1 | call:1 |
| `0x00368e70` | `D3D_GetAdapterDisplayMode` | 1 | 1 | call:1 |
| `0x00368f70` | `Direct3D_CreateDevice` | 1 | 1 | call:1 |
| `0x003691b0` | `D3DDevice_SetRenderState_FogColor` | 1 | 1 | call:1 |
| `0x003692e0` | `D3DDevice_SetRenderState_TextureFactor` | 1 | 1 | call:1 |
| `0x00369410` | `D3DDevice_SetRenderState_ZBias` | 1 | 1 | call:1 |
| `0x0036a080` | `D3DDevice_GetDeviceCaps` | 1 | 1 | call:1 |
| `0x0036a320` | `D3DDevice_Release` | 1 | 1 | call:1 |
| `0x0036a360` | `D3DDevice_BlockOnFence` | 1 | 1 | call:1 |
| `0x0036a3e0` | `D3DDevice_InsertFence` | 1 | 1 | call:1 |
| `0x0036b350` | `D3DDevice_PersistDisplay` | 1 | 1 | call:1 |
| `0x0036bda0` | `D3DDevice_Present` | 1 | 1 | call:1 |
| `0x0036d520` | `D3DDevice_CreatePixelShader` | 1 | 1 | call:1 |
| `0x0036da30` | `D3DResource_GetType` | 1 | 1 | call:1 |
| `0x0036dae0` | `D3DResource_BlockUntilNotBusy` | 1 | 1 | call:1 |
| `0x0037ad6c` | `DAT_0037ad6c` | 1 | 1 | write:1 |
| `0x0037ad88` | `DAT_0037ad88` | 1 | 1 | write:1 |
| `0x0037ad8c` | `DAT_0037ad8c` | 1 | 1 | write:1 |

### DSOUND

16 targets, 21 sites.

| Target | Name | Sites | Game functions | How |
|---|---|---|---|---|
| `0x003aad96` | `IDirectSound_Release` | 2 | 2 | call:2 |
| `0x003abbb3` | `IDirectSound_SetPosition` | 2 | 2 | call:2 |
| `0x003abbe8` | `IDirectSound_SetVelocity` | 2 | 2 | call:2 |
| `0x003abc35` | `IDirectSound_SetI3DL2Listener` | 2 | 2 | call:2 |
| `0x003abf55` | `IDirectSoundStream_Pause` | 2 | 2 | call:2 |
| `0x003aadb1` | `IDirectSoundBuffer_Release` | 1 | 1 | call:1 |
| `0x003ababe` | `IDirectSound_GetCaps` | 1 | 1 | call:1 |
| `0x003abada` | `IDirectSound_DownloadEffectsImage` | 1 | 1 | call:1 |
| `0x003abb01` | `IDirectSound_SetMixBinHeadroom` | 1 | 1 | call:1 |
| `0x003abb21` | `IDirectSound_SetDistanceFactor` | 1 | 1 | call:1 |
| `0x003abb45` | `IDirectSound_SetDopplerFactor` | 1 | 1 | call:1 |
| `0x003abb69` | `IDirectSound_SetOrientation` | 1 | 1 | call:1 |
| `0x003abc1d` | `IDirectSound_CommitDeferredSettings` | 1 | 1 | call:1 |
| `0x003abf5a` | `DirectSoundDoWork` | 1 | 1 | call:1 |
| `0x003acbd2` | `DirectSoundCreate` | 1 | 1 | call:1 |
| `0x003accae` | `DirectSoundCreateStream` | 1 | 1 | call:1 |

### XAPILIB

51 targets, 187 sites.

| Target | Name | Sites | Game functions | How |
|---|---|---|---|---|
| `0x002de90d` | `XCalculateSignatureUpdate` | 28 | 8 | call:28 |
| `0x0002dee2` | `CloseHandle` | 13 | 10 | call:13 |
| `0x002df39a` | `GetTickCount` | 13 | 8 | call:13 |
| `0x0002ae13` | `CreateFileA` | 12 | 9 | call:12 |
| `0x002dfa6a` | `QueryPerformanceCounter` | 11 | 9 | call:11 |
| `0x002dfa7f` | `QueryPerformanceFrequency` | 10 | 8 | call:10 |
| `0x002de927` | `XCalculateSignatureEnd` | 10 | 2 | call:10 |
| `0x0002a6d6` | `OutputDebugStringA` | 8 | 5 | call:8 |
| `0x0002a9f0` | `Sleep` | 7 | 7 | call:7 |
| `0x0002acce` | `GetLastError` | 5 | 4 | call:5 |
| `0x0002a88b` | `XLaunchNewImageA` | 4 | 4 | call:4 |
| `0x003bbf42` | `XInputClose` | 4 | 2 | call:4 |
| `0x0002a99e` | `SleepEx` | 3 | 3 | call:3 |
| `0x0002ab5e` | `XPhysicalAlloc` | 3 | 3 | call:3 |
| `0x0002aba1` | `MmFreeContiguousMemory` | 3 | 3 | call:3 |
| `0x002dfdd5` | `GetFileTime` | 3 | 3 | call:3 |
| `0x0002b280` | `FUN_0002b280` | 3 | 2 | call:3 |
| `0x002dfef1` | `GetFileSize` | 3 | 2 | call:3 |
| `0x003b4fa0` | `g_DeviceType_Gamepad` | 3 | 2 | addr:3 |
| `0x002de8cf` | `XCalculateSignatureBegin` | 2 | 2 | call:2 |
| `0x002df482` | `CompareFileTime` | 2 | 2 | call:2 |
| `0x002dfa19` | `FindNextFileA` | 2 | 2 | call:2 |
| `0x002dff2c` | `CreateDirectoryA` | 2 | 2 | call:2 |
| `0x003bbecd` | `XInputOpen` | 2 | 2 | call:2 |
| `0x003bc140` | `XInputGetState` | 2 | 2 | call:2 |
| `0x0002e454` | `FUN_0002e454` | 2 | 1 | call:2 |
| `0x002dfcdb` | `FUN_002dfcdb` | 2 | 1 | call:2 |
| `0x003b5d3f` | `XGetDeviceChanges` | 2 | 1 | call:2 |
| `0x0002a7e2` | `XGetLaunchInfo` | 1 | 1 | call:1 |
| `0x0002a9fe` | `SetFileAttributesA` | 1 | 1 | call:1 |
| `0x0002ab32` | `XSetFileCacheSize` | 1 | 1 | call:1 |
| `0x0002aba7` | `MmQueryAddressProtect` | 1 | 1 | call:1 |
| `0x0002abc4` | `GetProcessHeap` | 1 | 1 | call:1 |
| `0x0002df55` | `SetThreadPriority` | 1 | 1 | call:1 |
| `0x0002dfa7` | `FUN_0002dfa7` | 1 | 1 | call:1 |
| `0x0002e023` | `ExitThread` | 1 | 1 | call:1 |
| `0x0002e128` | `CreateThread` | 1 | 1 | call:1 |
| `0x002dedd3` | `FUN_002dedd3` | 1 | 1 | call:1 |
| `0x002df036` | `FUN_002df036` | 1 | 1 | call:1 |
| `0x002df105` | `FUN_002df105` | 1 | 1 | call:1 |
| `0x002df20c` | `FUN_002df20c` | 1 | 1 | call:1 |
| `0x002df253` | `FUN_002df253` | 1 | 1 | call:1 |
| `0x002df90c` | `FUN_002df90c` | 1 | 1 | call:1 |
| `0x002dfa94` | `FUN_002dfa94` | 1 | 1 | call:1 |
| `0x002dfb81` | `FUN_002dfb81` | 1 | 1 | call:1 |
| `0x002dfea0` | `FUN_002dfea0` | 1 | 1 | call:1 |
| `0x003b4f20` | `g_DeviceType_MU` | 1 | 1 | addr:1 |
| `0x003b5d18` | `XInitDevices` | 1 | 1 | call:1 |
| `0x003b5d1d` | `XGetDevices` | 1 | 1 | call:1 |
| `0x003bbf4e` | `XInputGetCapabilities` | 1 | 1 | call:1 |
| `0x003bc1b1` | `XInputSetState` | 1 | 1 | call:1 |

### D3DX8

19 targets, 61 sites.

| Target | Name | Sites | Game functions | How |
|---|---|---|---|---|
| `0x002e02f2` | `D3DXMatrixMultiply` | 12 | 7 | call:12 |
| `0x002e03f0` | `D3DXMatrixTranspose` | 8 | 7 | call:8 |
| `0x0037b7c1` | `FUN_0037b7c1` | 5 | 4 | call:5 |
| `0x00395263` | `FUN_00395263` | 5 | 4 | call:5 |
| `0x00389fb3` | `FUN_00389fb3` | 4 | 2 | call:4 |
| `0x00389e79` | `FUN_00389e79` | 3 | 2 | call:3 |
| `0x00389f9c` | `FUN_00389f9c` | 3 | 2 | call:3 |
| `0x002e08a2` | `D3DXMatrixPerspectiveFovLH` | 3 | 3 | call:3 |
| `0x002e045d` | `D3DXMatrixInverse` | 3 | 2 | call:3 |
| `0x0037b8e4` | `FUN_0037b8e4` | 2 | 1 | call:2 |
| `0x003951ae` | `FUN_003951ae` | 2 | 1 | call:2 |
| `0x002e07ea` | `D3DXMatrixScaling` | 2 | 2 | call:2 |
| `0x002e0846` | `D3DXMatrixTranslation` | 2 | 2 | call:2 |
| `0x002e0936` | `D3DXMatrixOrthoOffCenterLH` | 2 | 2 | call:2 |
| `0x0037c289` | `FUN_0037c289` | 1 | 1 | call:1 |
| `0x0037c2b2` | `FUN_0037c2b2` | 1 | 1 | call:1 |
| `0x00394f87` | `FUN_00394f87` | 1 | 1 | call:1 |
| `0x00394fe3` | `FUN_00394fe3` | 1 | 1 | call:1 |
| `0x00395346` | `FUN_00395346` | 1 | 1 | call:1 |

### XGRAPHC

6 targets, 23 sites.

| Target | Name | Sites | Game functions | How |
|---|---|---|---|---|
| `0x00397fb4` | `FUN_00397fb4` | 8 | 3 | call:8 |
| `0x003a0949` | `XGSwizzleBox` | 6 | 1 | call:6 |
| `0x0039ea9a` | `FUN_0039ea9a` | 4 | 3 | call:4 |
| `0x0039fae1` | `XGSwizzleRect` | 3 | 2 | call:3 |
| `0x0039f26f` | `FUN_0039f26f` | 1 | 1 | call:1 |
| `0x003a0d7d` | `XGSetSurfaceHeader` | 1 | 1 | call:1 |

### xboxkrnl

2 targets, 2 sites.

| Target | Name | Sites | Game functions | How |
|---|---|---|---|---|
| `0x003bc22c` | `MmQueryStatistics` | 1 | 1 | read:1 |
| `0x003bc300` | `RtlInitializeCriticalSection` | 1 | 1 | read:1 |

### CRT

137 targets, 8562 sites.

| Target | Name | Sites | Game functions | How |
|---|---|---|---|---|
| `0x002cf914` | `___CxxFrameHandler` | 1825 | 1825 | call:1825 |
| `0x002cfff6` | `_atexit` | 1686 | 1663 | call:1686 |
| `0x002d01ab` | `_fprintf` | 1003 | 46 | call:1003 |
| `0x002cf37c` | `__RTtypeid` | 660 | 570 | call:660 |
| `0x002cf01a` | `type_info::operator==` | 607 | 527 | call:607 |
| `0x002d0f84` | `_ftol` | 405 | 111 | call:405 |
| `0x002d1263` | `type_info::name` | 324 | 2 | call:324 |
| `0x002cf7af` | `__RTDynamicCast` | 311 | 106 | call:311 |
| `0x002cfd19` | `_CxxThrowException` | 160 | 160 | call:160 |
| `0x002cf117` | `_sprintf` | 126 | 57 | call:126 |
| `0x002d0440` | `_alloca_probe` | 109 | 58 | call:109 |
| `0x002d015a` | `_fclose` | 96 | 75 | call:96 |
| `0x002d0436` | `_atol` | 90 | 39 | call:90 |
| `0x002d0265` | `_fopen` | 86 | 72 | call:86 |
| `0x002d0382` | `_atof` | 83 | 30 | call:83 |
| `0x002d0d60` | `__allshr` | 59 | 16 | call:59 |
| `0x002d08f0` | `_strstr` | 58 | 17 | call:58 |
| `0x002cf059` | `type_info::operator!=` | 53 | 43 | call:53 |
| `0x002d1330` | `_strchr` | 51 | 21 | call:51 |
| `0x002d0322` | `??_M@YGXPAXIHP6EX0@Z@Z` | 47 | 42 | call:47 |
| `0x002d0c5b` | `__stricmp` | 47 | 40 | call:47 |
| `0x002d1198` | `FUN_002d1198` | 45 | 36 | call:45 |
| `0x002d0d90` | `__allmul` | 38 | 7 | call:38 |
| `0x002d05b5` | `??_L@YGXPAXIHP6EX0@Z1@Z` | 37 | 35 | call:37 |
| `0x002d075f` | `FID_conflict:_fread` | 33 | 11 | call:33 |
| `0x002d08a3` | `FID_conflict:_fread` | 30 | 20 | call:30 |
| `0x002d0010` | `_strncpy` | 22 | 19 | call:22 |
| `0x002cddd6` | `FUN_002cddd6` | 22 | 11 | call:22 |
| `0x002cddf7` | `FUN_002cddf7` | 22 | 11 | call:22 |
| `0x002cfcf7` | `_rand` | 22 | 8 | call:22 |
| `0x002d1760` | `_strrchr` | 20 | 16 | call:20 |
| `0x002ceb2f` | `FUN_002ceb2f` | 16 | 10 | call:16 |
| `0x002d10c7` | `FUN_002d10c7` | 16 | 10 | call:16 |
| `0x002d1788` | `FUN_002d1788` | 13 | 13 | call:13 |
| `0x002d19c4` | `FUN_002d19c4` | 13 | 10 | call:13 |
| `0x002d1306` | `FUN_002d1306` | 13 | 8 | call:13 |
| `0x002d13f0` | `thunk_FUN_002d13f2` | 12 | 9 | call:12 |
| `0x002d1853` | `FUN_002d1853` | 11 | 7 | call:11 |
| `0x002ce4b6` | `FUN_002ce4b6` | 11 | 4 | call:11 |
| `0x002d1e7e` | `_fseek` | 11 | 3 | call:11 |

...and 97 more in api-surface.csv.

