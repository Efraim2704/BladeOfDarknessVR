#pragma once

namespace BladeVR {

// ---------------------------------------------------------------------
// Hook de la raiz del culling de escena, Blade.exe+0x80f20 (FUN_140080f20):
// una llamada por fotograma, en el hilo del juego, justo antes del
// recorrido de portales. Su param_2 es el bloque de camara del culling:
//   param_2+0x98 : rotacion 3x4 (copia de fromWorld[0..11])
//   param_2+0x68 : posicion de la camara en mundo (3 doubles) y angulos
//                  ([3] guinada = atan2(fila2.x, fila2.z), [4] = -asin(fila1.z))
// Aqui se sincroniza ese bloque con la fromWorld ya reescrita por el
// seguimiento de cabeza (HeadTrackSyncCullingCamera): sin ello, el
// culling de sombras/entidades usa la orientacion de la camara del juego
// y las sombras solo aparecen hacia donde mira el personaje.
// ---------------------------------------------------------------------
bool InstallSceneCullingRootHook();
void UninstallSceneCullingRootHook();

} // namespace BladeVR
