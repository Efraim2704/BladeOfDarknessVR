#pragma once

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

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

// true mientras dura la fase plana de arranque (video de intro y carga
// inicial): todo se dibuja en la pantalla virtual y aun no hay escena 3D.
bool StereoInFlatPhase();
// true si el fotograma que se acaba de dibujar se dibujo en fase plana
// (imagen normal); false si se dibujo en estereo (lado a lado).
bool StereoFrameWasFlat();
// Avisos desde el hilo del juego: el juego acaba de escribir la pose de la
// camara / empieza una pasada de culling. Con ellos se sabe si la logica
// esta parada (menus) o viva (seleccion de personaje, partida).
void StereoNotifyCameraPose();
void StereoNotifyCullingPass();
// Textura RGBA con la interfaz del fotograma que se acaba de dibujar, si ese
// fotograma se dibujo en "modo interfaz anclada" (pantalla de combos de F1:
// escena 3D en estereo en el lado a lado, interfaz aparte para ponerla en un
// overlay anclado). nullptr si no.
ID3D11Texture2D* StereoUiOverlayTextureOfFrame();
// Llamar en Present ANTES de entregar el fotograma a SteamVR: cierra las
// cuentas del fotograma y decide la fase plana del siguiente.
void StereoEndFrame();
// Geometria de la pantalla virtual (la misma que usa la interfaz).
float StereoUiDistanceM();
float StereoUiHalfWidthM();

// Separacion entre ojos en unidades del juego (Re Pag / Av Pag, +-4).
float StereoGetEyeSeparationUnits();

// Tamano de la pantalla virtual de la interfaz: 0, 1 (por defecto) o 2 (Fin).
int StereoGetUiSizePreset();

// Llamar una vez por Present: teclas Re Pag / Av Pag / Fin.
void StereoPollKeys();


} // namespace BladeVR
