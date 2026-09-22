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

// Deja de hablar con OpenVR sin apagarlo (para el cierre del proceso: apagar
// OpenVR con el juego todavia vivo terminaba en una excepcion en vrclient).
void StopOpenVRUse();

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

// Pantalla plana anclada al mundo para la fase de arranque (video de intro,
// carga inicial): un overlay de SteamVR con el fotograma entero, colocado con
// la transformacion screenPose12 (3x4, en el espacio de seguimiento del
// compositor; el cuadro mira hacia +z local) y de widthM metros de ancho,
// sobre una escena negra enviada al compositor. HideFlatScreen la oculta
// (idempotente).
// leftHalfOnly: la textura es lado a lado y solo se muestra la mitad izquierda
// (fotograma de transicion dibujado en estereo).
bool ShowFlatScreen(ID3D11Texture2D* frameTex, const float* screenPose12, float widthM, bool leftHalfOnly);
// Solo el overlay (sin escena negra): para poner la interfaz, dibujada en
// una textura RGBA aparte, anclada delante del usuario sobre la escena 3D en
// estereo que se entrega con SubmitSideBySideTexture.
bool ShowAnchoredOverlay(ID3D11Texture2D* tex, const float* screenPose12, float widthM, bool leftHalfOnly);
void HideFlatScreen();
// Solo la escena negra (el overlay se deja como este): para el fotograma de
// salida de la fase plana, en el que la pantalla anclada sigue ensenando el
// ultimo fotograma plano.
bool SubmitBlackScene(ID3D11Texture2D* like);

// Devuelve una interfaz de OpenVR por su version (la misma que usaria
// vr::VR_GetGenericInterface); nullptr si OpenVR no esta cargado. La usa
// VrInput para pedir IVRInput sin volver a cargar openvr_api.dll.
void* GetOpenVRInterface(const char* interfaceVersion);

// Abre el panel de SteamVR (boton Menu mantenido).
void OpenVRShowDashboard();

void LogOpenVRSummary();

} // namespace BladeVR
