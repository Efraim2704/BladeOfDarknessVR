#include "Dx11Hook.h"
#include "HookLogger.h"
#include "ProjectionHook.h"
#include "StereoHook.h"
#include "OpenVRHook.h"
#include "HeadTrackHook.h"
#include "FovHook.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <cstdint>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace BladeVR {

// ---------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_FactoryCreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

static std::recursive_mutex g_stateMutex;
static bool g_installed = false;
static PFN_Present g_originalPresent = nullptr;
static PFN_ResizeBuffers g_originalResizeBuffers = nullptr;
static PFN_FactoryCreateSwapChain g_originalFactoryCreateSwapChain = nullptr;
static void* g_hookedFactoryCreateSwapChainAddr = nullptr;
static bool g_contextHooksInstalled = false;

static std::atomic<uint64_t> g_presentCallCount{0};
static ID3D11Device* g_gameDevice = nullptr;             // con AddRef propio
static ID3D11DeviceContext* g_gameContext = nullptr;     // con AddRef propio
static IDXGISwapChain* g_lastSwapChain = nullptr;        // solo para detectar cambios (sin AddRef)

// Copia del backbuffer que se entrega a SteamVR (el backbuffer del juego no
// tiene BIND_SHADER_RESOURCE y el compositor no lo acepta directamente).
static ID3D11Texture2D* g_sbsSnapshot = nullptr;
static ID3D11Texture2D* g_flatCopy = nullptr;
static HWND g_gameWindow = nullptr;   // copia del fotograma que ensena la pantalla anclada
static UINT g_sbsWidth = 0;
static UINT g_sbsHeight = 0;
static DXGI_FORMAT g_sbsFormat = DXGI_FORMAT_UNKNOWN;
static std::atomic<uint64_t> g_sbsSubmits{0};

// Indices de vtable (layout publico del SDK, estable por herencia COM).
static constexpr size_t kSwapChainVTablePresent = 8;
static constexpr size_t kSwapChainVTableResizeBuffers = 13;
static constexpr size_t kFactoryVTableCreateSwapChain = 10;

// MinHook se inicializa una sola vez, desde el primer hilo que lo necesite
// (el de instalacion o el diferido del factory).
static bool EnsureMinHookInitialized() {
    MH_STATUS st = MH_Initialize();
    return st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED;
}

// ---------------------------------------------------------------------
// Instalacion de los hooks del contexto del juego (una sola vez). Las
// vtables de ID3D11DeviceContext son compartidas por todas las instancias,
// asi que da igual desde que device se instalen.
// ---------------------------------------------------------------------
static void InstallContextHooks(ID3D11Device* device, const char* origin) {
    if (!device) return;
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (g_contextHooksInstalled) return;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return;
    HookLogger::Instance().Line(std::string("[DX11] Instalando hooks del contexto (") + origin + ").");
    bool ok = InstallProjectionHook(context);
    ok &= InstallStereoHook(context);
    g_contextHooksInstalled = ok;
    context->Release();
}

// ---------------------------------------------------------------------
// Factory: CreateSwapChain nos da el device del juego antes del primer
// Present. Todo lo que implica MH_CreateHook/MH_EnableHook (que suspende
// brevemente los hilos del proceso) se hace en un hilo propio, nunca dentro
// de la llamada del juego a CreateDXGIFactory/CreateSwapChain.
// ---------------------------------------------------------------------
struct DeferredDeviceNotification { IUnknown* device; };

static DWORD WINAPI DeferredDeviceThreadProc(LPVOID param) {
    DeferredDeviceNotification* data = static_cast<DeferredDeviceNotification*>(param);
    // Los hooks del contexto NO se instalan aqui: el juego crea una cadena de
    // intercambio antes que la suya de verdad (con otro dispositivo Direct3D),
    // y al engancharnos a ese contexto nos llegaban unos pocos draws por
    // segundo en vez de los del juego. Se instalan en el primer Present, que
    // es siempre el de la cadena que se esta dibujando de verdad.
    (void)data;
    if (data->device) data->device->Release();
    delete data;
    return 0;
}

static HRESULT STDMETHODCALLTYPE HookedFactoryCreateSwapChain(
    IDXGIFactory* self, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    HRESULT hr = g_originalFactoryCreateSwapChain(self, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && pDevice) {
        pDevice->AddRef();
        auto* data = new DeferredDeviceNotification{pDevice};
        HANDLE h = CreateThread(nullptr, 0, &DeferredDeviceThreadProc, data, 0, nullptr);
        if (h) {
            CloseHandle(h);
        } else {
            pDevice->Release();
            delete data;
        }
    }
    return hr;
}

struct DeferredFactoryHookRequest { void* factory; int interfaceVersion; };

static DWORD WINAPI DeferredFactoryThreadProc(LPVOID param) {
    DeferredFactoryHookRequest* data = static_cast<DeferredFactoryHookRequest*>(param);
    IDXGIFactory* factory = static_cast<IDXGIFactory*>(data->factory);
    if (factory) {
        void** vtable = *reinterpret_cast<void***>(factory);
        void* addr = vtable[kFactoryVTableCreateSwapChain];
        std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
        if (addr != g_hookedFactoryCreateSwapChainAddr && EnsureMinHookInitialized()) {
            bool ok = (MH_CreateHook(addr, &HookedFactoryCreateSwapChain,
                                     reinterpret_cast<void**>(&g_originalFactoryCreateSwapChain)) == MH_OK);
            ok &= (MH_EnableHook(addr) == MH_OK);
            HookLogger::Instance().Line(std::string("[DX11] Hook de IDXGIFactory::CreateSwapChain (factory v") +
                                        std::to_string(data->interfaceVersion) + "): " + (ok ? "OK" : "FALLO"));
            if (ok) g_hookedFactoryCreateSwapChainAddr = addr;
        }
    }
    delete data;
    return 0;
}

// Llamada por el proxy dxgi.dll tras cada CreateDXGIFactory* con exito.
extern "C" __declspec(dllexport) void WINAPI BladeVR_OnDXGIFactoryCreated(void* factoryPtr, int interfaceVersion) {
    auto* data = new DeferredFactoryHookRequest{factoryPtr, interfaceVersion};
    HANDLE h = CreateThread(nullptr, 0, &DeferredFactoryThreadProc, data, 0, nullptr);
    if (h) CloseHandle(h); else delete data;
}

// ---------------------------------------------------------------------
// Captura del backbuffer, entrega a SteamVR y espejo en el monitor
// ---------------------------------------------------------------------

// Si esta activado, con el estereo en marcha el monitor muestra solo el ojo
// izquierdo centrado (con bandas negras a los lados) en vez de las dos
// mitades. Es una copia de la GPU sin escalado: coste despreciable.
static constexpr bool kMirrorLeftEyeOnMonitor = true;
static ID3D11RenderTargetView* g_monitorRtv = nullptr;   // RTV del backbuffer actual
static ID3D11Texture2D* g_monitorRtvTexture = nullptr;   // backbuffer para el que se creo (sin AddRef)

static void ReleaseSbsSnapshot() {
    if (g_sbsSnapshot) { g_sbsSnapshot->Release(); g_sbsSnapshot = nullptr; }
    if (g_flatCopy) { g_flatCopy->Release(); g_flatCopy = nullptr; }
    g_sbsWidth = 0; g_sbsHeight = 0; g_sbsFormat = DXGI_FORMAT_UNKNOWN;
    if (g_monitorRtv) { g_monitorRtv->Release(); g_monitorRtv = nullptr; }
    g_monitorRtvTexture = nullptr;
}

// Copia el backbuffer (ya en formato lado a lado) a g_sbsSnapshot. Devuelve
// el backbuffer con una referencia (el llamante hace Release) o nullptr.
static ID3D11Texture2D* CaptureBackbuffer(IDXGISwapChain* swapChain, D3D11_TEXTURE2D_DESC* outDesc) {
    if (!g_gameDevice || !g_gameContext) return nullptr;
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))) || !backbuffer) {
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);

    if (!g_sbsSnapshot || g_sbsWidth != desc.Width || g_sbsHeight != desc.Height || g_sbsFormat != desc.Format) {
        ReleaseSbsSnapshot();
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = desc.Width;
        sd.Height = desc.Height;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = desc.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_gameDevice->CreateTexture2D(&sd, nullptr, &g_sbsSnapshot)) || !g_sbsSnapshot) {
            backbuffer->Release();
            HookLogger::Instance().Line("[DX11] ERROR: no se pudo crear la textura de captura.");
            return nullptr;
        }
        if (FAILED(g_gameDevice->CreateTexture2D(&sd, nullptr, &g_flatCopy))) g_flatCopy = nullptr;
        g_sbsWidth = desc.Width;
        g_sbsHeight = desc.Height;
        g_sbsFormat = desc.Format;
        std::ostringstream o;
        o << "[DX11] Textura de captura creada " << desc.Width << "x" << desc.Height
          << " formato=" << desc.Format << " (cada ojo " << (desc.Width / 2) << "x" << desc.Height << ")";
        HookLogger::Instance().Line(o.str());
    }

    if (desc.SampleDesc.Count > 1) {
        g_gameContext->ResolveSubresource(g_sbsSnapshot, 0, backbuffer, 0, desc.Format);
    } else {
        g_gameContext->CopyResource(g_sbsSnapshot, backbuffer);
    }
    *outDesc = desc;
    return backbuffer;
}

// Entrega g_sbsSnapshot a SteamVR. Orden: Submit (con la pose con la que se
// dibujo) y DESPUES WaitGetPoses. La primera vez se hace un WaitGetPoses
// previo: el compositor exige uno antes de aceptar el primer Submit.
// Fase plana (video de intro, cargas, menu principal): el fotograma entero se muestra
// en un overlay de SteamVR anclado al mundo, colocado delante del visor
// segun su pose en el primer fotograma (o al pulsar Inicio), nivelada
// (solo guinada). Asi el usuario puede mover la cabeza y la pantalla no va
// pegada a la cara. (Se probo enviar el fotograma al compositor con esa pose
// fija para que lo reproyectara y se descarto: al girar la cabeza el
// compositor funde la imagen y muestra su entorno.)
static float g_flatPose[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
static bool g_flatPoseValid = false;

// (Se probo alejar la pantalla anclada a 8 m agrandandola en la misma
// proporcion, para quitar el paralaje que se nota en la ficha de personaje
// al girar la cabeza -- los ojos se desplazan unos centimetros sobre el
// cuello y una pantalla a 1.8 m se separa unos grados del escenario lejano.
// Descartado: el efecto es pequeno y solo se aprecia en esa pantalla.)

static bool CaptureFlatPose() {
    float hmd[12];
    if (!GetHmdPoseMatrix34(hmd)) return false;
    // La pantalla va siempre "al frente": en la direccion que el visor miraba
    // al centrar la vista (la misma referencia que usa el seguimiento de
    // cabeza), nivelada y a la altura de los ojos. Asi el menu aparece
    // centrado mire donde mire el usuario en ese momento. (Se probo colocarla
    // en la direccion de la mirada y se descarto.)
    double centerYaw = 0.0;
    float fx, fz;
    if (HeadTrackGetCenterYawRad(&centerYaw)) {
        fx = static_cast<float>(sin(centerYaw));
        fz = static_cast<float>(-cos(centerYaw));
    } else {
        // Sin referencia todavia (video de introduccion, seguimiento apagado):
        // la direccion actual del visor, nivelada.
        fx = -hmd[0 * 4 + 2];
        fz = -hmd[2 * 4 + 2];
        float len = sqrtf(fx * fx + fz * fz);
        if (len < 1e-3f) { fx = 0.0f; fz = -1.0f; len = 1.0f; }   // mirando al techo o al suelo
        fx /= len; fz /= len;
    }
    // Columnas de la rotacion: 0 = derecha, 1 = arriba (mundo), 2 = atras.
    float d = StereoUiDistanceM();
    g_flatPose[0] = -fz;  g_flatPose[1] = 0.0f; g_flatPose[2]  = -fx; g_flatPose[3]  = hmd[3]  + d * fx;
    g_flatPose[4] = 0.0f; g_flatPose[5] = 1.0f; g_flatPose[6]  = 0.0f; g_flatPose[7]  = hmd[7];
    g_flatPose[8] =  fx;  g_flatPose[9] = 0.0f; g_flatPose[10] = -fz; g_flatPose[11] = hmd[11] + d * fz;
    g_flatPoseValid = true;
    HookLogger::Instance().Line("[STEREO] Pantalla anclada centrada al frente.");
    return true;
}

static void SubmitFrameToVR() {
    if (!IsOpenVRAvailable() || !g_sbsSnapshot) return;
    static bool s_firstPumpDone = false;
    if (!s_firstPumpDone) {
        PumpOpenVRFrameTiming();
        s_firstPumpDone = true;
    }
    bool mono = false;
    float renderPose[12];
    const float* posePtr = (!mono && HeadTrackGetRenderPose(renderPose)) ? renderPose : nullptr;
    // Fase plana para este fotograma si se dibujo plano, o si es el de
    // transicion (dibujado en estereo pero el siguiente ya sera plano: se
    // muestra su mitad izquierda para que el menu no aparezca un instante
    // en estereo).
    bool frameFlat = StereoFrameWasFlat();
    ID3D11Texture2D* uiOverlayTex = StereoUiOverlayTextureOfFrame();
    if (!mono && frameFlat && !StereoInFlatPhase() && g_flatCopy && g_flatPoseValid) {
        // Fotograma de salida de la fase plana: dibujado plano (ya sin menu)
        // pero el siguiente ya es estereo. No se ensena: la pantalla anclada
        // sigue con la copia del ultimo fotograma plano (el menu) un
        // fotograma mas, sobre negro, y el siguiente ya llega en estereo.
        if (SubmitBlackScene(g_sbsSnapshot)) {
            PumpOpenVRFrameTiming();
            return;
        }
        mono = true;
    } else if (!mono && frameFlat) {
        // Fotograma dibujado plano: va entero a la pantalla anclada. Un
        // fotograma dibujado en ESTEREO nunca pasa por aqui: se entrega como
        // estereo aunque el siguiente vaya a ser plano. (Se probo ensenar su
        // mitad izquierda en la pantalla anclada y se descarto: al pausar se
        // veia un instante el juego plano con otro encuadre.)
        if ((GetAsyncKeyState(VK_HOME) & 0x8000) != 0) g_flatPoseValid = false;   // recentrar
        if (!g_flatPoseValid) CaptureFlatPose();
        // La pantalla anclada ensena una COPIA del fotograma, para que en el
        // fotograma de salida no cambie aunque se vuelva a capturar el backbuffer.
        ID3D11Texture2D* shown = g_sbsSnapshot;
        if (g_flatCopy) { g_gameContext->CopyResource(g_flatCopy, g_sbsSnapshot); shown = g_flatCopy; }
        if (g_flatPoseValid && ShowFlatScreen(shown, g_flatPose, 2.0f * StereoUiHalfWidthM(), false)) {
            PumpOpenVRFrameTiming();
            return;
        }
        mono = true;   // sin overlay: al menos imagen plana identica en los dos ojos
        posePtr = nullptr;
    } else if (!mono && uiOverlayTex) {
        // Modo interfaz anclada (pantalla de combos): escena en estereo y la
        // interfaz, dibujada aparte, en el overlay anclado delante.
        if ((GetAsyncKeyState(VK_HOME) & 0x8000) != 0) g_flatPoseValid = false;
        if (!g_flatPoseValid) CaptureFlatPose();
        if (g_flatPoseValid) ShowAnchoredOverlay(uiOverlayTex, g_flatPose, 2.0f * StereoUiHalfWidthM(), false);
    } else {
        HideFlatScreen();
        g_flatPoseValid = false;   // al volver a la fase plana (menu principal) se recoloca delante
    }
    if (SubmitSideBySideTexture(g_sbsSnapshot, posePtr, mono)) g_sbsSubmits.fetch_add(1, std::memory_order_relaxed);
    PumpOpenVRFrameTiming();
}

// Sobrescribe el backbuffer con el ojo izquierdo centrado sobre negro, para
// que el monitor no muestre la imagen partida. Solo con el estereo activo y
// backbuffer sin multimuestreo (CopySubresourceRegion no admite MSAA).
static void MirrorLeftEyeToMonitor(ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc) {
    if (!kMirrorLeftEyeOnMonitor || StereoFrameWasFlat() || !g_sbsSnapshot) return;
    if (desc.SampleDesc.Count > 1 || (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) return;

    if (!g_monitorRtv || g_monitorRtvTexture != backbuffer) {
        if (g_monitorRtv) { g_monitorRtv->Release(); g_monitorRtv = nullptr; }
        g_monitorRtvTexture = nullptr;
        if (FAILED(g_gameDevice->CreateRenderTargetView(backbuffer, nullptr, &g_monitorRtv)) || !g_monitorRtv) return;
        g_monitorRtvTexture = backbuffer;
    }

    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_gameContext->ClearRenderTargetView(g_monitorRtv, black);

    UINT halfW = desc.Width / 2;
    D3D11_BOX leftEye{};
    leftEye.left = 0;
    leftEye.top = 0;
    leftEye.front = 0;
    leftEye.right = halfW;
    leftEye.bottom = desc.Height;
    leftEye.back = 1;
    g_gameContext->CopySubresourceRegion(backbuffer, 0, desc.Width / 4, 0, 0, g_sbsSnapshot, 0, &leftEye);
}

// ---------------------------------------------------------------------
// Present / ResizeBuffers
// ---------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
        if (pSwapChain != g_lastSwapChain) {
            g_lastSwapChain = pSwapChain;
            DXGI_SWAP_CHAIN_DESC desc{};
            if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
                g_gameWindow = desc.OutputWindow;
                std::ostringstream o;
                o << "[DX11] Swap chain " << desc.BufferDesc.Width << "x" << desc.BufferDesc.Height
                  << " formato=" << desc.BufferDesc.Format << " buffers=" << desc.BufferCount
                  << " ventana=" << (desc.Windowed ? "si" : "no");
                HookLogger::Instance().Line(o.str());
            }
            // Solo se adopta como cadena del juego una de tamano razonable: al
            // cargarnos tan pronto podemos ver antes alguna cadena auxiliar
            // (pequena) y engancharnos al contexto equivocado.
            bool looksLikeGame = (desc.BufferDesc.Width >= 800 && desc.BufferDesc.Height >= 600);
            if (!g_gameDevice && !looksLikeGame) {
                HookLogger::Instance().Line("[DX11] Cadena de intercambio auxiliar ignorada (demasiado pequena).");
            }
            if (!g_gameDevice && looksLikeGame) {
                ID3D11Device* device = nullptr;
                if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
                    g_gameDevice = device;                       // nos quedamos la referencia
                    device->GetImmediateContext(&g_gameContext); // idem
                    InstallContextHooks(device, "primer Present");   // por si el factory no se vio
                }
            }
        }
        g_presentCallCount++;
    }

    StereoEndFrame();   // decide la fase plana del fotograma siguiente (y sabe como se dibujo este)
    {
        D3D11_TEXTURE2D_DESC desc{};
        ID3D11Texture2D* backbuffer = CaptureBackbuffer(pSwapChain, &desc);
        if (backbuffer) {
            MirrorLeftEyeToMonitor(backbuffer, desc);
            backbuffer->Release();
        }
    }
    SubmitFrameToVR();
    StereoPollKeys();
    HeadTrackOnPresent();

    return g_originalPresent(pSwapChain, SyncInterval, Flags);
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
        ReleaseSbsSnapshot();   // la textura de captura depende del tamano del backbuffer
    }
    return g_originalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// Crea un device + swap chain minimos sobre una ventana oculta solo para
// leer la vtable de IDXGISwapChain (compartida con el swap chain del juego).
static bool CaptureSwapChainVTable(void** outPresentAddr, void** outResizeBuffersAddr) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "BladeVRDummyWndClass";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "BladeVR dummy", WS_OVERLAPPEDWINDOW,
                                0, 0, 4, 4, nullptr, nullptr, wc.hInstance, nullptr);
    bool ok = false;
    if (hwnd) {
        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferCount = 1;
        scd.BufferDesc.Width = 4;
        scd.BufferDesc.Height = 4;
        scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferDesc.RefreshRate.Numerator = 60;
        scd.BufferDesc.RefreshRate.Denominator = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = hwnd;
        scd.SampleDesc.Count = 1;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        IDXGISwapChain* swapChain = nullptr;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &scd, &swapChain, &device, &level, &context);
        if (SUCCEEDED(hr) && swapChain) {
            void** vtable = *reinterpret_cast<void***>(swapChain);
            *outPresentAddr = vtable[kSwapChainVTablePresent];
            *outResizeBuffersAddr = vtable[kSwapChainVTableResizeBuffers];
            ok = true;
            swapChain->Release();
        }
        if (context) context->Release();
        if (device) device->Release();
        DestroyWindow(hwnd);
    }
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return ok;
}

bool InstallDx11Hook() {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (g_installed) return true;

    if (!EnsureMinHookInitialized()) {
        HookLogger::Instance().Line("[DX11] ERROR: MH_Initialize fallo.");
        return false;
    }

    void* presentAddr = nullptr;
    void* resizeBuffersAddr = nullptr;
    if (!CaptureSwapChainVTable(&presentAddr, &resizeBuffersAddr)) {
        HookLogger::Instance().Line("[DX11] ERROR: no se pudo crear el swap chain dummy para leer la vtable.");
        return false;
    }

    bool ok = true;
    ok &= (MH_CreateHook(presentAddr, &HookedPresent, reinterpret_cast<void**>(&g_originalPresent)) == MH_OK);
    ok &= (MH_EnableHook(presentAddr) == MH_OK);
    ok &= (MH_CreateHook(resizeBuffersAddr, &HookedResizeBuffers, reinterpret_cast<void**>(&g_originalResizeBuffers)) == MH_OK);
    ok &= (MH_EnableHook(resizeBuffersAddr) == MH_OK);
    HookLogger::Instance().Line(ok ? "[DX11] Hooks de IDXGISwapChain::Present/ResizeBuffers instalados."
                                   : "[DX11] ERROR: fallo al instalar los hooks de Present/ResizeBuffers.");
    g_installed = ok;
    return ok;
}

void UninstallDx11Hook() {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (!g_installed) return;
    UninstallProjectionHook();
    UninstallStereoHook();
    ReleaseSbsSnapshot();
    if (g_gameContext) { g_gameContext->Release(); g_gameContext = nullptr; }
    if (g_gameDevice) { g_gameDevice->Release(); g_gameDevice = nullptr; }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_installed = false;
    HookLogger::Instance().Line("[DX11] Hooks desinstalados.");
}

void LogDx11Summary() {
    HookLogger::Instance().Line("DX11_SUMMARY PRESENTS=" + std::to_string(g_presentCallCount.load()) +
                                " VR_SUBMITS=" + std::to_string(g_sbsSubmits.load()) +
                                " DEVICE=" + (g_gameDevice ? "si" : "no"));
}

ID3D11Device* GetGameDevice() { return g_gameDevice; }
ID3D11DeviceContext* GetGameDeviceContext() { return g_gameContext; }
void* GetGameWindow() { return g_gameWindow; }

unsigned long long GetPresentCallCount() { return g_presentCallCount.load(std::memory_order_relaxed); }

} // namespace BladeVR
