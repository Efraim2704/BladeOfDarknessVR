#pragma once

namespace BladeVR {

// ---------------------------------------------------------------------
// Seguimiento de cabeza 6DOF (ver el comentario de cabecera en
// HeadTrackHook.cpp). Teclas:
//   Insert   : seguimiento activo (por defecto) / apagado; cada activacion recentra.
//   Espacio  : recentra la posicion (la camara vuelve a los ojos del personaje).
//   Supr     : personaje independiente (vista libre) <-> el personaje
//              camina hacia donde mira el visor.
// ---------------------------------------------------------------------
bool InstallHeadTrackHooks();
void UninstallHeadTrackHooks();

// Llamar una vez por fotograma desde HookedPresent: teclas, lazo de
// seguimiento del personaje y log por segundo.
void HeadTrackOnPresent();

// 0 apagado, 1 activo.
int HeadTrackGetMode();

// Pose 3x4 del visor con la que se reescribio fromWorld por ultima vez
// (para Submit_TextureWithPose). false si el seguimiento esta apagado.
bool HeadTrackGetRenderPose(float* out12);

// Llamar desde el hook de +0x80f20 con su param_2: sincroniza el bloque de
// camara del culling con la fromWorld reescrita.
void HeadTrackSyncCullingCamera(long long param_2);

} // namespace BladeVR
