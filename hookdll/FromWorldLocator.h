#pragma once

namespace BladeVR {

// Puntero a la matriz de vista global fromWorld (4x4 doubles): filas
// [0..2]=derecha, [4..6]=arriba, [8..10]=adelante, [12..14]=-(c*M), [15]=1.
// Se resuelve la primera vez que se pide (busqueda alrededor del objeto de
// aplicacion); nullptr si aun no se encuentra.
double* TryGetOrResolveFromWorldPointer();

} // namespace BladeVR
