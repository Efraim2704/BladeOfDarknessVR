#include <windows.h>
#include <string>
#include "HookLogger.h"
#include "Dx11Hook.h"
#include "OpenVRHook.h"
#include "StereoHook.h"
#include "SceneCullingRootHook.h"
#include "HeadTrackHook.h"

// ---------------------------------------------------------------------
// BladeVR.dll -- mod de realidad virtual para Severance: Blade of Darkness
// (remaster DX11 de 2021). Lo carga el proxy dxgi.dll (proxydll/) al
// arrancar Blade.exe.
//
// Modulos:
//   Dx11Hook              Present/ResizeBuffers/CreateSwapChain; copia del
//                         backbuffer (lado a lado) y entrega a SteamVR.
//   ProjectionHook        captura de la matriz de proyeccion (VS slot 0).
//   StereoHook            estereo por viewport partido: cada draw 3D se
//                         emite dos veces, una por ojo. Teclas de estereo.
//   OpenVRHook            SteamVR: poses, frustums por ojo, Submit.
//   HeadTrackHook         seguimiento de cabeza 6DOF: reescritura de la
//                         matriz de vista del juego (fromWorld) con la pose
//                         del visor, con colision contra el nivel.
//   SceneCullingRootHook  mantiene el culling del juego coherente con la
//                         vista reescrita (sombras en todas direcciones).
//   FromWorldLocator      localiza la matriz de vista global en memoria.
//   HookLogger            log en BladeVR_logs\ junto a Blade.exe.
//
// Todo el trabajo se hace en un hilo propio: DllMain no debe bloquear.
// ---------------------------------------------------------------------

static HANDLE g_installThread = nullptr;

static DWORD WINAPI InstallThreadProc(LPVOID) {
    DWORD pid = GetCurrentProcessId();
    auto& log = BladeVR::HookLogger::Instance();
    log.Init(pid);
    log.Line("[BOOT] BladeVR.dll cargado en PID " + std::to_string(pid));

    bool dx11Ok = BladeVR::InstallDx11Hook();
    log.Line(dx11Ok ? "[BOOT] Hooks DX11 instalados." : "[BOOT] Hooks DX11 NO pudieron instalarse.");

    bool cullOk = BladeVR::InstallSceneCullingRootHook();
    log.Line(cullOk ? "[BOOT] Hook de la raiz de culling instalado."
                    : "[BOOT] Hook de la raiz de culling NO pudo instalarse.");

    bool headOk = BladeVR::InstallHeadTrackHooks();
    log.Line(headOk ? "[BOOT] Seguimiento de cabeza instalado."
                    : "[BOOT] Seguimiento de cabeza NO pudo instalarse (ver [HEAD_TRACK]).");

    bool openVrOk = BladeVR::InstallOpenVRHook();
    log.Line(openVrOk ? "[BOOT] Inicializacion de OpenVR lanzada (asincrona)."
                      : "[BOOT] No se pudo lanzar la inicializacion de OpenVR; el juego sigue sin VR.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reasonForCall, LPVOID) {
    switch (reasonForCall) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            g_installThread = CreateThread(nullptr, 0, InstallThreadProc, nullptr, 0, nullptr);
            break;

        case DLL_PROCESS_DETACH:
            BladeVR::LogDx11Summary();
            BladeVR::LogOpenVRSummary();
            BladeVR::LogStereoSummary();
            BladeVR::HookLogger::Instance().Line("[SHUTDOWN] Proceso terminando. Desinstalando hooks.");
            BladeVR::UninstallOpenVRHook();
            BladeVR::UninstallSceneCullingRootHook();
            BladeVR::UninstallHeadTrackHooks();
            BladeVR::UninstallDx11Hook();
            if (g_installThread) { CloseHandle(g_installThread); g_installThread = nullptr; }
            break;

        default:
            break;
    }
    return TRUE;
}
