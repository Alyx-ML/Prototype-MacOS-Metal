# Prototype: macOS ARM64 static recompilation log

This log records each porting step. Each section states what was verified and what was not verified.

Rule: do not share game files, generated `.llasm`, `.ll`, object files, or any app that contains translated game code. The `work/` and `build/` directories contain translated game code.

## 2026-11-11: Step 1, identify

Host: macOS 27.3 (build 26C5035r), Apple silicon. Homebrew LLVM 23.1.1, LDC 1.43.0.

### Game binaries (Steam installation)

| File | Size (bytes) | SHA-256 | Role |
|---|---|---|---|
| `prototypef.exe` | 2275824 | `ac1542512ab5eddffcbf949919f3dcc4b26e38f07b57cb6a094def2a621b0b82` | Launcher. Image base 0x400000. Entry RVA 0x25B7. Code size 0x1C3C. Relocations stripped. |
| `prototypeenginef.dll` | 20205568 | `d331be205bdde99897952ee88261ab7e63c5c65e36a4660e2a367886a3537071` | Game engine. Image base 0x10000000. Entry RVA 0xA888C5. Code size 0xD8E364. Has a `.reloc` section. 77 exports (Scaleform GFx classes). |
| `binkw32.dll` | 2248704 | `d09f7f58854af699f0b70bd3e774a37481b5be6944a3a9f7b5cf84c3e8170693` | Third-party proxy DLL (ASI loader). Not part of the port. |
| `binkw32Hooked.dll` | 171008 | `dba257f26517d57ecf3c62aca7cccfabbad868dddf97160f71c9ab81d37a8627` | RAD Bink library. The port replaces it with a native Bink decoder. |
| `prototype_fix.asi` | 640000 | `3eee479c3d18769ddba73d1b72b07a70f1e84bda1146d3f5bb4a2be8c1c94e7e` | Third-party fix mod. Not part of the port. |

Both PE files are 32-bit x86, linked against `MSVCR80.dll` (Visual C++ 2005). No packer section and no DRM section was found (sections: `.text`, `.rdata`, `.data`, `.idata`, `.rsrc`, `.reloc`).

### Engine imports

`d3d9.dll` (`Direct3DCreate9`), `DSOUND.dll` (by ordinal), `XINPUT1_3.dll` (by ordinal), `binkw32.dll` (17 functions), `WINMM.dll` (waveOut, timers), `WS2_32.dll` (by ordinal), `iphlpapi.dll`, `SETUPAPI.dll`, `ADVAPI32.dll` (registry, CryptoAPI), `ole32.dll`/`OLEAUT32.dll` (COM), `KERNEL32.dll`, `USER32.dll`, `SHELL32.dll`, `SHLWAPI.dll`, `RPCRT4.dll`, `MSVCR80.dll`.

### Instruction mix (linear sweep of engine `.text`)

About 3.16 million instructions (7.17 million decoded items, of which 4.0 million are `int3` padding). Main SSE use: `movss` 154423, `mulss` 23718, `movaps` 19936, `subss` 14559, `addss` 14470, `comiss` 9618, `xorps` 6231, `shufps` 2494, `cvtsi2ss` 2273. Atomics: `lock` 553, `xadd` 395, `cmpxchg` 120, `cmpxchg8b` 20. x87: `fld` 37412, `fstp` 35528, `fcomi`/`fcompi` 1851.

Verified: hashes, PE headers, sections, import lists, instruction counts.
Not verified: the launcher-to-engine call sequence (not traced yet).

## 2026-11-11: Toolchain

- SR source: https://github.com/M-HT/SR, commit `ac690ddf3010bc3d2c875cb1b5e8da4f53a8d5b3` (2026-08-27), stored in `third_party/SR`.
- Local SR changes:
  - `SRW/SR_defs.h`: `OUTPUT_TYPE` set to `OUT_LLASM`.
  - `SRW/SRW_loader.c`: unknown ordinal imports get the name `ord_<dll>_<ordinal>` (for example `ord_ws2_32_16`). Before this change, SRW stopped with `Error: Import by ordinal not supported`.
- Built `build/srw/SRW` (arm64) with clang and `build/llasm/llasm` (arm64) with LDC.

Verified: both tools build and run on arm64. SRW loads all 17 engine import DLLs.

## 2026-11-11: Step 3, first SRW run on the engine

Result: SRW stops at the first SSE instruction:
`Error: unknown flags - 0 - 378957 (0x1005d84d) - movss [ecx+0x8], xmm3`

Cause: upstream SRW (llasm output) has no support for SSE, SSE2, `lock` prefixes, `xadd`, `cmpxchg`, `cmpxchg8b`, `fcomi`, `fucomi`, or `cpuid`. The SRW llasm backend and llasm must be extended before translation can continue.

## 2026-11-11: SSE support design (not implemented yet)

Findings from the SRW and llasm source:

- llasm IR has integer operations only. x87 instructions become calls to C helpers (`SR/llasm-support/llasm_float.c`), declared in `.llinc` files with `func`/`funcv`.
- A `func` helper returns its value in `tmp0`.
- llasm stores all guest registers before each `call` and loads them again after it. A helper can therefore write `cpu->_eflags` or a guest register directly.
- The `_cpu` structure has no XMM state. The llasm IR type of `_cpu` covers only the first 14 fields, so new fields can go at the end of the C structure without a change to existing offsets.

Planned design:

1. Add `_xmm[8]` (16 bytes each, 16-byte aligned) and `_mxcsr` at the end of `_cpu` in `llasm_cpu.h`.
2. Add `llasm_sse.c` and `llasm_sse.llinc`. Each SSE instruction form becomes one helper call with XMM register numbers, guest addresses, and immediates as arguments.
3. Link the helper LLVM IR with the translated IR (`llvm-link`) before `opt`, so that LLVM inlines the helpers.
4. Add SRW flag-table entries (`udis86_dep.c`) and lowering (`SR_full_llasm_sse.inc`, included from `SR_full_llasm_instr.c`).
5. `comiss`/`ucomiss`/`fcomi` helpers write ZF, PF, CF and clear OF, SF, AF.
6. `lock`-prefixed instructions, `xchg` with memory, `xadd`, `cmpxchg`, `cmpxchg8b` become C11 atomic helpers.

Open risk: x86 has a strong memory order (TSO). ARM64 does not. Guest threads that use lock-free code without `lock` instructions can fail on ARM64. A possible solution is to emit guest loads as acquire and guest stores as release. The performance cost is not measured.

## 2026-11-11: SSE, atomic and x87 support in SRW and llasm

The design in the previous section changed as follows:

- The helpers are in one file, `SR/llasm-support/llasm_x86ext.c` (254 exported helpers). The declarations are in `llasm_x86ext.llinc`. The tool `tools/gen_llinc.py` generates this file from the preprocessed C source. `llasm.llinc` includes it.
- The helpers are linked as normal C objects. They are not linked as IR with `llvm-link`, so LLVM does not inline them at this time.
- The SRW lowering is in `SRW/SR_full_llasm_x86ext.inc`. `SR_full_llasm_instr.c` calls it before its own instruction switch. The function returns 1 (instruction done), 0 (not an extension instruction), or -1 (operand form not supported, SRW stops with an error).
- All memory operands use the `MADR_REG` mode, so each helper gets a guest address in a register. This prevents the SRW error `address size error: 128`.

Instructions added (one helper call each):

- SSE and SSE2 data movement, arithmetic, logic, compare, shuffle, unpack, conversion, and packed-integer instructions, `ldmxcsr`, `stmxcsr`.
- `lock` forms of `add`, `or`, `adc`, `sbb`, `and`, `sub`, `xor`, `inc`, `dec`, `not`, `neg`, `bts`, `btr`, `btc`; `xchg` with memory, `xadd`, `cmpxchg`, `cmpxchg8b`. These use `__atomic` compare-exchange loops. An atomic access that crosses a 16-byte boundary stops the program with a message, because ARM64 (LSE2) does not make it atomic.
- `cmovcc`, `setcc` (o, no, ae, p, np), `fcmovcc`, `fcomi`, `fucomi`, `fcomip`, `fucomip`, `fpatan`, `fsincos`, `fscale`, `frndint`, `f2xm1`, `fldl2e`, `fldl2t`, `fldpi`, `ffree`, `ffreep`, `fisttp m32`.
- `cmpsw`, `cmpsd`, `scasw`, `scasd` (with and without `rep`), `imul m8`, `bsf`, `bsr` with a memory source.
- `cpuid` (reports a family 15 GenuineIntel CPU with FPU, TSC, CX8, CMOV, MMX, FXSR, SSE, SSE2), `pause`, `lfence`, `sfence`, `mfence`. `prefetch*` and `emms` have no effect.

Behavior notes:

- `ldmxcsr` sets ARM64 FPCR.FZ only when the guest sets both FTZ and DAZ.
- `rcpss`, `rcpps`, `rsqrtss`, `rsqrtps` return exact results. x86 hardware returns approximations, so results can differ in the low bits.
- SSE2 `cmpsd` (compare scalar double) is not supported. SRW stops with an error if it occurs, because the SRW flag table has one entry for SSE2 `cmpsd` and string `cmpsd`. The engine does not use it.
- `x86_cpu` in `llasm_cpu.h` has two new fields at the end: `_mxcsr` and `_xmm[8]`.

Verified: `llasm_x86ext.c` compiles with `-Wall -Wextra` and has no warnings.
Not verified: helper results against x86 hardware. No test of the helpers runs yet.

## 2026-11-11: Step 3, SRW translates the whole engine

SRW changes for this step:

- `SRW/udis86_dep.c`: flag-table entries for all new instructions.
- `SRW/SR_full.c`: SRW continues after a region error, counts all region errors, and reports `Error: %u regions failed`. Before this change, SRW stopped at the first region error.
- `SRW/SR_full_win32.c`: in `SR_disassemble_change_cjump_iflags`, a flag entry from `instruction_flags.sci` and a flag entry that SRW makes for a conditional jump are merged with OR. Before this change, SRW stopped with `iflags->begin conflict`.
- `SRW/SR_full_llasm_instr.c`: `test`, `or` or `and` of a register with itself, whose flags no instruction reads, becomes `NOP`. Before this change, SRW stopped with `llasm instruction error`.

Flag values that are live across a branch target: SRW reported 551 region starts with the error `unknown flags on start of region`. `tools/fix_region_flags.py` writes `instruction_flags.sci` entries for each such start and its predecessors. The predecessors come from a linear `llvm-objdump` disassembly. `tools/verify_region_flags.py` compares these predecessors with the jumps in the SRW output.

Two warnings remain: `unknown weak flags on start of region` at 0x10a88f8f and 0x10a89266. Both addresses are in MSVC CRT error-report functions. These functions use `pushfd` to copy EFLAGS into a diagnostic record only. The incorrect flag values do not change program behavior.

Instruction replacements (`work/engine/instruction_replacements.sci`): at 0x1062E45B, a call to 0x1062E1B0 returns into `int3` padding at 0x1062E460. llasm stopped with `Proc inside proc/dataseg`. The replacement pushes the return address, jumps to the callee, and makes 0x1062E460 a procedure that executes `int 3`. The callee has 7 other call sites that return, so it cannot be marked as a function that does not return.

`tools/find_code_seeds.py` adds code addresses that only data references to `fixup_interpret_as_code.sci`. Its first version did not read export aliases (`define loc_X name`) and added 17 export addresses. SRW already translates these exports, so the 17 entries have no effect. The current version reads the aliases from `engine.llasm` and finds 0 new seeds.

Result: SRW returns status 0. The output has 528,701 procedures. `verify_region_flags.py`: 561 region starts checked, 0 failures.

Verified: SRW status, the flag-entry check, procedure count.
Not verified: that every translated procedure is correct. No translated code has run yet.

## 2026-11-11: Step 4, llasm to LLVM IR

Changes:

- `llasm/llasm.d`: the final `daddr` check resolves `define` aliases. SRW writes export aliases (`define loc_10965E00 <export>`) after the data includes, so llasm stopped with `Daddr not a proc nor a dlabel`.
- `tools/gen_extern_llinc.py` writes `runtime/llasm/extern.llinc` with `proc NAME external` for the 78 imports that only data references (for example `_HUGE`).
- `SRW/SRW_loader.c`: import and export names that contain characters other than `[A-Za-z0-9_]` are encoded as `X_` names (for example `??2@YAPAXI@Z` becomes `X__Q_Q2_AYAPAXI_AZ`). MSVC C++ names are not valid LLVM or C identifiers, so `opt` stopped with `expected function name`. The encoding is in the comment of `SymbolName`. The runtime must use the same encoding for `GetProcAddress` names.
- `tools/translate_engine.sh`: llasm options are now `-m64 -inline-idiv -inline-float -pic -ptrofs -O`. These are the upstream SR options for arm64-darwin with offset pointers.

Result: `engine.ll` (753 MB). `opt` with the arm64-apple-macosx14.0.0 triple and data layout reads the file, and the LLVM verifier finds no errors.

Address model (from the upstream Septerra Core port, `Game-Memory.c`): the runtime sets `pointer_offset` to a free region below the program image and calls `ptr_initialize_pointers`. Guest pointers are 32-bit offsets from `pointer_offset`. The program image, the guest stacks and the guest heap must be in the 2 GB above `pointer_offset`.

## 2026-11-12: Step 4, the engine compiles to ARM64

Changes:

- `llasm/llasm.d`: `initialize_pointers` is table-driven. For each data segment, llasm writes two constant tables: `@__pi_slots_<segment>` (offsets) and `@__pi_vals_<segment>` (targets). One loop for each segment stores each target minus the pointer offset. Before this change, the function had one store for each pointer, and `clang` did not complete the compilation of the function.
- `work/launcher/global_aliases.sci` and `work/engine/global_aliases.sci`: the image bases have the names `launcher_image` (0x400000) and `engine_image` (0x10000000). The runtime uses these names for module handles.
- `tools/gen_thunks.py` also writes `runtime/src/rt_import_table.c` (dll, name, ordinal and thunk of each import). `GetProcAddress` uses this table for system DLLs.
- `tools/gen_exports.py` (new) writes the export table of a module (`work/engine/exports.c`: 77 exports; `work/launcher/exports.c`: 0 exports).

Result: `tools/compile_engine.sh` makes 96 objects (102,543,016 bytes) with no warnings. The launcher compiles to one object.

`runtime/llasm/extern.llinc` is obsolete. The new llasm output declares the external procedures itself. The file is not deleted.

Verified: the compiler exit status and the object sizes; each of the 77 export symbols is defined in the objects.
Not verified: no translated code has run.

## 2026-11-12: Step 5, Win32 and C runtime implementation

The runtime is C (C2x). `tools/cc_runtime.sh` compiles each file with `-Wall -Wextra -Wshadow -Wconversion -Werror`. An unsupported condition stops the process with a message (`rt_fatal`). There is no fallback behavior.

Design:

- Each guest call, jump and return is a tail call. Only `rt_call` makes a native frame. `rt_call` records the guest stack pointer and a `setjmp` buffer for each level. `rt_transfer` continues guest code at a guest stack position (for SEH and `longjmp`).
- Structured exception handling follows the Windows model. The exception records and the contexts are on the guest stack. The runtime implements `_except_handler4_common` (EH4 scope tables, cookie checks and local unwinds), `RtlUnwind` semantics, `_setjmp3` and `longjmp`.
- Kernel objects (events, mutexes, semaphores, threads, files, find handles, registry keys, crypt objects, device lists) are in one handle table. Waits use one mutex and one condition variable.
- Threads are host threads with an 8 MiB native stack. Fibers save and restore the guest registers and the TEB fields.
- The file system maps `C:\Prototype` to the game directory, `C:\Users\Player` to the user directory and `C:\ProgramData` to a port directory. Path components are found without regard to case.

Files (`runtime/src`): `rt_core.c`, `rt_mem.c`, `rt_object.c`, `rt_thread.c`, `rt_sync.c`, `rt_seh.c`, `rt_module.c`, `rt_file.c`, `rt_kernel_misc.c`, `rt_crt_start.c`, `rt_crt_str.c`, `rt_crt_math.c`, `rt_crt_fmt.c`, `rt_crt_file.c`, `rt_crt_rtti.c`, `rt_undname.c`.

C runtime behavior that the runtime copies from MSVCR80:

- `printf` family: at most 17 significant digits, 3-digit exponents, `1.#INF`, `1.#QNAN`, `1.#IND` with the digit rounding of the Microsoft runtime (for example `%.2f` of infinity gives `1.#J`), `%n` only after `_set_printf_count_output`, and the `_snprintf` and `_snprintf_s` truncation rules.
- Streams: text mode translation (`\r\n`, Ctrl+Z), the read-after-write error rule, and `fflush` of an input stream discards the buffered input.
- `type_info::name`: `rt_undname.c` undecorates type descriptor names, including templates and function-local classes.

Tests (`tools/run_tests.sh`, programs `tests/test_*.c` with `tests/harness.c`):

- `test_crt`: 189 checks of `printf`, `scanf` and stream functions. Result: 0 failures.
- `test_rtti`: 35 checks of `__RTDynamicCast` (single, multiple and virtual inheritance), `__RTtypeid`, `type_info` and the undecorator. With `RT_UNDNAME_ORACLE` set to a file of `llvm-undname` results, the test checks all 11,837 type descriptor names of `prototypeenginef.dll`. Result: 0 failures. (The comparison ignores spaces, because `llvm-undname` puts spaces in other positions.)
- `test_kernel`: 72 checks of `VirtualAlloc`, `VirtualFree`, `GetVersionExA`, `GlobalMemoryStatus`, `GetLocaleInfoA`, `MultiByteToWideChar` and `FormatMessageA`. Result: 0 failures.

Known limitations:

- x87 precision control, x87 rounding control and the MXCSR rounding and flush-to-zero modes have no effect on arithmetic. `_controlfp_s` records the values only.
- Hardware faults (access violations, division by zero) do not become SEH exceptions.
- `TerminateThread` of a running thread, named kernel objects and the share modes of `CreateFileA` are not supported. The first two stop the process.
- At process exit, the other threads are suspended before the DLL detach notifications. A suspended thread that holds a lock can make the exit wait.
- The system reports Windows XP SP3, English (United States), code page 1252. Other locales stop the process in `GetLocaleInfoA`.
- `printf` rounding uses the exact binary value rounded to 40 digits, then to 17 digits. The Microsoft runtime uses 80-bit arithmetic. Some halfway values can round differently.
- A failed `dynamic_cast` to a reference stops the process with a message. The game has no C++ catch handlers, so `std::bad_cast` also ends the process on Windows.

Not done: user32 (window and messages), gdi32, advapi32 (registry and crypt), ole32 and oleaut32, setupapi, iphlpapi, rpcrt4, ws2_32, winmm, XInput, Bink, DirectSound, XAudio2, DirectInput8, D3D9 to Metal, `main`, the build and link script.

## 2026-11-12: Step 5, registry, CryptoAPI and COM

Verification time: 2026-11-12T01:51:55+0000.

Registry and CryptoAPI (`rt_registry.c`, `rt_crypt.c`):

- The registry is a tree in host memory. The runtime saves keys that are not volatile to a text file after each change. The first start makes the values of `10150_install.vdf` (install path `C:\Prototype`, English language values).
- `rt_registry_get_sz` reads a `REG_SZ` value for the runtime. `CLSIDFromString` uses it for ProgID lookup.
- CryptoAPI: MD5 (own RFC 1321 code), SHA-1 (CommonCrypto), RSA public-key encryption with PKCS #1 v1.5 or OAEP (Security framework), `CryptGenRandom` (`arc4random_buf`). Only the providers "Microsoft Base", "Enhanced" and "Strong" and `PUBLICKEYBLOB` keys are supported.

COM (`rt_ole32.c`, `rt_oleaut32.c`, `rt_wmi.c`, `rt_rpcrt4.c`, `rt_setupapi.c`, `rt_gpu_info.m`):

- Object model: `runtime/include/rt_com.h`. A runtime COM object has a guest block (object id and one vtable pointer for each interface slot) and a host part (class, reference count, data). The guest calls methods through the vtable. Each vtable entry is a generated llasm thunk.
- `runtime/com.spec` lists the vtables. `tools/gen_com_thunks.py` writes `runtime/llasm/com_thunks.llasm`, `runtime/include/rt_com_thunks.h` and `runtime/src/rt_com_vtables.c`. A method marked `-` stops the process with a message.
- `tools/build_llasm_rt.sh` translates runtime llasm files (`calls`, `com_thunks`) to ARM64 objects in `build/rt`.
- `QueryInterface` for an interface that a runtime object does not implement stops the process with the IID.
- `CoCreateInstance` classes: WbemLocator is implemented. GameExplorer (a Windows Vista class that the launcher asks for) returns `REGDB_E_CLASSNOTREG`, as on Windows XP. Every other class stops the process. DirectInput8, DirectSound8 and XAudio2 are not implemented yet.
- WMI: the engine reads `Win32_VideoController.AdapterRAM` from `ROOT\CIMV2`. The runtime gives one video controller (the Metal device). AdapterRAM is the Metal recommended working set size, limited to 0x7FF00000 bytes, because the engine reads it as a signed 32-bit value.
- OLEAUT32: BSTR functions, `VariantInit`, `VariantClear`, `VariantChangeType` (integer, floating-point, Boolean and string types; round to nearest with ties to even; English number text), and the error information objects.
- SETUPAPI: the engine enumerates kernel streaming audio interfaces for its WDM-KS audio driver. The emulated system gives audio only through the interfaces that the runtime implements. All device information sets are empty.
- Audio driver selection: the engine reads `audio.cfg`. The default is `[XAudio2]`. The engine also has ASIO, DirectSound, MME (`waveOut`), WDM-KS and Null drivers.

Tests:

- `test_advapi`: 67 checks (registry, MD5 and SHA-1 test vectors, RSA-OAEP encryption that the test decrypts with the private key). Result: 0 failures.
- `test_com`: 146 checks. The test calls the COM methods through the guest vtables with `rt_call` and the generated thunks. Result: 0 failures.
- The macOS sandbox of the agent session blocks Metal device access. `MTLCreateSystemDefaultDevice` returns nil there, and `test_com` stops with "no Metal device is available". The result above is from a run outside the sandbox.

Imports without an implementation: 99 (user32 32, ws2_32 30, binkw32 17, winmm 10, iphlpapi 4, xinput1_3 3, gdi32 1, dsound 1, d3d9 1).

## 2026-11-12: Step 5, coverage fix, WS2_32, USER32, GDI32, WINMM, XINPUT

Time of the last check: 2026-11-12T02:34:20+0000.

### Code coverage fix

- `tools/find_code_seeds.py` also collects immediate operands from the
  comment lines of translated code. It does not collect memory operands or
  branch targets.
- `tools/find_code_seeds.py` takes data references only from data files of
  other sections. References in the data of the same section are jump
  tables of code that is not decoded. Such seeds made SRW stop with a
  segmentation fault.
- `third_party/SR/SRW/SR_full_llasm.c` stops with error 4 when an
  instruction has no text. Before this change, SRW used a NULL pointer.
- The engine translation used 7 passes. The launcher translation got 5 new
  seeds.
- `tools/compile_engine.sh` compiled the engine with no warnings
  (112,617,936 bytes of ARM64 objects). The launcher object compiled with no
  warnings.

### Imports of kind "unreachable"

- An import of kind `unreachable` has no implementation. Its generated
  function stops the process with a message.
- `tools/check_unreachable.py` makes sure that no translated code refers to
  such an import. Result: 8 imports checked, 0 referenced.
- The 8 imports are the 4 IPHLPAPI imports, WSAAsyncGetHostByName,
  WSACancelAsyncRequest, BinkOpenTrack and BinkCloseTrack.

### WS2_32 (`runtime/src/rt_ws2_32.c`)

- The import table has the BSD socket functions, `select` (on `poll`) and
  `getaddrinfo`.
- Fix: `accept` read the listening socket after the reference was released.
- Fix: `getaddrinfo` dropped the canonical name when the allocation failed.
  It now returns `WSA_NOT_ENOUGH_MEMORY`.
- Fix: macOS does not accept `MSG_TRUNC` as an input flag of `recv`. A
  datagram that was larger than the buffer was cut without
  `WSAEMSGSIZE`. Datagram receives now use `recvmsg` and read `MSG_TRUNC`
  from `msg_flags`.
- `tests/test_ws2.c`: 164 checks, 0 failures. The test binds local ports,
  so it must run outside the agent sandbox, which blocks local binding
  (`WSAEACCES`).

### USER32 (`runtime/src/rt_user32.c`, `runtime/src/rt_window.m`)

- Each top-level window is one `NSWindow`. Its content view has a
  `CAMetalLayer` for the renderer (`rt_cocoa_window_layer`).
- Windows are supported on the main thread only. The runtime stops with a
  message for child windows, menus and style changes.
- The posted-message queue and the input queue hold 10000 messages each.
  `PeekMessageA` returns posted messages first, then input, then `WM_QUIT`.
  Mouse moves are combined.
- `GetKeyState` gives the key state at the last message that
  `PeekMessageA` removed.
- Keys map by position to the US layout. Option is Alt. Command is the
  Windows key. Num Lock reports "on".
- Activation, `WM_CLOSE` and resize go through the Windows message order.
  Command-Q sends `SC_CLOSE` to each window.
- `MessageBoxA` uses `CFUserNotificationDisplayAlert`, so that any thread
  can show an alert.
- `SystemParametersInfoA` keeps the StickyKeys, ToggleKeys and FilterKeys
  values for the process only. macOS has no such hotkeys.

### GDI32, WINMM, DSOUND, XINPUT

- `runtime/src/rt_gdi32.c`: `GetStockObject` returns fixed handle values.
- `runtime/src/rt_winmm.c`:
  - Each multimedia timer has its own callback thread. The thread has a
    guest thread state.
  - `waveOut` plays through `AudioQueue`. Each open device has a callback
    thread. The CoreAudio thread only queues the finished buffers.
  - 8-bit PCM is converted to 16-bit PCM.
  - `DirectSoundEnumerateA` lists the CoreAudio output devices. The GUID of
    a device is a hash of the device UID.
- `runtime/src/rt_xinput.m`: the controllers come from
  GameController.framework. Vibration uses CoreHaptics.

### What was not verified

The user said to stop testing, so no unit tests were written for USER32,
WINMM or XInput. These files only compile with `-Werror`. Nothing has run
them yet.

### Remaining imports: 16

- BINKW32: 15 functions. The engine installs its own reader with
  `BinkSetIO` (`loc_105A1960`). That reader fills a 0x144-byte `BINKIO`
  structure with 6 callbacks. The engine also reads `BINK` fields
  directly. A replacement on FFmpeg 9.0.2 (Bink video and audio decoders
  are present) must copy this RAD structure layout. The layout beyond the
  fields that the engine uses is UNCONFIRMED.
- D3D9: `Direct3DCreate9` (step 6).
- COM classes that are not yet supported: XAudio2 (the default audio
  driver in audio.cfg) and DirectInput8.

## 2026-11-12: BINKW32 on FFmpeg

Time: 2026-11-12T03:01:19+0000.

### What changed

- `runtime/src/rt_bink.c` (new): the 15 BINKW32 imports on the FFmpeg
  9.0.2 decoders `binkvideo`, `binkaudio_dct` and `binkaudio_rdft`.
  - The engine's `BINKIO` reader supplies all file data. The runtime
    calls its callbacks: `ReadHeader`, `ReadFrame`, `GetBufferSize`,
    `SetInfo`, `Idle` and `Close`.
  - The runtime parses the Bink 1 header and frame index. It checks each
    value before it allocates memory.
  - Video decodes into the planes that the engine registers with
    `BinkRegisterFrameBuffers`. The runtime reports one frame set. A
    negative pitch is supported.
  - Audio decodes to 16-bit interleaved PCM in a 4-second ring for each
    track. The engine's `BINKSND` driver gets the PCM through its
    `Ready`, `Lock` and `Unlock` callbacks.
  - The host clock controls frame timing. `BinkDoFrame` returns nonzero
    when playback is behind. The engine uses this value to skip frames.
  - Open flags that are not supported, Bink 2 files and missing
    `BinkSetIO` cause `rt_fatal`.
- `tools/cc_runtime.sh`: adds `-isystem /opt/homebrew/opt/ffmpeg/include`.
  The FFmpeg headers do not compile without errors under `-Wconversion`.
  The `-isystem` option keeps the warnings off for those third-party
  headers only.

### UNCONFIRMED items

- `FrameNum` starts at 1, as recalled from the RAD SDK.
- The `BINK` fields after `+0xC`.
- The `BINKSND` order: `Pause` at `+0x16C`, then `SetOnOff` at `+0x170`.
- With no `BINKSNDTRACK`, the runtime plays the first track of the file.
- The FFmpeg decoders get their settings from `codec_tag` (the file
  signature) and a 4-byte `extradata` field. Video gets the video flags.
  Audio gets the signature.

### What was not verified

The user said not to test. `rt_bink.c` compiles with `-Werror`. Nothing
has run it yet. The final link must add `-lavcodec -lavutil` from
`/opt/homebrew/opt/ffmpeg/lib`.

### Remaining imports: 1

- D3D9: `Direct3DCreate9` (step 6).
- COM classes that are not yet supported: XAudio2 and DirectInput8.

## 2026-11-12: Direct3D 9 on Metal

Time: 2026-11-12T03:49:13+0000.

### Changes

- `runtime/src/rt_d3d9_shader.c` and `runtime/include/rt_d3d9_shader.h`:
  the translator from D3D9 shader bytecode (shader model 2 and 3) to Metal
  Shading Language. The vertex shader adds the constant depth bias
  (`vs_misc.z`) to z/w.
- `runtime/src/rt_d3d9_int.h`: the shared declarations. These are the
  format table, the resource classes, the device state, the render state
  numbers and the sampler state numbers.
- `runtime/src/rt_d3d9_res.m`: the formats and the resources. These are
  textures, cube textures, surfaces, vertex buffers, index buffers, vertex
  declarations, shaders and event queries.
- `runtime/src/rt_d3d9_draw.m`: the render passes, the pipelines and the
  draws. It also has `Clear`, `StretchRect`, `ColorFill` and `Present`.
- `runtime/src/rt_d3d9.m`: `Direct3DCreate9` and `IDirect3D9`. These give
  the adapter, the display modes, the format checks, the capabilities and
  `CreateDevice`. The file also has the device methods for state,
  resources, `Reset` and `Present`.
- `runtime/com.spec`: the D3D9 vtables. The generated thunks are new.
- `runtime/src/rt_user32.c` and `runtime/include/rt_user32.h`:
  `rt_user32_window_layer` gives the `CAMetalLayer` of a window.
  `rt_user32_client_size` gives the client size of a window.
- `runtime/src/rt_gpu_info.m`: `rt_gpu_device` gives the Metal device.
- `tools/cc_runtime.sh`: the script also compiles `.m` files, with ARC.

### Behavior

- Draws must use a vertex shader and a pixel shader. A fixed-function draw
  causes `rt_fatal`.
- These features cause `rt_fatal` when a draw uses them: user clip planes,
  `D3DRS_WRAP*`, flat shading, point fill mode, fog with a shader model 2
  pixel shader, W-buffering, point sprites, N-patches, a nonzero
  `D3DSAMP_MIPMAPLODBIAS`, and a border color that is not transparent
  black, black or white.
- Event queries work. Other query types return `D3DERR_NOTAVAILABLE`.
- Metal has no 24-bit depth format. The D24 formats use 32-bit float depth.
- A full-screen device does not change the display mode. `Present` scales
  the back buffer to the window and keeps the aspect ratio. The gamma ramp
  applies in full-screen mode only.
- At most 2 frames are in flight.
- The pipeline cache, the depth-stencil state cache and the sampler cache
  have size limits. A full cache is emptied.

### UNCONFIRMED items

- The IIDs of the D3D9 interfaces.
- The flag values in `D3DCAPS9`.
- The Metal winding agrees with the D3D9 winding (front faces are
  clockwise).
- An unbound sampler reads (0, 0, 0, 1). A shader input that the
  declaration does not supply reads (0, 0, 0, 1).
- Metal converts integer vertex formats to float shader inputs.
- Metal needs a 4-byte aligned index buffer offset. The runtime copies
  unaligned index data.
- Metal accepts a depth attachment that is larger than the color
  attachments.
- `GetLightEnable` returns 128 for an enabled light.
- `MultiplyTransform` multiplies the new matrix by the current matrix.

### What was not verified

The user said not to test. The four D3D9 files compile with `-Werror`.
Nothing has run them yet. Metal does not work in the agent sandbox.

### Remaining imports: 0

- COM classes that are not yet supported: XAudio2 and DirectInput8.

## 2026-11-12: XAudio2 and DirectInput 8

Time: 2026-11-12T04:17:27+0000.

### What changed

- `runtime/src/rt_xaudio2.c` (new): XAudio2 2.3 on a CoreAudio default
  output unit. The engine uses CLSID {4C5E637A-16C7-4DE3-9C46-5ED22181962D}
  and IID {8BCF1F58-9FE7-4583-8AC6-E2ADC465C8BB}. Both GUIDs are in the
  engine binary.
- `runtime/src/rt_dinput.c` (new) and `runtime/include/rt_dinput.h` (new):
  IDirectInput8A and IDirectInputDevice8A for the system mouse and the
  system keyboard.
- `runtime/src/rt_wmi.c`: `IWbemServices::CreateInstanceEnum` for the class
  `Win32_PNPEntity`, and the enumerator methods `Reset` and `Next`. The
  engine runs the XInput device check of the DirectX SDK for each
  DirectInput device. This check enumerates `Win32_PNPEntity`.
- `runtime/src/rt_ole32.c`: the `CoCreateInstance` class table has the
  DirectInput8 class and the XAudio2 2.3 class.
- `runtime/src/rt_user32.c`, `runtime/include/rt_user32.h`: each key event
  goes to DINPUT8 with its DirectInput key code. A focus change goes to
  DINPUT8. The new function `rt_user32_dik_codes` gives the key codes that
  the Mac keyboard can make.
- `runtime/src/rt_window.m`: mouse motion (the event delta), all mouse
  buttons and the wheel go to DINPUT8. The window messages are the same as
  before.
- `runtime/com.spec`: the vtables `EnumWbemClassObject`, `DirectInput8A`,
  `DirectInputDevice8A`, `XAudio2`, `XAudio2SourceVoice` and
  `XAudio2MasteringVoice`. `CreateInstanceEnum` is now implemented. The
  generated files `runtime/llasm/com_thunks.llasm`,
  `runtime/include/rt_com_thunks.h` and `runtime/src/rt_com_vtables.c` were
  made again with `tools/gen_com_thunks.py`.

### How the engine uses the two APIs

The engine calls were read from the translated listing (static
inspection only).

- XAudio2: `Initialize`, `GetDeviceCount` (the engine uses at most 1
  device), `GetDeviceDetails`, `CreateMasteringVoice` (default channels and
  rate), `CreateSourceVoice` (16-bit PCM in the device format, maximum
  frequency ratio 1.0, a voice callback), `SubmitSourceBuffer`, `Start`,
  `Stop`, `DestroyVoice` and `Release`. The callback `OnBufferEnd` tells the
  engine mixer to fill the next buffer.
- DirectInput: `Initialize(0x0800)`, `EnumDevices(DI8DEVCLASS_ALL,
  DIEDFL_ATTACHEDONLY)`, `CreateDevice`, `EnumObjects`, `SetDataFormat` (one
  DWORD for each object), `SetCooperativeLevel` (non-exclusive and
  foreground first), `Acquire`, `Poll`, `GetDeviceState`, `Unacquire` and
  `Release`. The engine reads a button as `byte & 0x80` and a relative axis
  as a LONG.

### Behavior

- XAudio2 has one device: the default CoreAudio output device. A
  processing thread with a guest thread state runs 10 ms passes. The
  output ring holds 4 passes. When the ring is empty, the output plays
  silence, and the runtime writes the number of silent frames to the log.
- The voice callbacks run without the XAudio2 lock. They come after the
  mixing of a pass. A buffer that a callback submits plays from the next
  pass.
- Source data can be 8, 16, 24 or 32-bit PCM or 32-bit float. The
  resampler is a 4-point Hermite interpolator.
- DirectInput has two devices: the mouse (3 axes, 8 buttons) and the
  keyboard (each key that the Mac keyboard can make). Game controllers stay
  on XInput.
- A key or a button that is pressed and released between two
  `GetDeviceState` calls is reported down by the next call. Thus a short
  press is not lost. Caps Lock needs this, because macOS reports one event
  for each Caps Lock change.
- When the game window stops being the key window, all keys and buttons
  are released. Foreground devices become unacquired.
  `GetDeviceState` then returns `DIERR_INPUTLOST` one time.
- The `Win32_PNPEntity` enumeration has no instances. The emulated system
  has no Plug and Play device database. The SetupDi device sets are also
  empty.
- These conditions cause `rt_fatal`: submix voices, effects, filters,
  volumes, matrices, frequency ratios, compressed formats, deferred
  operation sets, voice flags, a source channel count that is not the
  mastering channel count, exclusive DirectInput cooperative levels,
  DirectInput properties, buffered input and force feedback.

### UNCONFIRMED items

- The IXAudio2 2.3 method order and argument counts. The engine calls agree
  with them.
- The XAUDIO2_BUFFER validation rules. The order of `OnBufferEnd` and
  `OnStreamEnd`. The value of `BytesRequired`.
- The `OutputFormat` that Windows reports. The runtime reports 16-bit PCM,
  because the engine changes only the sample size.
- The DirectInput object names, the mouse object instance numbers, the
  device subtypes, and the enumeration order.
- The mouse motion unit. The runtime uses the macOS event delta, which
  includes the system acceleration.
- `IID_IEnumWbemClassObject`. The engine does not query it.

### What was not verified

The user said not to test. All runtime files compile with `-Werror`. The
COM thunks translate with `tools/build_llasm_rt.sh com_thunks`. Each COM
function that the vtables name has a definition. Nothing has run this
code yet.

## 2026-11-12: Process start, application build and converter

Time of this entry: 2026-11-12T04:46:41+0000.

### Structure

- The application has two parts:
  - The loader (`runtime/loader/rt_loader.m`) is the executable
    `Contents/MacOS/Prototype`. It links the frameworks and FFmpeg.
  - The game bundle (MH_BUNDLE) holds the runtime and the translated code.
    The loader opens it with `dlopen` and calls `rt_game_main`
    (`runtime/include/rt_game.h`) on the main thread.
- The runtime and the translated code must be in one Mach-O image. The
  guest window must hold the translated code, the import thunks and the
  COM vtables, and the linker sets their distances. `rt_mem.c` now gets
  the range of the image that holds the runtime (`dladdr`), not the range
  of image 0.
- The bundle is linked with `-undefined dynamic_lookup`. dyld finds the
  system, framework and FFmpeg symbols in the images that the loader
  loaded. The development bundle has 335 undefined symbols. None of them
  is a runtime or translated-code symbol.

### Game files

- The loader finds the game directory in this order:
  1. `--game-dir`
  2. "Put game files here" beside the application
  3. the folder that holds the application
  4. the folder that the player selected before (NSUserDefaults)
  5. an open panel
- The Option key at start opens the panel.
- If a folder is not correct, the message tells the player the cause: an
  installer folder, a parent folder or no game files.
- The loader checks the SHA-256 values of `prototypef.exe` and
  `prototypeenginef.dll` (the Steam version).
- `rt_modules_init(game_dir)` makes the export table of the engine from
  the export directory of `prototypeenginef.dll`. It uses the SRW symbol
  name rule of `tools/importspec.py` and `dlsym(RTLD_SELF)`. Thus no
  generated C file is necessary, and the builds do not compile
  `work/*/exports.c` now. `tools/gen_exports.py` is not used by the
  builds now.

### Converter (release build)

- `runtime/loader/rt_convert.c` runs the translation on the computer of
  the player. It uses the tools in `Contents/Resources/Recompiler`: SRW,
  llasm, opt, llvm-split, clang and ld64.lld.
- The SHA-256 values of the SRW output files must agree with the values
  in `manifest.txt`. The values come from the tested translation in
  `work/`. Thus the converter does not need Python: the analysis files
  (`*.sci`, `SR.cfg`, `relocations.csv`, `extern.llinc`) are the
  converged files of the development translation.
- clang runs as `clang -cc1` with fixed options. The clang driver runs
  `xcrun` to find an SDK, and the computer of a player has no SDK.
- A part that writes compiler messages stops the conversion. The
  development build compiled all parts with no messages.
- The loader copies the Recompiler directory (`copyfile` with clone) and
  removes `com.apple.quarantine` from the copy. Then it runs the
  converter on a second thread and shows a progress window.
- The cache folder is
  `~/Library/Application Support/Prototype Mac Port/GameCode/<id>`.
  `<id>` comes from the SHA-256 of the loader and of `manifest.txt`. The
  loader removes the folders of other releases. A closed progress window
  stops the conversion.
- The conversion needs 8 GB of free disk space. The development
  translation used 5.8 GiB.
- The number of parallel compiler processes is the smaller of the CPU
  count, the memory in units of 2 GiB, and 16.

### Build scripts

- `tools/build_app.sh` (development build) links the bundle from the
  `work/` objects into `Contents/Resources/Prototype.bundle`. It compiles
  the loader with `RT_LOADER_DEV=1`. Result: it built and signed
  `build/app/Prototype.app` with no warnings, except the FFmpeg warning
  below.
- `tools/package_release.sh` makes the release package:
  - the runtime archive `libprototype_rt.a`,
  - the converter tools with their libraries (with `@rpath`, ad hoc
    signed),
  - the analysis files, the manifest and the licenses,
  - a README and the zip file.
- `tools/package_release.sh` stops for:
  - a GPL or nonfree FFmpeg,
  - a file with game code,
  - a symbol of the translated code,
  - a path of the build computer.
- `rt_x86_hooks.c` gives the SR support hooks: guest memory access
  through the guest window, and `rt_fatal` for interrupt instructions.
- Casts were added in `llasm_float.c` and `llasm_stos.c` (SR support) for
  `-Wconversion`.
- `llasm_inout.c` is not linked, because the game has no `in` or `out`
  instruction.

### Blockers

- The release package cannot be made on this computer now.
  `tools/package_release.sh` stops with "libavcodec.dylib is a GPL or
  nonfree build". The Homebrew FFmpeg is a GPL build: it uses x264, x265
  and OpenSSL. An LGPL FFmpeg build must be made from the FFmpeg source,
  then given in `FFMPEG_PREFIX`.
- The Homebrew FFmpeg, LLVM and lld libraries have the minimum macOS
  version 27.0 to 27.2. The development application loads only on macOS
  27 or later. The release script sets `LSMinimumSystemVersion` to the
  highest minimum version of the files that it puts in the package.

### UNCONFIRMED items

- `-undefined dynamic_lookup` for the Objective-C classes of the runtime
  (the superclasses in AppKit) and for `libclang_rt.osx.a` symbols.
- The behavior of `copyfile` with `COPYFILE_CLONE | COPYFILE_RECURSIVE`
  for a directory.
- The progress window without `finishLaunching`. The loader does not call
  `finishLaunching`, because `rt_cocoa_init` calls it later.
- The memory that one compiler process uses (2 GiB is an estimate).
- That the `clang -cc1` options give the same code as the driver options
  of `tools/compile_engine.sh`. The options are the options that the
  driver gave, without the SDK version, the host CPU tuning and the
  directories.

### What was not verified

The user said not to test. All changed C and Objective-C files compile
with `-Werror`. `tools/build_app.sh` built the development application.
No code of the loader, the converter, `rt_game_main` or the new export
table has run.

## 2026-11-12: First runs of the game (05:42)

The user asked why the game does not run. The development application ran
outside the agent sandbox. Each run stopped at one defect. The list gives
each defect and its correction, in order.

1. `rt_cocoa_init` stopped with "the application could not become a
   regular application". An application bundle already has the regular
   activation policy, and `setActivationPolicy:` returns NO for no change.
   `rt_window.m` now sets the policy only when it is different.
2. `EngineInitialize` read guest address 0. The C++ static constructor
   tables held zeros, because no code called `ptr_initialize_pointers`.
   `rt_main.m` now calls it after `rt_mem_init` (declared in
   `rt_module.h`).
3. `x87_fstp_double` gave SIGBUS. The SR helpers return guest addresses of
   `_cpu` fields, and `_cpu` was host memory. `rt_core.c` now puts each
   `_cpu` in guest memory.
4. `MultiByteToWideChar` refused code page 3 (`CP_THREAD_ACP`).
   `rt_kernel_misc.c` now treats it as code page 1252.
5. The shader length search halved the length and missed the readable
   end. `rt_d3d9_res.m` now uses a binary search.
6. `CreateVolumeTexture` was not implemented. `rt_d3d9_res.m`,
   `rt_d3d9.m`, `rt_d3d9_draw.m`, `rt_d3d9_int.h` and `com.spec` now have
   `IDirect3DVolumeTexture9` (Metal 3D textures, `LockBox`, `UnlockBox`,
   volume samplers, a 1x1x1 texture for an empty volume sampler).
7. `lock cmpxchg8b` at a 4-byte aligned address across a 16-byte boundary
   stopped the process. ARM64 atomics cannot cross 16 bytes.
   `llasm_x86ext.c` now does such locked instructions under a lock that
   the crossed boundary selects. Limit: they are not atomic against a
   non-crossing locked access or a plain store on the same bytes.
8. `dynamic_cast` found no RTTI data. `rt_va_is_committed` did not know
   the program image. `rt_mem.c` now counts the image pages as committed.
9. DINPUT8 refused the exclusive mouse. `rt_dinput.c` now accepts it. While
   an exclusive mouse is acquired, `rt_cocoa_mouse_capture` (`rt_window.m`)
   hides the cursor, moves it to the window centre and separates it from
   the mouse.
10. libdispatch stopped the process when XAudio2 released its semaphore.
    The semaphore count was below its creation value. `rt_xaudio2.c` now
    makes the semaphore with 0 and signals it `RING_PASSES` times.
11. A job thread (`loc_105E9510`) read a null pointer. The job data is the
    global `loc_11276380`, which is 16-byte aligned in the DLL. The code
    rounds the address down to 16 bytes. llasm placed the data segments
    with `align 4`, so the global was at an address 4 modulo 16.
    Correction: llasm (`llasm.d`) now gives each data segment `align 4096`.
    SRW (`SR_output.c`) writes zero bytes before the first item of a data
    section when the item is not at the section start. Each data address
    now has its original value modulo 4096. Both modules must be
    translated again.

A test with one reported processor gave the same failure as item 11. Thus
item 11 is not a memory-order defect.

### FFmpeg

`tools/build_ffmpeg_lgpl.sh` builds FFmpeg 9.0.2 from the source tarball
(ffmpeg.org) as LGPL shared libraries with only the Bink decoders. The
output is `build/ffmpeg-lgpl`. `tools/build_app.sh` now uses this build.
It is the `FFMPEG_PREFIX` for `tools/package_release.sh`.

## 2026-11-12: Game reaches the 3D world, headless runs (06:30)

The retranslated modules (page-aligned data) got past the job thread
defect. These corrections followed, in order:

1. `rt_object.c`, `rt_object.h`: named events, mutexes and semaphores in
   one process. A second `CreateXxxA` with the same name gives a handle of
   the same object and `ERROR_ALREADY_EXISTS`.
2. `rt_core.c`, `rt_thread.h`, `rt_thread.c`: `rt_call_thread_start`
   accepts a thread start routine with the cdecl convention. Windows ends
   the thread after the return, so the stack difference has no effect.
3. `rt_rpcrt4.c`, `imports.spec`: `UuidCreateSequential` (for
   `GetProcAddress`), from `uuid_generate_time`.
4. `rt_sync.c`: `DeleteCriticalSection` of an all-zero critical section
   does nothing, as on Windows.
5. `rt_d3d9_draw.m`: the vertex descriptor has a buffer layout only for a
   stream that an attribute reads. Metal stopped the process for an
   unused layout.
6. `rt_d3d9_draw.m`: a texture whose type is not the declared sampler type
   gives the empty texture of the sampler type (logged one time).
   UNCONFIRMED: the value that Windows drivers give.

New development aids:

- `RT_HEADLESS=1` (`rt_window.m`, `rt_window.h`): no window, no Dock icon,
  no activation, no cursor change. A shown guest window is reported as
  active. Present does not show frames.
- `RT_INPUT_SCRIPT` (`rt_window.m`): timed key presses and holds, for
  example `8-90/3:return,110-140:w`.
- `RT_PERF_LOG=1` (`rt_d3d9_draw.m`): every 5 seconds, the frame rate, the
  longest frame and the process CPU time.

Headless run (180 s, Return every 3 s from 8 s to 90 s): the menus ran at
about 450 frames per second with 2 to 3 cores. At 40 s the game loaded the
world (memory 265 MB to 700 MB). After that it ran at 25 to 32 frames per
second, longest frame about 40 to 150 ms, with about 7 cores busy. The
process did not stop until the agent ended it at 180 s.

Not verified: without screenshots, the agent cannot see whether the 3D
scene was play or an in-engine cutscene. The high CPU use at about 30
frames per second is not analysed.

## 2026-11-12: CPU profile and wait wakeups (06:20)

`sample` on a headless run in the 3D world showed about 9,900 samples in
`__psynch_mutexwait` under `wait_objects`. All waitable objects used one
lock and one condition variable, and each `SetEvent` and
`ReleaseSemaphore` woke every waiting thread. The job threads then
competed for the one lock.

Correction (`rt_object.c`, `rt_object.h`, `rt_file.c`, `rt_winmm.c`):

- Each thread in `wait_objects` has its own condition variable and one
  node on the wait list of each object that it waits for.
- `rt_obj_signal(o, n)` wakes up to n waiters of o in arrival order. An
  auto-reset event wakes 1, a manual-reset event wakes all, and
  `ReleaseSemaphore(n)` wakes n.
- A waiter that was woken but did not take an object passes the wakeup to
  the next waiter of each object that is still available.
- Thread exit and `ResumeThread` still wake all waiters (`rt_obj_broadcast`).

Result, same headless input script, 3D world:

| Measurement | Before | After |
|---|---|---|
| Frame rate | 25 to 32 fps | 55 to 64 fps |
| Process CPU | about 700% of one core | about 210% to 240% of one core |
| Samples in `__psynch_mutexwait` (10 s) | 9,888 | 380 |

The largest remaining CPU users are the SSE helper calls
(`sse_shufps_xxi`, `sse_movss_xm`, `sse_mulps_xx`). UNCONFIRMED: whether
about 60 fps is a frame limit of the game.

## 2026-11-12: SSE helpers inlined (07:21)

The translated code called each SSE helper of `llasm_x86ext.c` as an
external function. Each call saved and loaded the guest registers, and
the register numbers and shuffle masks were run-time values in the
helper. The helpers used about 2,800 top-of-stack samples in 10 s.

Correction (`tools/compile_engine.sh`):

- The script compiles `llasm_x86ext.c` to bitcode and extracts the
  `sse_*` functions with `llvm-extract`.
- The script stops with an error if the extracted helpers use global
  data, because each part gets its own copy.
- The script removes the `target-cpu`, `target-features` and `tune-cpu`
  attributes from the helpers. Without this, the inliner refuses each
  call with "conflicting target features", because the translated
  functions have no such attributes. clang compiles both for the same
  default CPU.
- `llvm-link --only-needed --internalize` copies the called helpers into
  each part before clang compiles the part with `-O2`.

Result: 0 calls to `sse_*` remain in the engine objects (3,231 in part 82
before). The engine objects decreased from 112,628,952 to 109,928,528
bytes. A headless run with the same input script, in the 3D world:

| Measurement | Before | After |
|---|---|---|
| Frame rate | 55 to 64 fps | 61 to 65 fps |
| Process CPU | about 210% to 240% of one core | about 165% to 190% of one core |
| SSE helper top-of-stack samples (10 s) | about 2,800 | 0 |

Limits:

- The release converter (`runtime/loader/rt_convert.c`) does not do this
  step. It does not ship `llvm-link` or `llvm-extract`.
- One 5 s period fell to 30 to 34 fps with a 625 ms frame. The cause is
  not analysed.
- UNCONFIRMED: whether about 60 fps is a frame limit of the game.

## 2026-11-12: SSE inlining in the release converter (07:50)

The release converter now does the same SSE helper step as the
development build.

Changes:

- `tools/build_sse_inline.sh` (new) makes the SSE helper bitcode. It has
  the steps of the 07:21 section. It also replaces the source file name
  with `llasm_x86ext.c`, so that the file holds no build computer path.
  `tools/compile_engine.sh` and `tools/package_release.sh` both use it.
- `tools/package_release.sh` copies `llvm-link` to `Recompiler/bin` and
  writes `Recompiler/include/llasm-support/sse_inline.bc`. The file check
  permits this one bitcode file, and the symbol check includes it.
- `runtime/loader/rt_convert.c`: `compile_parts` is now `run_parts`. For
  the engine, a first pass runs `llvm-link --only-needed --internalize`
  on each part in parallel and replaces the part with the result. The
  second pass compiles the parts with clang as before. The launcher has
  no link pass, as in the development build.

Packaging had not run before. Three errors that were already present
stopped it. Each one is now corrected:

1. The symbol check of `package_release.sh` matched the import thunks of
   `runtime/llasm/thunks.llasm` (for example `_X__Q_Q2_AYAPAXI_AZ_asm2c`).
   These are runtime code. The check now ignores `_X_..._asm2c` names.
   The translated objects define no `_asm2c` symbol (checked: 0 in all
   engine parts and the launcher).
2. The FFmpeg libraries held the build directory in their configure
   string. `tools/build_ffmpeg_lgpl.sh` now configures with the prefix
   `/ffmpeg-lgpl`, installs with `DESTDIR` and moves the result to
   `build/ffmpeg-lgpl`.
3. `load_manifest` in `rt_convert.c` read the 64-digit `content` value
   into a 31-character field and rejected the line. The content line is
   now read with the 64-character field.

Verification:

- `FFMPEG_PREFIX=build/ffmpeg-lgpl sh tools/package_release.sh` completed
  with all package checks.
- A command-line program in the scratch directory called `rt_convert_run`
  with the packaged Recompiler directory and the game directory (no
  window). The conversion completed in 3 min 9 s.
- The converted bundle and the development bundle both have 0 `bl`
  instructions to `_sse_*` functions. The same search finds the x87
  helper calls (for example 1,873 to `x87_fc*`), so the search works.
- The game was not started from the release app: the release loader
  always opens a progress window.

Remaining items:

- The package reports a minimum macOS of 27.3. The Homebrew LLVM tools
  were built for this macOS version. This is not changed.
- The FFmpeg build writes `ld: warning: -single_module is obsolete`. This
  flag comes from the FFmpeg configure script for Darwin. The linker
  ignores it.
- `build/convtest` holds the test conversion (translated game code). Do
  not distribute it.

## 2026-11-12: Dynamic buffer renames without a GPU wait (11:17)

A new CPU profile (`sample`, 10 s, in the 3D world, headless) showed that
`IDirect3DVertexBuffer9::Lock` and `IDirect3DIndexBuffer9::Lock` used
1,645 samples in `d3d_wait_serial`. This is approximately 16% of the
time of the render thread. All of the waits came from the
`D3DLOCK_DISCARD` path in `rt_d3d9_res.m`.

Cause: a discard of a buffer that the GPU can still read gives the
buffer new storage and keeps the old storage until its command buffer
completes. The limit was 8 old storage blocks for each buffer. The
engine discards some buffers more than 8 times in one frame. The 9th
discard then waited for the oldest block. That block was in the current
command buffer, so the wait committed the command buffer and waited for
the GPU to complete it. The CPU and the GPU then worked one after the
other.

Change in `runtime/src/rt_d3d9_res.m`:

- The limit for the old storage of a buffer is now 16 MiB, with a
  minimum of 8 blocks and a maximum of 512 blocks. A small buffer can
  thus be renamed many times in one frame. The memory of each buffer
  stays bounded.
- A lock with `D3DLOCK_DISCARD` now examines only the oldest block. The
  list is in serial order, because each rename adds the current
  `last_use` value and `last_use` is always set to the current serial.
- Textures keep the old limit of 8 blocks. The profile did not show
  texture waits.

Result (same input script, same 3D scene, `RT_PERF_LOG`):

| Measurement | Before | After |
| --- | --- | --- |
| Frames per second, steady | 62 to 68 | 95 to 97 |
| Longest frame in 5 s, steady | 19 ms to 22 ms | 11.7 ms to 12.1 ms |
| CPU, in percent of one core | 175 to 185 | 227 to 237 |
| `d3d_wait_serial` samples in 10 s | 1,645 | 0 |
| Physical footprint at sample | 971 MB | 999 MB |

The CPU use is higher because the game makes approximately 45% more
frames. The pause at approximately 83 s (one long frame) occurs in both
runs. It is not analysed. The game picture was not examined (no
screenshots), so the visible result of the change is UNCONFIRMED.

Next costs in the new profile: `do_draw` (1,509 samples), of which the
pipeline cache lookup with `NSData` keys (`objectForKeyedSubscript:`,
330 samples, and `isEqualToData:`) is the largest runtime part.

## 2026-11-12: State caches without NSData keys (12:25)

The profile after the buffer change showed that the lookups of the
pipeline, depth-stencil and sampler caches used approximately 670
samples in 10 s on the render thread. Each draw made `NSData` keys
(`dataWithBytes:length:`), and `NSMutableDictionary` hashed and compared
them (`CFHashBytes`, `isEqualToData:`).

Changes:

- `runtime/src/rt_d3d9_int.h`: new class `D3DStateCache`. It is a hash
  table with open addressing and fixed-size byte keys. The device fields
  `pipelines`, `ds_states` and `samplers` now use this class. New
  function `d3d_init_caches`.
- `runtime/src/rt_d3d9_draw.m`: the cache code (`state_cache_new`,
  `state_cache_get`, `state_cache_put`, `key_hash`) and
  `d3d_init_caches`. A lookup makes no allocation. The entry limits are
  the same as before (4,096 pipelines, 1,024 depth-stencil states, 1,024
  samplers). When a cache is full, it is emptied and the log message is
  the same as before. The depth-stencil keys are now 12 words. A clear
  key (2 words) is filled with zeros. Its first word (`0xC1EA....`) is
  different from the first word of a draw key (0 or 1), so the two kinds
  of key cannot be equal.
- `runtime/src/rt_d3d9.m`: the device creation calls `d3d_init_caches`
  instead of making three dictionaries.

Result (same input script, headless, `RT_PERF_LOG`):

| Measurement | Before | After |
| --- | --- | --- |
| Frames per second, steady | 95 to 97 | 108 to 116 |
| Longest frame in 5 s, steady | 11.7 ms to 12.1 ms | 10.2 ms to 11.1 ms |
| Cache lookup samples in 10 s | approximately 670 | 67 |

The 10 s sample of the second run included the pause of one long frame
(approximately 600 ms), so its total sample counts are lower. The pause
occurs in all runs and is not analysed. The game picture was not
examined (no screenshots), so the visible result is UNCONFIRMED. No
"cache is full" message occurred.

## 2026-11-12: Shader constants sent by used range (12:31)

The profile after the cache change showed `bind_constants` at 517
samples in 10 s. Each draw copied and sent all 256 vertex shader float
constants (4,096 bytes) and all 224 pixel shader float constants (3,584
bytes) with `setVertexBytes` and `setFragmentBytes`.

Changes:

- `runtime/include/rt_d3d9_shader.h`: new field `nconst_f` in
  `d3d_shader`. It is the number of float constant registers (from c0)
  that the shader can read.
- `runtime/src/rt_d3d9_shader.c`:
  - `d3d_shader_parse` sets `nconst_f` from the source parameters of
    all instructions (not `dcl` and the `def` instructions). A source
    with relative addressing sets the full range (256 or 224).
  - The MSL generator stops with an error if a shader reads a constant
    register at or above `nconst_f`, or uses relative addressing without
    the full range. A mistake in the scan thus stops the shader creation.
    It cannot give an incorrect picture.
- `runtime/src/rt_d3d9_draw.m`: `bind_constants` copies and sends only
  `nconst_f` float registers (minimum 1, because the shader declares the
  buffer). The integer and boolean constants are not changed.

Result (same input script, headless, `RT_PERF_LOG`):

| Measurement | Before | After |
| --- | --- | --- |
| Frames per second, steady | 108 to 111 | 130 to 139 |
| Longest frame in 5 s, steady | 10.6 ms to 11.0 ms | 8.5 ms to 9.1 ms |
| `bind_constants` samples in 10 s | 517 | 119 |
| CPU, in percent of one core | 247 to 251 | 272 to 281 |

The run gave no shader error, so the generator check passed for all
shaders that the game made in the run. The game picture was not
examined (no screenshots), so the visible result is UNCONFIRMED. The
pause of one long frame (approximately 580 ms) occurs again. It is not
analysed.

## 2026-11-12: COM object lookup without a global lock (15:09)

The profile after the constant change showed that the render thread was
busy for 95 percent of the time. Seven engine worker threads lock and
unlock vertex buffers at the same time as the render thread draws. Each
COM method call found its object with `rt_com_this`, which took the
global COM lock and then the virtual memory lock (in
`rt_va_is_committed`). `rt_com_addref` and `rt_com_release` also took
the COM lock. The threads waited for each other on these two locks.

Changes:

- `runtime/src/rt_ole32.c`:
  - The object table has a fixed size (`MAX_OBJECTS` entries), so that
    it does not move while a thread reads it.
  - An entry keeps its `rt_com_object` after the object is destroyed.
    The next object with the same id uses the same `rt_com_object`
    again. The runtime thus never frees an `rt_com_object`, and a lookup
    with a stale guest pointer reads valid host memory.
  - `refs` is changed with atomic compare-and-exchange operations.
    `refs` 0 means that the entry has no live object.
    `rt_com_create` stores `refs` 1 with release order after it sets
    the other fields. The lookup loads `refs` with acquire order before
    it reads them.
  - `rt_com_this`, `rt_com_lookup`, the IUnknown methods,
    `rt_com_addref` and `rt_com_release` do not take the COM lock. The
    COM lock now protects only the id allocation, the free id list and
    the guest vtable copies.
  - `rt_com_release` puts the id on the free list after the object is
    fully destroyed. Before, the id went on the list before the class
    destroy function ran.
  - `rt_com_addref` stops the process if the object has no references
    (AddRef of a destroyed object).
- `runtime/include/rt_com.h`: the comment of `refs` tells that it is
  atomic.
- `runtime/src/rt_mem.c`: the committed-page bitmap is changed with
  atomic operations (with `va_lock` held), and `rt_va_is_committed`
  reads it with atomic loads and without `va_lock`. The image range
  does not change after start-up.

Result (same input script, headless, `RT_PERF_LOG`; the second run had
no sampling):

| Measurement | Before | After |
| --- | --- | --- |
| Frames per second, steady | 130 to 139 | 135 to 141 |
| Longest frame in 5 s, steady | 8.5 ms to 9.1 ms | 8.5 ms to 9.0 ms |
| `rt_com_this` samples in 10 s | 268 | 57 |
| `rt_com_addref` samples in 10 s | 91 | 9 |
| `rt_va_is_committed` samples in 10 s | 84 | 26 |
| CPU, in percent of one core | 272 to 281 | 254 to 268 |

The first run after the change had three frames of 14 ms to 24 ms in
the steady part. The second run did not have them. The device lock
(`d3d_lock`) is now the lock that the worker threads wait for (376
samples in 10 s, before 315). The render thread waits for it for
approximately 1 percent of its time. The game picture was not examined
(no screenshots), so the visible result is UNCONFIRMED. The pause of
one long frame (approximately 600 ms) occurs again.

## 2026-11-12: Draws set only the encoder state that changed (15:30)

The render thread is busy for approximately 95 percent of the time.
`DrawIndexedPrimitive` used 988 samples of approximately 3,100 in 10 s
on that thread. Each draw set all of the Metal encoder state again: the
pipeline, the depth-stencil state, the stencil reference, the blend
color, the cull mode, the winding, the fill mode, the depth bias, the
viewport, the scissor rectangle, the misc constants, the shader
constants, all textures, all samplers and all vertex buffers. The
`dirty` field of `D3DDevice` was set, but no code read it.

Changes:

- `runtime/src/rt_d3d9_int.h`: the unused `dirty` field and the unused
  `D3DDIRTY_*` constants are removed. New fields record the encoder
  state that the draws set (`es_valid`, `es_pipe`, `es_ds`, the raster
  values, `es_vp`, `es_sr`, the misc constants, the shader id and the
  constant version of the bound constants, `es_tex`, `es_smp`, `es_vb`,
  `es_vb_off`, `es_null_vb`). The object fields are strong references,
  so that the address of a recorded object cannot become the address of
  a new object. New counters `vs_const_ver` and `ps_const_ver`.
- `runtime/src/rt_d3d9.m`: `SetVertexShaderConstantF/I/B` and
  `SetPixelShaderConstantF/I/B` increment the constant version of their
  shader stage. A device reset increments both versions.
- `runtime/src/rt_d3d9_draw.m`:
  - New function `enc_state_reset`. `begin_pass` calls it for each new
    encoder. `clear_quads` calls it after the clear draws, because they
    set the pipeline, the raster state and vertex buffer 0.
  - `do_draw` sets a state only if it is different from the recorded
    state. The shader constants are sent again only if the shader id or
    the constant version changed. A vertex buffer with a new offset only
    gets `setVertexBufferOffset`.
  - `bind_texture` sets a texture or a sampler only if it changed.
  - The texture and buffer `last_use` serials are still set on each
    draw.

A second change was tried and removed: the buffer copies of
`IDirect3DVertexBuffer9::Lock` and `Unlock` ran without the device lock.
Two runs with that change had stalls (frames of 200 ms to 5.7 s, with
CPU at approximately 50 percent of one core). Two runs without it had
no stalls. The cause is not known.

Result (same input script, headless, `RT_PERF_LOG`, two runs without
sampling):

| Measurement | Before | After |
| --- | --- | --- |
| Frames per second, steady | 135 to 141 | 137 to 152 |
| `DrawIndexedPrimitive` samples in 10 s | 988 | 814 |
| `bind_constants` samples in 10 s | 115 | 75 |
| `bind_texture` samples in 10 s | 131 | 108 |
| AGX `drawIndexedPrimitives` samples in 10 s | 239 | 193 |

The longest frame in a 5 s period is 8.4 ms to 18.6 ms, with one frame
of 159 ms in one run. Runs before the COM lock change had 8.5 ms to
9.1 ms. Of the two runs after the COM lock change (without this
change), one had frames of 14 ms to 24 ms and one did not. The cause of
these longer frames is not known. The game picture was not examined (no
screenshots), so the visible result is UNCONFIRMED. The pause of one
long frame (approximately 600 ms) occurs again.

## 2026-11-12: First windowed run with the Metal HUD (15:40)

The user allowed screenshots and asked for a run with the Metal
performance HUD (`MTL_HUD_ENABLED=1`). The HUD draws into presented
frames, and headless mode presents no frames, so the run used a window.

Changes:

- `runtime/src/rt_window.m`: the input script (`RT_INPUT_SCRIPT`) also
  works when a window is shown. It sends its keys to the guest window of
  the key native window (new variable `key_hwnd`, set in
  `windowDidBecomeKey` and cleared in `windowDidResignKey`). Before, the
  script sent keys only in headless mode.
- `runtime/src/rt_sync.c`: the fatal message of `DeleteCriticalSection`
  for a critical section that is in use now gives the guest address, the
  owner thread, the recursion count, the calling thread and the return
  address.

Build: Homebrew LLVM was not available (`/opt/homebrew/Cellar` did not
exist at 15:35). The runtime was compiled with Apple clang 21.0.0 from a
copy of `tools/build_app.sh` in the scratch directory. The llasm runtime
objects of 15:26 were used again; their input files did not change.
`tools/build_app.sh` is not changed. The build had no warnings.

Results:

- First run: the game stopped after approximately 25 s, in the menus,
  with `DeleteCriticalSection of a critical section that is in use (16)`.
  The guest main thread deleted a critical section that another thread
  held. This did not occur in approximately 15 headless runs or in the
  second windowed run. The cause is not known. The new message gives the
  data for the next occurrence.
- Second run: the game got to the first mission. Screenshots of the
  window show correct menus and a correct 3D scene with the HUD of the
  game. This is the first visual check of the rendering.
- Metal HUD in the game, 1800 x 1125, presentation "Composited":
  117 fps, GPU time 6.48 ms per frame, frame interval 8.54 ms, Metal
  memory 421 MB, application memory 1.05 GB. With a window, the frame
  rate is limited by the display (120 Hz). The perf log gave 117 fps to
  120 fps with a longest frame of 8.6 ms in the last 10 s.

## 2026-11-12: DeleteCriticalSection of a section that is in use (16:46)

The first windowed run (15:40) stopped with
`DeleteCriticalSection of a critical section that is in use (16)`. The
guest main thread deleted a critical section, and
`pthread_mutex_destroy` returned `EBUSY`. On macOS this occurs when the
mutex is locked or when a thread waits for it.

Investigation:

- Four more windowed runs (45 s each) did not stop.
- A windowed run that lost the focus to Finder at 18 s did not stop.
- Headless runs that quit from the menus (25 s) and from the game
  (70 s, 80 s) exited with status 0. For these runs, the input script
  has a new step, `quit`, which does what the Quit menu item does.
- The trigger in the game was not found. The game state at the stop
  (menus, approximately 25 s) is the only data.

Cause in the runtime: Windows `DeleteCriticalSection` does not fail when
a thread owns the critical section or waits for it. Microsoft documents
only that the state of the waiting threads is undefined. The runtime
stopped the process in this case, so it did not behave as Windows does.

Changes:

- `runtime/src/rt_sync.c`:
  - `cs_entry` has a new atomic field `deleted`.
  - If `pthread_mutex_destroy` returns `EBUSY`,
    `DeleteCriticalSection` marks the entry as deleted, tries the
    destroy again once (after a full fence), and logs the guest
    address, the owner thread, the recursion count, the calling thread
    and the return address. The function then returns, as on Windows.
  - `LeaveCriticalSection` (after a full fence) checks the mark. If it
    is set, `cs_reap` destroys the mutex when it is free, frees the
    index, and sets `LockSemaphore` to 0 if the guest did not initialize
    the memory again.
  - A second `DeleteCriticalSection` of a deleted section stops the
    process with a message.
  - The common code that frees an entry is in `cs_free_entry`.
  - An entry whose mutex never becomes free (the owner never leaves)
    stays in use. The table has a limit of 65,536 entries.
- `runtime/src/rt_window.m`: new input script step `quit`.

Verification: the build (Apple clang, see 15:40) had no warnings. Two
headless runs went into the game and quit at 80 s with exit status 0.
The new log line did not occur, so the new path did not run. The new
path is thus not verified at run time.

Run-time result (windowed run with the Metal HUD, same day, after this
change): at approximately 18 s, in the menus, the new path ran:
`DeleteCriticalSection of the critical section at 0x0ed2065c while it
is in use (owner thread 256, recursion 1, calling thread 256, return
address 0x01409c9c)`. The guest main thread deleted a critical section
that it owned itself (one Enter without a Leave). Windows allows this.
The game continued to run, and later reached the pause menu.

## 2026-11-12: Depth-stencil surface with a different sample count (17:19)

The user changed the graphics settings in the game (antialiasing on),
and the game stopped with
`D3D9: depth-stencil surface and render target sample counts differ`.
The game saved the setting in `profile.bin`, so the next start stopped
after 2 s. The new message data: render target 2560 x 1600 with 1 sample,
depth-stencil surface 2560 x 1600 with 2 samples, `D3DRS_ZENABLE` 1,
`D3DRS_ZWRITEENABLE` 0, `D3DRS_STENCILENABLE` 1, no pending clear.

Windows behavior (secondary source): Wine's d3d9 test
`test_multisample_mismatch` (`dlls/d3d9/tests/visual.c`, Wine source
on GitHub, read 2026-11-12) runs on Windows. With a 2-sample
depth-stencil surface, a single-sample render target and
`D3DRS_ZENABLE` TRUE, the draw is done and the geometry is visible.
With a multisample render target and a single-sample depth-stencil
surface, NVIDIA and AMD drivers differ. Wine's wined3d does not attach
the depth-stencil surface when the sample counts differ (commit
"wined3d: Disable the depth stencil on multisample mismatch", 2015).

Change:

- `runtime/src/rt_d3d9_draw.m`: `begin_pass` does not attach the
  depth-stencil surface when its sample count is not the render target
  sample count. Depth and stencil tests are then off for the pass, and
  a pending depth or stencil clear has no effect. The first occurrence
  is logged. The fatal message is removed. The behavior for a
  multisample render target with a single-sample depth-stencil surface
  follows Wine and is UNCONFIRMED for this game.

Verification: the build (Apple clang) had no warnings. A headless run
with the saved antialiasing setting went into the game and quit at 80 s
with exit status 0 (135 fps to 143 fps). A windowed run with the Metal
HUD went into the game. A screenshot at 80 s shows a correct 3D scene
with correct depth order (Metal HUD: 112 fps, GPU 6.1 ms, Metal memory
600 MB, application memory 1.34 GB). That run was left open for the
user.
