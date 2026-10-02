#include "OpenVRHook.h"
#include "HookLogger.h"
#include "openvr.h"
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <sstream>
#include <string>
#include <cstdint>
#include <mutex>

namespace BladeVR {

// Puntos de entrada C de openvr_api.dll, resueltos con GetProcAddress.
using PFN_VR_InitInternal2 = uint32_t(VR_CALLTYPE*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
using PFN_VR_ShutdownInternal = void(VR_CALLTYPE*)();
using PFN_VR_GetGenericInterface = void*(VR_CALLTYPE*)(const char*, vr::EVRInitError*);
using PFN_VR_IsHmdPresent = bool(VR_CALLTYPE*)();
using PFN_VR_GetVRInitErrorAsEnglishDescription = const char*(VR_CALLTYPE*)(vr::EVRInitError);

static HMODULE g_openvrDll = nullptr;
static PFN_VR_InitInternal2 g_VR_InitInternal2 = nullptr;
static PFN_VR_ShutdownInternal g_VR_ShutdownInternal = nullptr;
static PFN_VR_GetGenericInterface g_VR_GetGenericInterface = nullptr;
static PFN_VR_IsHmdPresent g_VR_IsHmdPresent = nullptr;
static PFN_VR_GetVRInitErrorAsEnglishDescription g_VR_GetVRInitErrorAsEnglishDescription = nullptr;

static vr::IVRSystem* g_vrSystem = nullptr;
static vr::IVRCompositor* g_vrCompositor = nullptr;
static vr::IVROverlay* g_vrOverlay = nullptr;
static vr::VROverlayHandle_t g_flatOverlay = vr::k_ulOverlayHandleInvalid;
static vr::VROverlayHandle_t g_toastOverlay = vr::k_ulOverlayHandleInvalid;   // aviso anclado al visor
static bool g_toastVisible = false;
static ID3D11Texture2D* g_blackTexture = nullptr;   // escena negra bajo la pantalla plana
static bool g_flatScreenVisible = false;
static std::atomic<bool> g_available{false};

static std::atomic<uint64_t> g_submitCount{0};
static std::atomic<uint64_t> g_submitFailureCount{0};
static std::atomic<uint64_t> g_pumpCount{0};

// Submit y WaitGetPoses nunca deben solaparse desde hilos distintos
// (IVRCompositor no es seguro para llamadas concurrentes).
static std::mutex g_compositorMutex;

// Ultima pose del visor devuelta por WaitGetPoses.
static std::mutex g_hmdPoseMutex;
static float g_hmdPoseMatrix[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
static bool g_hmdPoseValid = false;

static HANDLE g_installThread = nullptr;
static std::atomic<bool> g_installAttempted{false};

// Direccion de una funcion de ESTE DLL, para que GetModuleHandleEx nos de
// nuestro propio HMODULE y de ahi el directorio donde buscar openvr_api.dll.
static void SelfAddressMarker() {}

static bool GetOwnModuleDirectory(std::string* outDir) {
    HMODULE hSelf = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCSTR>(&SelfAddressMarker), &hSelf) || !hSelf) {
        return false;
    }
    char path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(hSelf, path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    std::string full(path, len);
    size_t lastSlash = full.find_last_of("\\/");
    if (lastSlash == std::string::npos) return false;
    *outDir = full.substr(0, lastSlash);
    return true;
}

static bool LoadOpenVRLibrary() {
    std::string dir;
    if (!GetOwnModuleDirectory(&dir)) {
        HookLogger::Instance().Line("[OPENVR] ERROR: no se pudo determinar el directorio de BladeVR.dll.");
        return false;
    }
    std::string dllPath = dir + "\\openvr_api.dll";
    g_openvrDll = LoadLibraryA(dllPath.c_str());
    if (!g_openvrDll) {
        HookLogger::Instance().Line("[OPENVR] ERROR: no se pudo cargar openvr_api.dll desde '" + dllPath +
                                     "'. Colocalo junto a BladeVR.dll.");
        return false;
    }
    g_VR_InitInternal2 = reinterpret_cast<PFN_VR_InitInternal2>(GetProcAddress(g_openvrDll, "VR_InitInternal2"));
    g_VR_ShutdownInternal = reinterpret_cast<PFN_VR_ShutdownInternal>(GetProcAddress(g_openvrDll, "VR_ShutdownInternal"));
    g_VR_GetGenericInterface =
        reinterpret_cast<PFN_VR_GetGenericInterface>(GetProcAddress(g_openvrDll, "VR_GetGenericInterface"));
    g_VR_IsHmdPresent = reinterpret_cast<PFN_VR_IsHmdPresent>(GetProcAddress(g_openvrDll, "VR_IsHmdPresent"));
    g_VR_GetVRInitErrorAsEnglishDescription = reinterpret_cast<PFN_VR_GetVRInitErrorAsEnglishDescription>(
        GetProcAddress(g_openvrDll, "VR_GetVRInitErrorAsEnglishDescription"));
    if (!g_VR_InitInternal2 || !g_VR_ShutdownInternal || !g_VR_GetGenericInterface || !g_VR_IsHmdPresent) {
        HookLogger::Instance().Line("[OPENVR] ERROR: openvr_api.dll cargada pero faltan puntos de entrada esperados.");
        FreeLibrary(g_openvrDll);
        g_openvrDll = nullptr;
        return false;
    }
    return true;
}

// VR_Init es bloqueante (y puede no volver si el runtime no esta listo), por
// eso corre en su propio hilo: el resto del mod no espera por el.
static DWORD WINAPI OpenVRInstallThreadProc(LPVOID) {
    auto& log = HookLogger::Instance();
    if (!LoadOpenVRLibrary()) return 1;

    bool hmdPresent = g_VR_IsHmdPresent();
    log.Line(std::string("[OPENVR] VR_IsHmdPresent() = ") + (hmdPresent ? "true" : "false"));

    vr::EVRInitError initError = vr::VRInitError_None;
    g_VR_InitInternal2(&initError, vr::VRApplication_Scene, nullptr);
    if (initError != vr::VRInitError_None) {
        std::string errDesc = g_VR_GetVRInitErrorAsEnglishDescription
                                   ? g_VR_GetVRInitErrorAsEnglishDescription(initError)
                                   : "(sin descripcion)";
        log.Line("[OPENVR] ERROR: VR_Init fallo, codigo=" + std::to_string(static_cast<int>(initError)) +
                 " (" + errDesc + "). Es normal si SteamVR no esta corriendo o no hay visor; el juego sigue sin VR.");
        FreeLibrary(g_openvrDll);
        g_openvrDll = nullptr;
        return 1;
    }

    vr::EVRInitError ifaceError = vr::VRInitError_None;
    g_vrSystem = static_cast<vr::IVRSystem*>(g_VR_GetGenericInterface(vr::IVRSystem_Version, &ifaceError));
    if (!g_vrSystem || ifaceError != vr::VRInitError_None) {
        log.Line("[OPENVR] ERROR: no se pudo obtener IVRSystem tras VR_Init.");
        g_VR_ShutdownInternal();
        FreeLibrary(g_openvrDll);
        g_openvrDll = nullptr;
        return 1;
    }
    g_vrCompositor = static_cast<vr::IVRCompositor*>(g_VR_GetGenericInterface(vr::IVRCompositor_Version, &ifaceError));
    if (!g_vrCompositor || ifaceError != vr::VRInitError_None) {
        log.Line("[OPENVR] ERROR: no se pudo obtener IVRCompositor tras VR_Init.");
        g_VR_ShutdownInternal();
        FreeLibrary(g_openvrDll);
        g_openvrDll = nullptr;
        g_vrSystem = nullptr;
        return 1;
    }

    uint32_t recWidth = 0, recHeight = 0;
    g_vrSystem->GetRecommendedRenderTargetSize(&recWidth, &recHeight);
    log.Line("[OPENVR] Inicializado. Resolucion recomendada por ojo: " + std::to_string(recWidth) + "x" +
             std::to_string(recHeight) + ".");
    g_available = true;
    return 0;
}

bool InstallOpenVRHook() {
    if (g_installAttempted.exchange(true)) return g_available.load(std::memory_order_relaxed);
    g_installThread = CreateThread(nullptr, 0, OpenVRInstallThreadProc, nullptr, 0, nullptr);
    if (!g_installThread) {
        HookLogger::Instance().Line("[OPENVR] ERROR: no se pudo crear el hilo de inicializacion de OpenVR.");
        return false;
    }
    return true;
}

void StopOpenVRUse() {
    g_available = false;
    std::lock_guard<std::mutex> lock(g_compositorMutex);
    g_vrCompositor = nullptr;
    g_vrSystem = nullptr;
    g_vrOverlay = nullptr;
}

void UninstallOpenVRHook() {
    g_available = false;
    HideFlatScreen();
    if (g_vrOverlay && g_flatOverlay != vr::k_ulOverlayHandleInvalid) g_vrOverlay->DestroyOverlay(g_flatOverlay);
    g_flatOverlay = vr::k_ulOverlayHandleInvalid;
    if (g_vrOverlay && g_toastOverlay != vr::k_ulOverlayHandleInvalid) g_vrOverlay->DestroyOverlay(g_toastOverlay);
    g_toastOverlay = vr::k_ulOverlayHandleInvalid;
    g_toastVisible = false;
    g_vrOverlay = nullptr;
    if (g_blackTexture) { g_blackTexture->Release(); g_blackTexture = nullptr; }
    if (g_VR_ShutdownInternal && g_vrSystem) g_VR_ShutdownInternal();
    g_vrSystem = nullptr;
    g_vrCompositor = nullptr;

    // Solo se libera la DLL si el hilo de inicializacion termino de verdad;
    // si sigue atascado en VR_Init, descargarla ahora causaria un fallo al
    // volver a memoria liberada (el proceso esta terminando de todas formas).
    bool installThreadFinished = true;
    if (g_installThread) {
        installThreadFinished = (WaitForSingleObject(g_installThread, 0) == WAIT_OBJECT_0);
        CloseHandle(g_installThread);
        g_installThread = nullptr;
    }
    if (g_openvrDll && installThreadFinished) {
        FreeLibrary(g_openvrDll);
        g_openvrDll = nullptr;
    }
    g_VR_InitInternal2 = nullptr;
    g_VR_ShutdownInternal = nullptr;
    g_VR_GetGenericInterface = nullptr;
    g_VR_IsHmdPresent = nullptr;
    g_VR_GetVRInitErrorAsEnglishDescription = nullptr;
}

bool IsOpenVRAvailable() {
    return g_available.load(std::memory_order_relaxed);
}

bool SubmitSideBySideTexture(ID3D11Texture2D* sbsTex, const float* renderPose12, bool mono) {
    if (!g_available.load(std::memory_order_relaxed) || !g_vrCompositor || !sbsTex) return false;

    vr::VRTextureWithPose_t textureWithPose{};
    textureWithPose.handle = sbsTex;
    textureWithPose.eType = vr::TextureType_DirectX;
    textureWithPose.eColorSpace = vr::ColorSpace_Auto;
    vr::EVRSubmitFlags flags = vr::Submit_Default;
    if (renderPose12) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) textureWithPose.mDeviceToAbsoluteTracking.m[r][c] = renderPose12[r * 4 + c];
        }
        flags = vr::Submit_TextureWithPose;
    }
    vr::Texture_t& texture = textureWithPose;

    // Mitad izquierda para el ojo izquierdo, mitad derecha para el derecho
    // (o la textura entera para ambos en modo mono).
    vr::VRTextureBounds_t leftBounds{0.0f, 0.0f, 0.5f, 1.0f};
    vr::VRTextureBounds_t rightBounds{0.5f, 0.0f, 1.0f, 1.0f};
    if (mono) {
        leftBounds = vr::VRTextureBounds_t{0.0f, 0.0f, 1.0f, 1.0f};
        rightBounds = leftBounds;
    }

    vr::EVRCompositorError errL;
    vr::EVRCompositorError errR;
    {
        std::lock_guard<std::mutex> lock(g_compositorMutex);
        errL = g_vrCompositor->Submit(vr::Eye_Left, &texture, &leftBounds, flags);
        errR = g_vrCompositor->Submit(vr::Eye_Right, &texture, &rightBounds, flags);
    }
    uint64_t callNum = g_submitCount.fetch_add(1, std::memory_order_relaxed);
    if (callNum < 5) {
        HookLogger::Instance().Line("[OPENVR] Submit SBS #" + std::to_string(callNum) +
                                     " errIzq=" + std::to_string(static_cast<int>(errL)) +
                                     " errDer=" + std::to_string(static_cast<int>(errR)));
    }
    if (errL != vr::VRCompositorError_None || errR != vr::VRCompositorError_None) {
        uint64_t n = g_submitFailureCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 5 || (n % 300 == 0)) {
            HookLogger::Instance().Line("[OPENVR] AVISO: Submit SBS fallo, errIzq=" +
                                         std::to_string(static_cast<int>(errL)) + " errDer=" +
                                         std::to_string(static_cast<int>(errR)));
        }
        return false;
    }
    return true;
}

bool GetEyeProjectionRaw(bool isRightEye, float* left, float* right, float* top, float* bottom) {
    if (!g_available.load(std::memory_order_relaxed) || !g_vrSystem) return false;
    if (!left || !right || !top || !bottom) return false;
    g_vrSystem->GetProjectionRaw(isRightEye ? vr::Eye_Right : vr::Eye_Left, left, right, top, bottom);
    return true;
}

bool GetHmdPoseMatrix34(float* out12) {
    if (!out12) return false;
    std::lock_guard<std::mutex> lock(g_hmdPoseMutex);
    if (!g_hmdPoseValid) return false;
    for (int i = 0; i < 12; ++i) out12[i] = g_hmdPoseMatrix[i];
    return true;
}

// --- mandos ------------------------------------------------------------------
static std::mutex g_handMutex;
static VrHandState g_hands[2] = {};
static bool g_gripLatch[2] = {false, false};
static bool g_triggerLatch[2] = {false, false};
static int32_t g_axisType[vr::k_unMaxTrackedDeviceCount][3];   // tipo de los ejes 0..2 (cache)
static bool g_axisTypeKnown[vr::k_unMaxTrackedDeviceCount] = {};
static constexpr float kAnalogPress = 0.80f;
static constexpr float kAnalogRelease = 0.55f;

// Solo con g_compositorMutex tomado (hilo de Present). Agarre: boton Grip o
// eje de tipo gatillo en el 2 (Touch de Meta); gatillo: boton Trigger o su
// eje (el 1). Con histeresis en los analogicos.
static void UpdateHandsLocked(const vr::TrackedDevicePose_t* poses) {
    if (!g_vrSystem) return;
    const vr::ETrackedControllerRole roles[2] = { vr::TrackedControllerRole_LeftHand,
                                                  vr::TrackedControllerRole_RightHand };
    VrHandState next[2] = {};
    for (int h = 0; h < 2; ++h) {
        vr::TrackedDeviceIndex_t idx = g_vrSystem->GetTrackedDeviceIndexForControllerRole(roles[h]);
        if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount) continue;
        if (!poses[idx].bPoseIsValid) continue;
        vr::VRControllerState_t st{};
        if (!g_vrSystem->GetControllerState(idx, &st, sizeof(st))) continue;
        if (!g_axisTypeKnown[idx]) {
            g_axisType[idx][0] = g_vrSystem->GetInt32TrackedDeviceProperty(idx, vr::Prop_Axis0Type_Int32);
            g_axisType[idx][1] = g_vrSystem->GetInt32TrackedDeviceProperty(idx, vr::Prop_Axis1Type_Int32);
            g_axisType[idx][2] = g_vrSystem->GetInt32TrackedDeviceProperty(idx, vr::Prop_Axis2Type_Int32);
            g_axisTypeKnown[idx] = true;
        }
        next[h].valid = true;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) next[h].pose[r * 4 + c] = poses[idx].mDeviceToAbsoluteTracking.m[r][c];
        }
        bool gripBit = (st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
        bool triggerBit = (st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
        float trigger = (g_axisType[idx][1] == vr::k_eControllerAxis_Trigger) ? st.rAxis[1].x : 0.0f;
        float grip = (g_axisType[idx][2] == vr::k_eControllerAxis_Trigger) ? st.rAxis[2].x : 0.0f;
        g_triggerLatch[h] = triggerBit || trigger > (g_triggerLatch[h] ? kAnalogRelease : kAnalogPress);
        g_gripLatch[h] = gripBit || grip > (g_gripLatch[h] ? kAnalogRelease : kAnalogPress);
        next[h].grip = g_gripLatch[h];
        next[h].trigger = g_triggerLatch[h];
        next[h].stickClick = (st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;
        if (g_axisType[idx][0] == vr::k_eControllerAxis_Joystick) {
            next[h].stickX = st.rAxis[0].x;
            next[h].stickY = st.rAxis[0].y;
        }
        next[h].buttonA = (st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_A)) != 0;
        next[h].buttonB = (st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0;
    }
    std::lock_guard<std::mutex> lock(g_handMutex);
    g_hands[0] = next[0];
    g_hands[1] = next[1];
}

bool GetVrHandState(int hand, VrHandState* out) {
    if (!out || hand < 0 || hand > 1) return false;
    if (!g_available.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(g_handMutex);
    *out = g_hands[hand];
    return out->valid;
}

void PumpOpenVRFrameTiming() {
    if (!g_available.load(std::memory_order_relaxed) || !g_vrCompositor) return;
    {
        std::lock_guard<std::mutex> lock(g_compositorMutex);
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        g_vrCompositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        UpdateHandsLocked(poses);
        const vr::TrackedDevicePose_t& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (hmd.bPoseIsValid) {
            std::lock_guard<std::mutex> poseLock(g_hmdPoseMutex);
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) g_hmdPoseMatrix[r * 4 + c] = hmd.mDeviceToAbsoluteTracking.m[r][c];
            }
            g_hmdPoseValid = true;
        }
    }
    g_pumpCount.fetch_add(1, std::memory_order_relaxed);
}

// --- pantalla plana (overlay) ----------------------------------------------
static bool EnsureOverlayInterface() {
    if (g_vrOverlay) return true;
    if (!g_VR_GetGenericInterface) return false;
    vr::EVRInitError ifaceError = vr::VRInitError_None;
    g_vrOverlay = static_cast<vr::IVROverlay*>(g_VR_GetGenericInterface(vr::IVROverlay_Version, &ifaceError));
    if (!g_vrOverlay || ifaceError != vr::VRInitError_None) {
        HookLogger::Instance().Line("[OPENVR] ERROR: no se pudo obtener IVROverlay.");
        g_vrOverlay = nullptr;
        return false;
    }
    return true;
}

static bool EnsureFlatOverlay() {
    if (g_flatOverlay != vr::k_ulOverlayHandleInvalid) return true;
    if (!EnsureOverlayInterface()) return false;
    vr::EVROverlayError err = g_vrOverlay->CreateOverlay("bladevr.flatscreen", "BladeVR flat screen", &g_flatOverlay);
    if (err != vr::VROverlayError_None) {
        std::ostringstream o;
        o << "[OPENVR] ERROR: CreateOverlay fallo (" << static_cast<int>(err) << ").";
        HookLogger::Instance().Line(o.str());
        g_flatOverlay = vr::k_ulOverlayHandleInvalid;
        return false;
    }
    g_vrOverlay->SetOverlayAlpha(g_flatOverlay, 1.0f);
    vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
    g_vrOverlay->SetOverlayTextureBounds(g_flatOverlay, &bounds);
    return true;
}

static bool EnsureBlackTexture(ID3D11Texture2D* like) {
    if (g_blackTexture) return true;
    ID3D11Device* device = nullptr;
    like->GetDevice(&device);
    if (!device) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    static const unsigned char zeros[64 * 64 * 4] = {};
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = zeros;
    init.SysMemPitch = 64 * 4;
    HRESULT hr = device->CreateTexture2D(&desc, &init, &g_blackTexture);
    device->Release();
    if (FAILED(hr)) { g_blackTexture = nullptr; return false; }
    return true;
}

bool ShowAnchoredOverlay(ID3D11Texture2D* frameTex, const float* screenPose12, float widthM, bool leftHalfOnly) {
    if (!g_available.load(std::memory_order_relaxed) || !g_vrCompositor || !frameTex || !screenPose12) return false;
    if (!EnsureFlatOverlay()) return false;

    vr::HmdMatrix34_t xform{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) xform.m[r][c] = screenPose12[r * 4 + c];
    }
    g_vrOverlay->SetOverlayWidthInMeters(g_flatOverlay, widthM);
    vr::VRTextureBounds_t bounds{0.0f, 0.0f, leftHalfOnly ? 0.5f : 1.0f, 1.0f};
    g_vrOverlay->SetOverlayTextureBounds(g_flatOverlay, &bounds);
    g_vrOverlay->SetOverlayTransformAbsolute(g_flatOverlay, g_vrCompositor->GetTrackingSpace(), &xform);
    vr::Texture_t texture{};
    texture.handle = frameTex;
    texture.eType = vr::TextureType_DirectX;
    texture.eColorSpace = vr::ColorSpace_Auto;
    g_vrOverlay->SetOverlayTexture(g_flatOverlay, &texture);
    if (!g_flatScreenVisible) {
        // Transformacion y textura ya estan puestas en esta misma llamada, asi
        // que se puede mostrar ya (sin esperar un fotograma, que dejaba un
        // parpadeo negro al entrar en el menu).
        g_vrOverlay->ShowOverlay(g_flatOverlay);
        g_flatScreenVisible = true;
    }
    return true;
}

bool ShowFlatScreen(ID3D11Texture2D* frameTex, const float* screenPose12, float widthM, bool leftHalfOnly) {
    if (!ShowAnchoredOverlay(frameTex, screenPose12, widthM, leftHalfOnly)) return false;
    return SubmitBlackScene(frameTex);
}

bool SubmitBlackScene(ID3D11Texture2D* like) {
    if (!g_available.load(std::memory_order_relaxed) || !g_vrCompositor || !like) return false;
    if (!EnsureBlackTexture(like)) return false;

    // Escena negra debajo (sin ella el compositor mostraria su entorno).
    vr::Texture_t black{};
    black.handle = g_blackTexture;
    black.eType = vr::TextureType_DirectX;
    black.eColorSpace = vr::ColorSpace_Auto;
    vr::VRTextureBounds_t full{0.0f, 0.0f, 1.0f, 1.0f};
    std::lock_guard<std::mutex> lock(g_compositorMutex);
    g_vrCompositor->Submit(vr::Eye_Left, &black, &full, vr::Submit_Default);
    g_vrCompositor->Submit(vr::Eye_Right, &black, &full, vr::Submit_Default);
    g_submitCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void HideFlatScreen() {
    if (!g_flatScreenVisible) return;
    g_flatScreenVisible = false;
    if (g_vrOverlay && g_flatOverlay != vr::k_ulOverlayHandleInvalid) g_vrOverlay->HideOverlay(g_flatOverlay);
    HookLogger::Instance().Line("[OPENVR] Pantalla plana ocultada.");
}

// --- aviso anclado al visor --------------------------------------------------
// Texto dibujado con GDI en un mapa de bits y subido con SetOverlayRaw a un
// overlay pegado al visor (SetOverlayTransformTrackedDeviceRelative), arriba
// a la derecha del campo de vision. Gris y blanco: el orden BGRA/RGBA da
// igual.
static constexpr int kToastW = 640;
static constexpr int kToastH = 150;
static constexpr float kToastWidthM = 0.26f;
static uint64_t g_toastHideTick = 0;
static unsigned char g_toastPixels[kToastW * kToastH * 4];

static bool RenderToastPixels(const wchar_t* title, const wchar_t* text) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kToastW;
    bi.bmiHeader.biHeight = -kToastH;   // de arriba abajo
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT all{0, 0, kToastW, kToastH};
    HBRUSH bg = CreateSolidBrush(RGB(22, 22, 22));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    RECT bar{0, 0, 10, kToastH};
    HBRUSH accent = CreateSolidBrush(RGB(200, 200, 200));
    FillRect(dc, &bar, accent);
    DeleteObject(accent);
    SetBkMode(dc, TRANSPARENT);
    HFONT fontTitle = CreateFontW(40, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT fontText = CreateFontW(58, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, fontTitle);
    SetTextColor(dc, RGB(175, 175, 175));
    RECT r1{34, 10, kToastW - 20, 62};
    DrawTextW(dc, title ? title : L"", -1, &r1, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, fontText);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r2{34, 62, kToastW - 20, kToastH - 10};
    DrawTextW(dc, text ? text : L"", -1, &r2, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    GdiFlush();
    const unsigned char* src = static_cast<const unsigned char*>(bits);
    for (int i = 0; i < kToastW * kToastH; ++i) {
        g_toastPixels[i * 4 + 0] = src[i * 4 + 2];
        g_toastPixels[i * 4 + 1] = src[i * 4 + 1];
        g_toastPixels[i * 4 + 2] = src[i * 4 + 0];
        g_toastPixels[i * 4 + 3] = 225;
    }
    SelectObject(dc, oldFont);
    DeleteObject(fontTitle);
    DeleteObject(fontText);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}

bool OpenVRShowToast(const wchar_t* title, const wchar_t* text, unsigned durationMs) {
    if (!g_available.load(std::memory_order_relaxed) || !g_vrCompositor) return false;
    if (!EnsureOverlayInterface()) return false;
    if (g_toastOverlay == vr::k_ulOverlayHandleInvalid) {
        vr::EVROverlayError err = g_vrOverlay->CreateOverlay("bladevr.toast", "BladeVR aviso", &g_toastOverlay);
        if (err != vr::VROverlayError_None) {
            g_toastOverlay = vr::k_ulOverlayHandleInvalid;
            return false;
        }
        g_vrOverlay->SetOverlayWidthInMeters(g_toastOverlay, kToastWidthM);
        g_vrOverlay->SetOverlaySortOrder(g_toastOverlay, 10);
        // Arriba a la derecha, a 60 cm, de cara al ojo.
        vr::HmdMatrix34_t xform{};
        xform.m[0][0] = 1.0f; xform.m[1][1] = 1.0f; xform.m[2][2] = 1.0f;
        xform.m[0][3] = 0.15f;
        xform.m[1][3] = 0.11f;
        xform.m[2][3] = -0.60f;
        g_vrOverlay->SetOverlayTransformTrackedDeviceRelative(g_toastOverlay, vr::k_unTrackedDeviceIndex_Hmd, &xform);
    }
    if (!RenderToastPixels(title, text)) return false;
    g_vrOverlay->SetOverlayRaw(g_toastOverlay, g_toastPixels, kToastW, kToastH, 4);
    g_vrOverlay->ShowOverlay(g_toastOverlay);
    g_toastVisible = true;
    g_toastHideTick = GetTickCount64() + durationMs;
    return true;
}

void OpenVRUpdateToast() {
    if (!g_toastVisible || GetTickCount64() < g_toastHideTick) return;
    g_toastVisible = false;
    if (g_vrOverlay && g_toastOverlay != vr::k_ulOverlayHandleInvalid) g_vrOverlay->HideOverlay(g_toastOverlay);
}

void* GetOpenVRInterface(const char* interfaceVersion) {
    if (!g_available.load(std::memory_order_relaxed) || !g_VR_GetGenericInterface || !interfaceVersion) return nullptr;
    vr::EVRInitError err = vr::VRInitError_None;
    void* iface = g_VR_GetGenericInterface(interfaceVersion, &err);
    return (err == vr::VRInitError_None) ? iface : nullptr;
}

void OpenVRShowDashboard() {
    if (!EnsureFlatOverlay() || !g_vrOverlay) return;
    g_vrOverlay->ShowDashboard("");
}

void LogOpenVRSummary() {
    HookLogger::Instance().Line(
        "OPENVR_SUMMARY AVAILABLE=" + std::string(g_available.load() ? "true" : "false") +
        " SUBMITS=" + std::to_string(g_submitCount.load()) +
        " SUBMIT_FAILURES=" + std::to_string(g_submitFailureCount.load()) +
        " PUMP_COUNT=" + std::to_string(g_pumpCount.load()));
}

} // namespace BladeVR
