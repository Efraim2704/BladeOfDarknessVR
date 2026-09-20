#pragma once

struct ID3D11DeviceContext;

namespace BladeVR {

// ---------------------------------------------------------------------
// Estereo por viewport partido (ver el comentario de cabecera de
// StereoHook.cpp). Hookea Draw/DrawIndexed en la vtable del contexto del
// juego y emite cada draw 3D dos veces, una por ojo, en cada mitad del
// render target. Tambien guarda los ajustes de usuario y lee sus teclas.
// ---------------------------------------------------------------------
bool InstallStereoHook(ID3D11DeviceContext* context);
void UninstallStereoHook();
void LogStereoSummary();

// Estereo activo (true) o imagen plana (false). Arranca apagado y se activa
// solo al llegar al menu (ver StereoPollKeys); F5 alterna.
bool StereoIsEnabled();

// Separacion entre ojos en unidades del juego (Re Pag / Av Pag, +-4).
float StereoGetEyeSeparationUnits();

// Tamano de la pantalla virtual de la interfaz: 0, 1 (por defecto) o 2 (Fin).
int StereoGetUiSizePreset();

// Llamar una vez por Present: teclas F5 / Re Pag / Av Pag / Fin y arranque
// automatico del estereo.
void StereoPollKeys();

} // namespace BladeVR
