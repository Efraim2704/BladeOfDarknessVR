#pragma once

namespace BladeVR {

// ---------------------------------------------------------------------
// Seguimiento de cabeza 6DOF (ver el comentario de cabecera en
// HeadTrackHook.cpp). Teclas:
//   Insert   : seguimiento activo (por defecto) / apagado; cada activacion recentra.
//   Inicio   : recentra la posicion (la camara vuelve a los ojos del personaje).
//   Supr     : personaje independiente (vista libre) <-> el personaje
//              camina hacia donde mira el visor (el giro le llega solo al
//              juego; el raton de Windows no se mueve).
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
// Olvida las referencias de guinada y posicion del visor (se toman de nuevo
// en el siguiente fotograma): se llama al empezar un nivel, para que la
// vista salga centrada mire donde mirase el usuario durante la carga.
void HeadTrackResetReferences();

// Guinada (rad) de la direccion "al frente": la que el visor miraba cuando se
// centro la vista (arranque de nivel, Insert o Inicio). Misma convencion que
// el juego: 0 = -z del espacio de seguimiento, positiva hacia +x. false si el
// seguimiento esta apagado, si aun no hay referencia o si el personaje camina
// hacia donde mira (ahi el frente es la propia mirada).
bool HeadTrackGetCenterYawRad(double* outYaw);
// Llamar justo despues de la funcion original: devuelve al objeto de pose la
// posicion y los angulos del juego (que el culling vio con el giro del visor).
void HeadTrackRestoreCullingCamera(long long param_2);

} // namespace BladeVR
