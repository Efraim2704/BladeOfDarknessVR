#pragma once
#include <d3d11.h>

namespace BladeVR {

// ---------------------------------------------------------------------
// Captura de la matriz de proyeccion del juego.
//
// Blade of Darkness (remaster DX11) transforma los vertices en la CPU: a la
// GPU llegan ya en espacio de camara, y TODA la geometria 3D (nivel, objetos,
// personaje, arma) pasa por una unica matriz de proyeccion perspectiva en el
// constant buffer de VS slot 0. La interfaz, los sprites y la composicion
// final usan otras matrices (ortograficas) en ese mismo slot.
//
// Este modulo hookea, sobre la vtable del ID3D11DeviceContext del juego:
//   VSSetConstantBuffers  -> que buffer esta vinculado ahora en cada slot
//   Map/Unmap y UpdateSubresource -> contenido de la ultima subida de cada
//                                    constant buffer pequeno
// y con ello responde a "que matriz esta activa en VS slot 0 en este draw",
// que es lo que StereoHook necesita para decidir si un draw es 3D y para
// derivar de ella las matrices de cada ojo.
// ---------------------------------------------------------------------
bool InstallProjectionHook(ID3D11DeviceContext* context);
void UninstallProjectionHook();

// Matriz (16 floats, vector fila, w = z) vinculada AHORA en VS slot 0 si es
// una perspectiva; false si el slot esta vacio, no se conoce su contenido o
// la matriz es ortografica (draw 2D).
bool ProjectionGetBoundVSSlot0Matrix(float out[16]);

// Buffer vinculado ahora en VS slot 0 (puede ser nullptr).
ID3D11Buffer* ProjectionGetBoundVSSlot0Buffer();

// Vincula un buffer en VS slot 0 llamando a la funcion ORIGINAL (sin pasar
// por el hook, para no perder la pista del buffer del juego).
void ProjectionBindVSSlot0Raw(ID3D11DeviceContext* ctx, ID3D11Buffer* buf);

} // namespace BladeVR
