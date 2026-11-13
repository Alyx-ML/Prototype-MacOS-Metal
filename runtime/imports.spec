# Import specification for the Win32 runtime.
#
# One line per imported function or data object:
#   <dll> <import-name> <kind> <stack-dwords> [<c-name>]
#
# import-name: the name that SRW uses (ordinal imports: ord_<dll>_<n>;
#   names with characters other than [A-Za-z0-9_] are encoded by SRW,
#   see SymbolName in SRW/SRW_loader.c; write them here in the original form).
# kind:
#   std   the C function void rt_<c-name>(_cpu *cpu) reads the arguments
#         from the guest stack and sets the result registers. The thunk then
#         returns to the guest and removes <stack-dwords> argument dwords
#         (callee cleanup, __stdcall and __thiscall). Use 0 for __cdecl.
#   jump  the C function uint32_t rt_<c-name>(_cpu *cpu) sets all guest
#         registers (including esp) and returns the guest address where
#         execution continues. <stack-dwords> must be 0.
#   data  a data object. The runtime defines it with the symbol
#         <encoded-name>_asm2c. <stack-dwords> must be 0.
#   unreachable
#         no translated code calls this import: only code bytes that no
#         code or data refers to (dead code that SRW does not translate)
#         call it. tools/gen_thunks.py writes the C function; a call stops
#         the process with a message.
# c-name: optional; the default is the import name (encoded).

# ---------------------------------------------------------------- KERNEL32
kernel32 CloseHandle std 1
kernel32 CompareFileTime std 2
kernel32 ConvertFiberToThread std 0
kernel32 ConvertThreadToFiber std 1
kernel32 CopyFileA std 3
kernel32 CreateDirectoryA std 2
kernel32 CreateEventA std 4
kernel32 CreateFiber std 3
kernel32 CreateFileA std 7
kernel32 CreateMutexA std 3
kernel32 CreateSemaphoreA std 4
kernel32 CreateThread std 6
kernel32 DeleteCriticalSection std 1
kernel32 DeleteFiber std 1
kernel32 DeleteFileA std 1
kernel32 DeviceIoControl std 8
kernel32 EnterCriticalSection std 1
kernel32 ExitThread jump 0
kernel32 FindClose std 1
kernel32 FindFirstFileA std 2
kernel32 FindNextFileA std 2
kernel32 FlushFileBuffers std 1
kernel32 FormatMessageA std 7
kernel32 FreeLibrary std 1
kernel32 GetACP std 0
kernel32 GetCurrentDirectoryA std 2
kernel32 GetCurrentProcess std 0
kernel32 GetCurrentProcessId std 0
kernel32 GetCurrentThread std 0
kernel32 GetCurrentThreadId std 0
kernel32 GetDiskFreeSpaceExA std 4
kernel32 GetDriveTypeA std 1
kernel32 GetExitCodeThread std 2
kernel32 GetFileAttributesA std 1
kernel32 GetFileSize std 2
kernel32 GetFileTime std 4
kernel32 GetFullPathNameA std 4
kernel32 GetLastError std 0
kernel32 GetLocalTime std 1
kernel32 GetLocaleInfoA std 4
kernel32 GetLogicalDrives std 0
kernel32 GetModuleFileNameA std 3
kernel32 GetModuleHandleA std 1
kernel32 GetProcAddress std 2
kernel32 GetProcessAffinityMask std 3
kernel32 GetProcessHeap std 0
kernel32 GetStartupInfoA std 1
kernel32 GetSystemDefaultLangID std 0
kernel32 GetSystemTimeAsFileTime std 1
kernel32 GetThreadLocale std 0
kernel32 GetTickCount std 0
kernel32 GetTimeZoneInformation std 1
kernel32 GetVersionExA std 1
kernel32 GetVolumeInformationA std 8
kernel32 GlobalMemoryStatus std 1
kernel32 HeapFree std 3
kernel32 InitializeCriticalSection std 1
kernel32 InterlockedCompareExchange std 3
kernel32 InterlockedDecrement std 1
kernel32 InterlockedExchange std 2
kernel32 IsDebuggerPresent std 0
kernel32 LeaveCriticalSection std 1
kernel32 LoadLibraryA std 1
kernel32 MoveFileA std 2
kernel32 MultiByteToWideChar std 6
kernel32 OpenFile std 3
kernel32 OutputDebugStringA std 1
kernel32 QueryPerformanceCounter std 1
kernel32 QueryPerformanceFrequency std 1
kernel32 RaiseException jump 0
kernel32 ReadFile std 5
kernel32 ReleaseSemaphore std 3
kernel32 RemoveDirectoryA std 1
kernel32 ResetEvent std 1
kernel32 ResumeThread std 1
kernel32 SetCurrentDirectoryA std 1
kernel32 SetEndOfFile std 1
kernel32 SetErrorMode std 1
kernel32 SetEvent std 1
kernel32 SetFileAttributesA std 2
kernel32 SetFilePointer std 4
kernel32 SetThreadAffinityMask std 2
kernel32 SetThreadPriority std 2
kernel32 SetUnhandledExceptionFilter std 1
kernel32 Sleep std 1
kernel32 SuspendThread std 1
kernel32 SwitchToFiber jump 0
kernel32 TerminateProcess std 2
kernel32 TerminateThread std 2
kernel32 TlsAlloc std 0
kernel32 TlsFree std 1
kernel32 TlsGetValue std 1
kernel32 TlsSetValue std 2
kernel32 TryEnterCriticalSection std 1
kernel32 UnhandledExceptionFilter std 1
kernel32 VirtualAlloc std 4
kernel32 VirtualFree std 3
kernel32 WaitForMultipleObjects std 4
kernel32 WaitForSingleObject std 2
kernel32 WriteFile std 5
kernel32 lstrlenA std 1
kernel32 lstrlenW std 1

# ---------------------------------------------------------------- USER32
user32 AdjustWindowRect std 3
user32 CallWindowProcA std 5
user32 CharLowerBuffA std 2
user32 CreateWindowExA std 12
user32 DefWindowProcA std 4
user32 DestroyWindow std 1
user32 DispatchMessageA std 1
user32 FindWindowExA std 4
user32 GetClientRect std 2
user32 GetDesktopWindow std 0
user32 GetForegroundWindow std 0
user32 GetKeyState std 1
user32 GetKeyboardLayout std 1
user32 GetSystemMetrics std 1
user32 GetWindowLongA std 2
user32 GetWindowRect std 2
user32 LoadCursorA std 2
user32 LoadIconA std 2
user32 MapVirtualKeyExA std 3
user32 MessageBoxA std 4
user32 PeekMessageA std 5
user32 PostQuitMessage std 1
user32 RegisterClassA std 1
user32 RegisterClassExA std 1
user32 ScreenToClient std 2
user32 SetForegroundWindow std 1
user32 SetWindowLongA std 3
user32 ShowCursor std 1
user32 ShowWindow std 2
user32 SystemParametersInfoA std 4
user32 TranslateMessage std 1
user32 UnregisterClassA std 2
user32 wsprintfA std 0
user32 wvsprintfA std 3

# ---------------------------------------------------------------- GDI32
gdi32 GetStockObject std 1

# ---------------------------------------------------------------- ADVAPI32
advapi32 CryptAcquireContextA std 5
advapi32 CryptCreateHash std 5
advapi32 CryptDestroyHash std 1
advapi32 CryptDestroyKey std 1
advapi32 CryptEncrypt std 7
advapi32 CryptGenRandom std 3
advapi32 CryptGetHashParam std 5
advapi32 CryptHashData std 4
advapi32 CryptImportKey std 6
advapi32 CryptReleaseContext std 2
advapi32 RegCloseKey std 1
advapi32 RegCreateKeyExA std 9
advapi32 RegEnumKeyA std 4
advapi32 RegOpenKeyA std 3
advapi32 RegOpenKeyExA std 5
advapi32 RegQueryValueExA std 6
advapi32 RegSetValueExA std 6

# ---------------------------------------------------------------- OLE32, OLEAUT32, RPCRT4
ole32 CLSIDFromString std 2
ole32 CoCreateGuid std 1
ole32 CoCreateInstance std 5
ole32 CoInitialize std 1
ole32 CoSetProxyBlanket std 8
ole32 CoUninitialize std 0
oleaut32 ord_oleaut32_2 std 1 SysAllocString
oleaut32 ord_oleaut32_4 std 2 SysAllocStringLen
oleaut32 ord_oleaut32_6 std 1 SysFreeString
oleaut32 ord_oleaut32_8 std 1 VariantInit
oleaut32 ord_oleaut32_9 std 1 VariantClear
oleaut32 ord_oleaut32_12 std 4 VariantChangeType
oleaut32 ord_oleaut32_200 std 2 GetErrorInfo
oleaut32 ord_oleaut32_201 std 2 SetErrorInfo
oleaut32 ord_oleaut32_202 std 1 CreateErrorInfo
rpcrt4 RpcStringFreeA std 1
rpcrt4 UuidToStringA std 2
# GetProcAddress only (the engine looks it up at run time).
rpcrt4 UuidCreateSequential std 1

# ---------------------------------------------------------------- SETUPAPI, SHELL32, SHLWAPI, IPHLPAPI
setupapi SetupDiDestroyDeviceInfoList std 1
setupapi SetupDiEnumDeviceInterfaces std 5
setupapi SetupDiGetClassDevsA std 4
setupapi SetupDiGetDeviceInterfaceAlias std 4
setupapi SetupDiGetDeviceInterfaceDetailA std 6
setupapi SetupDiGetDeviceRegistryPropertyA std 7
shell32 SHGetFolderPathA std 5
shlwapi PathRelativePathToA std 5
iphlpapi GetAdaptersInfo unreachable 2
iphlpapi GetIfEntry unreachable 1
iphlpapi GetIpAddrTable unreachable 3
iphlpapi GetPerAdapterInfo unreachable 3

# ---------------------------------------------------------------- WINMM
winmm timeGetTime std 0
winmm timeKillEvent std 1
winmm timeSetEvent std 5
winmm waveOutClose std 1
winmm waveOutGetDevCapsA std 3
winmm waveOutGetNumDevs std 0
winmm waveOutOpen std 6
winmm waveOutPrepareHeader std 3
winmm waveOutReset std 1
winmm waveOutUnprepareHeader std 3
winmm waveOutWrite std 3

# ---------------------------------------------------------------- WS2_32
ws2_32 ord_ws2_32_1 std 3 ws_accept
ws2_32 ord_ws2_32_2 std 3 ws_bind
ws2_32 ord_ws2_32_3 std 1 ws_closesocket
ws2_32 ord_ws2_32_4 std 3 ws_connect
ws2_32 ord_ws2_32_5 std 3 ws_getpeername
ws2_32 ord_ws2_32_6 std 3 ws_getsockname
ws2_32 ord_ws2_32_7 std 5 ws_getsockopt
ws2_32 ord_ws2_32_8 std 1 ws_htonl
ws2_32 ord_ws2_32_9 std 1 ws_htons
ws2_32 ord_ws2_32_10 std 3 ws_ioctlsocket
ws2_32 ord_ws2_32_11 std 1 ws_inet_addr
ws2_32 ord_ws2_32_13 std 2 ws_listen
ws2_32 ord_ws2_32_14 std 1 ws_ntohl
ws2_32 ord_ws2_32_15 std 1 ws_ntohs
ws2_32 ord_ws2_32_16 std 4 ws_recv
ws2_32 ord_ws2_32_17 std 6 ws_recvfrom
ws2_32 ord_ws2_32_18 std 5 ws_select
ws2_32 ord_ws2_32_19 std 4 ws_send
ws2_32 ord_ws2_32_20 std 6 ws_sendto
ws2_32 ord_ws2_32_21 std 5 ws_setsockopt
ws2_32 ord_ws2_32_22 std 2 ws_shutdown
ws2_32 ord_ws2_32_23 std 3 ws_socket
ws2_32 ord_ws2_32_103 unreachable 5 ws_WSAAsyncGetHostByName
ws2_32 ord_ws2_32_108 unreachable 1 ws_WSACancelAsyncRequest
ws2_32 ord_ws2_32_111 std 0 ws_WSAGetLastError
ws2_32 ord_ws2_32_115 std 2 ws_WSAStartup
ws2_32 ord_ws2_32_116 std 0 ws_WSACleanup
ws2_32 ord_ws2_32_151 std 2 ws___WSAFDIsSet
ws2_32 getaddrinfo std 4 ws_getaddrinfo
ws2_32 freeaddrinfo std 1 ws_freeaddrinfo

# ---------------------------------------------------------------- DirectX, XInput
d3d9 Direct3DCreate9 std 1
dsound DirectSoundEnumerateA std 2
xinput1_3 ord_xinput1_3_2 std 2 XInputGetState
xinput1_3 ord_xinput1_3_3 std 2 XInputSetState
xinput1_3 ord_xinput1_3_4 std 3 XInputGetCapabilities

# ---------------------------------------------------------------- binkw32
binkw32 _BinkClose@4 std 1 BinkClose
binkw32 _BinkCloseTrack@4 unreachable 1 BinkCloseTrack
binkw32 _BinkDoFrame@4 std 1 BinkDoFrame
binkw32 _BinkGetFrameBuffersInfo@8 std 2 BinkGetFrameBuffersInfo
binkw32 _BinkNextFrame@4 std 1 BinkNextFrame
binkw32 _BinkOpen@8 std 2 BinkOpen
binkw32 _BinkOpenTrack@8 unreachable 2 BinkOpenTrack
binkw32 _BinkPause@8 std 2 BinkPause
binkw32 _BinkRegisterFrameBuffers@8 std 2 BinkRegisterFrameBuffers
binkw32 _BinkSetIO@4 std 1 BinkSetIO
binkw32 _BinkSetMemory@8 std 2 BinkSetMemory
binkw32 _BinkSetMixBins@16 std 4 BinkSetMixBins
binkw32 _BinkSetSoundSystem@8 std 2 BinkSetSoundSystem
binkw32 _BinkSetSoundTrack@8 std 2 BinkSetSoundTrack
binkw32 _BinkSetVolume@12 std 3 BinkSetVolume
binkw32 _BinkWait@4 std 1 BinkWait
binkw32 _RADTimerRead@0 std 0 RADTimerRead

# ---------------------------------------------------------------- MSVCR80
# C++ runtime. Methods of type_info use __thiscall (this in ecx, callee cleanup).
msvcr80 ??2@YAPAXI@Z std 0 operator_new
msvcr80 ??3@YAXPAX@Z std 0 operator_delete
msvcr80 ??_V@YAXPAX@Z std 0 operator_delete_array
msvcr80 ??8type_info@@QBE_NABV0@@Z std 1 type_info_eq
msvcr80 ?_name_internal_method@type_info@@QBEPBDPAU__type_info_node@@@Z std 1 type_info_name_internal
msvcr80 ?_type_info_dtor_internal_method@type_info@@QAEXXZ std 0 type_info_dtor_internal
msvcr80 ?before@type_info@@QBEHABV1@@Z std 1 type_info_before
msvcr80 ?terminate@@YAXXZ std 0 terminate
msvcr80 _CxxThrowException jump 0
msvcr80 __RTDynamicCast std 0
msvcr80 __RTtypeid std 0
msvcr80 __clean_type_info_names_internal std 0
msvcr80 _purecall std 0
msvcr80 _set_purecall_handler std 0
# Exceptions and startup
msvcr80 _XcptFilter std 0
msvcr80 __CppXcptFilter std 0
msvcr80 _except_handler4_common std 0
msvcr80 __dllonexit std 0
msvcr80 __getmainargs std 0
msvcr80 __iob_func std 0
msvcr80 __p__commode std 0
msvcr80 __p__fmode std 0
msvcr80 __set_app_type std 0
msvcr80 __setusermatherr std 0
msvcr80 _acmdln data 0
msvcr80 _adjust_fdiv data 0
msvcr80 _HUGE data 0
msvcr80 _amsg_exit std 0
msvcr80 _cexit std 0
msvcr80 _configthreadlocale std 0
msvcr80 _controlfp_s std 0
msvcr80 _crt_debugger_hook std 0
msvcr80 _decode_pointer std 0
msvcr80 _encode_pointer std 0
msvcr80 _encoded_null std 0
msvcr80 _errno std 0
msvcr80 _exit std 0
msvcr80 _initterm jump 0
msvcr80 _initterm_e jump 0
msvcr80 _invoke_watson std 0
msvcr80 _lock std 0
msvcr80 _unlock std 0
msvcr80 _malloc_crt std 0
msvcr80 _onexit std 0
msvcr80 _setjmp3 std 0
msvcr80 longjmp jump 0
msvcr80 abort std 0
msvcr80 exit std 0
# Math. The _CI functions take their arguments on the x87 stack.
msvcr80 _CIacos std 0
msvcr80 _CIasin std 0
msvcr80 _CIcos std 0
msvcr80 _CIcosh std 0
msvcr80 _CIfmod std 0
msvcr80 _CIpow std 0
msvcr80 _CIsinh std 0
msvcr80 _CIsqrt std 0
msvcr80 _CItanh std 0
msvcr80 _isnan std 0
msvcr80 atof std 0
msvcr80 ceil std 0
msvcr80 floor std 0
msvcr80 frexp std 0
msvcr80 ldexp std 0
msvcr80 modf std 0
msvcr80 strtod std 0
msvcr80 rand std 0
msvcr80 srand std 0
# Memory
msvcr80 malloc std 0
msvcr80 free std 0
msvcr80 realloc std 0
msvcr80 memchr std 0
msvcr80 memcpy std 0
msvcr80 memcpy_s std 0
msvcr80 memmove std 0
msvcr80 memmove_s std 0
msvcr80 memset std 0
msvcr80 qsort std 0
# Time
msvcr80 _ftime64 std 0
msvcr80 _gmtime64 std 0
msvcr80 _localtime64 std 0
# Strings and characters
msvcr80 _ismbblead std 0
msvcr80 _itoa_s std 0
msvcr80 _stricmp std 0
msvcr80 _strlwr std 0
msvcr80 _strnicmp std 0
msvcr80 _strtoui64 std 0
msvcr80 _strupr std 0
msvcr80 atoi std 0
msvcr80 isalnum std 0
msvcr80 isalpha std 0
msvcr80 iscntrl std 0
msvcr80 isdigit std 0
msvcr80 islower std 0
msvcr80 ispunct std 0
msvcr80 isspace std 0
msvcr80 isupper std 0
msvcr80 iswalnum std 0
msvcr80 iswspace std 0
msvcr80 isxdigit std 0
msvcr80 localeconv std 0
msvcr80 strcat_s std 0
msvcr80 strchr std 0
msvcr80 strcoll std 0
msvcr80 strcpy_s std 0
msvcr80 strcspn std 0
msvcr80 strerror std 0
msvcr80 strncat std 0
msvcr80 strncmp std 0
msvcr80 strncpy std 0
msvcr80 strncpy_s std 0
msvcr80 strnlen std 0
msvcr80 strpbrk std 0
msvcr80 strrchr std 0
msvcr80 strspn std 0
msvcr80 strstr std 0
msvcr80 strtok std 0
msvcr80 strtol std 0
msvcr80 strtoul std 0
msvcr80 tolower std 0
msvcr80 toupper std 0
msvcr80 towlower std 0
msvcr80 towupper std 0
msvcr80 wcscat_s std 0
msvcr80 wcscpy_s std 0
msvcr80 wcsrtombs_s std 0
msvcr80 wcsstr std 0
# Formatted input and output
msvcr80 _set_printf_count_output std 0
msvcr80 _snprintf std 0
msvcr80 _snprintf_s std 0
msvcr80 _vsnprintf std 0
msvcr80 _vsnprintf_s std 0
msvcr80 sprintf std 0
msvcr80 sprintf_s std 0
msvcr80 fprintf std 0
msvcr80 sscanf std 0
msvcr80 sscanf_s std 0
msvcr80 swscanf std 0
# Files
msvcr80 _stat64 std 0
msvcr80 fclose std 0
msvcr80 feof std 0
msvcr80 ferror std 0
msvcr80 fflush std 0
msvcr80 fgets std 0
msvcr80 fopen std 0
msvcr80 fopen_s std 0
msvcr80 fputc std 0
msvcr80 fputs std 0
msvcr80 fread std 0
msvcr80 fseek std 0
msvcr80 ftell std 0
msvcr80 fwrite std 0
msvcr80 getc std 0
msvcr80 rewind std 0
msvcr80 ungetc std 0
