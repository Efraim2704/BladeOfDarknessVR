#include "FovHook.h"
#include "HookLogger.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <sstream>

namespace BladeVR {

static constexpr uintptr_t kSetFovOffset = 0x55510;   // void SetFOV(App* app, float grados)
static constexpr float kMinFovDegrees = 145.0f;       // el maximo que admite el menu del juego
static constexpr uintptr_t kAppPointerOffset = 0xF4A6A8; // Blade.exe+0xF4A6A8 -> objeto de aplicacion
static constexpr uintptr_t kAppFovFactorOffset = 0x28c;  // float 1/tan(FOV/2) dentro del objeto de aplicacion
static constexpr uintptr_t kAppCameraEntityOffset = 0xE8; // app+0xE8 -> entidad Camera
static constexpr uintptr_t kCameraFovFactorOffset = 0x638; // copia del factor en la entidad Camera
// 1/tan(145/2 grados): factor MAXIMO admitido (a menor factor, mayor FOV).
static constexpr float kMaxFovFactor = 0.31529877f;
static std::atomic<uint64_t> g_clampsApplied{0};

using PFN_SetFov = void(__fastcall*)(long long app, float degrees);
static PFN_SetFov g_originalSetFov = nullptr;
static bool g_installed = false;
static std::atomic<uint32_t> g_calls{0};
static float g_lastLoggedDegrees = -1.0f;

static void __fastcall HookedSetFov(long long app, float degrees) {
    float applied = degrees;
    if (applied < kMinFovDegrees) applied = kMinFovDegrees;
    uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed);
    // Log solo cuando cambia el valor pedido (las cinematicas lo llaman cada
    // fotograma con un zoom progresivo) y como mucho unas pocas veces.
    if (degrees != g_lastLoggedDegrees && n < 64) {
        g_lastLoggedDegrees = degrees;
        std::ostringstream o;
        o << "[FOV] SetFOV pedido=" << degrees << " aplicado=" << applied;
        HookLogger::Instance().Line(o.str());
    }
    g_originalSetFov(app, applied);
}

static bool SafeResolveAppFovAddress(uintptr_t* out) {
    __try {
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        uintptr_t app = *reinterpret_cast<uintptr_t*>(base + kAppPointerOffset);
        if (!app) return false;
        volatile float touch = *reinterpret_cast<float*>(app + kAppFovFactorOffset);
        (void)touch;
        *out = app + kAppFovFactorOffset;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Los controladores de camara de la seleccion de personaje (+0x58c18) y de
// las cinematicas (+0x5952a, zoom progresivo) escriben el factor de FOV en
// app+0x28c CADA fotograma sin pasar por SetFOV, asi que se recorta en el
// hilo del juego justo antes de que el motor lo consuma (raiz del culling y
// escritor de fromWorld). POD-only (usa __try).
static bool SafeClampFovFactor() {
    __try {
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        uintptr_t app = *reinterpret_cast<uintptr_t*>(base + kAppPointerOffset);
        if (!app) return false;
        bool clamped = false;
        float* appFactor = reinterpret_cast<float*>(app + kAppFovFactorOffset);
        if (*appFactor > kMaxFovFactor) { *appFactor = kMaxFovFactor; clamped = true; }
        uintptr_t camera = *reinterpret_cast<uintptr_t*>(app + kAppCameraEntityOffset);
        if (camera) {
            float* camFactor = reinterpret_cast<float*>(camera + kCameraFovFactorOffset);
            if (*camFactor > kMaxFovFactor) { *camFactor = kMaxFovFactor; clamped = true; }
        }
        return clamped;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FovClampOnGameThread() {
    if (!SafeClampFovFactor()) return;
    uint64_t n = g_clampsApplied.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 1000 == 0) {
        HookLogger::Instance().Line("[FOV] factor de FOV recortado a 145 grados (" + std::to_string(n) + " veces).");
    }
}

bool InstallFovHook() {
    if (g_installed) return true;
    HMODULE hMain = GetModuleHandleA(nullptr);
    if (!hMain) return false;
    void* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hMain) + kSetFovOffset);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedSetFov),
                      reinterpret_cast<void**>(&g_originalSetFov)) != MH_OK) {
        HookLogger::Instance().Line("[FOV] ERROR: MH_CreateHook fallo.");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        HookLogger::Instance().Line("[FOV] ERROR: MH_EnableHook fallo.");
        return false;
    }
    g_installed = true;
    return true;
}

void UninstallFovHook() {
    if (!g_installed) return;
    HMODULE hMain = GetModuleHandleA(nullptr);
    if (hMain) MH_DisableHook(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hMain) + kSetFovOffset));
    g_installed = false;
}

} // namespace BladeVR
