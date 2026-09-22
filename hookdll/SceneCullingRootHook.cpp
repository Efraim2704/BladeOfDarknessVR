#include "SceneCullingRootHook.h"
#include "HeadTrackHook.h"
#include "FovHook.h"
#include "StereoHook.h"
#include "HookLogger.h"
#include <windows.h>
#include <MinHook.h>
#include <cstdint>

namespace BladeVR {

static constexpr uintptr_t kSceneCullingRootOffset = 0x80f20;

using PFN_SceneCullingRoot = void(__fastcall*)(long long, long long, long long);
static PFN_SceneCullingRoot g_originalSceneCullingRoot = nullptr;
static bool g_installed = false;

static void __fastcall HookedSceneCullingRoot(long long param_1, long long param_2, long long param_3) {
    FovClampOnGameThread();
    StereoNotifyCullingPass();
    HeadTrackSyncCullingCamera(param_2);
    g_originalSceneCullingRoot(param_1, param_2, param_3);
    HeadTrackRestoreCullingCamera(param_2);
}

bool InstallSceneCullingRootHook() {
    if (g_installed) return true;
    HMODULE hMain = GetModuleHandleA(nullptr);
    if (!hMain) return false;
    void* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hMain) + kSceneCullingRootOffset);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedSceneCullingRoot),
                      reinterpret_cast<void**>(&g_originalSceneCullingRoot)) != MH_OK) {
        HookLogger::Instance().Line("[SCENE_CULLING_ROOT] ERROR: MH_CreateHook fallo.");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        HookLogger::Instance().Line("[SCENE_CULLING_ROOT] ERROR: MH_EnableHook fallo.");
        return false;
    }
    g_installed = true;
    return true;
}

void UninstallSceneCullingRootHook() {
    if (!g_installed) return;
    HMODULE hMain = GetModuleHandleA(nullptr);
    if (!hMain) return;
    MH_DisableHook(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hMain) + kSceneCullingRootOffset));
    g_installed = false;
}

} // namespace BladeVR
