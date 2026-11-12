# COM vtable specification for the Win32 runtime.
#
# The runtime implements COM objects that the guest calls through vtables.
# Each vtable entry is a guest-callable thunk (generated llasm procedure)
# that calls one C function. tools/gen_com_thunks.py reads this file.
#
# Format:
#   vtable <name> <interface>
#       <method> <stack-dwords> [<c-name> | -]
#
# name: the vtable name. The runtime refers to it as rt_vt_<name>.
# interface: the COM interface name (for messages).
# method: the method name, in vtable order.
# stack-dwords: the argument dwords including the "this" pointer. COM
#   methods are __stdcall, so the thunk removes these dwords.
# c-name: the C function void <c-name>(_cpu *cpu). The default is
#   rt_com_<name>_<method>. "-" means that the runtime does not support the
#   method: the generated function stops the process with a message.
#
# The IUnknown methods of vtable slot n of an object use the shared
# functions rt_com_unk<n>_QueryInterface, rt_com_unk<n>_AddRef and
# rt_com_unk<n>_Release (runtime/src/rt_ole32.c).

# ---------------------------------------------------------------- WMI (rt_wmi.c)
vtable WbemLocator IWbemLocator
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    ConnectServer 9

vtable WbemServices IWbemServices
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    OpenNamespace 6 -
    CancelAsyncCall 2 -
    QueryObjectSink 3 -
    GetObject 6
    GetObjectAsync 5 -
    PutClass 5 -
    PutClassAsync 5 -
    DeleteClass 5 -
    DeleteClassAsync 5 -
    CreateClassEnum 5 -
    CreateClassEnumAsync 5 -
    PutInstance 5 -
    PutInstanceAsync 5 -
    DeleteInstance 5 -
    DeleteInstanceAsync 5 -
    CreateInstanceEnum 5
    CreateInstanceEnumAsync 5 -
    ExecQuery 6 -
    ExecQueryAsync 6 -
    ExecNotificationQuery 6 -
    ExecNotificationQueryAsync 6 -
    ExecMethod 8 -
    ExecMethodAsync 7 -

vtable WbemClassObject IWbemClassObject
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetQualifierSet 2 -
    Get 6
    Put 5 -
    Delete 2 -
    GetNames 5 -
    BeginEnumeration 2 -
    Next 6 -
    EndEnumeration 1 -
    GetPropertyQualifierSet 3 -
    Clone 2 -
    GetObjectText 3 -
    SpawnDerivedClass 3 -
    SpawnInstance 3 -
    CompareTo 3 -
    GetPropertyOrigin 3 -
    InheritsFrom 2 -
    GetMethod 5 -
    PutMethod 5 -
    DeleteMethod 2 -
    BeginMethodEnumeration 2 -
    NextMethod 5 -
    EndMethodEnumeration 1 -
    GetMethodQualifierSet 3 -
    GetMethodOrigin 3 -

vtable EnumWbemClassObject IEnumWbemClassObject
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    Reset 1
    Next 5
    NextAsync 3 -
    Clone 2 -
    Skip 3 -

# ---------------------------------------------------------------- error information (rt_oleaut32.c)
# One object with two interfaces: slot 0 ICreateErrorInfo, slot 1 IErrorInfo.
vtable CreateErrorInfo ICreateErrorInfo
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    SetGUID 2
    SetSource 2
    SetDescription 2
    SetHelpFile 2
    SetHelpContext 2

vtable ErrorInfo IErrorInfo
    QueryInterface 3 rt_com_unk1_QueryInterface
    AddRef 1 rt_com_unk1_AddRef
    Release 1 rt_com_unk1_Release
    GetGUID 2
    GetSource 2
    GetDescription 2
    GetHelpFile 2
    GetHelpContext 2

# ---------------------------------------------------------------- Direct3D 9 (rt_d3d9.m, rt_d3d9_res.m)
# Methods marked "-" are not supported; a call stops the process with the
# method name. The C functions are rt_com_<vtable>_<method>.
vtable Direct3D9 IDirect3D9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    RegisterSoftwareDevice 2 -
    GetAdapterCount 1
    GetAdapterIdentifier 4
    GetAdapterModeCount 3
    EnumAdapterModes 5
    GetAdapterDisplayMode 3
    CheckDeviceType 6
    CheckDeviceFormat 7
    CheckDeviceMultiSampleType 7
    CheckDepthStencilMatch 6
    CheckDeviceFormatConversion 5
    GetDeviceCaps 4
    GetAdapterMonitor 2 -
    CreateDevice 7

vtable D3DDevice9 IDirect3DDevice9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    TestCooperativeLevel 1
    GetAvailableTextureMem 1
    EvictManagedResources 1
    GetDirect3D 2
    GetDeviceCaps 2
    GetDisplayMode 3
    GetCreationParameters 2
    SetCursorProperties 4 -
    SetCursorPosition 4
    ShowCursor 2
    CreateAdditionalSwapChain 3 -
    GetSwapChain 3 -
    GetNumberOfSwapChains 1
    Reset 2
    Present 5
    GetBackBuffer 5
    GetRasterStatus 3
    SetDialogBoxMode 2 -
    SetGammaRamp 4
    GetGammaRamp 3
    CreateTexture 9
    CreateVolumeTexture 10
    CreateCubeTexture 8
    CreateVertexBuffer 7
    CreateIndexBuffer 7
    CreateRenderTarget 9
    CreateDepthStencilSurface 9
    UpdateSurface 5
    UpdateTexture 3
    GetRenderTargetData 3
    GetFrontBufferData 3 -
    StretchRect 6
    ColorFill 4
    CreateOffscreenPlainSurface 7
    SetRenderTarget 3
    GetRenderTarget 3
    SetDepthStencilSurface 2
    GetDepthStencilSurface 2
    BeginScene 1
    EndScene 1
    Clear 7
    SetTransform 3
    GetTransform 3
    MultiplyTransform 3
    SetViewport 2
    GetViewport 2
    SetMaterial 2
    GetMaterial 2
    SetLight 3
    GetLight 3
    LightEnable 3
    GetLightEnable 3
    SetClipPlane 3
    GetClipPlane 3
    SetRenderState 3
    GetRenderState 3
    CreateStateBlock 3 -
    BeginStateBlock 1 -
    EndStateBlock 2 -
    SetClipStatus 2
    GetClipStatus 2
    GetTexture 3
    SetTexture 3
    GetTextureStageState 4
    SetTextureStageState 4
    GetSamplerState 4
    SetSamplerState 4
    ValidateDevice 2
    SetPaletteEntries 3 -
    GetPaletteEntries 3 -
    SetCurrentTexturePalette 2 -
    GetCurrentTexturePalette 2 -
    SetScissorRect 2
    GetScissorRect 2
    SetSoftwareVertexProcessing 2
    GetSoftwareVertexProcessing 1
    SetNPatchMode 2
    GetNPatchMode 1
    DrawPrimitive 4
    DrawIndexedPrimitive 7
    DrawPrimitiveUP 5
    DrawIndexedPrimitiveUP 9
    ProcessVertices 7 -
    CreateVertexDeclaration 3
    SetVertexDeclaration 2
    GetVertexDeclaration 2
    SetFVF 2
    GetFVF 2
    CreateVertexShader 3
    SetVertexShader 2
    GetVertexShader 2
    SetVertexShaderConstantF 4
    GetVertexShaderConstantF 4
    SetVertexShaderConstantI 4
    GetVertexShaderConstantI 4
    SetVertexShaderConstantB 4
    GetVertexShaderConstantB 4
    SetStreamSource 5
    GetStreamSource 5
    SetStreamSourceFreq 3
    GetStreamSourceFreq 3
    SetIndices 2
    GetIndices 2
    CreatePixelShader 3
    SetPixelShader 2
    GetPixelShader 2
    SetPixelShaderConstantF 4
    GetPixelShaderConstantF 4
    SetPixelShaderConstantI 4
    GetPixelShaderConstantI 4
    SetPixelShaderConstantB 4
    GetPixelShaderConstantB 4
    DrawRectPatch 4 -
    DrawTriPatch 4 -
    DeletePatch 2 -
    CreateQuery 3

vtable D3DTexture9 IDirect3DTexture9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDevice 2
    SetPrivateData 5 -
    GetPrivateData 4 -
    FreePrivateData 2 -
    SetPriority 2
    GetPriority 1
    PreLoad 1
    GetType 1
    SetLOD 2
    GetLOD 1
    GetLevelCount 1
    SetAutoGenFilterType 2
    GetAutoGenFilterType 1
    GenerateMipSubLevels 1
    GetLevelDesc 3
    GetSurfaceLevel 3
    LockRect 5
    UnlockRect 2
    AddDirtyRect 2

vtable D3DCubeTexture9 IDirect3DCubeTexture9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDevice 2 rt_com_D3DTexture9_GetDevice
    SetPrivateData 5 -
    GetPrivateData 4 -
    FreePrivateData 2 -
    SetPriority 2 rt_com_D3DTexture9_SetPriority
    GetPriority 1 rt_com_D3DTexture9_GetPriority
    PreLoad 1 rt_com_D3DTexture9_PreLoad
    GetType 1 rt_com_D3DTexture9_GetType
    SetLOD 2 rt_com_D3DTexture9_SetLOD
    GetLOD 1 rt_com_D3DTexture9_GetLOD
    GetLevelCount 1 rt_com_D3DTexture9_GetLevelCount
    SetAutoGenFilterType 2 rt_com_D3DTexture9_SetAutoGenFilterType
    GetAutoGenFilterType 1 rt_com_D3DTexture9_GetAutoGenFilterType
    GenerateMipSubLevels 1 rt_com_D3DTexture9_GenerateMipSubLevels
    GetLevelDesc 3 rt_com_D3DTexture9_GetLevelDesc
    GetCubeMapSurface 4
    LockRect 6
    UnlockRect 3
    AddDirtyRect 3

vtable D3DVolumeTexture9 IDirect3DVolumeTexture9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDevice 2 rt_com_D3DTexture9_GetDevice
    SetPrivateData 5 -
    GetPrivateData 4 -
    FreePrivateData 2 -
    SetPriority 2 rt_com_D3DTexture9_SetPriority
    GetPriority 1 rt_com_D3DTexture9_GetPriority
    PreLoad 1 rt_com_D3DTexture9_PreLoad
    GetType 1 rt_com_D3DTexture9_GetType
    SetLOD 2 rt_com_D3DTexture9_SetLOD
    GetLOD 1 rt_com_D3DTexture9_GetLOD
    GetLevelCount 1 rt_com_D3DTexture9_GetLevelCount
    SetAutoGenFilterType 2 rt_com_D3DTexture9_SetAutoGenFilterType
    GetAutoGenFilterType 1 rt_com_D3DTexture9_GetAutoGenFilterType
    GenerateMipSubLevels 1 rt_com_D3DTexture9_GenerateMipSubLevels
    GetLevelDesc 3
    GetVolumeLevel 3 -
    LockBox 5
    UnlockBox 2
    AddDirtyBox 2

vtable D3DSurface9 IDirect3DSurface9
    QueryInterface 3
    AddRef 1
    Release 1
    GetDevice 2
    SetPrivateData 5 -
    GetPrivateData 4 -
    FreePrivateData 2 -
    SetPriority 2
    GetPriority 1
    PreLoad 1
    GetType 1
    GetContainer 3
    GetDesc 2
    LockRect 4
    UnlockRect 1
    GetDC 2 -
    ReleaseDC 2 -

vtable D3DVertexBuffer9 IDirect3DVertexBuffer9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDevice 2
    SetPrivateData 5 -
    GetPrivateData 4 -
    FreePrivateData 2 -
    SetPriority 2
    GetPriority 1
    PreLoad 1
    GetType 1
    Lock 5
    Unlock 1
    GetDesc 2

vtable D3DIndexBuffer9 IDirect3DIndexBuffer9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDevice 2 rt_com_D3DVertexBuffer9_GetDevice
    SetPrivateData 5 -
    GetPrivateData 4 -
    FreePrivateData 2 -
    SetPriority 2 rt_com_D3DVertexBuffer9_SetPriority
    GetPriority 1 rt_com_D3DVertexBuffer9_GetPriority
    PreLoad 1 rt_com_D3DVertexBuffer9_PreLoad
    GetType 1 rt_com_D3DVertexBuffer9_GetType
    Lock 5 rt_com_D3DVertexBuffer9_Lock
    Unlock 1 rt_com_D3DVertexBuffer9_Unlock
    GetDesc 2

vtable D3DVertexDeclaration9 IDirect3DVertexDeclaration9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDevice 2
    GetDeclaration 3

vtable D3DVertexShader9 IDirect3DVertexShader9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDevice 2
    GetFunction 3

vtable D3DPixelShader9 IDirect3DPixelShader9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDevice 2 rt_com_D3DVertexShader9_GetDevice
    GetFunction 3 rt_com_D3DVertexShader9_GetFunction

vtable D3DQuery9 IDirect3DQuery9
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDevice 2
    GetType 1
    GetDataSize 1
    Issue 2
    GetData 4

# ---------------------------------------------------------------- DirectInput 8 (rt_dinput.c)
vtable DirectInput8A IDirectInput8A
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    CreateDevice 4
    EnumDevices 5
    GetDeviceStatus 2
    RunControlPanel 3 -
    Initialize 3
    FindDevice 4 -
    EnumDevicesBySemantics 6 -
    ConfigureDevices 5 -

vtable DirectInputDevice8A IDirectInputDevice8A
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetCapabilities 2
    EnumObjects 4
    GetProperty 3 -
    SetProperty 3 -
    Acquire 1
    Unacquire 1
    GetDeviceState 3
    GetDeviceData 5 -
    SetDataFormat 2
    SetEventNotification 2 -
    SetCooperativeLevel 3
    GetObjectInfo 4 -
    GetDeviceInfo 2
    RunControlPanel 3 -
    Initialize 4 -
    CreateEffect 5 -
    EnumEffects 4 -
    GetEffectInfo 3 -
    GetForceFeedbackState 2 -
    SendForceFeedbackCommand 2 -
    EnumCreatedEffectObjects 4 -
    Escape 2 -
    Poll 1
    SendDeviceData 5 -
    EnumEffectsInFile 5 -
    WriteEffectToFile 5 -
    BuildActionMap 4 -
    SetActionMap 4 -
    GetImageInfo 2 -

# ---------------------------------------------------------------- XAudio2 2.3 (rt_xaudio2.c)
# The voice vtables are not COM interfaces (no IUnknown).
vtable XAudio2 IXAudio2
    QueryInterface 3 rt_com_unk0_QueryInterface
    AddRef 1 rt_com_unk0_AddRef
    Release 1 rt_com_unk0_Release
    GetDeviceCount 2
    GetDeviceDetails 3
    Initialize 3
    RegisterForCallbacks 2 -
    UnregisterForCallbacks 2 -
    CreateSourceVoice 8
    CreateSubmixVoice 8 -
    CreateMasteringVoice 7
    StartEngine 1 -
    StopEngine 1 -
    CommitChanges 2 -
    GetPerformanceData 2 -
    SetDebugConfiguration 3 -

vtable XAudio2SourceVoice IXAudio2SourceVoice
    GetVoiceDetails 2 -
    SetOutputVoices 2 -
    SetEffectChain 2 -
    EnableEffect 3 -
    DisableEffect 3 -
    GetEffectState 3 -
    SetEffectParameters 5 -
    GetEffectParameters 4 -
    SetFilterParameters 3 -
    GetFilterParameters 2 -
    SetVolume 3 -
    GetVolume 2 -
    SetChannelVolumes 4 -
    GetChannelVolumes 3 -
    SetOutputMatrix 6 -
    GetOutputMatrix 5 -
    DestroyVoice 1
    Start 3
    Stop 3
    SubmitSourceBuffer 3
    FlushSourceBuffers 1 -
    Discontinuity 1 -
    ExitLoop 2 -
    GetState 2 -
    SetFrequencyRatio 3 -
    GetFrequencyRatio 2 -

vtable XAudio2MasteringVoice IXAudio2MasteringVoice
    GetVoiceDetails 2 -
    SetOutputVoices 2 -
    SetEffectChain 2 -
    EnableEffect 3 -
    DisableEffect 3 -
    GetEffectState 3 -
    SetEffectParameters 5 -
    GetEffectParameters 4 -
    SetFilterParameters 3 -
    GetFilterParameters 2 -
    SetVolume 3 -
    GetVolume 2 -
    SetChannelVolumes 4 -
    GetChannelVolumes 3 -
    SetOutputMatrix 6 -
    GetOutputMatrix 5 -
    DestroyVoice 1
