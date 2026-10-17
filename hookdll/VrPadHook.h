#pragma once
#include "OpenVRHook.h"

namespace BladeVR {

// ---------------------------------------------------------------------
// Mandos de Quest como mando del juego (version 2.0).
//
// El juego lee el mando por Steam Input (ISteamInput, SteamGamepad en
// +0x270fc0): pide los mandos conectados, el tipo de cada uno y, por nombre,
// 15 botones y 4 ejes del manifiesto game_actions_1710170.vdf. Aqui se le
// presenta un mando de Xbox One mas, el de los Quest, cambiando 7 entradas
// de la tabla virtual del interfaz (lo demas pasa al original, y el
// enganche del diorama sobre los datos del mando de Xbox sigue igual). Asi
// el juego lo trata exactamente como a un mando real: ataque, carga del
// lanzamiento, combos y menus.
//
// Modos de combate (CombatVR; se recuerda el ultimo en BladeVR.ini,
// "modo_combate", de serie el 1). Orden al cambiar de modo:
//   1 mandos de movimiento (con cuerpo): se ataca moviendo el arma (o con
//     punetazos con las manos libres); A no va al juego en partida.
//   3 manos flotantes (sin cuerpo): como el 1, con los antebrazos, las manos
//     y las armas siguiendo a los mandos sin limite del brazo.
//   2 mandos + combos: A y las direcciones hacen los combos del juego; el
//     gesto, el ataque basico.
//   0 VR basico: sin mandos de movimiento; A ataca como en el juego.
//   y vuelta al 1.
// Ronda 74: al pasar a 1, 3 o 2 la vista se pone en primera persona; al pasar
// al 0, se recentra como con Insert dos veces.
//
// Controles en el juego (fuera del diorama):
//   Joystick izq. = moverse; siempre andar (limitado a 0,85: el juego no
//   corre). Solo cuenta con el pulgar encima (toque del joystick; si ese toque
//   no llega, no cuenta con el pulgar en el reposapulgares): la deriva del
//   mando se ignora. Sin ninguna tecla del joystick pulsada en el juego se le
//   manda 0. Con un enemigo fijado va respecto al enemigo, como en el juego
//   (ronda 72): adelante hacia el, atras alejandose, a los lados rodeandolo.
//   Clic corto del joystick izq. = sprint (a fondo en la direccion del
//   joystick, si se inclina mas de la mitad) hasta soltarlo o volver a hacer
//   clic.
//   Joystick der. = camara. Clic corto del joystick der. = recentrar la vista
//   (como Inicio).
//   A = atacar (A del mando), salvo en partida en los modos 1 y 3 mientras el
//   ataque por gesto este listo: ahi A no va al juego. En el modo 2 CombatVR
//   suelta los brazos para que el combo mueva el arma.
//   X = usar.
//   B = saltar (subirse a mesas y cajas) solo con el joystick izq. hacia
//   delante o atras (con un enemigo fijado, hacia cualquier lado: esquivas);
//   si no (joystick en reposo o de lado), en partida solo alterna entre los
//   objetos cercanos.
//   Y = soltar al enemigo fijado (en partida; el juego no tiene boton para
//   eso).
//   En los menus, A, B e Y son los del mando.
//   Agarre izq. = bloquear (LB) solo en el modo 0 y en los menus. En partida
//   con los modos 1, 2 y 3 el agarre izq. no bloquea: se bloquea alzando el
//   escudo (delante, a la altura de los hombros o mas arriba y de cara hacia
//   delante, con el dorso de la mano izquierda hacia delante: LB mientras se
//   mantiene) o con un arma a dos manos cogida con las dos manos y alzada
//   (CombatVR). Con un arma a dos manos en la derecha (modos 1, 2 y 3) el
//   agarre izq. la sujeta con la mano izquierda mientras se mantiene
//   (CombatVR).
//   Agarre der. = lanzar (RB). En los modos 1 y 3, con el agarre der.
//   mantenido al menos 0,45 s, bajar deprisa el brazo derecho desde la altura
//   de la cabeza lanza (se manda A, como mantener lanzar + atacar en el
//   mando).
//   Gatillos = LT / RT (el derecho fija al enemigo). En partida con los modos
//   1, 2 y 3 y la mano derecha vacia, el gatillo der. fija sin sacar las armas
//   (lo hace CombatVR; el juego no lo recibe). Con el arco en los modos 1 y 3,
//   el gatillo der. apretado junto al arco agarra la cuerda (no va al juego).
//   Gesto de la espalda (modos 1, 2 y 3, con los brazos siguiendo a los
//   mandos): la mano por encima del hombro y detras de la cabeza y su agarre
//   guardan las armas o, si estan guardadas, las sacan (ese agarre no va al
//   juego hasta soltarlo). Con el arco en los modos 1 y 3, el agarre der. en
//   la espalda saca una flecha del carcaj (el izq. sigue guardando y sacando
//   las armas).
//   Pulgar izquierdo en el reposapulgares: joystick der. = cruceta (la camara
//   se queda quieta), A = Start (menu), B = Back (libro de viaje).
//   Pulgar izquierdo en reposo + clic del joystick der. mantenido 1 s = al
//   diorama (un clic corto recentra).
//   Pulgar derecho en reposo + clic del joystick izq. mantenido 1 s = otro
//   modo de combate (un clic corto es el sprint).
//   La vibracion que pide el juego llega a los mandos.
// Modo 0 (ronda 71): los Quest son un mando de Xbox. Joysticks sin tocar (solo
// la zona muerta del juego; a fondo corre, sin sprint ni toque del pulgar), A
// ataca, B salta y selecciona, Y se da la vuelta, agarre izq. = LB, clic izq. =
// L3 (cambia la capa de la maqueta, como el Xbox; ronda 72) y clic der. = R3.
// Recentrar = pulgar izquierdo en reposo + clic derecho corto. Pulgar
// izquierdo en reposo: cruceta, Start y Back, como en los otros modos.
// En el diorama los mandos son del diorama, como en la 1.6; pulgar izquierdo
// en reposo + clic del joystick der. (sin mantenerlo) vuelve al juego en
// primera persona. Ronda 62: pulgar derecho en reposo + clic del joystick izq.
// mantenido 1 s = los mandos manejan al personaje con los botones de fuera del
// diorama (joystick izq. andar y clic sprint, der. camara, A atacar, B saltar,
// X usar, Y darse la vuelta, agarres LB / RB, gatillos LT / RT, pulgar izq. en
// reposo: cruceta y Start / Back; clic der. recoloca la maqueta); lo mismo
// otra vez, vuelven a la maqueta.
// ---------------------------------------------------------------------

// Hilo de Present (OpenVRHook, tras leer los mandos y antes de guardarlos):
// plantillas, gestos y el estado del mando para el juego. Puede ocultar un
// clic de joystick que sea suyo. actions = se leyeron por IVRInput (sin uso).
void VrPadOnHands(VrHandState hands[2], bool actions);

// Hilo de Present (CombatVROnPresent, despues del DioramaPollKeys del mismo
// fotograma): engancha Steam Input cuando ya esta y ensena los avisos.
void VrPadOnPresent();

// Cualquier hilo: joystick izquierdo de los Quest que se manda al juego (tras
// la zona muerta). false si no hay mandos.
bool VrPadGetLeftStick(float sent[2]);

// Cualquier hilo: el sprint (clic del joystick izquierdo) esta activo con los
// mandos de Quest. Sin el, CombatVR mantiene Run = 0 (andar siempre).
bool VrPadSprintActive();

// Ronda 50, cualquier hilo: el gatillo derecho hace de cuerda del arco (lo
// tomo el mod al apretarlo junto al arco, CombatVRBowWantsTrigger) y esta
// apretado.
bool VrPadBowTriggerHeld();

// Ronda 62, cualquier hilo: en el diorama, los mandos de Quest manejan al
// personaje como el mando del juego (pulgar derecho en reposo + clic del
// joystick izquierdo mantenido 1 s; otra vez, vuelven a la maqueta). El diorama
// no ve entonces los mandos.
bool VrPadDioramaGameActive();

// Ronda 65, cualquier hilo: el asa de Steam Input es la del mando de los Quest
// (el juego puede tener otros mandos a la vez, p. ej. uno de Xbox).
bool VrPadIsQuestController(unsigned long long controllerHandle);

} // namespace BladeVR
