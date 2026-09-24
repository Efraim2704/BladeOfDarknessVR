#pragma once

namespace BladeVR {

// ---------------------------------------------------------------------
// Hook de B_Map::Render, Blade.exe+0x80f20: todo el mundo 3D pasa por aqui,
// una vez por fotograma, en el hilo del juego. Su param_2 es el bloque de
// pose de la camara:
//   param_2+0x68 : posicion de la camara en mundo (3 doubles) y angulos
//   param_2+0x98 : fromWorld (4x4 doubles)
// Aqui se sincroniza ese bloque con la fromWorld ya reescrita por el
// seguimiento de cabeza (HeadTrackSyncCullingCamera) y el mundo se dibuja
// DOS veces, una desde cada ojo, cada pasada en su mitad del render target
// (ver el comentario de cabecera de SceneCullingRootHook.cpp).
// ---------------------------------------------------------------------
bool InstallSceneCullingRootHook();
void UninstallSceneCullingRootHook();

// true mientras el mundo se dibuja desde cada ojo (vistas partidas puestas).
bool SceneEyeModeActive();
// Hilo de render: 0 / 1 si el viewport es la mitad izquierda / derecha de
// una vista partida por ojo (el draw ya viene de la camara de ese ojo);
// -1 si no.
int SceneEyeForViewport(float x, float y, float w, float h);
// Tiempo medio y maximo de una pasada del mundo desde la ultima consulta
// (para el resumen del log). Pone los contadores a cero.
void SceneTakePassStats(double* avgMs, double* maxMs);

} // namespace BladeVR
