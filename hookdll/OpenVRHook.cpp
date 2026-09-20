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

void UninstallOpenVRHook() {
    g_available = false;
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

void PumpOpenVRFrameTiming() {
    if (!g_available.load(std::memory_order_relaxed) || !g_vrCompositor) return;
    {
        std::lock_guard<std::mutex> lock(g_compositorMutex);
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        g_vrCompositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
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

void LogOpenVRSummary() {
    HookLogger::Instance().Line(
        "OPENVR_SUMMARY AVAILABLE=" + std::string(g_available.load() ? "true" : "false") +
        " SUBMITS=" + std::to_string(g_submitCount.load()) +
        " SUBMIT_FAILURES=" + std::to_string(g_submitFailureCount.load()) +
        " PUMP_COUNT=" + std::to_string(g_pumpCount.load()));
}

} // namespace BladeVR
