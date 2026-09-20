; BladeVR -- thunks de reenvio hacia el dxgi.dll REAL del sistema.
; Cada Fwd_X salta a la direccion guardada en g_pfn_X (rellenada en DllMain
; de ProxyMain.cpp con GetProcAddress sobre %SystemRoot%\System32\dxgi.dll).
; Sin tocar registros ni pila: la firma de la funcion real da igual.

EXTERN g_pfn_ApplyCompatResolutionQuirking:QWORD
EXTERN g_pfn_CompatString:QWORD
EXTERN g_pfn_CompatValue:QWORD
EXTERN g_pfn_DXGID3D10CreateDevice:QWORD
EXTERN g_pfn_DXGID3D10CreateLayeredDevice:QWORD
EXTERN g_pfn_DXGID3D10GetLayeredDeviceSize:QWORD
EXTERN g_pfn_DXGID3D10RegisterLayers:QWORD
EXTERN g_pfn_DXGIDeclareAdapterRemovalSupport:QWORD
EXTERN g_pfn_DXGIDisableVBlankVirtualization:QWORD
EXTERN g_pfn_DXGIDumpJournal:QWORD
EXTERN g_pfn_DXGIGetDebugInterface1:QWORD
EXTERN g_pfn_DXGIReportAdapterConfiguration:QWORD
EXTERN g_pfn_PIXBeginCapture:QWORD
EXTERN g_pfn_PIXEndCapture:QWORD
EXTERN g_pfn_PIXGetCaptureState:QWORD
EXTERN g_pfn_SetAppCompatStringPointer:QWORD
EXTERN g_pfn_UpdateHMDEmulationStatus:QWORD

.code

Fwd_ApplyCompatResolutionQuirking PROC
    jmp qword ptr [g_pfn_ApplyCompatResolutionQuirking]
Fwd_ApplyCompatResolutionQuirking ENDP

Fwd_CompatString PROC
    jmp qword ptr [g_pfn_CompatString]
Fwd_CompatString ENDP

Fwd_CompatValue PROC
    jmp qword ptr [g_pfn_CompatValue]
Fwd_CompatValue ENDP

Fwd_DXGID3D10CreateDevice PROC
    jmp qword ptr [g_pfn_DXGID3D10CreateDevice]
Fwd_DXGID3D10CreateDevice ENDP

Fwd_DXGID3D10CreateLayeredDevice PROC
    jmp qword ptr [g_pfn_DXGID3D10CreateLayeredDevice]
Fwd_DXGID3D10CreateLayeredDevice ENDP

Fwd_DXGID3D10GetLayeredDeviceSize PROC
    jmp qword ptr [g_pfn_DXGID3D10GetLayeredDeviceSize]
Fwd_DXGID3D10GetLayeredDeviceSize ENDP

Fwd_DXGID3D10RegisterLayers PROC
    jmp qword ptr [g_pfn_DXGID3D10RegisterLayers]
Fwd_DXGID3D10RegisterLayers ENDP

Fwd_DXGIDeclareAdapterRemovalSupport PROC
    jmp qword ptr [g_pfn_DXGIDeclareAdapterRemovalSupport]
Fwd_DXGIDeclareAdapterRemovalSupport ENDP

Fwd_DXGIDisableVBlankVirtualization PROC
    jmp qword ptr [g_pfn_DXGIDisableVBlankVirtualization]
Fwd_DXGIDisableVBlankVirtualization ENDP

Fwd_DXGIDumpJournal PROC
    jmp qword ptr [g_pfn_DXGIDumpJournal]
Fwd_DXGIDumpJournal ENDP

Fwd_DXGIGetDebugInterface1 PROC
    jmp qword ptr [g_pfn_DXGIGetDebugInterface1]
Fwd_DXGIGetDebugInterface1 ENDP

Fwd_DXGIReportAdapterConfiguration PROC
    jmp qword ptr [g_pfn_DXGIReportAdapterConfiguration]
Fwd_DXGIReportAdapterConfiguration ENDP

Fwd_PIXBeginCapture PROC
    jmp qword ptr [g_pfn_PIXBeginCapture]
Fwd_PIXBeginCapture ENDP

Fwd_PIXEndCapture PROC
    jmp qword ptr [g_pfn_PIXEndCapture]
Fwd_PIXEndCapture ENDP

Fwd_PIXGetCaptureState PROC
    jmp qword ptr [g_pfn_PIXGetCaptureState]
Fwd_PIXGetCaptureState ENDP

Fwd_SetAppCompatStringPointer PROC
    jmp qword ptr [g_pfn_SetAppCompatStringPointer]
Fwd_SetAppCompatStringPointer ENDP

Fwd_UpdateHMDEmulationStatus PROC
    jmp qword ptr [g_pfn_UpdateHMDEmulationStatus]
Fwd_UpdateHMDEmulationStatus ENDP

END
