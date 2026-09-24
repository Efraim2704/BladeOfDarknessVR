#include <windows.h>
#include <string>
#include "HookLogger.h"
#include "Dx11Hook.h"
#include "OpenVRHook.h"
#include "StereoHook.h"
#include "SceneCullingRootHook.h"
#include "HeadTrackHook.h"
#include "FovHook.h"

// ---------------------------------------------------------------------
// BladeVR.dll -- mod de realidad virtual para Severance: Blade of Darkness
// (remaster DX11 de 2021). Lo carga el proxy dxgi.dll (proxydll/) al
// arrancar Blade.exe.
//
// Modulos:
//   Dx11Hook              Present/ResizeBuffers/CreateSwapChain; copia del
//                         backbuffer (lado a lado) y entrega a SteamVR.
//   ProjectionHook        captura de la matriz de proyeccion (VS slot 0).
//   StereoHook            estereo lado a lado: los draws del mundo (ya
//                         dibujado por ojo) van a su mitad con el frustum de
//                         su ojo; el resto de draws 3D se emite dos veces.
//                         Fase plana, interfaz anclada, teclas.
//   OpenVRHook            SteamVR: poses, frustums por ojo, Submit.
//   HeadTrackHook         seguimiento de cabeza 6DOF: reescritura de la
//                         matriz de vista del juego (fromWorld) con la pose
//                         del visor, con colision contra el nivel.
//   SceneCullingRootHook  dibuja el mundo dos veces por fotograma, una desde
//                         cada ojo (recorte por portales, sombras y reflejos
//                         propios de cada ojo), y mantiene el culling
//                         coherente con la vista reescrita.
//   FovHook               FOV minimo: las camaras de seleccion de personaje
//                         y cinematicas piden FOVs estrechos (recuadro negro
//                         en el visor).
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
    log.Line(std::string("[BOOT] BladeVR ") + BladeVR::kBladeVRVersion + " cargado en PID " + std::to_string(pid));

    bool dx11Ok = BladeVR::InstallDx11Hook();
    log.Line(dx11Ok ? "[BOOT] Hooks DX11 instalados." : "[BOOT] Hooks DX11 NO pudieron instalarse.");

    bool cullOk = BladeVR::InstallSceneCullingRootHook();
    log.Line(cullOk ? "[BOOT] Hook de la raiz de culling instalado."
                    : "[BOOT] Hook de la raiz de culling NO pudo instalarse.");

    bool fovOk = BladeVR::InstallFovHook();
    log.Line(fovOk ? "[BOOT] Hook de FOV minimo instalado." : "[BOOT] Hook de FOV minimo NO pudo instalarse.");

    bool headOk = BladeVR::InstallHeadTrackHooks();
    log.Line(headOk ? "[BOOT] Seguimiento de cabeza instalado."
                    : "[BOOT] Seguimiento de cabeza NO pudo instalarse (ver [HEAD_TRACK]).");

    bool openVrOk = BladeVR::InstallOpenVRHook();
    log.Line(openVrOk ? "[BOOT] Inicializacion de OpenVR lanzada (asincrona)."
                      : "[BOOT] No se pudo lanzar la inicializacion de OpenVR; el juego sigue sin VR.");

    return 0;
}

// El mod solo tiene sentido dentro del juego: los proxys los carga cualquier
// ejecutable de la carpeta (lanzador, informador de fallos...), y ahi solo
// puede estorbar (hooks a ciegas y otra instancia peleandose por SteamVR).
static bool HostIsGame() {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') name = p + 1;
    }
    return lstrcmpiA(name, "Blade.exe") == 0;
}

static bool g_activeInThisProcess = false;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reasonForCall, LPVOID) {
    switch (reasonForCall) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            g_activeInThisProcess = HostIsGame();
            if (!g_activeInThisProcess) break;
            g_installThread = CreateThread(nullptr, 0, InstallThreadProc, nullptr, 0, nullptr);
            break;

        case DLL_PROCESS_DETACH:
            if (!g_activeInThisProcess) break;
            // Al cerrar, el proceso se esta muriendo: Windows libera todo de
            // todas formas. Aqui solo se apagan nuestros hooks y se deja de
            // hablar con OpenVR; NO se llama a VR_Shutdown ni se desinstala
            // nada mas. (Apagar OpenVR aqui terminaba en una excepcion dentro
            // de vrclient, con otros hilos del juego todavia dentro de sus
            // llamadas, y el proceso se quedaba colgado con su ventana
            // abierta.)
            BladeVR::LogDx11Summary();
            BladeVR::LogStereoSummary();
            BladeVR::HookLogger::Instance().Line("[SHUTDOWN] Proceso terminando.");
            BladeVR::StopOpenVRUse();
            if (g_installThread) { CloseHandle(g_installThread); g_installThread = nullptr; }
            break;

        default:
            break;
    }
    return TRUE;
}
