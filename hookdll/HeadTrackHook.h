#pragma once

namespace BladeVR {

// ---------------------------------------------------------------------
// Seguimiento de cabeza 6DOF (ver el comentario de cabecera en
// HeadTrackHook.cpp). Teclas:
//   Insert   : seguimiento activo (por defecto) / apagado; cada activacion recentra.
//   Inicio   : recentra la posicion (la camara vuelve a los ojos del personaje).
//   Supr     : el personaje camina hacia donde mira el visor (por defecto
//              desde la 2.0) <-> personaje independiente (vista libre). El
//              giro le llega solo al juego; el raton de Windows no se mueve.
//   Ctrl + Re Pag / Av Pag  : ojos adelantados +-1 cm (0-20; de serie 10).
//   Mayus + Re Pag / Av Pag : altura anadida a los ojos en el modo de manos
//              flotantes, +-5 cm (0-80; de serie 0; se guarda en BladeVR.ini).
//   (Estas dos las lee StereoHook.cpp y llaman a las funciones de abajo.)
//
// Ronda 29: con una camara que no es la de partida (cinematicas, seleccion de
// personaje, menu) la vista sigue la guinada de esa camara mas el giro del
// visor desde que empezo la escena (Inicio la vuelve a alinear), con Supr o
// sin el; Supr no gira al personaje ahi. Los ojos adelantados, la altura
// suavizada y el desplazamiento del visor ya no se paran en el borde del
// sector de la camara del juego (los sectores de partida del dibujo salen de
// la posicion de cada ojo), solo al salir del nivel.
//
// Ronda 37: con un enemigo fijado, los ojos anclados van siempre detras de
// hacia donde mira el personaje (antes saltaban 30 cm adelante si la camara
// del juego quedaba delante) y los ojos adelantados, en esa misma direccion.
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

// true si la ultima reescritura de fromWorld fue la camara del modo diorama
// (DioramaHook.cpp) y el seguimiento esta activo.
bool HeadTrackDioramaCameraActive();
// Recentrar la posicion en el siguiente fotograma, como Inicio (al salir del
// diorama, para que la vista vuelva a los ojos del personaje).
void HeadTrackRequestRecenter();
// Ronda 26: ojos adelantados respecto a la camara de primera persona del juego
// (que va 0,15 m por detras de su punto en la cabeza: al mirar abajo se veia
// el cuello). Ctrl + Re Pag / Av Pag: +-1 cm (0-20; de serie 10). Hilo de Present.
void HeadTrackAdjustEyeForward(int deltaCm);
// Ronda 46: altura anadida a los ojos sobre la camara del juego (0-80 cm; se
// guarda en BladeVR.ini, altura_ojos_cm). Mayus + Re Pag / Av Pag: +-5 cm. Como
// subirse a algo: el suelo se ve mas lejos y los brazos y las armas siguen a
// tamano real. Ronda 47: solo en el modo sin cuerpo (el de las manos flotantes);
// ronda 54: de serie 0. Hilo de Present.
void HeadTrackAdjustEyeLift(int deltaCm);
// Esa altura en metros, 0 fuera del modo sin cuerpo (la usa CombatVR para no
// avisar de "visor por encima" y para el escudo).
double HeadTrackEyeLiftM();

// Paso del espacio de seguimiento de SteamVR al mundo del juego, tal como se
// uso en la ultima reescritura de fromWorld de la camara de partida (sin
// diorama y con desplazamiento posicional). Para un punto p del espacio de
// seguimiento (metros): conv(p) = (p.x, -p.y, -p.z);
// punto_juego = cam + RotY(conv(p) - hmd) * unitsPerMeter, con
// RotY(v) = (v.x*cosYaw + v.z*sinYaw, v.y, -v.x*sinYaw + v.z*cosYaw).
// Para los mandos (CombatVRHook.cpp). false si no hay transformacion valida.
struct TrackingToGameMap {
    double cam[3];         // posicion de la camara escrita (unidades del juego)
    double hmd[3];         // posicion del visor con que se escribio (conv, metros)
    double cosYaw;
    double sinYaw;
    double unitsPerMeter;
    unsigned long long tick;   // GetTickCount64 de la escritura
    double base[3];        // camara del propio juego, antes de sumar el visor
    double right[3];       // ejes de la camara escrita en el mundo del juego
    double down[3];
    double fwd[3];
};
bool HeadTrackGetTrackingToGameMap(TrackingToGameMap* out);

// Ronda 70, cualquier hilo: pone Supr (true = el personaje camina hacia donde
// mira el visor; false = vista libre, como el juego). CombatVR lo pone segun el
// modo de combate; la tecla Supr lo sigue cambiando.
void HeadTrackSetWalkFollowsView(bool on, int combatMode);

// Ronda 74, cualquier hilo: recentra la vista como Insert pulsado dos veces
// (guinada y posicion del visor se toman de nuevo en el siguiente fotograma;
// el seguimiento de cabeza se queda como estaba). CombatVR lo llama al pasar
// al modo de combate 0.
void HeadTrackRecenterLikeInsert();

} // namespace BladeVR
