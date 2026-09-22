#pragma once

namespace BladeVR {

// ---------------------------------------------------------------------
// FOV minimo. El juego calcula TODO (matriz de proyeccion y recorte de
// geometria en la CPU) a partir de un unico valor: 1/tan(FOV/2), que fija la
// funcion SetFOV(app, grados) de Blade.exe (+0x55510). El menu la llama con
// el ajuste del usuario, pero la seleccion de personaje y las cinematicas la
// llaman desde los scripts del juego con FOVs estrechos (incluido un zoom
// progresivo), y con el visor eso se ve como un recuadro rodeado de negro.
// Este hook impone un FOV minimo a cualquier peticion, para que el motor
// siempre genere geometria para todo el campo de vision del visor.
// ---------------------------------------------------------------------
bool InstallFovHook();
void UninstallFovHook();

// Recorta el factor de FOV del juego (app+0x28c y su copia en la entidad
// Camera) al equivalente de 145 grados. Llamar desde el hilo del juego, una
// vez por fotograma, antes del culling (SceneCullingRootHook) y tras cada
// escritura de fromWorld (HeadTrackHook).
void FovClampOnGameThread();

} // namespace BladeVR
