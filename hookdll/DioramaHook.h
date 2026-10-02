#pragma once

namespace BladeVR {

// ---------------------------------------------------------------------
// Modo diorama (ver el comentario de cabecera de DioramaHook.cpp).
//
// Teclado (las del diorama, con el diorama puesto salvo F5, F6 y F7):
//   F5            : diorama si / no (al entrar, la maqueta delante con el
//                   personaje en el centro).
//   Inicio        : recolocar la maqueta delante, centrada en el personaje y
//                   a 1:15 (para localizarlo). En una cinematica: volver a su
//                   camara conservando el tamano.
//   Re Pag/Av Pag : maqueta mas grande / mas pequena.
//   F4            : cono de vision: apagado -> pequeno -> mediano -> grande.
//   F6            : area visible: mapa entero, 60, 30, 15 m alrededor.
//   F7            : fondo: el del juego, magenta o verde (clave de color del
//                   passthrough de Virtual Desktop).
//   Pausa         : personajes en los cortes: siempre a la vista / se cortan.
//   Supr          : mover al personaje respecto a la mirada sobre la maqueta
//                   (por defecto) o respecto a la camara del juego.
// Mandos de Quest:
//   un grip           : llevar la maqueta con esa mano.
//   los dos grips     : escalar (juntar / separar las manos) y girar la
//                       maqueta; el punto entre las manos se queda quieto.
//   un gatillo        : cortes. El primer eje en que la mano pasa de 5 cm
//                       decide cual se mueve en ese gesto: subir / bajar la
//                       mano, corte de altura; acercarla a la maqueta o
//                       traerla hacia uno, corte vertical en el lado de la
//                       maqueta que queda de cara (recto, como las paredes).
//   los dos gatillos  : tamano del area visible (muy separadas: mapa entero).
//   stick izquierdo   : mover la mesa en horizontal respecto a la mirada;
//                       clic corto: modo; clic largo: cono (como F4).
//   stick derecho     : izquierda / derecha rodear la maqueta; arriba / abajo
//                       zoom; clic corto: area (mapa entero -> 50 m -> 20 m;
//                       "mapa entero" quita tambien los cortes); clic largo:
//                       personajes en los cortes.
//   A / B (derecho)   : bajar / subir la mesa.
//   X (izquierdo)     : recolocar (como Inicio).
//   Y (izquierdo)     : passthrough si / no con el ultimo color.
// Mando de Xbox (Steam Input): L3 cambia todo el mando entre el juego y la
// maqueta (el juego no recibe L3). Con la maqueta: R3 diorama si / no (R3
// mantenido: personajes en los cortes), X recolocar, LB modo, RB area,
// cruceta izquierda / derecha cono si / no, cruceta arriba / abajo ancho del
// cono, Y passthrough, A corte vertical delante del personaje (A mantenido:
// corte recto o que gira con la maqueta, en el modo por la espalda), B corte
// de altura sobre el personaje, stick izquierdo mover la mesa, stick derecho
// rodear y zoom, RT / LT zoom.
// Modos de la maqueta: 1 fija; 2 acoplada al personaje (por defecto: el
// punto de la mesa le sigue); 3 acoplada por la espalda (la maqueta gira
// despacio hasta verse desde la camara del juego).
// Durante una cinematica la vista es la camara de la cinematica y solo
// funcionan el zoom, el cono, los cortes, el area, los personajes en los
// cortes y el passthrough. Cada cambio sale unos segundos en un aviso en el
// visor, tambien en las cinematicas (no hay aviso de inicio ni de fin).
// ---------------------------------------------------------------------
bool InstallDioramaHooks();
void UninstallDioramaHooks();

// Hilo de Present, una vez por fotograma (despues de WaitGetPoses): teclas,
// mandos y avisos.
void DioramaPollKeys();

// true si el usuario tiene puesto el diorama (F5).
bool DioramaEnabled();
// La maqueta se vuelve a colocar en el siguiente fotograma (nivel nuevo).
void DioramaRequestAnchor();
// Hilo del juego (HeadTrackHook), justo antes de DioramaComputeCamera con la
// camara de una cinematica: esa llamada sigue la camara del juego (su
// recorrido) en vez de la maqueta. Ver DioramaComputeCamera.
void DioramaMarkCinematicCall();
// true mientras dura una cinematica en el diorama (cualquier hilo): los
// mandos no mueven ni giran la maqueta (solo zoom, cono y cortes).
bool DioramaCinematicActive();
// Escala de la maqueta: metros del juego por metro real (10 = 1:10).
double DioramaScale();

// Hilo del juego (HeadTrackHook, tras el escritor de fromWorld): matriz de
// vista del diorama a partir de la pose del visor (3x4 de OpenVR) y de la
// fromWorld que acaba de escribir el juego. recenter = Inicio. false si no
// se puede calcular (se usa entonces el seguimiento normal).
bool DioramaComputeCamera(const float hmd34[12], const double gameFromWorld[16], bool recenter,
                          double outFromWorld[16]);

// Hilo del juego (SceneCullingRootHook), alrededor de la pasada del mundo.
// active = diorama puesto y la camara de este fotograma es la suya;
// pose = segundo argumento de B_Map::Render (fromWorld en +0x98);
// renderArg3 = tercero (frustum en +0x18).
void DioramaBeginWorld(bool active, long long pose, long long renderArg3);
void DioramaEndWorld();

// Hilo de render: los draws del mundo que se estan dibujando son del
// diorama (el nivel necesita prueba de profundidad y la separacion entre
// ojos va multiplicada por la escala).
bool DioramaDrawActive();

// false en el modo "vista por la espalda": la maqueta sigue a la camara del
// juego, asi que HeadTrackHook no debe girar esa camara hacia la mirada.
bool DioramaViewSteeringAllowed();

} // namespace BladeVR
