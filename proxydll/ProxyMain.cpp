#include <windows.h>
#include <dxgi.h>

// ---------------------------------------------------------------------
// BladeVR -- dxgi.dll proxy
//
// Se coloca junto a Blade.exe (bin\bin\). Como Windows busca las DLL en la
// carpeta del ejecutable ANTES que en System32, el juego carga ESTE dxgi.dll
// en vez del real, y en nuestro DllMain cargamos BladeVR.dll (el mod) en
// cuanto arranca el proceso, mucho antes de que el juego cree su device
// D3D11. No hace falta ningun inyector externo.
//
// Este DLL no reimplementa DXGI: cada export real de dxgi.dll se reenvia al
// DXGI del sistema, cargado por RUTA ABSOLUTA desde System32 (el nombre a
// secas "dxgi.dll" resolveria a este mismo proxy).
//
//  * 17 exports son thunks en ensamblador (ProxyThunks.asm):
//    "jmp [g_pfn_X]", con g_pfn_X rellenado aqui via GetProcAddress. Un jmp
//    no toca registros ni pila, asi que la firma real de cada funcion da
//    igual.
//  * CreateDXGIFactory / CreateDXGIFactory1 / CreateDXGIFactory2 son codigo
//    nuestro: llaman a la version real y, si tiene exito, avisan a
//    BladeVR.dll del factory obtenido (BladeVR_OnDXGIFactoryCreated). Desde
//    ese factory el mod hookea IDXGIFactory::CreateSwapChain y asi descubre
//    el ID3D11Device del juego antes del primer Present.
//
// Nota de build: <dxgi.h> ya declara CreateDXGIFactory/1 con extern "C", y
// definir una funcion con ese mismo nombre choca con esa declaracion
// (C2375). Por eso las nuestras se llaman ProxyCreateDXGIFactory* y se
// exportan bajo el nombre publico con "/export:Publico=Interno". La lista de
// exports es la de dxgi.dll de Windows 10/11 (dumpbin /exports).
// ---------------------------------------------------------------------

#pragma comment(linker, "/export:CreateDXGIFactory=ProxyCreateDXGIFactory")
#pragma comment(linker, "/export:CreateDXGIFactory1=ProxyCreateDXGIFactory1")
#pragma comment(linker, "/export:CreateDXGIFactory2=ProxyCreateDXGIFactory2")

#pragma comment(linker, "/export:ApplyCompatResolutionQuirking=Fwd_ApplyCompatResolutionQuirking")
#pragma comment(linker, "/export:CompatString=Fwd_CompatString")
#pragma comment(linker, "/export:CompatValue=Fwd_CompatValue")
#pragma comment(linker, "/export:DXGID3D10CreateDevice=Fwd_DXGID3D10CreateDevice")
#pragma comment(linker, "/export:DXGID3D10CreateLayeredDevice=Fwd_DXGID3D10CreateLayeredDevice")
#pragma comment(linker, "/export:DXGID3D10GetLayeredDeviceSize=Fwd_DXGID3D10GetLayeredDeviceSize")
#pragma comment(linker, "/export:DXGID3D10RegisterLayers=Fwd_DXGID3D10RegisterLayers")
#pragma comment(linker, "/export:DXGIDeclareAdapterRemovalSupport=Fwd_DXGIDeclareAdapterRemovalSupport")
#pragma comment(linker, "/export:DXGIDisableVBlankVirtualization=Fwd_DXGIDisableVBlankVirtualization")
#pragma comment(linker, "/export:DXGIDumpJournal=Fwd_DXGIDumpJournal")
#pragma comment(linker, "/export:DXGIGetDebugInterface1=Fwd_DXGIGetDebugInterface1")
#pragma comment(linker, "/export:DXGIReportAdapterConfiguration=Fwd_DXGIReportAdapterConfiguration")
#pragma comment(linker, "/export:PIXBeginCapture=Fwd_PIXBeginCapture")
#pragma comment(linker, "/export:PIXEndCapture=Fwd_PIXEndCapture")
#pragma comment(linker, "/export:PIXGetCaptureState=Fwd_PIXGetCaptureState")
#pragma comment(linker, "/export:SetAppCompatStringPointer=Fwd_SetAppCompatStringPointer")
#pragma comment(linker, "/export:UpdateHMDEmulationStatus=Fwd_UpdateHMDEmulationStatus")

// dxgi.dll real, cargado por ruta absoluta desde System32.
static HMODULE GetOrigDxgiModule() {
    static HMODULE h = nullptr;
    if (h) return h;
    char path[MAX_PATH] = {};
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 10) return nullptr;
    lstrcatA(path, "\\dxgi.dll");
    h = LoadLibraryA(path);
    return h;
}

// Destinos de los thunks de ProxyThunks.asm (extern "C" para que el nombre
// coincida con el EXTERN del ensamblador).
extern "C" {
void* g_pfn_ApplyCompatResolutionQuirking = nullptr;
void* g_pfn_CompatString = nullptr;
void* g_pfn_CompatValue = nullptr;
void* g_pfn_DXGID3D10CreateDevice = nullptr;
void* g_pfn_DXGID3D10CreateLayeredDevice = nullptr;
void* g_pfn_DXGID3D10GetLayeredDeviceSize = nullptr;
void* g_pfn_DXGID3D10RegisterLayers = nullptr;
void* g_pfn_DXGIDeclareAdapterRemovalSupport = nullptr;
void* g_pfn_DXGIDisableVBlankVirtualization = nullptr;
void* g_pfn_DXGIDumpJournal = nullptr;
void* g_pfn_DXGIGetDebugInterface1 = nullptr;
void* g_pfn_DXGIReportAdapterConfiguration = nullptr;
void* g_pfn_PIXBeginCapture = nullptr;
void* g_pfn_PIXEndCapture = nullptr;
void* g_pfn_PIXGetCaptureState = nullptr;
void* g_pfn_SetAppCompatStringPointer = nullptr;
void* g_pfn_UpdateHMDEmulationStatus = nullptr;
}

static void ResolveForwardedExports() {
    HMODULE h = GetOrigDxgiModule();
    if (!h) return;
    g_pfn_ApplyCompatResolutionQuirking = reinterpret_cast<void*>(GetProcAddress(h, "ApplyCompatResolutionQuirking"));
    g_pfn_CompatString = reinterpret_cast<void*>(GetProcAddress(h, "CompatString"));
    g_pfn_CompatValue = reinterpret_cast<void*>(GetProcAddress(h, "CompatValue"));
    g_pfn_DXGID3D10CreateDevice = reinterpret_cast<void*>(GetProcAddress(h, "DXGID3D10CreateDevice"));
    g_pfn_DXGID3D10CreateLayeredDevice = reinterpret_cast<void*>(GetProcAddress(h, "DXGID3D10CreateLayeredDevice"));
    g_pfn_DXGID3D10GetLayeredDeviceSize = reinterpret_cast<void*>(GetProcAddress(h, "DXGID3D10GetLayeredDeviceSize"));
    g_pfn_DXGID3D10RegisterLayers = reinterpret_cast<void*>(GetProcAddress(h, "DXGID3D10RegisterLayers"));
    g_pfn_DXGIDeclareAdapterRemovalSupport = reinterpret_cast<void*>(GetProcAddress(h, "DXGIDeclareAdapterRemovalSupport"));
    g_pfn_DXGIDisableVBlankVirtualization = reinterpret_cast<void*>(GetProcAddress(h, "DXGIDisableVBlankVirtualization"));
    g_pfn_DXGIDumpJournal = reinterpret_cast<void*>(GetProcAddress(h, "DXGIDumpJournal"));
    g_pfn_DXGIGetDebugInterface1 = reinterpret_cast<void*>(GetProcAddress(h, "DXGIGetDebugInterface1"));
    g_pfn_DXGIReportAdapterConfiguration = reinterpret_cast<void*>(GetProcAddress(h, "DXGIReportAdapterConfiguration"));
    g_pfn_PIXBeginCapture = reinterpret_cast<void*>(GetProcAddress(h, "PIXBeginCapture"));
    g_pfn_PIXEndCapture = reinterpret_cast<void*>(GetProcAddress(h, "PIXEndCapture"));
    g_pfn_PIXGetCaptureState = reinterpret_cast<void*>(GetProcAddress(h, "PIXGetCaptureState"));
    g_pfn_SetAppCompatStringPointer = reinterpret_cast<void*>(GetProcAddress(h, "SetAppCompatStringPointer"));
    g_pfn_UpdateHMDEmulationStatus = reinterpret_cast<void*>(GetProcAddress(h, "UpdateHMDEmulationStatus"));
}

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void** ppFactory);
typedef void(WINAPI* PFN_BladeVR_OnDXGIFactoryCreated)(void* factoryPtr, int interfaceVersion);

// Avisa a BladeVR.dll del factory recien creado. Toda la logica de hookeo
// vive en BladeVR.dll (que tiene su propia instancia de MinHook); este proxy
// no enlaza MinHook a proposito, para no tener dos copias en el proceso.
static void NotifyHookDllOfFactory(HRESULT hr, void** ppFactory, int interfaceVersion) {
    if (FAILED(hr) || !ppFactory || !*ppFactory) return;
    HMODULE hHook = GetModuleHandleA("BladeVR.dll");
    if (!hHook) return;
    PFN_BladeVR_OnDXGIFactoryCreated notify = reinterpret_cast<PFN_BladeVR_OnDXGIFactoryCreated>(
        GetProcAddress(hHook, "BladeVR_OnDXGIFactoryCreated"));
    if (notify) notify(*ppFactory, interfaceVersion);
}

extern "C" HRESULT WINAPI ProxyCreateDXGIFactory(REFIID riid, void** ppFactory) {
    HMODULE h = GetOrigDxgiModule();
    PFN_CreateDXGIFactory fn = h ? reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(h, "CreateDXGIFactory")) : nullptr;
    HRESULT hr = fn ? fn(riid, ppFactory) : E_FAIL;
    NotifyHookDllOfFactory(hr, ppFactory, 0);
    return hr;
}

extern "C" HRESULT WINAPI ProxyCreateDXGIFactory1(REFIID riid, void** ppFactory) {
    HMODULE h = GetOrigDxgiModule();
    PFN_CreateDXGIFactory1 fn = h ? reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(h, "CreateDXGIFactory1")) : nullptr;
    HRESULT hr = fn ? fn(riid, ppFactory) : E_FAIL;
    NotifyHookDllOfFactory(hr, ppFactory, 1);
    return hr;
}

extern "C" HRESULT WINAPI ProxyCreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    HMODULE h = GetOrigDxgiModule();
    PFN_CreateDXGIFactory2 fn = h ? reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(h, "CreateDXGIFactory2")) : nullptr;
    HRESULT hr = fn ? fn(Flags, riid, ppFactory) : E_FAIL;
    NotifyHookDllOfFactory(hr, ppFactory, 2);
    return hr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reasonForCall, LPVOID) {
    if (reasonForCall == DLL_PROCESS_ATTACH) {
        // Dentro de DllMain solo lo imprescindible: resolver los reenvios y
        // cargar el mod. BladeVR.dll hace todo su trabajo en un hilo propio.
        DisableThreadLibraryCalls(hModule);
        ResolveForwardedExports();
        LoadLibraryA("BladeVR.dll");
    }
    return TRUE;
}
