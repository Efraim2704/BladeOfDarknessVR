#pragma once

#include <string>

// ---------------------------------------------------------------------
// Ronda 54 (pedido del usuario): ajustes que se recuerdan entre partidas,
// juntos en BladeVR.ini (junto a Blade.exe):
//   modo_combate    ultimo modo de combate (0 VR basico, 1 mandos de
//                   movimiento, 2 mandos + combos, 3 manos flotantes); de serie 1
//   diorama         1 = se salio del juego en el diorama: se vuelve a entrar
//                   al empezar a jugar; de serie 0
//   altura_ojos_cm  altura anadida a los ojos en el modo de manos flotantes
//                   (Mayus + Re Pag / Av Pag); de serie 0
//   resistencia     (ronda 60) 1 = los golpes con los mandos gastan la
//                   resistencia como el ataque del juego (Mayus + F8); de serie 1
//   agarre_izquierda / agarre_derecha, alcance_izquierda / alcance_derecha
//                   (ronda 55) la calibracion de Ctrl+F8; sin ellas, la de serie
// Formato "clave=valor" (las lineas con ; o # son comentarios). Se lee una vez
// y se reescribe entero al cambiar un valor. Cualquier hilo.
// ---------------------------------------------------------------------

namespace BladeVR {

int IniGetInt(const char* key, int def);
bool IniSetInt(const char* key, int value);
bool IniGetString(const char* key, std::string* out);   // false si no esta
bool IniSetString(const char* key, const std::string& value);

} // namespace BladeVR
