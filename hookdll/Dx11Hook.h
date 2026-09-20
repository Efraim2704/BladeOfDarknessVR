#pragma once
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace BladeVR {

// ---------------------------------------------------------------------
// Enganche a DirectX 11 / DXGI (ver Dx11Hook.cpp):
//   * IDXGISwapChain::Present y ResizeBuffers (vtable leida de un swap chain
//     "dummy" propio: todas las instancias comparten vtable);
//   * IDXGIFactory::CreateSwapChain, a partir del factory que nos pasa el
//     proxy dxgi.dll (BladeVR_OnDXGIFactoryCreated), para conocer el device
//     del juego ANTES del primer Present e instalar ahi los hooks del
//     contexto (ProjectionHook, StereoHook).
// En cada Present: copia del backbuffer (ya en formato lado a lado) y
// entrega a SteamVR, teclas y seguimiento de cabeza.
// ---------------------------------------------------------------------
bool InstallDx11Hook();
void UninstallDx11Hook();
void LogDx11Summary();

// Device y contexto inmediato del juego (validos tras el primer Present).
ID3D11Device* GetGameDevice();
ID3D11DeviceContext* GetGameDeviceContext();

// Numero de Present observados (se usa como identificador de fotograma).
unsigned long long GetPresentCallCount();

} // namespace BladeVR
