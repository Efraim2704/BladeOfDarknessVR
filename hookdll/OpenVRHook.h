#pragma once

struct ID3D11Texture2D;

namespace BladeVR {

// ---------------------------------------------------------------------
// Integracion con SteamVR (OpenVR).
//
// openvr_api.dll se carga DINAMICAMENTE (LoadLibrary + GetProcAddress sobre
// VR_InitInternal2 / VR_ShutdownInternal / VR_GetGenericInterface, los
// puntos de entrada C que usan por debajo los helpers de openvr.h) desde el
// MISMO directorio que BladeVR.dll. No se enlaza contra openvr_api.lib:
// asi Windows no exige la DLL al cargar el mod, y si falta (o SteamVR no
// esta corriendo) el juego sigue funcionando en modo normal.
//
// Orden por fotograma (en HookedPresent, ver Dx11Hook.cpp):
//   1) SubmitSideBySideTexture(frame, poseConLaQueSeDibujo)
//   2) PumpOpenVRFrameTiming()  -> WaitGetPoses, guarda la pose del visor
// El compositor reproyecta cada fotograma respecto a la pose que se le
// adjunta en el Submit (Submit_TextureWithPose); sin ella asume la de la
// ultima WaitGetPoses y se ven "chasquidos" (imagen doble un instante).
// ---------------------------------------------------------------------

// Lanza la inicializacion de OpenVR en un hilo propio (VR_Init es bloqueante
// y puede tardar). Devuelve true si el hilo se lanzo, no si VR esta listo:
// consultar IsOpenVRAvailable().
bool InstallOpenVRHook();
void UninstallOpenVRHook();

bool IsOpenVRAvailable();

// Entrega una textura lado a lado (mitad izquierda = ojo izquierdo) a los
// dos ojos de SteamVR. renderPose12: pose 3x4 del visor con la que se dibujo
// el fotograma (opcional). mono = la textura entera a los dos ojos (imagen
// plana, cuando el estereo esta apagado).
bool SubmitSideBySideTexture(ID3D11Texture2D* sbsTex, const float* renderPose12 = nullptr, bool mono = false);

// Tangentes del frustum de cada ojo (IVRSystem::GetProjectionRaw). El
// compositor asume que la textura de un ojo cubre exactamente ese frustum,
// asi que la proyeccion de cada ojo se construye con estos valores.
bool GetEyeProjectionRaw(bool isRightEye, float* left, float* right, float* top, float* bottom);

// WaitGetPoses: una vez por Present. Guarda la pose del visor.
void PumpOpenVRFrameTiming();

// Ultima pose del visor (matriz 3x4 fila-mayor de OpenVR: out12[fila*4+col];
// columnas = ejes del visor en el espacio de seguimiento, ultima columna =
// posicion en metros). false si aun no hay pose valida.
bool GetHmdPoseMatrix34(float* out12);

void LogOpenVRSummary();

} // namespace BladeVR
