#include "CombatVRHook.h"
#include "BladeVRIni.h"
#include "CombatVRIk.h"
#include "HookLogger.h"
#include "HeadTrackHook.h"
#include "OpenVRHook.h"
#include "DioramaHook.h"
#include "VrPadHook.h"
#include "VrInput.h"
#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

// ---------------------------------------------------------------------
// Combate con los mandos de movimiento (version 2.0; los controles y el
// historial por rondas, en CombatVRHook.h).
//
// Brazos: las manos del personaje van a donde estan los mandos (cinematica
// inversa de los dos brazos, o manos flotantes en el modo 3) en primera
// persona, con el cuerpo bajo el visor. Se enganchan los dos evaluadores
// del esqueleto:
//   +0x1394f0 perezoso, nodo a nodo (posicion de lo enganchado, camara...)
//   +0x139100 todos los nodos en orden (dibujo y malla de los golpes)
// Cuando alguien pide un nodo de los brazos del jugador y aun no esta
// calculado para este fotograma, se calcula la animacion de la cadena con
// el original, se resuelve y se sobrescribe W de brazo, codo, antebrazo,
// muneca y mano. La marca de tiempo queda la del fotograma, asi que todo lo
// demas (malla dibujada, malla de los golpes, arma y escudo enganchados)
// usa la solucion. La mano no tiene hijos.
//
// Ademas: tajos y punetazos por gesto (la IA se defiende de ellos por
// +0x263e60, "necesito defenderme"), escudo VR y arma a dos manos alzada,
// arco VR, comida y pociones en la mano, resistencia de los golpes, la
// parada del personaje al soltar el joystick, Supr sin pasos y los modos de
// combate. Offsets y estructuras: claude/combate_fase0.md (Blade.exe md5
// 8ed91d06...).
// ---------------------------------------------------------------------

namespace BladeVR {

namespace {

// --- direcciones (relativas a Blade.exe) --------------------------------
constexpr uintptr_t kAppObjectPtr = 0xF4A6A8;   // *(base+..) = objeto de aplicacion
constexpr uintptr_t kAppPlayer = 0xC8;          // app+0xC8 = entidad del jugador
// --- vista de la camara (Camera.PViewType) ----------------------------------
constexpr uintptr_t kAppInGame = 0x48;          // char: LastView/NextView solo actuan con el != 0
constexpr uintptr_t kAppCamera = 0xe8;          // entidad "Camera"
constexpr uintptr_t kCamViewCtl = 0x268;        // control de la vista de persona
constexpr uintptr_t kCamViewType = 0x4c4;       // = kCamViewCtl + 0x25c: 0..3 (3 = primera persona, 150 mm)
constexpr uintptr_t kFnSetViewType = 0x5c2d0;   // (camara+0x268, tipo): lo mismo que Camera.PViewType = tipo
constexpr int kViewFirstPerson = 3;
constexpr double kFirstPersonForceSec = 3.0;    // tras cargar, salir del diorama o cambiar de modo; luego mandan las teclas del juego
constexpr uintptr_t kVtPerson = 0x7a4a70;       // B_PersonEntity::vftable
constexpr uintptr_t kVtWeapon = 0x7aa090;       // B_WeaponEntity::vftable
constexpr uintptr_t kFnDefenceNeeded = 0x263e60;
constexpr uintptr_t kFnIsBlocking = 0xb2240;      // ronda 33: (persona) -> 1 si "esta bloqueando" (por su accion)
constexpr uintptr_t kFnShieldTest = 0xbded0;      // ronda 33: (persona, a, b, punto) -> 1 si el segmento a-b toca su escudo
// Ronda 39: los pies para la fisica del juego. El contacto con el suelo de las
// personas (+0xa07d0, ...), el "paso demasiado bajo" que empieza una caida
// (+0x130180 -> +0x129bb0) y el suelo bajo la camara (+0x5ca30) leen la
// posicion de los pies del esqueleto (nodos de (persona+0x268)+0xd0 / +0xd4,
// por el evaluador perezoso). Estas cuatro funciones son las que la leen.
constexpr uintptr_t kFnFootPosA = 0xa7990;        // (persona, out, out): pie (+0xd0)
constexpr uintptr_t kFnFootPosB = 0xa6db0;        // (persona, out, out): pie (+0xd4)
constexpr uintptr_t kFnFootRayA = 0xa68d0;        // (persona, ?, out) -> double: suelo bajo el pie (+0xd0)
constexpr uintptr_t kFnFootRayB = 0xa6690;        // (persona, ?, out) -> double: suelo bajo el pie (+0xd4)
// Ronda 41: el sistema de animacion del juego tambien lee la raiz y los pies
// del esqueleto: al empezar una transicion (+0x135cf0 con un pie apoyado,
// +0x136590 normal, +0x136230...) y en cada fotograma con un pie apoyado
// (+0x136d20 coloca la raiz para que el pie no se mueva). Todas sus llamadas al
// evaluador perezoso salen de [+0x134000, +0x1394f0). Con el cuerpo bajo el
// visor esas lecturas llevaban el desplazamiento y el juego movia al personaje.
constexpr uintptr_t kAnimSysCallLo = 0x134000;
constexpr uintptr_t kAnimSysCallHi = 0x1394f0;
// Ronda 41: lanzar una accion con transicion (LaunchMyMpxAnim) y la variante de
// Rlx_block: (persona, nombre, transicion, tiempo, ...). Transicion 3/4 = con
// el pie izquierdo/derecho apoyado, 5 = el que este apoyado, 0 = sin pie.
constexpr uintptr_t kFnLaunchAction = 0x12c160;
constexpr uintptr_t kFnLaunchActionB = 0x12bdd0;
constexpr uintptr_t kFnPersonAttach = 0x114a;     // persona vtable[2] (-> +0xb77a0): coloca lo enganchado

// --- persona ------------------------------------------------------------
constexpr uintptr_t kEntName = 0x08;            // B_Name: char* en +0
constexpr uintptr_t kPChildArray = 0x150;       // objetos enganchados (0x90 c/u)
constexpr uintptr_t kPChildCount = 0x158;
constexpr uintptr_t kChildStride = 0x90;
constexpr uintptr_t kChildLink = 0x88;          // 16 bits bajos indice; altos 0 = anclaje, 1 = nodo
constexpr uintptr_t kPSkel = 0x278;
constexpr uintptr_t kPObj3D = 0x280;
constexpr uintptr_t kPAnimName = 0x1958;        // B_Name (AnimName)
constexpr uintptr_t kPAnimFullName = 0x1968;    // B_Name (AnimFullName)
constexpr uintptr_t kPActiveEnemy = 0x2a48;
// Peticiones de giro (Supr, +0x5ece0 sobre persona+0x18f8): si la accion es
// "B1"/"B2" o persona+0x2b70 / +0x2ce8 != 0 (+0x5e020), el juego las convierte
// en pasos laterales (+0x12b4/+0x12b8) con el importe del giro como velocidad
// (+0x1535e0): el personaje se desliza (ronda 10: 1,4-2,3 m a 2-4 m/s
// bloqueando con el escudo y girando la cabeza).
constexpr uintptr_t kPStrafeMode = 0x2b70;      // int
constexpr uintptr_t kPStrafeMode2 = 0x2ce8;     // int
constexpr uintptr_t kPLife = 0x2b28;            // int
constexpr uintptr_t kPInventory = 0x2e70;
constexpr uintptr_t kInvRight = 0x70;           // InvRight
constexpr uintptr_t kInvLeft = 0x60;            // InvLeft
constexpr uintptr_t kInvLeft2 = 0x68;           // InvLeft2 (ronda 21: la pocion en combate; enlazada en L_Hand, +0x150530)

// --- esqueleto (persona+0x278) ------------------------------------------
constexpr uintptr_t kSkelNodeTable = 0x90;      // -> {+8 array, +0x10 numero}
constexpr uintptr_t kSkelTime = 0xb8;           // tiempo del fotograma (double)
constexpr uintptr_t kTabArray = 0x08;
constexpr uintptr_t kTabCount = 0x10;
constexpr uintptr_t kNodeStride = 0x20;
constexpr uintptr_t kNodeParent = 0x00;         // int (-1 en la raiz)
constexpr uintptr_t kNodeBind = 0x08;           // -> matriz de reposo B
constexpr uintptr_t kNodeTime = 0x10;           // double
constexpr uintptr_t kNodeWorld = 0x18;          // -> matriz W en el mundo

// --- malla (persona+0x280 -> +0x28) -------------------------------------
constexpr uintptr_t kObjMesh = 0x28;
constexpr uintptr_t kMeshNodeNames = 0xc0;      // array de punteros (nombre en +8)
constexpr uintptr_t kMeshNodeNameCount = 0xc8;
constexpr uintptr_t kMeshAnchors = 0x188;       // array de punteros a anclajes
constexpr uintptr_t kMeshAnchorCount = 0x190;
constexpr uintptr_t kAnchorName = 0x08;
constexpr uintptr_t kAnchorMatrix = 0x18;
constexpr uintptr_t kAnchorNode = 0x98;

// --- arma / escudo (B_WeaponEntity) -------------------------------------
constexpr uintptr_t kWObj = 0x288;              // objeto 3D
constexpr uintptr_t kObjDesc = 0xb0;
constexpr uintptr_t kObjWorld = 0xb8;           // matriz en el mundo
constexpr uintptr_t kDescImpale = 0x120;
constexpr uintptr_t kDescImpaleCount = 0x128;
constexpr uintptr_t kDescSlash = 0x1c8;
constexpr uintptr_t kDescSlashCount = 0x1d0;
constexpr uintptr_t kAreaCenter = 0x20;
constexpr uintptr_t kAreaHalf = 0x38;
constexpr uintptr_t kAreaCut = 0x50;
constexpr uintptr_t kWState = 0x4a8;            // 0 apagada, 1 recien encendida, 2 activa
constexpr uintptr_t kWHitCount = 0x418;

// --- IA -------------------------------------------------------------------
constexpr uintptr_t kAiEnemy = 0x108;           // persona de la que se defiende

// --- evaluadores del esqueleto y cinematica inversa ------------------------
constexpr uintptr_t kFnEvalNode = 0x1394f0;     // (skel, nodo) -> registro del nodo (perezoso)
constexpr uintptr_t kFnEvalAll = 0x139100;      // (skel): todos los nodos en orden
constexpr uintptr_t kSkelEntity = 0x6f8;        // matriz de la entidad (padre de la raiz)
constexpr double kIkBlendSec = 0.2;             // entrada / salida suave de los brazos
constexpr double kFirstPersonMaxM = 0.5;        // ViewPoint a menos de esto de la camara del juego (en 3a persona: 2-4 m)
// Ronda 49: ya en primera persona se sale a mas de esto. Girando deprisa con
// un enemigo fijado la camara del juego se iba a 0,50-0,53 m del ViewPoint uno
// a tres fotogramas (10 veces en 7 minutos): los brazos soltaban y, en el modo
// 3, se veia el cuerpo y los ojos bajaban la altura anadida (un tiron).
constexpr double kFirstPersonExitM = 1.2;
constexpr double kPoleHandWeight = 0.5;         // el codo: mitad cuerpo, mitad orientacion de la mano
// Parte del giro de la muneca que se lleva el antebrazo (el nodo del
// antebrazo esta junto al codo: girarlo entero retuerce la malla desde el
// codo). Con escudo la mano va fija al antebrazo y gira con el: el escudo y
// la mano giran kTwistShareShield del giro real (ronda 9: 1,0 se retorcia).
constexpr double kTwistShareLeft = 0.5;
constexpr double kTwistShareRight = 0.5;
constexpr double kTwistShareShield = 1.0;     // ronda 19 (antes 0,7)
// Ronda 19: con el escudo, el codo va hacia donde lo pone la orientacion del
// mando (la muneca va fija al antebrazo) y el antebrazo se lleva todo el giro
// (tope 150 grados). Con 0,7, tope 90 y el codo del cuerpo, al acercar el
// escudo al pecho el codo quedaba hasta 0,3 m del real y el escudo giraba de
// mas (en el log, giros de la muneca de -191 a -236 grados).
constexpr double kShieldPoleHandWeight = 1.0;
constexpr double kShieldForeMaxDeg = 150.0;
// Ronda 10: el giro de codo, antebrazo y muneca se mide desde la pose de
// reposo (la de la calibracion), no desde la animacion. Las animaciones del
// brazo izquierdo (guardia, escudo) giran el antebrazo, y con un reparto
// menor que 1 ese giro de la animacion se colaba en el antebrazo. El derecho
// sigue con la animacion (va bien).
constexpr bool kRestTwist[2] = { true, false };
// Ronda 57 (pedido del usuario: el brazo izquierdo se retorcia raro tambien sin
// escudo; "que el hombro no se mueva o quede como el derecho"). En la guardia
// la animacion lleva el hombro izquierdo atras y a un lado y gira su brazo: el
// eje del codo de la animacion quedaba a 47-69 grados del de reposo y el giro de
// la muneca medido desde el reposo salia de -150 a 180 grados (el antebrazo se
// llevaba hasta 90). Con kRestShoulder la clavicula y el brazo del juego van en
// su pose de reposo sobre el pecho y el resto de la cadena de la animacion se
// coloca rigida sobre ese hombro: el hombro no se mueve con la animacion. Solo el
// izquierdo (el derecho va bien); con el arma a dos manos cogida, la animacion
// (como en las rondas 22-27). Ronda 58: tambien la animacion con un arma a dos
// manos en la derecha aunque no se coja (con el hombro movido el agarre fijo de
// Eclipse se midio a 53 cm de la mano derecha y la izquierda no lo cogia) y con
// el arco (pedia 0.90-0.97 m de 0.69: saturado, la mano a 20-29 cm del mando).
constexpr bool kRestShoulder[2] = { true, false };
constexpr double kStretchMax = 1.25;
// Ronda 55 (pedido del usuario: que valga para todos sin calibrar): agarre y
// alcance de serie; el de la ronda 2 (55 grados de cabeceo) estaba a 16-28
// grados de los mandos que calibro el usuario con Ctrl+F8 (Quest, cogidos con
// naturalidad). El alcance (mano -> hombro del personaje, como lo mide Ctrl+F8)
// de un adulto: 0,80 m (el del usuario, 1,80 m, 0,85): con los cuatro
// personajes da el tope de alargamiento (kStretchMax), como su calibracion.
// Ronda 56: cada mando con SU calibracion (la ronda 55 usaba la media de los
// dos, la derecha reflejada, a 7,5 grados de cada uno, y el codo izquierdo
// doblaba mal: con el escudo el antebrazo, y con el el codo, los pone el giro
// del mando). Los dos no son simetricos (15 grados): las poses de los mandos
// izquierdo y derecho de OpenVR no son reflejo una de otra, y eso es igual
// para todos los Quest.
constexpr double kHangDefault[2][9] = {
    { 0.987952, 0.152813, 0.024486, -0.097337, 0.736547, -0.669346, -0.120320, 0.658898, 0.742547 },   // izquierda
    { 0.925456, -0.311537, -0.215582, 0.112768, 0.769764, -0.628289, 0.361683, 0.557143, 0.747514 } }; // derecha
constexpr double kDefaultReachM = 0.80;
// Ronda 34: el escudo delante de la cara pedia al hombro izquierdo 0,55-0,59 m
// de 0,54 (en Shattack_rlx el hombro queda 0,37-0,40 m a la izquierda de los
// ojos): brazo recto, mano y escudo a 8-11 cm del mando. El hombro se adelanta
// hacia la muneca hasta kShieldProtractM desde el 90 % del alcance.
// Ronda 35: hacia el codo que pide la orientacion del mando (el antebrazo y el
// escudo como el del jugador; lo que el hombro no alcanza lo pone la muneca,
// que se acerca), desde el 85 % del alcance y del todo al 95 % (CombatVRIk.h).
constexpr double kShieldProtractM = 0.10;
constexpr double kShieldProtractStart = 0.85;
constexpr double kBodyMaxOffsetM = 0.9;         // cuerpo bajo el visor: desplazamiento maximo
// Tajo con el arma (ataque basico por gesto): la punta (el extremo mas rapido
// de las areas del arma), medida con el mando real (en el espacio de
// seguimiento: la camara del juego, que salta al chocar con las paredes, y el
// movimiento del personaje no cuentan), por encima de kSwingOnMs durante
// kSwingOnFrames fotogramas seguidos enciende la deteccion del arma
// (MESSAGE_START_WEAPON, sin animacion: sin estela ni energia) y por debajo de
// kSwingOffMs durante 3 fotogramas la apaga.
constexpr double kSwingOnMs = 4.0;
// Ronda 72: el tajo se mide con el arma pasada al espacio del visor (punta a
// kSwingOnMs). Si la camara del juego salta (en la escalera, con el visor
// chocando: 76 recortes y 49 empujes en un minuto) el arma salta con ella y
// salian tajos que nadie hacia (25 en un minuto, 0 golpes, 19 sin resistencia:
// la barra se gastaba sola). Ademas el mando de verdad tiene que ir a
// kSwingHandMinMs o mas.
constexpr double kSwingHandMinMs = 0.8;
constexpr int kSwingOnFrames = 2;
constexpr double kSwingOffMs = 1.5;
constexpr double kSwingMaxSec = 1.0;
constexpr double kSwingCooldownSec = 0.12;
constexpr double kAiReactSec = 0.3;             // la IA "ve" el tajo durante sus primeros 0,3 s
constexpr int kMsgStartWeapon = 7;
constexpr int kMsgStopWeapon = 8;
constexpr uintptr_t kEntVtMessage = 50 * 8;     // vtable[50]: MessageEvent (entidad, mensaje, p3, p4)
// Ronda 31 (pedido del usuario): punetazos con las manos libres como los
// tajos, sin animacion ni combos: los mensajes 7 / 8 a la PERSONA encienden y
// apagan sus armas naturales (manejador +0xb2470: persona+0x2ef4 a 1 / 0, y
// el 7 vacia la lista de golpeados, cuenta en +0x3008). El tick de la persona
// (+0xb8a30 -> +0xb8880) barre cada area natural (punto en un nodo del
// descriptor de la malla, lista +0x120, nodo en +0x18; estado por area en
// persona+0x2e98, 0x90 bytes) desde su posicion anterior y manda
// MESSAGE_IMPALE (5) a lo que toque: el dano es el de un punetazo del juego.
// Los nodos son los de la mano con la cinematica inversa, asi que golpea
// donde va el puno. La velocidad, la del mando real.
constexpr uintptr_t kPNaturalCount = 0x2ea0;    // areas naturales (lista del descriptor +0x120)
constexpr uintptr_t kPNaturalCount2 = 0x2ec0;   // otra lista de areas naturales
constexpr uintptr_t kPNaturalState = 0x2ef4;    // 0 apagadas, 1 recien encendidas, 2 activas
constexpr uintptr_t kPNaturalHits = 0x3008;     // golpeados con este golpe
constexpr double kPunchOnMs = 2.0;              // m/s del mando
constexpr int kPunchOnFrames = 2;
constexpr double kPunchOffMs = 1.0;
constexpr double kPunchMaxSec = 0.8;
constexpr double kPunchCooldownSec = 0.12;
constexpr double kBodyYawSmoothSec = 0.12;      // suavizado del giro del pecho
// Personaje que anda solo: con el joystick izquierdo en reposo y el jugador
// moviendose sin el, se sueltan las teclas del joystick del mando del juego
// (como hace el propio juego al correr, +0x27bfd0).
constexpr uintptr_t kFnPadKey = 0x27ba60;        // BaseGamepad: (mando, pulsada, "JoyUp", tiempo): estado + evento
constexpr uintptr_t kPadStickX = 0x80;           // joystick izquierdo del mando del juego (float, tras su zona muerta)
constexpr uintptr_t kPadStickY = 0x84;
// Ronda 16: la puerta del mando del juego. En partida el juego solo procesa el
// mando si fue el ultimo dispositivo con actividad: cada fotograma +0x26ad30
// pone en el gestor de entrada (+0x58) el tipo del primero que la tuvo, y
// +0x19f8b0 lo devuelve (1 = mando). Los giros de Supr son movimientos de
// raton: mientras Supr gira, el raton pasa a ser lo ultimo usado y la
// actualizacion del mando de Steam Input (+0x271510) se salta entera (hasta
// +0x271780): el juego no ve el joystick en reposo (JoyUp sigue pulsada y anda
// solo), ni el joystick a fondo del sprint, y los botones en cola se tiran.
// Ronda 64: con los mandos de Quest el mando se procesa siempre (rondas 16-63:
// solo con un cambio por entregar o andando solo).
constexpr uintptr_t kFnInputGate = 0x19f8b0;     // (gestor de entrada) -> distinto de 0: se procesa el mando
// Ronda 65: el sprint seguia clavando al personaje. El eje Y de CADA mando del
// juego (+0x27bfd0, llamado desde +0x27b2c0 con (mando, Y, Y anterior, tiempo))
// suelta JoyUp y JoyDown si su Y es exactamente 0 y el jugador va a mas de
// 4 m/s (corriendo; andando no llega). Con otro mando ademas del de los Quest
// (en el log el juego leia (0,0) del ultimo mando procesado mientras se mandaba
// (0,1)), ese otro mando soltaba "adelante" al empezar a correr: el personaje
// se paraba con Run = 1 y Gof = 0, y como el de los Quest seguia a fondo (por
// encima de 0,91 no hay flanco) no volvia a pulsarse: clavado.
// Ronda 66: lo mismo hacia el mando de los Quest en reposo con el teclado o el
// mando de Xbox (no se podia correr con ellos sin apagar los Quest). Un mando
// con Y = 0 ahora y en su fotograma anterior no tiene nada que entregar: no se
// procesa su eje Y, salvo si el movimiento es de los Quest y acaban de soltarse
// (g_questDriven: entonces esa parada del juego ayuda a parar).
constexpr uintptr_t kFnPadAxisY = 0x27bfd0;
constexpr uintptr_t kPadController = 0x108;      // qword: asa de Steam Input del mando
// Ronda 67: con el mando de Xbox (SDL, +0x2794f0) y los Quest a la vez, el
// stick izquierdo del de Xbox no andaba. Cada mando procesado manda tambien sus
// ejes analogicos (X_LAxis, X_Axis, Y_Axis: +0x27b7b8..) aunque esten a 0, asi
// que el mando quieto pisaba al que se mueve. Un mando quieto (sticks y
// gatillos a 0 ahora y en su fotograma anterior) no se procesa mientras otro
// se mueve (hasta kOtherPadHoldSec despues).
constexpr uintptr_t kRetGateSdlPad = 0x279553;   // vuelta de la puerta en la actualizacion del mando SDL
constexpr uintptr_t kPadAxesNow = 0x80;          // 6 floats: stick izq x/y, dcho x/y, gatillos
constexpr uintptr_t kPadAxesPrev = 0x98;         // los mismos del ultimo fotograma procesado
constexpr double kOtherPadHoldSec = 0.5;
constexpr float kPadTriggerIdleMax = 0.19f;     // por debajo del umbral de los gatillos (+0x70, 0,2)
constexpr uintptr_t kPadQueueCount = 0xf0;       // qword: botones en cola
// Ronda 69: el stick del mando de Xbox volvia a no andar (con los Quest
// dormidos la puerta del SDL no se forzaba: en el log, saltado 2955 fotogramas
// de un minuto y forzado 0). El SDL en uso se procesa siempre, haya Quest o no.
// El mando SDL se reconoce por la tabla virtual de su clase.
constexpr uintptr_t kVtSdlPad = 0x7c4ca0;        // SDLGamepad
// Ronda 68: con el teclado y el raton, corriendo, el giro con el raton iba lento
// o a saltos. Cada mando procesado manda X_LAxis (stick izquierdo x) en cada
// fotograma, a 0 si esta quieto (+0x27b873; corriendo +0x27bbb0): es
// RotateOrMoveX -> RotateMouseX del juego, y con 0 pide un giro de 0 (+0x5ece0
// borra las peticiones de giro, +0x1298). La puerta de la ronda 64 procesaba el
// de los Quest quieto en cada fotograma, asi que podia borrar el giro del
// raton (probable, sin medir). Ahora la puerta solo lo procesa si tiene algo
// que entregar (stick o gatillo en uso, un soltar, botones en cola) o si el
// movimiento es de los Quest (g_questDriven: lo de andar solo de la ronda 63).
constexpr uintptr_t kRetGateSteamPad = 0x271577; // vuelta de la llamada en la actualizacion del mando de Steam Input
constexpr uintptr_t kPRunFlag = 0x2bd8;          // int: el juego lo pone con el joystick a fondo (> 0,91)
constexpr uintptr_t kPMoveFlags = 0x4d0;         // banderas del control del jugador (1 = adelante, 0x80 = bloqueo)
constexpr uintptr_t kFnPersonPos = 0x14b4a0;     // (persona+0x18f8, double[3]) -> posicion de la persona (la del juego)
constexpr uintptr_t kPPosState = 0x18f8;
// Un "episodio": el joystick lleva kWalkNeutralSec en reposo (parar de andar
// tarda menos) y el jugador se sigue moviendo a mas de kWalkSpeedMs; acaba
// cuando baja de kWalkStopMs o se toca el joystick (y con el, la peticion de
// soltar las teclas que quedara pendiente).
constexpr double kWalkNeutralSec = 1.0;
constexpr double kWalkSpeedMs = 0.5;
constexpr double kWalkStopMs = 0.25;
constexpr double kWalkRetrySec = 1.0;
// Parar al personaje: joystick en reposo kWalkStopNeutralSec, accion de andar
// o correr y moviendose: Gof/Gob/Run = 0 por Python (lo mismo que hace el
// juego al quedarse sin energia, Basic_Funcs.RestoreEnergyFunc) y se sueltan
// las teclas del joystick. Tras escalar o cerca de enemigos (InCombat) el
// juego no limpiaba Gof y Run (Actions.FrwdUp vuelve sin hacer nada).
// Ronda 12: tambien si el juego tiene puesto "ir adelante / atras / girar"
// (Gof, Gob, Tr, Tl) sin joystick, aunque la accion no sea de andar (con un
// enemigo fijado se movia en Rlx_f a 2 m/s), y antes (0,25 s, desde 0,3 m/s:
// se vio andar hacia atras despacio 1,4 s antes de pararlo).
constexpr double kWalkStopNeutralSec = 0.12;   // ronda 15 (antes 0,25)
// Ronda 70: en el modo 0 los joysticks van como los del mando de Xbox: sin la
// parada rapida al soltar (StopTest, ronda 40), sin la de tras escalar o caer
// y sin el guardian de Run (a fondo corre, como el Xbox). La parada por andar
// solo queda solo como seguro, con el joystick suelto mas tiempo.
constexpr double kWalkStopNeutralXboxSec = 1.5;
constexpr float kSentKeyMin = 0.40f;           // ronda 15: por debajo, el juego no pulsa JoyUp/Down/Left/Right
constexpr double kWalkStopSpeedMs = 0.3;
constexpr double kWalkStopRetrySec = 0.75;
// Atributos de Python de la persona (setter +0xdf510, casos 0x17-0x1b).
constexpr uintptr_t kPGof = 0x2bbc;              // int: ir adelante
constexpr uintptr_t kPGob = 0x2bc0;              // int: ir atras
constexpr uintptr_t kPTr = 0x2b90;               // int: girar a la derecha
constexpr uintptr_t kPTl = 0x2b94;               // int: girar a la izquierda
constexpr uintptr_t kFnPyRunSimple = 0x397880;   // PyRun_SimpleString (modulo __main__)
// Ronda 59: lo que llama PyRun_SimpleString (Python 1.5.2 enlazado), para
// evaluar una expresion y leer su valor (PyEvalNumbers). Comprobados en .pdata
// (salvo PyErr_Clear: hoja sin pila, xor r8d/edx/ecx + jmp PyErr_Restore) y por
// el prologo antes de usarlos. Objetos: ob_refcnt int en +0, ob_type en +8;
// int: long en +0x10; float: double en +0x10; tupla: ob_size int en +0x10 y
// los elementos desde +0x18; tipo: tp_name en +0x18, tp_dealloc en +0x28.
constexpr uintptr_t kFnPyImportAddModule = 0x399130;
constexpr uintptr_t kFnPyModuleGetDict = 0x3aa870;
constexpr uintptr_t kFnPyRunString = 0x397910;
constexpr uintptr_t kFnPyErrClear = 0x395ae0;
constexpr uintptr_t kPyIntType = 0x9dfd90;
constexpr uintptr_t kPyFloatType = 0x9e0040;
constexpr uintptr_t kPyTupleType = 0x9e0280;
constexpr int kPyEvalInput = 258;                 // Py_eval_input
const char* const kPyStopWalk =
    "import Bladex\n"
    "BladeVR_pj = Bladex.GetEntity('Player1')\n"
    "if BladeVR_pj:\n"
    "    BladeVR_pj.Gof = 0\n"
    "    BladeVR_pj.Gob = 0\n"
    "    BladeVR_pj.Tr = 0\n"
    "    BladeVR_pj.Tl = 0\n"
    "    BladeVR_pj.Run = 0\n";
// Ronda 40: sin el paso de mas al soltar el joystick. El juego solo mira si
// parar en los eventos "StopTest" de las animaciones de andar
// (Bladex.AddStopTests: dos por ciclo, al 30 % entre una pisada y la
// siguiente). Su funcion (StopWalk, StopJog, StopBack, StopSneak,
// StopWalkFacing... = +0x131a40) pasa a Rlx con +0x12c540 solo si la bandera de
// ese movimiento esta quitada en persona+0x4d0 (1 adelante, 2 atras, 4/8 de
// lado, 0x10 correr, 0x40 sigilo). Con el joystick suelto y ninguna de esas
// banderas se le manda el evento al momento con RaiseEvent (como hacen los
// scripts del juego con otros eventos): para por el camino del propio juego.
constexpr unsigned kMoveFlagBits = 0x5f;
constexpr int kStopTestMaxSends = 20;       // por cada vez que se suelta
constexpr double kStopTestEverySec = 0.03;
const char* const kPyStopTest =
    "import Bladex\n"
    "BladeVR_pj = Bladex.GetEntity('Player1')\n"
    "if BladeVR_pj:\n"
    "    BladeVR_pj.RaiseEvent('StopTest')\n";
// Ronda 10: tras escalar, caer o saltar con el joystick en reposo se para en
// cuanto vuelve a andar (sin esperar kWalkStopNeutralSec).
constexpr double kAfterAirSec = 1.0;
// Correr solo con el sprint (VrPadHook): con los mandos de Quest y sin sprint
// el juego no deberia tener Run = 1; si lo tiene (se quedo del sprint, el
// soltar llego durante una escalada), se quita por Python.
constexpr double kRunGuardRetrySec = 0.05;   // ronda 14: casi cada fotograma (antes 0,25 s)
const char* const kPyRunOff =
    "import Bladex\n"
    "BladeVR_pj = Bladex.GetEntity('Player1')\n"
    "if BladeVR_pj:\n"
    "    BladeVR_pj.Run = 0\n";
// Y: soltar al enemigo fijado (el juego no tiene boton para eso; es lo que
// hace Actions.StdToggleWeapons al guardar el arma con un enemigo activo).
// enemy_locked = 0: el siguiente gatillo fija al mas cercano otra vez.
const char* const kPyUnlockEnemy =
    "import Bladex\n"
    "BladeVR_pj = Bladex.GetEntity('Player1')\n"
    "if BladeVR_pj and BladeVR_pj.ActiveEnemy:\n"
    "    BladeVR_pj.SetActiveEnemy('')\n"
    "    try:\n"
    "        BladeVR_pj.Data.time_deactive_enemy = Bladex.GetTime()\n"
    "        BladeVR_pj.Data.enemy_locked = 0\n"
    "    except:\n"
    "        pass\n";
// B con el joystick en reposo: solo "SelectObj" (InputInit.JumpAndSelectObj
// sin el "Jump"): alterna entre los objetos cercanos sin subirse a nada.
const char* const kPySelectObj =
    "import Bladex\n"
    "Bladex.FakeInputAction('SelectObj', 1)\n"
    "Bladex.AddScheduledFunc(Bladex.GetTime() + 0.01, Bladex.FakeInputAction, ('SelectObj', 0))\n";
// Enemigo fijado: Supr y el pecho lo dan por fijado hasta kLockHoldSec despues
// de perderlo (sin parpadeos); el pecho gira con peso kChestLockedWeight.
constexpr double kLockHoldSec = 1.0;
constexpr double kChestLockSmoothSec = 0.25;
// Ronda 18: con un enemigo fijado el pecho vuelve a girar hacia la vista
// (peso 1, como sin el). Con peso 0 se quedaba la guardia de lado de la
// animacion (Rlx_f, Shattack_rlx: 50-60 grados): el hombro izquierdo 0,22-0,26 m
// por delante de la camara y el brazo del escudo doblado (0,13-0,35 m de
// hombro a muneca, codo a 26-80 grados), y el derecho 0,24-0,40 m por detras,
// estirado al tope (x1,25). El temblor que se le achacaba era el objetivo de
// la camara del juego (rondas 12-14).
constexpr double kChestLockedWeight = 1.0;
constexpr uintptr_t kAppMode = 0x6d8;              // char*: "Menu" con el menu abierto (lo mira +0x4f030)
// Visor muy por encima o por debajo de donde se recentro (el jugador se ha
// levantado o sentado): la camara queda lejos de los ojos del personaje y
// choca con techos y escaleras (ronda 9: 0,4 m durante un combate). Aviso.
constexpr double kViewHeightWarnM = 0.25;
constexpr double kViewHeightWarnSec = 3.0;
constexpr double kViewHeightHintEverySec = 120.0;
constexpr unsigned long long kGameplayFreshMs = 300;
// Comida y pociones en la mano (ronda 21; la comida desde la ronda 8): en vez
// de las animaciones "eat00", "drink" y "gulp00" el objeto se queda en la mano
// derecha (pocimac.UsePotion2 envuelta: al recogerlo o al usarlo del
// inventario, tambien tras guardar el arma con Chg_r). En combate con un arma,
// "attack_drink" lo pone en la mano izquierda (PickPotion envuelta: se quitan
// sus eventos de beber y tirar). Se come o se bebe al acercar ese mando a la
// boca (BladeVR_Comer('R' o 'L')): BeberPocima y SoltarPocima / SoltarFood,
// como el juego. Python 1.5: sin +=.
const char* const kPyFood =
    "import Bladex\n"
    "import pocimac\n"
    "def BladeVR_Aviso(t):\n"
    "    try:\n"
    "        import GameText\n"
    "        GameText.WriteTextAux(t, 3.0, 255, 255, 255, [], None, 1)\n"
    "    except:\n"
    "        pass\n"
    "def BladeVR_UsePotion2(NombrePocima):\n"
    "    try:\n"
    "        poc = Bladex.GetEntity(NombrePocima)\n"
    "        char = Bladex.GetEntity(poc.Data.UsedBy)\n"
    "        t = poc.Data.Type\n"
    "        if pocimac.BladeVR_Boca and char and char.Name == 'Player1' and (t == pocimac.POTION_TYPE_EAT or t == pocimac.POTION_TYPE_DRINK_RIGHT or t == pocimac.POTION_TYPE_GULP):\n"
    "            if char.InvRight != NombrePocima and not char.InvRight:\n"
    "                char.GetInventory().LinkRightHand(NombrePocima)\n"
    "            if char.InvRight == NombrePocima:\n"
    "                char.Data.obj_used = NombrePocima\n"
    "                char.AnmEndedFunc = ''\n"
    "                pocimac.BladeVR_Mano = NombrePocima\n"
    "                pocimac.BladeVR_ManoLado = 'R'\n"
    "                if t == pocimac.POTION_TYPE_EAT:\n"
    "                    BladeVR_Aviso('Acercate la comida a la boca para comerla')\n"
    "                else:\n"
    "                    BladeVR_Aviso('Acercate la pocion a la boca para beberla')\n"
    "                return\n"
    "    except:\n"
    "        pass\n"
    "    pocimac.BladeVR_UsePotion2_orig(NombrePocima)\n"
    "def BladeVR_PickPotion(Entidad, Evento):\n"
    "    pocimac.BladeVR_PickPotion_orig(Entidad, Evento)\n"
    "    try:\n"
    "        char = Bladex.GetEntity(Entidad)\n"
    "        poc = Bladex.GetEntity(char.Data.obj_used)\n"
    "        if Entidad == 'Player1' and pocimac.BladeVR_Boca and poc.Data.Type == pocimac.POTION_TYPE_DRINK_LEFT and char.InvLeft2 == poc.Name:\n"
    "            char.DelAnmEventFunc('drinkingEvent')\n"
    "            char.DelAnmEventFunc('ThrowBottle')\n"
    "            char.AnmEndedFunc = ''\n"
    "            pocimac.BladeVR_Mano = poc.Name\n"
    "            pocimac.BladeVR_ManoLado = 'L'\n"
    "            BladeVR_Aviso('Acercate la pocion a la boca para beberla')\n"
    "    except:\n"
    "        pass\n"
    "if not hasattr(pocimac, 'BladeVR_UsePotion2_orig'):\n"
    "    pocimac.BladeVR_UsePotion2_orig = pocimac.UsePotion2\n"
    "    pocimac.UsePotion2 = BladeVR_UsePotion2\n"
    "    pocimac.BladeVR_PickPotion_orig = pocimac.PickPotion\n"
    "    pocimac.PickPotion = BladeVR_PickPotion\n"
    "    pocimac.BladeVR_Mano = None\n"
    "    pocimac.BladeVR_ManoLado = 'R'\n"
    "if not hasattr(pocimac, 'BladeVR_Boca'):\n"
    "    pocimac.BladeVR_Boca = 1\n"
    "def BladeVR_Comer(lado):\n"
    "    name = pocimac.BladeVR_Mano\n"
    "    if not name or lado != pocimac.BladeVR_ManoLado:\n"
    "        return\n"
    "    char = Bladex.GetEntity('Player1')\n"
    "    poc = Bladex.GetEntity(name)\n"
    "    if not char or not poc:\n"
    "        pocimac.BladeVR_Mano = None\n"
    "        return\n"
    "    if lado == 'L':\n"
    "        if char.InvLeft2 != name:\n"
    "            pocimac.BladeVR_Mano = None\n"
    "            return\n"
    "    elif char.InvRight != name:\n"
    "        pocimac.BladeVR_Mano = None\n"
    "        return\n"
    "    pocimac.BladeVR_Mano = None\n"
    "    char.Data.obj_used = name\n"
    "    if poc.Data.Type == pocimac.POTION_TYPE_EAT:\n"
    "        try:\n"
    "            pocimac.CreateMiguillas('Player1', 'Bocado1Event')\n"
    "        except:\n"
    "            pass\n"
    "        try:\n"
    "            pocimac.BeberPocima('Player1', 'drinkingEvent')\n"
    "        except:\n"
    "            pass\n"
    "        pocimac.SoltarFood('Player1')\n"
    "        try:\n"
    "            poc.Alpha = 0.05\n"
    "        except:\n"
    "            pass\n"
    "        return\n"
    "    try:\n"
    "        pocimac.BeberPocima('Player1', 'drinkingEvent')\n"
    "    except:\n"
    "        pass\n"
    "    pocimac.SoltarPocima('Player1', 0)\n";
// Ronda 23: en el modo VR basico la comida y las pociones como en el juego
// (lo que estuviera esperando en la mano se toma al cambiar de modo).
const char* const kPyFoodBocaOff =
    "import pocimac\n"
    "pocimac.BladeVR_Boca = 0\n"
    "if pocimac.BladeVR_Mano:\n"
    "    BladeVR_Comer(pocimac.BladeVR_ManoLado)\n";
const char* const kPyFoodBocaOn =
    "import pocimac\n"
    "pocimac.BladeVR_Boca = 1\n";
// Ronda 50: arco VR (modos 1 y 3). Lo del juego (LIB/Actions.py:
// TestDrawBow, EndDrawBowEventHandler, TakeArrowEventHandler) sin sus
// animaciones: la cuerda se agarra a la flecha (GraspString) al tensar y la
// flecha sale con la velocidad que pide el mod (de la mano derecha hacia el
// arco), con lo mismo que hace el juego al soltarla. La siguiente flecha del
// carcaj (UnSheatheArrow), desde la ronda 51 con el agarre derecho en la
// espalda (antes sola a los 0,5 s). Python 1.5: sin +=.
const char* const kPyBow =
    "import Bladex\n"
    "import Actions\n"
    "import InitDataField\n"
    "import Reference\n"
    "def BladeVR_ArcoFlecha():\n"
    "    me = Bladex.GetEntity('Player1')\n"
    "    if not me or not me.InvRight:\n"
    "        return None\n"
    "    if Reference.GiveObjectFlag(me.InvRight) != Reference.OBJ_ARROW:\n"
    "        return None\n"
    "    return Bladex.GetEntity(me.InvRight)\n"
    "def BladeVR_Sonido(ent, nombre, fichero):\n"
    "    try:\n"
    "        s = Bladex.CreateEntity(ent.Name + nombre, 'Entity Sound', 0, 0, 0)\n"
    "        s.SetSound(fichero)\n"
    "        s.MinDistance = 5000\n"
    "        s.MaxDistance = 10000\n"
    "        ent.Link(s)\n"
    "        s.PlaySound(0)\n"
    "    except:\n"
    "        pass\n"
    "def BladeVR_Tensar():\n"
    "    arrow = BladeVR_ArcoFlecha()\n"
    "    if not arrow:\n"
    "        return\n"
    "    Actions.GraspString('Player1', 'GraspString')\n"
    "    BladeVR_Sonido(arrow, 'RedrawSound', '../../Sounds/M-CREAKCUERDA-3b.wav')\n"
    "def BladeVR_GuardarFlecha():\n"
    "    me = Bladex.GetEntity('Player1')\n"
    "    arrow = BladeVR_ArcoFlecha()\n"
    "    if not me or not arrow:\n"
    "        return\n"
    "    inv = me.GetInventory()\n"
    "    if inv:\n"
    "        Actions.SheatheArrow(inv, arrow.Name)\n"
    "def BladeVR_Destensar():\n"
    "    Actions.UnGraspString('Player1', 'UnGraspString')\n"
    "def BladeVR_RecargarArco():\n"
    "    me = Bladex.GetEntity('Player1')\n"
    "    if not me or me.InvRight:\n"
    "        return\n"
    "    inv = me.GetInventory()\n"
    "    if not inv or not inv.HoldingBow:\n"
    "        return\n"
    "    qn = inv.GetSelectedQuiver()\n"
    "    if not qn:\n"
    "        return\n"
    "    q = Bladex.GetEntity(qn)\n"
    "    if not q:\n"
    "        return\n"
    "    if q.Data.NumberOfArrows() <= 0:\n"
    "        return\n"
    "    Actions.UnSheatheArrow(inv)\n"
    "def BladeVR_Disparar(vx, vy, vz):\n"
    "    me = Bladex.GetEntity('Player1')\n"
    "    arrow = BladeVR_ArcoFlecha()\n"
    "    if not me or not arrow:\n"
    "        return\n"
    "    me.Unlink(arrow)\n"
    "    me.RemoveFromInventRight()\n"
    "    Actions.UnGraspString('Player1', 'UnGraspString')\n"
    "    arrow.ExcludeHitFor(me)\n"
    "    arrow.PutToWorld()\n"
    "    arrow.Fly(vx, vy, vz)\n"
    "    arrow.MessageEvent(Reference.MESSAGE_START_WEAPON, 0, 0)\n"
    "    arrow.MessageEvent(Reference.MESSAGE_START_TRAIL, 0, 0)\n"
    "    InitDataField.Initialise(arrow)\n"
    "    Bladex.AddScheduledFunc(Bladex.GetTime() + 2.0, Actions.ThrownWeaponStopFunc, (arrow.Name,), 'Stop Weapon: ' + arrow.Name)\n"
    "    arrow.Data.PrevInflictHitFunc = arrow.InflictHitFunc\n"
    "    arrow.InflictHitFunc = Actions.ThrownWeaponInflictHitFunc\n"
    "    arrow.Data.ThrownBy = me\n"
    "    BladeVR_Sonido(arrow, 'FlySound', '../../Sounds/ARCO-DISPARO-3.wav')\n";
// Ronda 59 (pedido del usuario): la resistencia del juego tambien para los tajos y
// punetazos con el mando. Lo de Damage.CalculateFatigue (la AttackFunc del
// jugador al lanzar un ataque) con el ataque basico del personaje y el arma, sin
// lanzar animacion: coste = (personaje + peso del arma) x animacion (+ lo que
// quedara por gastar); si pide tanto como el maximo de su nivel el ataque no
// sale ("You need more energy for this attack", por eso el caballero no puede
// usar la Eclipse al principio); si pide mas de lo que queda sale el ataque
// torpe (g_bad_*: mas lento, la energia baja de 0); sin energia no sale. Se
// gasta mientras dura el ataque, como el juego: RestoreEnergyFunc envuelta no
// recupera durante el ataque VR (BladeVR_AtaqueHasta), como hace con InAttack.
// "PlayerRestoreEnergy" no se guarda en la partida (GameState.SaveScheduledFuncs).
// Devuelve (codigo, coste, maximo, energia que tenia, duracion): 0 normal,
// 1 torpe, 2 el arma pide mas que el maximo, 3 sin energia, -1 no aplica.
const char* const kPyEnergy =
    "import Bladex\n"
    "import Basic_Funcs\n"
    "import Reference\n"
    "import CharStats\n"
    "import Actions\n"
    "import Damage\n"
    "import netgame\n"
    "def BladeVR_RestoreEnergy(EntityName):\n"
    "    pj = Bladex.GetEntity(EntityName)\n"
    "    if pj and EntityName == 'Player1' and not pj.InAttack and Bladex.GetTime() < Basic_Funcs.BladeVR_AtaqueHasta:\n"
    "        Bladex.AddScheduledFunc(Bladex.GetTime() + Basic_Funcs.RestoreEnergyTime, Basic_Funcs.RestoreEnergyFunc, (EntityName,), 'PlayerRestoreEnergy')\n"
    "        maxe = CharStats.GetCharMaxEnergy(pj.Kind, pj.Level)\n"
    "        e = min(pj.Data.LoseEnergyRate * Basic_Funcs.RestoreEnergyTime, pj.Data.Energy2Lose)\n"
    "        pj.Energy = max(pj.Energy - e, -maxe * Reference.ENERGY_LOW_LEVEL)\n"
    "        pj.Data.Energy2Lose = pj.Data.Energy2Lose - e\n"
    "        return\n"
    "    Basic_Funcs.BladeVR_RestoreEnergyOrig(EntityName)\n"
    "if not hasattr(Basic_Funcs, 'BladeVR_RestoreEnergyOrig'):\n"
    "    Basic_Funcs.BladeVR_RestoreEnergyOrig = Basic_Funcs.RestoreEnergyFunc\n"
    "    Basic_Funcs.RestoreEnergyFunc = BladeVR_RestoreEnergy\n"
    "Basic_Funcs.BladeVR_AtaqueHasta = 0.0\n"
    "BladeVR_AvisoEnergiaHasta = 0.0\n"
    "def BladeVR_AvisoEnergia(t):\n"
    "    global BladeVR_AvisoEnergiaHasta\n"
    "    if t >= BladeVR_AvisoEnergiaHasta:\n"
    "        BladeVR_AvisoEnergiaHasta = t + 2.0\n"
    "        try:\n"
    "            Actions.ReportMsg('You need more energy for this attack')\n"
    "        except:\n"
    "            pass\n"
    "def BladeVR_PesoArma(me):\n"
    "    w = me.GetInventory().GetActiveWeapon()\n"
    "    if not w:\n"
    "        w = me.InvRightBack\n"
    "    if not w:\n"
    "        return 0\n"
    "    if Reference.EntitiesObjectData.has_key(w):\n"
    "        d = Reference.EntitiesObjectData[w]\n"
    "    else:\n"
    "        e = Bladex.GetEntity(w)\n"
    "        if not e or not Reference.DefaultObjectData.has_key(e.Kind):\n"
    "            return 0\n"
    "        d = Reference.DefaultObjectData[e.Kind]\n"
    "    if (d[0] == Reference.OBJ_WEAPON or d[0] == Reference.OBJ_STANDARD) and len(d) > 1:\n"
    "        return d[1]\n"
    "    return 0\n"
    "def BladeVR_Duracion(anim):\n"
    "    try:\n"
    "        d = Bladex.GetAnimationDuration(anim)\n"
    "    except:\n"
    "        d = 0.3\n"
    "    if d <= 0.0:\n"
    "        d = 0.3\n"
    "    return d\n"
    "def BladeVR_Fatiga(anim):\n"
    "    me = Bladex.GetEntity('Player1')\n"
    "    if not me or netgame.GetNetState() != 0:\n"
    "        return (-1, 0, 0, 0, 0)\n"
    "    t = Bladex.GetTime()\n"
    "    cur = me.Energy\n"
    "    maxe = CharStats.GetCharMaxEnergy(me.Kind, me.Level)\n"
    "    if cur <= 0.0:\n"
    "        return (3, 0, maxe, cur, 0)\n"
    "    charF = CharStats.GetCharDamageData(me.CharType, me.Level)\n"
    "    animF = 1.0\n"
    "    if Damage.AnimationData.has_key(anim):\n"
    "        animF = Damage.AnimationData[anim]\n"
    "    elif Damage.AnimationData.has_key(anim[4:]):\n"
    "        animF = Damage.AnimationData[anim[4:]]\n"
    "    cost = max((charF + BladeVR_PesoArma(me)) * animF, 0.0) + me.Data.Energy2Lose\n"
    "    if me.Data.FAttack > 1.0:\n"
    "        cost = 0.0\n"
    "    if cost >= maxe:\n"
    "        BladeVR_AvisoEnergia(t)\n"
    "        return (2, cost, maxe, cur, 0)\n"
    "    code = 0\n"
    "    dur = BladeVR_Duracion(anim)\n"
    "    if cost > cur:\n"
    "        code = 1\n"
    "        torpe = 'g_bad_no'\n"
    "        w = me.GetInventory().GetActiveWeapon()\n"
    "        e = None\n"
    "        if w:\n"
    "            e = Bladex.GetEntity(w)\n"
    "        if e and not e.Person:\n"
    "            torpe = 'g_bad_1h'\n"
    "            try:\n"
    "                f = Reference.GiveWeaponFlag(w)\n"
    "                if f == Reference.W_FLAG_2W:\n"
    "                    torpe = 'g_bad_sword'\n"
    "                elif f == Reference.W_FLAG_AXE:\n"
    "                    torpe = 'g_bad_axe'\n"
    "                elif f == Reference.W_FLAG_SP:\n"
    "                    torpe = 'g_bad_spear'\n"
    "            except:\n"
    "                pass\n"
    "        dur = BladeVR_Duracion(anim[:4] + torpe)\n"
    "        BladeVR_AvisoEnergia(t)\n"
    "    me.Data.Energy2Lose = cost\n"
    "    me.Data.LoseEnergyRate = cost / dur\n"
    "    Basic_Funcs.BladeVR_AtaqueHasta = t + dur\n"
    "    return (code, cost, maxe, cur, dur)\n";
int g_foodBocaApplied = -1;
constexpr double kFoodInstallDelaySec = 2.0;
constexpr double kMouthDownM = 0.08;            // boca: bajo los ojos ...
constexpr double kMouthFwdM = 0.06;             // ... y un poco delante
constexpr double kMouthRadiusM = 0.15;          // mando derecho a menos de esto de la boca
constexpr double kMouthHoldSec = 0.2;
constexpr double kEatCooldownSec = 1.5;
// Centro de la mano (nodo R_Hand / L_Hand) respecto al mando, en sus ejes
// (derecha, abajo, adelante), metros.
constexpr double kGripOffM[3] = { 0.0, 0.03, -0.04 };
constexpr unsigned kToastMs = 2500;
constexpr unsigned long long kCalibCountdownMs = 5000;   // Ctrl+F8: tiempo para estirar los brazos

uintptr_t ModuleBase() {
    static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    return base;
}

void LogLine(const char* text) {
    HookLogger::Instance().Line(std::string(text));
}

double NowSec() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

// --- lecturas seguras (solo POD + __try, regla C2712) --------------------
bool RdPtr(uintptr_t a, uintptr_t* out) {
    if (a < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const uintptr_t*>(a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool RdI32(uintptr_t a, int* out) {
    if (a < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const int*>(a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool RdF32(uintptr_t a, float* out) {
    if (a < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const float*>(a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool RdF64(uintptr_t a, double* out) {
    if (a < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const double*>(a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool RdDoubles(uintptr_t a, double* out, int n) {
    if (a < 0x10000) return false;
    __try {
        const double* src = reinterpret_cast<const double*>(a);
        for (int i = 0; i < n; ++i) out[i] = src[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Copia una cadena C (como mucho n-1 caracteres; lo no imprimible pasa a '?').
bool RdCStr(uintptr_t p, char* buf, int n) {
    buf[0] = 0;
    if (p < 0x10000 || n < 2) return false;
    int i = 0;
    __try {
        const char* src = reinterpret_cast<const char*>(p);
        for (; i < n - 1; ++i) {
            char ch = src[i];
            if (ch == 0) break;
            buf[i] = (ch >= 32 && ch < 127) ? ch : '?';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        buf[0] = 0;
        return false;
    }
    buf[i] = 0;
    return i > 0;
}

// Nombre de un B_Name (su primer campo es el char*).
bool RdBName(uintptr_t nameObj, char* buf, int n) {
    uintptr_t p = 0;
    if (!RdPtr(nameObj, &p)) { buf[0] = 0; return false; }
    return RdCStr(p, buf, n);
}

void EntityName(uintptr_t e, char* buf, int n) {
    if (!e || !RdBName(e + kEntName, buf, n)) {
        std::snprintf(buf, n, "%s", e ? "?" : "-");
    }
}

// Ronda 21: el tipo (Kind de Python) de una entidad: vtable[6] de la entidad
// (+0xf75f0 en las armas) = B_Name en ((entidad+0x280)+0x28)+8.
constexpr uintptr_t kEntObj3D = 0x280;
constexpr uintptr_t kObj3DKind = 0x28;
bool EntityKind(uintptr_t e, char* buf, int n) {
    uintptr_t o3d = 0, kind = 0;
    buf[0] = 0;
    return e && RdPtr(e + kEntObj3D, &o3d) && o3d && RdPtr(o3d + kObj3DKind, &kind) && kind && RdBName(kind + 8, buf, n) && buf[0];
}

// Armas a dos manos (LIB/Reference.py: OBJ_WEAPON con W_FLAG_2W, W_FLAG_AXE o
// W_FLAG_SP). Con una en la mano derecha, la izquierda va al arma (ronda 21).
const char* const kTwoHandKinds[] = {
    "FireBigSword", "IceAxe", "Chaosword", "DeathSword", "LongSword", "Alfanje", "BigSword", "SawSword", "FlatSword",
    "Eclipse", "Guadanya", "Hacha2hojas", "RhinoClub", "Hacharrajada", "IceWand", "SteelFeather", "FireBo", "Bo",
    "Lanza", "Naginata", "Tridente", "Hachacuchilla", "Naginata2", "DeathBo", "CrushBo", "LanzaAncha", "Axpear",
    "Arpon", "Bichero", "Crosspear", "BladeSwordBarbarian", "BladeSword2Barbarian",
};

bool SameNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == 0 && *b == 0;
}

// Ronda 40: los n primeros caracteres sin distinguir mayusculas.
bool SameNoCaseN(const char* a, const char* b, int n) {
    for (int i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
        if (!x) return true;
    }
    return true;
}

bool IsTwoHandKind(const char* kind) {
    for (const char* k : kTwoHandKinds)
        if (SameNoCase(kind, k)) return true;
    return false;
}

bool HasVtable(uintptr_t e, uintptr_t vtOffset) {
    uintptr_t vt = 0;
    return e && RdPtr(e, &vt) && vt == ModuleBase() + vtOffset;
}

bool IsPerson(uintptr_t e) { return HasVtable(e, kVtPerson); }
bool IsWeapon(uintptr_t e) { return HasVtable(e, kVtWeapon); }

// --- matematicas (matrices 4x4 de doubles, vectores fila) ----------------
void XformPoint(const double m[16], const double p[3], double o[3]) {
    o[0] = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    o[1] = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    o[2] = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
}

void MatMul(const double a[16], const double b[16], double c[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 4; ++k) {
            double s = 0.0;
            for (int j = 0; j < 4; ++j) s += a[r * 4 + j] * b[j * 4 + k];
            c[r * 4 + k] = s;
        }
    }
}

double Len3(const double v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// 0 hasta a, 1 desde b, suave entre medias.
double SmoothStep(double a, double b, double x) {
    if (b <= a) return x >= b ? 1.0 : 0.0;
    double t = (x - a) / (b - a);
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return t * t * (3.0 - 2.0 * t);
}

double Dist3(const double a[3], const double b[3]) {
    double d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return Len3(d);
}

void Translation(const double m[16], double t[3]) { t[0] = m[12]; t[1] = m[13]; t[2] = m[14]; }

// Ronda 48: inversa de una matriz afin (vectores fila: filas 0-2 = ejes con su
// escala, fila 3 = posicion).
bool InvAffine(const double m[16], double o[16]) {
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-12)) return false;
    const double inv = 1.0 / det;
    double r[9] = { A * inv, -(b * i - c * h) * inv, (b * f - c * e) * inv,
                    B * inv, (a * i - c * g) * inv, -(a * f - c * d) * inv,
                    C * inv, -(a * h - b * g) * inv, (a * e - b * d) * inv };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) o[row * 4 + col] = r[row * 3 + col];
        o[row * 4 + 3] = 0.0;
    }
    for (int col = 0; col < 3; ++col) o[12 + col] = -(m[12] * r[col] + m[13] * r[3 + col] + m[14] * r[6 + col]);
    o[15] = 1.0;
    return true;
}

bool WrDoubles(uintptr_t a, const double* in, int n) {
    if (a < 0x10000) return false;
    __try {
        double* dst = reinterpret_cast<double*>(a);
        for (int i = 0; i < n; ++i) dst[i] = in[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool WrF64(uintptr_t a, double v) { return WrDoubles(a, &v, 1); }

bool RdU8(uintptr_t a, unsigned char* out) {
    if (a < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const unsigned char*>(a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Funcion del juego (hilo del juego): vista de la camara.
bool CallSetViewType(uintptr_t fn, uintptr_t viewCtl, int type) {
    __try {
        reinterpret_cast<void(__fastcall*)(uintptr_t, int)>(fn)(viewCtl, type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void CopyW(wchar_t* dst, int n, const wchar_t* src) {
    int i = 0;
    for (; src && src[i] && i < n - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// --- esqueleto ---------------------------------------------------------------
struct SkelView {
    uintptr_t skel;
    uintptr_t nodes;
    int count;
    double time;
};

bool GetSkel(uintptr_t person, SkelView* sv) {
    uintptr_t tab = 0;
    if (!RdPtr(person + kPSkel, &sv->skel) || !sv->skel) return false;
    if (!RdPtr(sv->skel + kSkelNodeTable, &tab) || !tab) return false;
    if (!RdPtr(tab + kTabArray, &sv->nodes) || !sv->nodes) return false;
    if (!RdI32(tab + kTabCount, &sv->count) || sv->count <= 0 || sv->count > 128) return false;
    if (!RdF64(sv->skel + kSkelTime, &sv->time)) return false;
    return true;
}

bool NodeWorld(const SkelView& sv, int i, double w[16], bool* fresh) {
    if (i < 0 || i >= sv.count) return false;
    uintptr_t rec = sv.nodes + static_cast<uintptr_t>(i) * kNodeStride;
    uintptr_t wp = 0;
    double t = 0.0;
    if (!RdPtr(rec + kNodeWorld, &wp) || !RdDoubles(wp, w, 16)) return false;
    if (fresh) *fresh = RdF64(rec + kNodeTime, &t) && t == sv.time;
    return true;
}

bool NodeBind(const SkelView& sv, int i, double b[16]) {
    if (i < 0 || i >= sv.count) return false;
    uintptr_t bp = 0;
    return RdPtr(sv.nodes + static_cast<uintptr_t>(i) * kNodeStride + kNodeBind, &bp) && RdDoubles(bp, b, 16);
}

int NodeParent(const SkelView& sv, int i) {
    int p = -2;
    if (i < 0 || i >= sv.count) return -2;
    RdI32(sv.nodes + static_cast<uintptr_t>(i) * kNodeStride + kNodeParent, &p);
    return p;
}

bool MeshOf(uintptr_t person, uintptr_t* mesh) {
    uintptr_t obj = 0;
    return RdPtr(person + kPObj3D, &obj) && obj && RdPtr(obj + kObjMesh, mesh) && *mesh;
}

bool NodeName(uintptr_t mesh, int i, char* buf, int n) {
    uintptr_t arr = 0, el = 0;
    int count = 0;
    buf[0] = 0;
    if (!RdPtr(mesh + kMeshNodeNames, &arr) || !RdI32(mesh + kMeshNodeNameCount, &count)) return false;
    if (i < 0 || i >= count) return false;
    if (!RdPtr(arr + static_cast<uintptr_t>(i) * 8, &el)) return false;
    return RdBName(el + 8, buf, n);
}

int NodeIndexByName(uintptr_t mesh, const char* name) {
    int count = 0;
    if (!RdI32(mesh + kMeshNodeNameCount, &count) || count <= 0 || count > 128) return -1;
    char buf[64];
    for (int i = 0; i < count; ++i) {
        if (NodeName(mesh, i, buf, sizeof(buf)) && _stricmp(buf, name) == 0) return i;
    }
    return -1;
}

struct AnchorInfo {
    char name[48];
    int node;
    double m[16];
};

bool ReadAnchor(uintptr_t mesh, int i, AnchorInfo* a) {
    uintptr_t arr = 0, an = 0;
    int count = 0;
    if (!RdPtr(mesh + kMeshAnchors, &arr) || !RdI32(mesh + kMeshAnchorCount, &count)) return false;
    if (i < 0 || i >= count || count > 256) return false;
    if (!RdPtr(arr + static_cast<uintptr_t>(i) * 8, &an) || !an) return false;
    if (!RdBName(an + kAnchorName, a->name, sizeof(a->name))) std::snprintf(a->name, sizeof(a->name), "?");
    if (!RdI32(an + kAnchorNode, &a->node)) a->node = -1;
    return RdDoubles(an + kAnchorMatrix, a->m, 16);
}

// Objeto enganchado a un anclaje del nodo dado (el escudo va en el anclaje
// "Shield", que esta en L_Forearm); 0 si no hay.
uintptr_t ItemOnNode(uintptr_t person, uintptr_t mesh, int node) {
    uintptr_t arr = 0;
    int n = 0;
    if (!mesh || node < 0 || !RdPtr(person + kPChildArray, &arr) || !arr || !RdI32(person + kPChildCount, &n)) return 0;
    for (int i = 0; i < n && i < 32; ++i) {
        uintptr_t entry = arr + static_cast<uintptr_t>(i) * kChildStride, child = 0;
        int link = 0;
        if (!RdPtr(entry, &child) || !child || !RdI32(entry + kChildLink, &link)) continue;
        int idx = link & 0xffff;
        int nodeIdx = -1;
        if ((link & 0xffff0000) == 0) {
            AnchorInfo a;
            if (ReadAnchor(mesh, idx, &a)) nodeIdx = a.node;
        } else {
            nodeIdx = idx;
        }
        if (nodeIdx == node) return child;
    }
    return 0;
}

// --- arma -------------------------------------------------------------------
constexpr int kMaxAreas = 8;

struct WeaponGeom {
    int nSlash;
    int nImpale;
    int n;                       // areas leidas
    double a[kMaxAreas][3];      // extremos en el espacio del arma
    double b[kMaxAreas][3];
    double cut[kMaxAreas][3];    // direccion de corte (estocada: eje de la punta)
};

bool WeaponWorld(uintptr_t w, double m[16]) {
    uintptr_t obj = 0;
    return RdPtr(w + kWObj, &obj) && obj && RdDoubles(obj + kObjWorld, m, 16);
}

bool ReadWeaponGeom(uintptr_t w, WeaponGeom* g) {
    std::memset(g, 0, sizeof(*g));
    uintptr_t obj = 0, desc = 0, arr = 0, ad = 0;
    if (!RdPtr(w + kWObj, &obj) || !obj || !RdPtr(obj + kObjDesc, &desc) || !desc) return false;
    RdI32(desc + kDescSlashCount, &g->nSlash);
    RdI32(desc + kDescImpaleCount, &g->nImpale);
    if (g->nSlash < 0 || g->nSlash > 64) g->nSlash = 0;
    if (g->nImpale < 0 || g->nImpale > 64) g->nImpale = 0;
    if (g->nSlash > 0 && RdPtr(desc + kDescSlash, &arr)) {
        for (int i = 0; i < g->nSlash && g->n < kMaxAreas; ++i) {
            double c[3], h[3], cut[3];
            if (!RdPtr(arr + static_cast<uintptr_t>(i) * 8, &ad)) continue;
            if (!RdDoubles(ad + kAreaCenter, c, 3) || !RdDoubles(ad + kAreaHalf, h, 3) || !RdDoubles(ad + kAreaCut, cut, 3)) continue;
            int k = g->n++;
            for (int j = 0; j < 3; ++j) { g->a[k][j] = c[j] + h[j]; g->b[k][j] = c[j] - h[j]; g->cut[k][j] = cut[j]; }
        }
    }
    if (g->nImpale > 0 && RdPtr(desc + kDescImpale, &arr)) {
        for (int i = 0; i < g->nImpale && g->n < kMaxAreas; ++i) {
            double base[3], dir[3];
            if (!RdPtr(arr + static_cast<uintptr_t>(i) * 8, &ad)) continue;
            if (!RdDoubles(ad + kAreaCenter, base, 3) || !RdDoubles(ad + kAreaHalf, dir, 3)) continue;
            int k = g->n++;
            for (int j = 0; j < 3; ++j) { g->a[k][j] = base[j]; g->b[k][j] = base[j] + dir[j]; g->cut[k][j] = dir[j]; }
        }
    }
    return true;
}

// --- seguimiento de los ataques del jugador ----------------------------------
// El arma de la mano derecha (con sus areas, para medir el tajo en UpdateSwing)
// y los ataques del juego (animaciones g_*: con A), para la cadencia medida.
struct AttackTrack {
    uintptr_t person;
    uintptr_t weapon;
    bool haveGeom;
    WeaponGeom geom;
    // ataque en curso
    bool inAttack;
    char anim[48];
    char full[64];
};

AttackTrack g_trkPlayer = {};

const char* CombatModeName(int m) {
    if (m == 3) return "manos flotantes (antebrazos, manos y armas en los mandos, sin cuerpo ni limite del brazo; ojos mas altos), A no ataca";   // ronda 48
    return m == 0 ? "VR basico, sin mandos de movimiento" : (m == 2 ? "mandos de movimiento + combos del juego con A" : "mandos de movimiento, A no ataca");
}

bool IsAttackAnimName(const char* s) {
    // Como el motor (+0x155ea0): "g_..." o "g2h_..." sin distinguir mayusculas.
    if (!s || (s[0] != 'g' && s[0] != 'G')) return false;
    if (s[1] == '_') return true;
    return s[1] == '2' && (s[2] == 'h' || s[2] == 'H') && s[3] == '_';
}

void ReadAnimNames(uintptr_t person, char* anim, int na, char* full, int nf) {
    if (!RdBName(person + kPAnimName, anim, na)) std::snprintf(anim, na, "?");
    if (full && !RdBName(person + kPAnimFullName, full, nf)) std::snprintf(full, nf, "?");
}

uintptr_t InventoryItem(uintptr_t person, uintptr_t slot) {
    uintptr_t inv = 0, item = 0;
    if (!RdPtr(person + kPInventory, &inv) || !inv) return 0;
    if (!RdPtr(inv + slot, &item)) return 0;
    return item;
}

// Arma de la mano derecha (sus areas, al cambiar) y ataque del juego en curso.
void UpdateTrack(AttackTrack& tr, uintptr_t person) {
    if (person != tr.person) {
        tr.inAttack = false;
        std::memset(&tr.geom, 0, sizeof(tr.geom));
        tr.person = person;
        tr.weapon = 0;
        tr.haveGeom = false;
    }
    if (!person) return;
    uintptr_t weapon = InventoryItem(person, kInvRight);
    if (weapon != tr.weapon) {
        tr.weapon = weapon;
        tr.haveGeom = weapon && ReadWeaponGeom(weapon, &tr.geom) && tr.geom.n > 0;
    }
    char anim[48], full[64];
    ReadAnimNames(person, anim, sizeof(anim), full, sizeof(full));
    bool attacking = IsAttackAnimName(anim);
    if (tr.inAttack && (!attacking || std::strcmp(anim, tr.anim) != 0 || std::strcmp(full, tr.full) != 0)) {
        tr.inAttack = false;
    }
    if (!tr.inAttack && attacking) {
        tr.inAttack = true;
        std::snprintf(tr.anim, sizeof(tr.anim), "%s", anim);
        std::snprintf(tr.full, sizeof(tr.full), "%s", full);
    }
}

// --- estado general ------------------------------------------------------------
bool g_installed = false;
uintptr_t g_player = 0;          // jugador del ultimo fotograma (hilo del juego)
uintptr_t g_lastPlayer = 0;      // jugador, malla y nodos del ultimo jugador nuevo (o nivel cargado)
uintptr_t g_lastMesh = 0;
int g_lastNodeCount = 0;
double g_prevFrame = 0.0;
double g_minuteStart = 0.0;

// Contadores del resumen del minuto (hilo del juego).
long long g_attacksPlayer = 0;   // ataques del juego (A)

// --- brazos del jugador ---------------------------------------------------------
enum { ARM_L = 0, ARM_R = 1 };   // mismo orden que GetVrHandState (0 izquierdo, 1 derecho)
const char* const kArmNodeNames[2][5] = { { "L_Arm", "L_Elbow", "L_Forearm", "L_Wrist", "L_Hand" },
                                          { "R_Arm", "R_Elbow", "R_Forearm", "R_Wrist", "R_Hand" } };
const char* const kArmLabel[2] = { "izq", "dcho" };

struct ArmInfo {
    bool ok;
    int node[5];             // brazo (hombro), codo, antebrazo, muneca, mano
    double bindH[16];        // B de la mano
    double handBody[9];      // mano en la pose de reposo, en la base del cuerpo (filas; R, D, F)
    double hingeLocal[3];    // eje del codo (flexion natural) en los ejes del brazo
    double lenUpper, lenLower, lenHand;
    bool restOk;             // giro desde la pose de reposo (ik::RestTwistRelations)
    double restRelE[9], restRelFo[9], restRelWr[9];
    // Ronda 48 (modo 3, manos flotantes): codo, antebrazo y muneca respecto a
    // la mano en la pose de reposo (W_k * inv(W_mano), muneca recta).
    bool floatOk;
    double floatRel[3][16];
    // Ronda 57: clavicula (padre del brazo; hijo del pecho) y las B de clavicula y brazo.
    int clav;
    bool restShoulderOk;
    double bindClav[16], bindArm[16];
};

struct ChainInfo {
    uintptr_t player;
    uintptr_t skel;
    uintptr_t mesh;
    int count;
    bool ok;                 // los dos brazos
    int head;
    int root;                // nodo sin padre (Center)
    int chest;               // "Chest": padre de la cabeza y de las claviculas
    unsigned long long legMask;   // ronda 39: la raiz y lo que cuelga de ella sin pasar por el pecho (piernas, pies)
    bool bodyOk;             // se puede llevar el cuerpo bajo el visor
    double chestFwdLocal[3]; // "adelante" del cuerpo en los ejes del pecho (de la pose de reposo)
    bool haveView;
    double viewAnchor[16];   // ViewPoint (camara en primera persona) en los ejes de Head
    ArmInfo arm[2];
};
ChainInfo g_chain = {};

// Agarre: ejes del mando (filas r, d, f) en la base del cuerpo con los brazos
// colgando relajados y los pulgares hacia delante (la pose de reposo del
// esqueleto). Ctrl+F8 lo mide y se guarda en BladeVR.ini (ronda 55; antes en
// BladeVR_manos.txt); sin calibrar, kHangDefault.
double g_ctrlHang[2][9];
// Alcance del jugador (hombro -> mano con el brazo estirado, metros), medido
// en la calibracion; 0 = sin medir.
double g_userReachM[2] = { 0.0, 0.0 };
bool g_ctrlHangCalibrated[2] = { false, false };

// Lo que necesitan los evaluadores enganchados (los escribe el hilo del juego
// una vez por fotograma; protegido por si algun evaluador corre en otro hilo).
struct IkShared {
    uintptr_t skel;
    uintptr_t nodes;         // array de registros de nodo (0x20 c/u)
    double weight[2];
    double handRot[2][9];    // mano pedida (filas, mundo)
    double handPos[2][3];    // mm, mundo
    double bodyRight[3];
    double bodyFwd[3];
    int node[2][5];
    double bindH[2][16];
    double hingeLocal[2][3];
    double maxStretch[2];    // alargamiento maximo de cada brazo (alcance del jugador / del personaje)
    bool lockWrist[2];       // algo enganchado en el antebrazo (escudo): la mano no se dobla
    bool restTwist[2];       // giro de codo, antebrazo y muneca desde la pose de reposo
    double restRel[2][3][9]; // codo, antebrazo, muneca respecto a la base del antebrazo (reposo)
    double twoHand;          // ronda 21: 0..1, la mano izquierda en el arma a dos manos de la derecha
    bool gripFixed;          // ronda 24: agarre fijo de este arma (relacion entre las manos del juego, quieto)
    double gripRel[3];       // mano izquierda en los ejes de la mano derecha (mm)
    double gripRelRot[9];
    bool gripCapture;        // ronda 24: guardar ya la relacion como agarre fijo del arma
    bool gripAccum;          // ronda 26: sumar la relacion de este fotograma a la media de reposo
    bool twoHandWeapon;      // ronda 27: arma a dos manos en la derecha (se calcula su agarre aunque no se coja)
    bool floating;           // ronda 48: modo 3, antebrazos y manos flotantes (sin cinematica inversa)
    bool restShoulder[2];    // ronda 57: hombro (clavicula y brazo) en reposo sobre el pecho
    bool bowLeft;            // ronda 58: arco en la izquierda (su hombro, el de la animacion)
    int clav[2];
    int chest;
    double bindClav[2][16], bindArm[2][16];
    bool floatOk[2];
    double floatRel[2][3][16];
};
SRWLOCK g_ikLock = SRWLOCK_INIT;
IkShared g_ikShared = {};
std::atomic<uintptr_t> g_ikSkel{0};             // esqueleto con los brazos resueltos (0 = ninguno)
std::atomic<unsigned long long> g_ikNodeMask{0};  // nodos de los brazos activos

// Cuerpo bajo el visor (hilo del juego; bajo g_ikLock): la raiz se desplaza
// en horizontal lo que el visor se ha separado de la camara del juego, el
// pecho gira hacia donde estan la vista y las manos, y a la cabeza se le
// deshacen las dos cosas (la camara del juego, que sale de ella, no cambia).
struct BodyShared {
    uintptr_t skel;
    uintptr_t nodes;
    int root, chest, head;
    double delta[3];         // mm, mundo (y = 0)
    double fwd[3];           // adelante pedido para el pecho (horizontal, unitario)
    double weight;
    double chestWeight;      // giro del pecho (kChestLockedWeight con un enemigo fijado; ronda 18)
    double chestFwdLocal[3];
};
BodyShared g_body = {};
std::atomic<uintptr_t> g_bodySkel{0};
std::atomic<unsigned long long> g_bodyNodeMask{0};
// Correccion que se dio al pecho la ultima vez (para deshacerla en la cabeza).
double g_chestRot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
double g_chestPivot[3] = { 0, 0, 0 };
double g_chestDelta[3] = { 0, 0, 0 };
double g_bodyFwdSmooth[3] = { 0, 0, 1 };
bool g_bodyFwdValid = false;
bool g_bodyWasActive = false;
double g_chestWeight = 1.0;      // suavizado: 1 sin enemigo fijado, kChestLockedWeight con el
int g_chestClampSide = 0;        // lado del tope (+1 / -1) con la vista detras del pecho (0 = sin tope)
double g_viewHeightSince = 0.0;
double g_viewHeightNextHint = 0.0;

// Tajo por gesto (hilo del juego; g_swingActive y g_swingT0 tambien los lee la IA).
struct SwingState {
    bool active;
    bool owned;              // la deteccion la encendio el mod (hay que apagarla)
    uintptr_t weapon;
    double t0;
    int slowFrames;
    int fastFrames;          // fotogramas seguidos por encima de kSwingOnMs (antes de encender)
    double cooldownUntil;
    double firstHitAt;       // ronda 34: primer golpe de este tajo (-1 ninguno)
    int retractFrames;       // ronda 34: fotogramas seguidos con la mano hacia el cuerpo
    bool energyDenied;       // ronda 59: sin resistencia para este ataque (no se enciende el arma)
    double clumsySec;        // ronda 59: ataque torpe (duracion de su animacion; 0 = normal)
};
SwingState g_swing = {};
// --- ronda 34 (pedido del usuario) ---------------------------------------------------
// 1) Recoger el brazo hacia el cuerpo no golpea: con la mano (mando real) yendo
//    hacia el pecho en horizontal (esa componente > kRetractRatio de su
//    velocidad, a mas de kRetractMinMs) no se enciende el arma ni los punos, y
//    si estaban encendidos se apagan (dos fotogramas seguidos). Al pegarse al
//    enemigo tras un tajo, subir el brazo le hacia dano. Solo en horizontal:
//    un tajo de arriba abajo baja hacia el pecho y si vale.
// 2) Cadencia del juego: tras un golpe que toca, el arma (o los punos) no
//    vuelve a encenderse hasta que pase lo que dura el ataque basico del juego
//    con esa clase de arma y ese personaje (la animacion de "A" de
//    SCRIPTS/Combos/*Combos.py, duracion de Bladex.GetAnimationDuration, nativa
//    +0x142f90). Asi no se hace mas dano por segundo que con el juego.
constexpr double kRetractMinMs = 0.5;
constexpr double kRetractRatio = 0.5;
constexpr int kRetractFrames = 2;
// Ronda 42: subir el brazo hacia uno mismo (volver a armar el golpe) tampoco
// golpea: la mano va hacia el cuello en 3D (> kRetractNeckRatio), hacia arriba
// (> kRetractUpRatio) y algo hacia el cuerpo en horizontal (> kRetractNeckHoriz).
// Antes solo contaba en horizontal y ese "tajo de vuelta" sacaba chispas de la
// pared. Un tajo hacia arriba por delante se aleja del cuerpo y si vale.
constexpr double kRetractNeckRatio = 0.6;
constexpr double kRetractUpRatio = 0.3;
constexpr double kRetractNeckHoriz = 0.15;
constexpr double kNeckBelowEyesM = 0.15;
// Ronda 42: cadencia cuando no se sabe la del personaje (mejor algo de limite
// que ninguno: en la prueba del 12-10 no se supo en toda la partida).
constexpr double kCadenceDefaultSec = 0.85;
constexpr double kChestBelowEyesM = 0.25;   // el pecho, bajo el visor
constexpr uintptr_t kFnAnimDuration = 0x142f90;   // (nombre, double* s) -> 1 si existe
struct HandMot {
    bool valid;
    double speed;            // m/s (espacio de seguimiento)
    double toward;           // parte de la velocidad que va hacia el pecho (-1..1)
    double towardNeck;       // ronda 42: parte que va hacia el cuello, en 3D (-1..1)
    double up;               // ronda 42: parte que va hacia arriba (-1..1)
};
HandMot g_handMot[2] = {};
double g_handPrev[2][3];
bool g_handHavePrev[2] = { false, false };
double g_handMotT = 0.0;
// Ataque basico de "A" por personaje (prefijo de sus animaciones) y clase de arma.
struct GiEntry { const char* chr; const char* cls; const char* anim; };
const GiEntry kGiAnims[] = {
    { "Kgt", "1H", "Kgt_g_08_new" }, { "Kgt", "NO", "Kgt_g_punch2" },
    { "Bar", "1H", "Bar_g_07" },     { "Bar", "2W", "Bar_g2h_08" },    { "Bar", "AXE", "Bar_g_axe08" }, { "Bar", "NO", "Bar_g_punch4" },
    { "Amz", "SP", "Amz_g_spears1" }, { "Amz", "NO", "Amz_g_kick5" },
    { "Dwf", "1H", "Dwf_g_01" },     { "Dwf", "NO", "Dwf_g_punch1" },
};
// Clases de LIB/Reference.py (W_FLAG_2W, W_FLAG_AXE, W_FLAG_SP); el resto, 1H.
const char* const k2WKinds[] = { "FireBigSword", "Chaosword", "DeathSword", "LongSword", "Alfanje", "BigSword", "SawSword",
                                 "FlatSword", "BladeSwordBarbarian", "BladeSword2Barbarian" };
const char* const kAxeKinds[] = { "IceAxe", "Eclipse", "Guadanya", "Hacha2hojas", "RhinoClub", "Hacharrajada" };
const char* const kSpKinds[] = { "IceWand", "SteelFeather", "FireBo", "Bo", "Lanza", "Naginata", "Tridente", "Hachacuchilla",
                                 "Naginata2", "DeathBo", "CrushBo", "LanzaAncha", "Axpear", "Arpon", "Bichero", "Crosspear" };
struct Cadence {
    uintptr_t weapon;        // 0 = punos
    char chr[8];
    double sec;              // 0 = sin cadencia (no se supo)
    double animSec;          // ronda 35: duracion de la animacion segun el juego
    char anim[40];
};
// Ronda 35: el juego deja empezar el siguiente ataque antes de que acabe la
// animacion (la vuelta a la guardia se corta): en los logs del juego normal
// (6-10-2026) el jugador pulsando A seguido daba Kgt_g_punch2 (1,22 s) cada
// 1,03-1,07 s como mucho (0,84-0,88). La cadencia es esa fraccion de la
// animacion. (Los ataques de la IA se cortan antes, 0,63-0,74: no valen.)
constexpr double kCadenceFactor = 0.85;   // punos (Kgt_g_punch2, ronda 35)
// Ronda 43: con arma, pulsando A seguido el juego empieza el siguiente ataque
// basico al acabar la animacion: Kgt_g_08_new (0,95 s) cada 0,99-1,16 s, nunca
// menos de 0,99 (logs del 12-10, sin contar los de la resistencia al minimo,
// Kgt_g_bad_1h, ni el siguiente). Con 0,85 (0,81 s) el mando daba un 20 % mas.
constexpr double kCadenceAfterEndSec = 0.04;
// Ronda 43: y si el jugador ataca con A, lo que se le mida al juego (el minimo
// entre dos ataques basicos seguidos iguales y sin cansancio) manda.
struct CadenceLearned {
    char anim[40];
    double sec;
};
CadenceLearned g_cadLearned[16] = {};
int g_cadLearnedN = 0;
double CadenceLearnedFor(const char* anim) {
    for (int i = 0; i < g_cadLearnedN; ++i)
        if (std::strcmp(g_cadLearned[i].anim, anim) == 0) return g_cadLearned[i].sec;
    return -1.0;
}
// Ronda 35: prefijo del personaje solo de los de kGiAnims; con otras
// animaciones (Rlx_*, las comunes) se queda el ultimo bueno (con "Rlx" la
// cadencia se iba a 0 y el Gladius hizo dano cada 0,70 s sin limite).
bool KnownCadenceChr(const char* chr) {
    for (const GiEntry& e : kGiAnims)
        if (std::strcmp(e.chr, chr) == 0) return true;
    return false;
}
Cadence g_cadW = {}, g_cadP = {};
double g_nextWeaponDamageAt = 0.0, g_nextPunchDamageAt = 0.0;
// Tajos y punetazos que dan (a alguien o a la pared: el barrido del arma mete
// tambien el nivel en la lista de golpeados, +0xfb270 -> +0xf9ae0), por minuto.
long long g_swingHits = 0, g_punchHits = 0;
// Ronda 43: dos ataques del juego (A) seguidos cuentan para la cadencia
// medida si empiezan a menos de kGameAttackGapSec.
constexpr double kGameAttackGapSec = 3.0;
double g_lastGameAttackAt = -1.0;
bool g_lastGameAttackBad = false;
char g_lastGameAttackFull[64] = "";

// Ronda 43: minimo medido entre dos ataques basicos del juego iguales y seguidos.
// Solo si es el ataque basico de la cadencia del arma o de los punos y parece
// razonable (entre 0,5 y 1,5 veces lo calculado).
void NoteLearnedCadence(const char* anim, double iv) {
    Cadence* c = nullptr;
    if (g_cadW.anim[0] && std::strcmp(g_cadW.anim, anim) == 0) c = &g_cadW;
    else if (g_cadP.anim[0] && std::strcmp(g_cadP.anim, anim) == 0) c = &g_cadP;
    if (!c) return;
    double formula = c->weapon ? c->animSec + kCadenceAfterEndSec : c->animSec * kCadenceFactor;
    if (formula <= 0.0 || iv < 0.5 * formula || iv > 1.5 * formula) return;
    double old = CadenceLearnedFor(anim);
    if (old > 0.0 && iv >= old - 0.005) return;
    int i = 0;
    for (; i < g_cadLearnedN; ++i)
        if (std::strcmp(g_cadLearned[i].anim, anim) == 0) break;
    if (i == g_cadLearnedN) {
        if (g_cadLearnedN >= 16) return;
        ++g_cadLearnedN;
        std::snprintf(g_cadLearned[i].anim, sizeof(g_cadLearned[i].anim), "%s", anim);
    }
    g_cadLearned[i].sec = iv;
    c->sec = iv;
}
// Ronda 32: ultimo fotograma de partida (IkFrame). Los evaluadores no tocan
// brazos ni cuerpo si no hubo ninguno en kIkStaleSec: al morir la camara pasa
// a la de la muerte, el escritor de la camara de partida deja de llamarse
// (CombatVROnGameFrame) y los brazos se quedaban donde estaban los mandos
// (levantados o enredados mientras el cuerpo caia). Igual en cinematicas.
constexpr double kIkStaleSec = 0.15;
std::atomic<double> g_ikFrameAt{0.0};
bool IkFrameFresh() { return NowSec() - g_ikFrameAt.load(std::memory_order_relaxed) <= kIkStaleSec; }
// Ronda 33 (pedido del usuario): escudo VR. A veces se subia el escudo y no
// paraba: el juego solo prueba el escudo si la persona "esta bloqueando"
// (+0xb2240: su accion es Rlx_block, Shattack_* ...), y el gesto que pulsa LB
// salto 3 veces en un minuto de combate. Con el escudo en el antebrazo
// izquierdo siguiendo al mando y la mano izquierda como mucho
// kShieldRaisedMaxDropM bajo los ojos, el jugador "esta bloqueando". La prueba
// del escudo (+0xbded0, cada lado del cuadrilatero del golpe; tambien las
// estocadas) se cambia por la del escudo real: para el golpe si pasa a menos
// de kShieldSphereM de el, o por delante (del lado del escudo) a su altura
// (+-kShieldBandM) y a menos de kShieldBodyReachM del cuerpo. El resto
// (rebote, sonido, desgaste, rotura) lo hace el juego.
// Ronda 35 (pedido del usuario: cubria con el escudo algo bajo): 0,30 m bajo
// los ojos (hombros y cara; antes 0,45, el pecho) para alzarlo, y se baja al
// pasar de kShieldLowerDropM (margen para que no parpadee en el limite).
constexpr double kShieldRaisedMaxDropM = 0.30;
constexpr double kShieldLowerDropM = 0.34;
constexpr double kShieldSphereM = 0.45;
constexpr double kShieldBandM = 0.30;
constexpr double kShieldBodyReachM = 1.0;
bool g_vrShieldUp = false;                 // hilo del juego (IkFrame)
uintptr_t g_vrShieldItem = 0;              // escudo en el antebrazo izquierdo con el brazo en el mando
uintptr_t g_vrShieldPlayer = 0;
// Ronda 57 (pedido del usuario): el arma a dos manos, cogida con las dos y
// alzada como el escudo (la mano mas alta a la altura de los hombros o mas
// arriba, fuera de un tajo), bloquea: el jugador "esta bloqueando" (+0xb2240)
// y el juego prueba el golpe con el bloqueo del arma (radio/alto/cono, +0x41c).
bool g_vr2hBlockUp = false;                // hilo del juego (IkFrame)
long long g_vrShieldStops = 0, g_vr2hStops = 0;   // golpes parados (escudo VR / arma a dos manos), por minuto
// Ronda 31: punetazo (armas naturales de la persona).
struct PunchState {
    bool active;
    bool owned;              // las encendio el mod (hay que apagarlas)
    int hand;
    double t0;
    int slowFrames;
    int fastFrames[2];
    double cooldownUntil;
    int hitsSeen;
    double firstHitAt;       // ronda 34
    int retractFrames;       // ronda 34
    bool energyDenied;       // ronda 59
    double clumsySec;        // ronda 59
};
PunchState g_punch = {};
bool g_weaponGestureReady = false;               // el tajo con el arma esta listo (UpdateSwing)
std::atomic<int> g_hapticHit{0};                 // 1 + mano: golpe de un punetazo (lo hace Present)
std::atomic<bool> g_swingActive{false};
std::atomic<double> g_swingT0{0.0};
long long g_swingStarts = 0, g_punches = 0;      // por minuto

// Personaje que anda solo (hilo del juego: el fotograma y el proceso del mando).
bool g_padNeutral = false;
double g_padNeutralSince = 0.0;
double g_padLastInput = 0.0;
double g_walkPrevPos[3] = { 0, 0, 0 };
double g_walkPrevT = 0.0;
bool g_walkHavePrev = false;
double g_walkSpeed = 0.0;                        // m/s en horizontal, suavizada (posicion del juego)
bool g_walkStuck = false;                        // pedir que se suelten las teclas
double g_walkNextRelease = 0.0;
// Episodio en curso.
bool g_walkEp = false;
double g_walkEpT0 = 0.0, g_walkEpSlowSince = 0.0;
double g_walkNextStop = 0.0;
// Ronda 40: parada al soltar el joystick (StopTest al momento).
bool g_stopTestEp = false;          // soltado y aun andando
int g_stopTestSends = 0;
double g_stopTestNext = 0.0;
// Comida en la mano (hilo del juego).
int g_foodInstallState = 0;                      // 0 pendiente, 1 instalado, -1 fallo
int g_foodInstallTries = 0;
double g_foodInstallAt = 0.0;
double g_mouthSince[2] = { 0.0, 0.0 };
double g_eatNextAt = 0.0;
bool g_dioramaIniDone = false;      // ronda 54: diorama de BladeVR.ini ya mirado (una vez por arranque)
double g_dioramaIniFpAt = -1.0;
// Ronda 50: arco VR (hilo del juego salvo los atomicos). Con el arco en la mano
// izquierda y una flecha en la derecha, en los modos 1 y 3: el gatillo derecho
// apretado con la mano derecha junto al arco agarra la cuerda; separando las
// manos se tensa (la flecha apunta de la mano derecha al arco) y al soltar el
// gatillo sale la flecha, mas rapida cuanto mas tensada.
constexpr double kBowNockM = 0.25;        // mano derecha a menos de esto de la izquierda para agarrar la cuerda
constexpr double kBowSlackM = 0.20;       // separacion de las manos sin tensar
constexpr double kBowFullDrawM = 0.65;    // tensado del todo
constexpr double kBowMinDrawM = 0.30;     // por debajo, soltar no dispara (se destensa)
constexpr double kBowSpeed = 40000.0;     // unidades/s a tope (la del juego: Rel2AbsVector(0,0,-40000))
constexpr double kBowMinSpeedFrac = 0.35;
struct BowVr {
    int pyState, pyTries;     // Python del arco: 0 pendiente, 1 instalado, -1 fallo
    double pyAt;
    bool equipped;            // arco en la izquierda con los brazos en los mandos (modo 1/3)
    bool drawing, startTried;
    uintptr_t arrow;          // la flecha que se tensa
    bool relValid;            // giro de la flecha respecto a la mano derecha (filas)
    double rel[9];
    bool prevBow;                               // ronda 52: arco en la izquierda el fotograma anterior
    double stripUntil;                          // ronda 52: hasta cuando se devuelve al carcaj la flecha que pone el juego al sacar el arco
};
BowVr g_bow = {};
long long g_bowShots = 0;   // flechas disparadas, por minuto
// Ronda 59: resistencia de los tajos y punetazos con el mando (kPyEnergy).
struct EnergyVr {
    int pyState, pyTries;     // Python de la resistencia: 0 pendiente, 1 instalado, -1 fallo
    double pyAt;
    int evalState;            // canal de Python (PyEvalNumbers): -1 sin mirar, 0 no va, 1 va
    long long denied;         // ataques que no salen (sin resistencia o el arma pide el maximo), por minuto
    int lines;                // lineas de error en este minuto
};
//                    pyState pyTries pyAt evalState denied lines
EnergyVr g_energy = { 0,      0,      0.0, -1,       0,     0 };
constexpr int kMaxEnergyLinesPerMin = 30;
// Ronda 60 (pedido del usuario): Mayus + F8 la pone o la quita (BladeVR.ini "resistencia").
std::atomic<bool> g_energyOn{true};
bool g_leftBowIk = false;   // ronda 58: arco en la izquierda (IkFrame; el brazo izquierdo usa el hombro de la animacion)
std::atomic<bool> g_bowZonePub{false};
std::atomic<bool> g_bowEquippedPub{false};   // ronda 51: para el gesto de la espalda (VrPadHook)
std::atomic<int> g_bowArrowReq{0};            // ronda 51: sacar una flecha del carcaj (gesto de la espalda)
std::atomic<int> g_hapticBow{0};          // 1 cuerda agarrada, 2 disparo (lo hace Present)
std::atomic<int> g_hapticRight{0};               // vibracion pedida para el mando derecho (la hace Present)
std::atomic<int> g_hapticLeft{0};                // vibracion pedida para el mando izquierdo (la hace Present)
// Ronda 10 (hilo del juego salvo los atomicos).
double g_airUntil = 0.0;                         // hasta cuando cuenta "acaba de escalar / caer / saltar"
double g_runGuardNext = 0.0;
double g_lockSeenAt = -1.0;                      // ultima vez con un enemigo fijado vivo
std::atomic<unsigned long long> g_gameplayTick{0};  // ultimo fotograma de partida (sin menu, jugador vivo)
std::atomic<bool> g_lockedPub{false};
std::atomic<int> g_unlockRequest{0};
std::atomic<int> g_selectRequest{0};
// Ronda 15: "suelto" = lo que se manda al juego desde los Quest es 0 (el
// joystick dentro de la zona muerta), sin depender de que el juego procese su
// mando ese fotograma.
bool g_questIdle = false;
// Ronda 66: el movimiento es de los Quest: su joystick se esta moviendo, o se
// solto con el personaje en marcha y aun no ha parado (escaladas y caidas
// incluidas). Con el teclado o el mando de Xbox el mod no para al personaje ni
// le quita Run, y los mandos quietos no sueltan "adelante".
bool g_questDriven = false;
double g_questIdleSince = 0.0;
// Ronda 12: tras bloquear, Supr sigue en pausa kStrafeHoldSec (al soltar el
// bloqueo el error acumulado mandaba muchos giros de golpe: 0,9-1,3 m de
// deslizamiento con 13-22 giros en Rlx / Chg_r).
constexpr double kStrafeHoldSec = 0.6;
double g_strafeHoldUntil = 0.0;
// Ronda 13: giro directo. Quieto, las peticiones de giro de Supr hacian que el
// personaje diera pasos al girar en el sitio (0,5-1,3 m con 20-30 giros
// seguidos, sobre todo al soltar el bloqueo). Ahora, quieto, Supr pone el
// angulo del personaje (atributo de Python Angle, como
// Actions.TurnToFaceEntityNow) poco a poco hacia la vista: sin animacion de
// giro ni pasos. El signo de Angle respecto a la guinada de la entidad se mide
// en marcha (+0xa00d0 lee Angle); hasta entonces, y andando, los giros de
// siempre.
constexpr uintptr_t kFnGetAngle = 0xa00d0;       // double (persona): Angle
constexpr double kDirectTurnDegPerSec = 360.0;
constexpr int kAngleVotesNeeded = 5;
int g_angleSign = 0;                             // +1 / -1 medido; 0 sin medir
int g_angleVotesPos = 0, g_angleVotesNeg = 0;
bool g_angleHavePrev = false;
double g_anglePrevA = 0.0, g_anglePrevY = 0.0;
// Ronda 62: el seguro ya no lo quita para toda la partida (el 13-10 salto al
// salir del diorama y desde ahi, quieto, los giros de raton hacian dar pasos
// al personaje): se vuelve a probar a los kDirectRetrySec, al cargar y al
// salir del diorama.
constexpr double kDirectRetrySec = 15.0;
double g_directOffUntil = 0.0;
double g_directLastT = 0.0;
double g_directStartErr = 0.0;
int g_directSteps = 0;                           // pasos seguidos sin acercarse
int g_directLines = 0;                           // errores apuntados en este minuto
uintptr_t g_movingPad = 0;                       // ronda 67: el ultimo mando con el stick izquierdo movido
bool g_movingPadIsQuest = false;
double g_movingPadAt = -1.0;
uintptr_t g_activePad = 0;                       // ronda 67: el ultimo mando en uso (sticks o gatillos)
double g_activePadAt = -1.0;
uintptr_t g_sdlDev = 0;                          // ronda 67: el ultimo mando que no es de los Quest (Xbox por SDL)
uintptr_t g_questDev = 0;                        // ronda 68: el mando del juego de los Quest
bool g_ikAvailable = false;                     // los dos evaluadores enganchados
std::atomic<int> g_calibRequest{0};
std::atomic<bool> g_firstPersonNow{false};

// Ultima solucion de cada brazo (bajo g_ikLock, exclusivo).
struct ArmStat {
    bool valid;
    double anim[5][3];       // posiciones de la animacion (el hombro, para el alcance de Ctrl+F8)
    double reachMax;         // brazo + antebrazo sin alargar (mm)
    bool twistValid;         // twistRad sirve de anterior para el siguiente (giro continuo)
    double twistRad;
    bool poleValid;          // ronda 23: codo de la resolucion anterior (ejes del cuerpo) para suavizarlo
    double poleBody[3];
    double poleT;
};
ArmStat g_armStat[2] = {};
unsigned g_frameThread = 0;
thread_local bool t_inSolve = false;

// Hilo del juego.
double g_ikWeight[2] = { 0.0, 0.0 };
double g_lastHandRot[2][9];
double g_lastHandPos[2][3];
bool g_haveLastTarget[2] = { false, false };
// Mapa visor -> juego con que se colocaron las manos en este fotograma (para
// llevar el arma de vuelta al espacio de seguimiento y medir el tajo).
TrackingToGameMap g_ikMap = {};
bool g_ikMapValid = false;
// Lo que va enganchado en cada antebrazo (escudo) en este fotograma.
uintptr_t g_forearmItem[2] = { 0, 0 };
// Ronda 21: arma a dos manos en la mano derecha (por su tipo) y peso de la
// mano izquierda en el arma (ronda 27: entra y sale en kGrabBlendSec al coger o soltar).
constexpr double kTwoHandSoftReach = 0.03;      // ronda 22: alcance suave del brazo izquierdo en el arma
uintptr_t g_rightItemSeen = 0;
bool g_rightTwoHanded = false;
double g_twoHandW = 0.0;
// Ronda 23: la relacion entre las manos de la animacion pasa por un filtro
// paso bajo (hasta fijarla, ronda 24), el codo se suaviza y el giro del
// antebrazo se mide desde la animacion (que ya agarra el arma), no desde la
// pose de reposo (en la simulacion pasaba de 140-270 grados, con saltos de
// 135, a menos de 95). Ronda 25: el temblor de las rondas 21-24 era el peso a
// dos manos alternando 1 / 0,93 en cada fotograma (ver ik::StepToward); se
// quitan el filtro "1 euro" del giro de la mano derecha y el deslizamiento
// por el mango, que intentaban tapar sus efectos.
constexpr double kGripRelCutHz = 0.5;
// Ronda 24 (pedido del usuario): la mano izquierda fija en el agarre del
// juego para cada arma (la relacion entre las manos de su animacion de
// reposo, guardada al llevar kGripFixRlxSec en ella), sin deslizarse por el
// mango; el brazo se puede alargar algo mas para no soltarla.
constexpr double kGripFixRlxSec = 1.0;
// Ronda 26: el agarre fijado del baston de la amazona no quedaba en el arma.
// Se guardaba la relacion filtrada (paso bajo 0,5 Hz) que arrastraba la de la
// animacion de sacar el arma (Chg_r), y con el baston la mano izquierda esta
// alli muy lejos de donde acaba. Ahora es la media de la animacion de reposo
// sola, saltando su primer medio segundo (el juego aun funde las dos), y se
// comprueba contra el eje del arma.
constexpr double kGripFixSkipSec = 0.5;
constexpr double kGripShaftSnapMm = 25.0;   // mas lejos del eje que la mano derecha: se lleva a el
constexpr double kGripShaftMaxRightMm = 100.0;   // el eje pasa por la mano derecha (mango, no el filo de un hacha)
constexpr double kTwoHandMaxStretch = 1.35;
constexpr double kTwoHandPoleSec = 0.06;
constexpr double kTwoHandRightGraceSec = 1.2;   // el baston deja la mano derecha vacia un momento al atacar
constexpr double kGripSameFrameSec = 0.002;     // dos resoluciones a menos de esto: el mismo fotograma
struct GripState {
    bool valid;
    double lastT;
    double relF[3];
    double relQ[4];
    double outRelRot[9], outRel[3];
    bool captured;           // ronda 24: relacion guardada para el agarre fijo (la recoge IkFrame)
    double capRel[3], capRelRot[9];
    int accN;                // ronda 26: media de la relacion en reposo (sin la animacion de sacar el arma)
    double accRel[3], accQ[4];
};
GripState g_grip = {};
// Ronda 24: agarre fijo de cada tipo de arma a dos manos (en esta partida).
struct GripFixed {
    char kind[32];
    double rel[3];
    double relRot[9];
};
GripFixed g_gripTable[32] = {};
int g_gripTableN = 0;
int g_gripCur = -1;              // agarre fijo del arma de la mano derecha (-1: aun no)
char g_gripKind[32] = "";
double g_gripRlxSince = -1.0;
bool g_gripCaptureReq = false;
bool g_gripAccumReq = false;     // ronda 26: sumando la relacion de la animacion de reposo
int g_gripShaftCheck = -1;       // ronda 26: agarre recien fijado por comprobar contra el eje del arma
// Ronda 27 (pedido del usuario): el arma a dos manos empieza solo en la mano
// derecha; la izquierda la coge al acercar el mando izquierdo a su agarre
// y la suelta al separarlo (hasta la ronda 35). El pecho pasa a la postura
// del juego al cogerla, poco a poco (kTwoHandChestSec).
// Ronda 36 (pedido del usuario): sin distancia para soltarla. La coge con el
// agarre izquierdo apretado y el mando a menos de kGrabOnM de su agarre, y la
// sujeta mientras se mantiene apretado (ese agarre no va al juego: VrPadHook);
// al soltarlo, la suelta. (Rondas 27-35: la cogia sola a 15 cm y la soltaba a
// 30-50 cm del agarre.)
constexpr double kGrabOnM = 0.20;
constexpr double kGrabBlendSec = 0.15;
constexpr double kTwoHandChestSec = 0.5;
bool g_twoHandGrabbed = false;
double g_twoHandChestW = 0.0;
double g_gripWorld[3] = { 0.0, 0.0, 0.0 };   // donde va la mano izquierda en el arma (bajo g_ikLock)
double g_gripWorldT = -1.0;
std::atomic<int> g_hapticGrab{0};            // 1 coge, 2 suelta (lo hace Present)
double g_rightEmptySince = 0.0;
// Ronda 36: altura del cuerpo bajo el visor. Desde la ronda 27 subia o bajaba
// lo que la camara suavizada se apartaba de la del juego, pero
// la altura de la camara del juego salta 10-20 cm de un fotograma a otro en
// combate sin que se mueva la persona (300-600 saltos de mas de 3 cm por
// minuto; desplazamiento del cuerpo 60-450 mm por fotograma en la segunda
// diferencia, y el pecho igual): todo el personaje temblaba. Ahora se suaviza
// la altura de la persona (la que baja cada escalon) con el mismo muelle que
// la camara: en las escaleras igual, y sin seguir los saltos de la camara.
constexpr double kBodyYSmoothSec = 0.20;   // como kCamYSmoothSec de HeadTrackHook
constexpr double kBodyYSnapM = 1.5;
bool g_bodyYValid = false;
double g_bodyY = 0.0, g_bodyYVel = 0.0;

// Muelle criticamente amortiguado (como SmoothDampStep de HeadTrackHook).
void SmoothDampStep(double* x, double* vel, double target, double smoothSec, double dt) {
    double omega = 2.0 / smoothSec;
    double t = omega * dt;
    double ex = 1.0 / (1.0 + t + 0.48 * t * t + 0.235 * t * t * t);
    double change = *x - target;
    double temp = (*vel + omega * change) * dt;
    *vel = (*vel - omega * temp) * ex;
    *x = target + (change + temp) * ex;
}

// --- ronda 23: modos de combate (pulgar derecho en el reposapulgares + clic del joystick izquierdo) ---
// 0 VR basico: sin mandos de movimiento (brazos de la animacion, A ataca y hace
//   los combos, la comida y las pociones como en el juego).
// 1 mandos de movimiento (por defecto): lo de siempre; A no se manda al juego
//   en partida (sin ataques ni combos del juego: el ataque es por gesto).
// 2 mandos + combos: A y las direcciones hacen los combos del juego; mientras
//   dura la animacion de ataque los brazos y el pecho son los de la animacion
//   (el arma la mueve el juego) y Supr no gira al personaje: si el combo lo
//   gira, la vista se queda al frente. Fuera de esas animaciones, el gesto
//   (ataque basico), el bloqueo y comer y beber como en el modo 1.
std::atomic<int> g_combatMode{1};
std::atomic<unsigned long long> g_comboPressTick{0};   // A pulsado en el modo 2 (GetTickCount64)
std::atomic<bool> g_comboHoldPub{false};
std::atomic<bool> g_gestureReadyPub{false};            // el ataque basico por gesto esta listo (modo 1: A tapado)
constexpr double kComboPressWaitSec = 0.35;   // tras pulsar A, los brazos sueltan ya (la animacion tarda un fotograma o dos)
constexpr double kComboTailSec = 0.15;        // y siguen sueltos esto tras la animacion (entre golpes del combo)
constexpr double kComboFadeOutSec = 0.08;
constexpr double kComboChestInSec = 0.25;
double g_comboLastAttackAnim = -1.0;
double g_bodyW = 0.0;          // cuerpo bajo el visor (la raiz; el pecho ademas con g_comboChestW)
double g_comboChestW = 1.0;
// --- ronda 30 (pedido del usuario): armas a la espalda con un gesto ---
// Llevar una mano por encima del hombro, detras de la cabeza (VrPadHook, en el
// espacio del visor) y apretar su agarre: con el arma o el escudo fuera las
// guarda, con las manos libres las saca (ronda 31: sin esperar quieta).
// Actions.StdToggleWeapons, lo mismo que el boton del juego (cruceta
// arriba); con un enemigo activo el juego solo lo soltaria, asi que antes se
// suelta aqui. Los punetazos con las manos libres van como los tajos (ronda
// 31, ver UpdatePunch).
std::atomic<bool> g_weaponsOutPub{false};    // arma en la derecha o algo en la izquierda (escudo, arco)
std::atomic<bool> g_unarmedPub{false};       // las dos manos vacias
std::atomic<bool> g_armsGestureOkPub{false}; // los brazos siguen a los mandos, fuera de un ataque del juego (gesto de la espalda)
std::atomic<bool> g_twoHandWeaponPub{false}; // ronda 36: arma a dos manos en la derecha con los brazos en los mandos
std::atomic<int> g_weaponToggleRequest{0};   // 1 guardar, 2 sacar (lo pide VrPadHook)
// Ronda 32 (pedido del usuario): fijar enemigo (gatillo derecho) sin sacar las
// armas guardadas, para pelear a punetazos. DefaultSelectionData.SelectEnemy
// (accion "Select Enemy") saca el arma si la mano derecha esta vacia y hay
// algo a la espalda, y fija al acabar la animacion. Con la mano derecha vacia
// VrPadHook no le manda el gatillo al juego y aqui se hace lo mismo sin sacar
// nada.
std::atomic<bool> g_rightEmptyPub{false};
std::atomic<int> g_selectEnemyRequest{0};
const char* const kPySelectEnemyNoDraw =
    "import Bladex\n"
    "import Select\n"
    "BladeVR_pj = Bladex.GetEntity('Player1')\n"
    "if BladeVR_pj:\n"
    "    if BladeVR_pj.Data.enemy_locked:\n"
    "        Select.SelectNextEnemy(BladeVR_pj, 0)\n"
    "    else:\n"
    "        Select.GetVisibleEnemies(BladeVR_pj)\n"
    "        if BladeVR_pj.Data.selected_enemy:\n"
    "            BladeVR_pj.Data.enemy_locked = 1\n"
    "    if BladeVR_pj.Data.selected_enemy:\n"
    "        BladeVR_ene = Bladex.GetEntity(BladeVR_pj.Data.selected_enemy[0])\n"
    "        if BladeVR_ene and BladeVR_ene.Person:\n"
    "            BladeVR_pj.SetActiveEnemy(BladeVR_ene)\n";
// Ronda 37: StdToggleWeapons no hace nada con AnmEndedFunc puesta. El fijar del
// juego (DefaultSelectionData.SelectEnemy, gatillo con la mano derecha vacia o
// el ataque instantaneo) saca las armas y deja IntermediateSelect para fijar al
// acabar la animacion; si esta se corta (un golpe recibido), queda pendiente y
// el gesto de guardar no hacia nada hasta que acababa otra animacion (en el log,
// 4 gestos seguidos sin efecto y el enemigo fijado solo despues). Se quita como
// hace el propio SelectEnemy; otras AnmEndedFunc (pociones, llaves) se respetan.
const char* const kPyToggleWeapons =
    "import Bladex\n"
    "import Actions\n"
    "import DefaultSelectionData\n"
    "BladeVR_pj = Bladex.GetEntity('Player1')\n"
    "if BladeVR_pj:\n"
    "    if BladeVR_pj.ActiveEnemy:\n"
    "        BladeVR_pj.SetActiveEnemy('')\n"
    "        try:\n"
    "            BladeVR_pj.Data.time_deactive_enemy = Bladex.GetTime()\n"
    "            BladeVR_pj.Data.enemy_locked = 0\n"
    "        except:\n"
    "            pass\n"
    "    if BladeVR_pj.AnmEndedFunc == DefaultSelectionData.IntermediateSelect:\n"
    "        BladeVR_pj.AnmEndedFunc = None\n"
    "    Actions.StdToggleWeapons('Player1')\n";
// El brazo izquierdo sigue al mando y lleva escudo: se puede bloquear con el
// gesto (lo lee VrPadHook en el hilo de Present).
std::atomic<bool> g_shieldGestureOk{false};
bool g_ikWasActive = false;

// Avisos para el visor: se piden desde el hilo del juego y se muestran desde
// el Present (donde vive el aviso del diorama).
SRWLOCK g_toastLock = SRWLOCK_INIT;
wchar_t g_toastTitle[64];
wchar_t g_toastText[200];
unsigned g_toastMsPending = 0;
bool g_toastPending = false;

void PostToast(const wchar_t* title, const wchar_t* text, unsigned ms) {
    AcquireSRWLockExclusive(&g_toastLock);
    CopyW(g_toastTitle, 64, title);
    CopyW(g_toastText, 200, text);
    g_toastMsPending = ms;
    g_toastPending = true;
    ReleaseSRWLockExclusive(&g_toastLock);
}

// --- la IA se defiende del tajo por gesto ------------------------------------------
using PFN_DefenceNeeded = bool(__fastcall*)(uintptr_t ai, double t);
PFN_DefenceNeeded g_origDefence = nullptr;

bool __fastcall HookedDefenceNeeded(uintptr_t ai, double t) {
    bool r = g_origDefence(ai, t);
    if (!g_installed) return r;
    uintptr_t target = 0;
    if (!RdPtr(ai + kAiEnemy, &target) || !target || target != g_player) return r;
    // Tajo por gesto (sin animacion de ataque, que es lo unico que mira el
    // original): si la IA tiene al jugador delante (menos de 45 grados, lo
    // calcula el original en ai+0x30), se defiende al principio del tajo.
    if (!r && g_swingActive.load(std::memory_order_relaxed) && NowSec() - g_swingT0.load(std::memory_order_relaxed) < kAiReactSec) {
        double ang = 10.0;
        if (RdF64(ai + 0x30, &ang) && std::fabs(ang) < 0.7853981633975) r = true;
    }
    return r;
}

// --- cinematica inversa: datos del esqueleto ------------------------------------
uintptr_t NodeRecord(uintptr_t nodes, int i) { return nodes + static_cast<uintptr_t>(i) * kNodeStride; }

// Ronda 20: la tabla de nodos de AHORA de un esqueleto vivo (el que pasa el
// juego al evaluarlo). Al cargar partida el esqueleto nuevo puede quedar en la
// misma direccion que el viejo mientras los punteros guardados (nodos) son del
// viejo, ya liberado: antes de escribir se comprueba que coinciden.
bool SkelNodesNow(uintptr_t skel, uintptr_t* nodes, int* count) {
    uintptr_t tab = 0;
    *nodes = 0;
    *count = 0;
    if (!skel || !RdPtr(skel + kSkelNodeTable, &tab) || !tab) return false;
    if (!RdPtr(tab + kTabArray, nodes) || !*nodes) return false;
    if (!RdI32(tab + kTabCount, count) || *count <= 0 || *count > 128) return false;
    return true;
}

bool NodesStillOf(uintptr_t skel, uintptr_t nodes, int maxIndex) {
    uintptr_t now = 0;
    int count = 0;
    return SkelNodesNow(skel, &now, &count) && now == nodes && maxIndex < count;
}

// Producto de las B desde el nodo hasta la raiz: el nodo en la pose de reposo,
// en el espacio de la entidad.
bool BindEntity(const SkelView& sv, int node, double out[16]) {
    double acc[16], b[16], tmp[16];
    if (!NodeBind(sv, node, acc)) return false;
    int p = NodeParent(sv, node);
    int guard = 0;
    while (p >= 0 && guard++ < 64) {
        if (!NodeBind(sv, p, b)) return false;
        MatMul(acc, b, tmp);
        std::memcpy(acc, tmp, sizeof(acc));
        p = NodeParent(sv, p);
    }
    if (p != -1) return false;
    std::memcpy(out, acc, sizeof(acc));
    return true;
}

// Busca los dos brazos (5 nodos encadenados cada uno) y el ViewPoint. Una
// linea en el log por jugador nuevo (o nivel cargado).
void BuildChain(uintptr_t player) {
    ChainInfo c = {};
    c.player = player;
    SkelView sv;
    uintptr_t mesh = 0;
    char line[300], name[48];
    EntityName(player, name, sizeof(name));
    if (!GetSkel(player, &sv) || !MeshOf(player, &mesh)) {
        g_chain = c;
        std::snprintf(line, sizeof(line), "[COMBATE] jugador %s (nuevo o nivel cargado): sin esqueleto o sin malla, brazos con los mandos no disponibles.",
                      name);
        LogLine(line);
        return;
    }
    c.skel = sv.skel;
    c.mesh = mesh;
    c.count = sv.count;
    c.head = NodeIndexByName(mesh, "Head");
    int anchors = 0;
    RdI32(mesh + kMeshAnchorCount, &anchors);
    for (int i = 0; i < anchors && i < 64; ++i) {
        AnchorInfo a;
        if (ReadAnchor(mesh, i, &a) && _stricmp(a.name, "ViewPoint") == 0 && a.node == c.head && c.head >= 0) {
            std::memcpy(c.viewAnchor, a.m, sizeof(c.viewAnchor));
            c.haveView = true;
        }
    }
    bool both = true;
    for (int s = 0; s < 2; ++s) {
        ArmInfo& arm = c.arm[s];
        arm.ok = true;
        for (int k = 0; k < 5; ++k) {
            arm.node[k] = NodeIndexByName(mesh, kArmNodeNames[s][k]);
            if (arm.node[k] < 0 || arm.node[k] >= 64) arm.ok = false;
            else if (k > 0 && NodeParent(sv, arm.node[k]) != arm.node[k - 1]) arm.ok = false;
        }
        double handE[16], armE[16];
        if (arm.ok && (!NodeBind(sv, arm.node[4], arm.bindH) || !BindEntity(sv, arm.node[4], handE) || !BindEntity(sv, arm.node[0], armE))) {
            arm.ok = false;
        }
        if (arm.ok) {
            double rot[9], scale[3], pos[3];
            ik::SplitM4(handE, rot, scale, pos);
            for (int r = 0; r < 3; ++r) ik::EntityToBody(rot + r * 3, arm.handBody + r * 3);
            ik::M3Orthonormalize(arm.handBody);
            // Flexion natural: brazo hacia abajo, antebrazo hacia delante ->
            // eje = abajo x adelante = derecha del cuerpo (= -x de la entidad).
            double armRot[9];
            ik::SplitM4(armE, armRot, scale, pos);
            const double rightEntity[3] = { -1.0, 0.0, 0.0 };
            ik::M3Vec(armRot, rightEntity, arm.hingeLocal);
            ik::Norm(arm.hingeLocal);
            // Giro desde el reposo: codo, antebrazo y muneca respecto a la base
            // del antebrazo (hueso + normal del plano del brazo) en la pose de reposo.
            double elbowE[16], foreE[16], wristE[16];
            arm.restOk = BindEntity(sv, arm.node[1], elbowE) && BindEntity(sv, arm.node[2], foreE) &&
                         BindEntity(sv, arm.node[3], wristE) &&
                         ik::RestTwistRelations(armE, elbowE, foreE, wristE, arm.hingeLocal, arm.restRelE, arm.restRelFo,
                                                arm.restRelWr);
            // Ronda 48: codo, antebrazo y muneca pegados a la mano como en el reposo.
            {
                double invH[16], restE[3][16];
                arm.floatOk = InvAffine(handE, invH) && BindEntity(sv, arm.node[1], restE[0]) &&
                              BindEntity(sv, arm.node[2], restE[1]) && BindEntity(sv, arm.node[3], restE[2]);
                for (int k = 0; k < 3 && arm.floatOk; ++k) MatMul(restE[k], invH, arm.floatRel[k]);
            }
            // Ronda 57: hombro en reposo (clavicula y brazo con sus B sobre el pecho).
            arm.clav = NodeParent(sv, arm.node[0]);
            arm.restShoulderOk = arm.clav >= 0 && arm.clav < 64 && NodeBind(sv, arm.clav, arm.bindClav) && NodeBind(sv, arm.node[0], arm.bindArm);
            double w0[16], w1[16], w3[16], w4[16];
            arm.lenUpper = arm.lenLower = arm.lenHand = 0.0;
            if (NodeWorld(sv, arm.node[0], w0, nullptr) && NodeWorld(sv, arm.node[1], w1, nullptr) &&
                NodeWorld(sv, arm.node[3], w3, nullptr) && NodeWorld(sv, arm.node[4], w4, nullptr)) {
                arm.lenUpper = Dist3(w0 + 12, w1 + 12);
                arm.lenLower = Dist3(w1 + 12, w3 + 12);
                arm.lenHand = Dist3(w3 + 12, w4 + 12);
            }
        }
        if (!arm.ok) both = false;
    }
    c.ok = both;
    // Cuerpo bajo el visor: raiz, pecho y cabeza, y las claviculas cuelgan del pecho.
    c.root = -1;
    for (int i = 0; i < sv.count && c.root < 0; ++i) {
        if (NodeParent(sv, i) == -1) c.root = i;
    }
    c.chest = NodeIndexByName(mesh, "Chest");
    // Ronda 39: raiz y piernas (todo lo que llega a la raiz sin pasar por el pecho).
    c.legMask = 0;
    if (c.root >= 0 && c.root < 64 && c.chest >= 0) {
        for (int i = 0; i < sv.count && i < 64; ++i) {
            int k = i, guard = 0;
            while (k >= 0 && k != c.chest && k != c.root && guard++ < 64) k = NodeParent(sv, k);
            if (k == c.root) c.legMask |= 1ull << i;
        }
    }
    c.bodyOk = c.ok && c.root >= 0 && c.chest >= 0 && c.head >= 0 && c.root < 64 && c.chest < 64 && c.head < 64 &&
               NodeParent(sv, c.head) == c.chest;
    for (int s2 = 0; s2 < 2 && c.bodyOk; ++s2) {
        int clav = NodeParent(sv, c.arm[s2].node[0]);
        if (clav < 0 || NodeParent(sv, clav) != c.chest) c.bodyOk = false;
    }
    if (c.bodyOk) {
        double chestE[16], rot[9], scale[3], pos[3];
        const double fwdEntity[3] = { 0.0, -1.0, 0.0 };   // entidad: y = atras
        if (BindEntity(sv, c.chest, chestE)) {
            ik::SplitM4(chestE, rot, scale, pos);
            ik::M3Vec(rot, fwdEntity, c.chestFwdLocal);
            if (!ik::Norm(c.chestFwdLocal)) c.bodyOk = false;
        } else {
            c.bodyOk = false;
        }
    }
    g_chain = c;

    std::snprintf(line, sizeof(line), "[COMBATE] jugador %s (nuevo o nivel cargado): brazos con los mandos %s | cuerpo bajo el visor %s.",
                  name, c.ok ? "si" : "NO (sin cinematica inversa)", c.bodyOk ? "si" : "no");
    LogLine(line);
}

// Base del cuerpo para el codo: la de la entidad si tiene sentido (z arriba),
// si no la de la vista.
void BodyFrame(uintptr_t skel, const TrackingToGameMap* map, double right[3], double fwd[3]) {
    double e[16];
    const double down[3] = { 0.0, 1.0, 0.0 };
    if (skel && RdDoubles(skel + kSkelEntity, e, 16)) {
        double up[3] = { e[8], e[9], e[10] };
        double back[3] = { e[4], 0.0, e[6] };
        if (ik::Norm(up) && up[1] < -0.9 && ik::Norm(back)) {
            fwd[0] = -back[0]; fwd[1] = 0.0; fwd[2] = -back[2];
            ik::Cross(down, fwd, right);
            return;
        }
    }
    if (map) {
        double f[3] = { map->fwd[0], 0.0, map->fwd[2] };
        if (ik::Norm(f)) {
            ik::Copy3(f, fwd);
            ik::Cross(down, fwd, right);
            return;
        }
    }
    fwd[0] = 0.0; fwd[1] = 0.0; fwd[2] = 1.0;
    right[0] = 1.0; right[1] = 0.0; right[2] = 0.0;
}

// --- agarre guardado en BladeVR.ini ---------------------------------------------------
const char* const kIniGrip[2] = { "agarre_izquierda", "agarre_derecha" };
const char* const kIniReach[2] = { "alcance_izquierda", "alcance_derecha" };

void DefaultHang(int s, double m[9]) {
    for (int i = 0; i < 9; ++i) m[i] = kHangDefault[s][i];
    ik::M3Orthonormalize(m);
}

// Ronda 55: en BladeVR.ini, por mando, los ejes con el brazo colgando (filas
// r, d, f en la base del cuerpo, 9 numeros) y el alcance del jugador en
// metros. BladeVR_manos.txt (rondas 3-54) ya no se lee: sin calibrar, el
// agarre y el alcance de serie, que valen para cualquiera.
void LoadCalibration() {
    for (int s = 0; s < 2; ++s) {
        DefaultHang(s, g_ctrlHang[s]);
        g_ctrlHangCalibrated[s] = false;
        g_userReachM[s] = 0.0;
        std::string v;
        double m[9];
        if (IniGetString(kIniGrip[s], &v) &&
            sscanf_s(v.c_str(), "%lf %lf %lf %lf %lf %lf %lf %lf %lf", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7], &m[8]) == 9) {
            ik::M3Orthonormalize(m);
            std::memcpy(g_ctrlHang[s], m, sizeof(m));
            g_ctrlHangCalibrated[s] = true;
        }
        double reach = 0.0;
        if (IniGetString(kIniReach[s], &v) && sscanf_s(v.c_str(), "%lf", &reach) == 1 && reach > 0.3 && reach < 1.5) g_userReachM[s] = reach;
    }
    char msg[300];
    std::snprintf(msg, sizeof(msg),
                  "[IK] agarre: izquierda %s, derecha %s | alcance %s (Ctrl+F8 calibra y lo guarda en BladeVR.ini).",
                  g_ctrlHangCalibrated[ARM_L] ? "calibrada (BladeVR.ini)" : "de serie",
                  g_ctrlHangCalibrated[ARM_R] ? "calibrada (BladeVR.ini)" : "de serie",
                  (g_userReachM[ARM_L] > 0.0 || g_userReachM[ARM_R] > 0.0) ? "calibrado" : "de serie (0,80 m)");
    LogLine(msg);
}

bool SaveCalibration() {
    bool ok = true;
    for (int s = 0; s < 2; ++s) {
        if (!g_ctrlHangCalibrated[s]) continue;
        const double* m = g_ctrlHang[s];
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
        ok = IniSetString(kIniGrip[s], buf) && ok;
        if (g_userReachM[s] > 0.0) {
            std::snprintf(buf, sizeof(buf), "%.3f", g_userReachM[s]);
            ok = IniSetString(kIniReach[s], buf) && ok;
        }
    }
    return ok;
}

// --- evaluadores enganchados ------------------------------------------------------------
using PFN_EvalNode = uintptr_t(__fastcall*)(uintptr_t skel, unsigned node);
using PFN_EvalAll = void(__fastcall*)(uintptr_t skel);
PFN_EvalNode g_origEvalNode = nullptr;
PFN_EvalAll g_origEvalAll = nullptr;

bool ReadNodeW(uintptr_t nodes, int i, double w[16], uintptr_t* wptr) {
    uintptr_t p = 0;
    if (!RdPtr(NodeRecord(nodes, i) + kNodeWorld, &p) || !p) return false;
    if (wptr) *wptr = p;
    return RdDoubles(p, w, 16);
}

// Alargamiento maximo del brazo s: el alcance medido del jugador (Ctrl+F8)
// entre el del personaje (brazo + antebrazo + mano), entre 1 y 1,25.
double MaxStretch(int s) {
    if (!g_chain.ok) return 1.0;
    double rc = g_chain.arm[s].lenUpper + g_chain.arm[s].lenLower + g_chain.arm[s].lenHand;   // mm
    // Ronda 11: los dos brazos del jugador miden lo mismo, asi que los dos usan
    // el mayor alcance medido. El del izquierdo salia corto al calibrar (0,41 m
    // frente a 0,85: se mide hasta el hombro del personaje, que en la guardia
    // va adelantado) y ese brazo no se alargaba nunca: se quedaba corto.
    double reach = g_userReachM[ARM_L] > g_userReachM[ARM_R] ? g_userReachM[ARM_L] : g_userReachM[ARM_R];
    if (reach <= 0.0) reach = kDefaultReachM;   // ronda 55: sin calibrar, el de un adulto
    if (rc <= 1.0) return kStretchMax;
    double k = reach * 1000.0 / rc;
    if (k < 1.0) k = 1.0;
    if (k > kStretchMax) k = kStretchMax;
    return k;
}

// Resuelve los brazos con peso > 0. Se llama con t_inSolve = true: las
// llamadas recursivas del original a los evaluadores pasan de largo.
// Ronda 37 (pedido del usuario: con el baston de la amazona, al alejar mucho
// una mano de la otra, la mano izquierda se separaba un poco del arma). Con el
// arma cogida con las dos, si su agarre queda fuera del alcance del brazo
// izquierdo (alargado al maximo, kTwoHandMaxStretch), el arma (la mano derecha
// pedida) se acerca al hombro izquierdo lo que falte: la izquierda no se sale
// del arma. En el log de la ronda 36 (Eclipse) llego a pedir 0,78 m a un brazo
// de 0,54 (x1,35 al tope, mano a 49 mm del arma).
constexpr double kTwoHandReachMargin = 0.96;   // por debajo del alcance suave (0,97)

void TwoHandReachLimit(const IkShared& sh, ik::ArmInput& in) {
    double relF[3] = { 0.0, 0.0, 0.0 };
    double reachL = 0.0;
    bool haveRel = false;
    AcquireSRWLockShared(&g_ikLock);
    if (sh.gripFixed) {
        ik::Copy3(sh.gripRel, relF);
        haveRel = true;
    } else if (g_grip.valid) {
        ik::Copy3(g_grip.outRel, relF);
        haveRel = true;
    }
    reachL = g_armStat[ARM_L].reachMax;
    ReleaseSRWLockShared(&g_ikLock);
    if (!haveRel || reachL < 100.0) return;
    // Hombro izquierdo de este fotograma (solo su posicion: la cinematica
    // inversa no la mueve sin escudo).
    WrF64(NodeRecord(sh.nodes, sh.node[ARM_L][0]) + kNodeTime, -1.0);
    g_origEvalNode(sh.skel, static_cast<unsigned>(sh.node[ARM_L][0]));
    double sw[16];
    uintptr_t wp = 0;
    if (!ReadNodeW(sh.nodes, sh.node[ARM_L][0], sw, &wp)) return;
    double along[3], grip[3], v[3];
    ik::VecM3(relF, in.handRot, along);
    for (int k = 0; k < 3; ++k) grip[k] = in.handPos[k] + along[k];
    double shL[3] = { sw[12], sw[13], sw[14] };
    ik::Sub(shL, grip, v);
    double d = ik::Len(v);
    double kMaxL = sh.maxStretch[ARM_L] > kTwoHandMaxStretch ? sh.maxStretch[ARM_L] : kTwoHandMaxStretch;
    double limit = reachL * kMaxL * kTwoHandReachMargin;
    if (d <= limit || d < 1.0) return;
    double mv = (d - limit) * (sh.twoHand < 1.0 ? sh.twoHand : 1.0);
    for (int k = 0; k < 3; ++k) in.handPos[k] += v[k] / d * mv;
}

// Ronda 57: mezcla de dos matrices de nodo (giro esferico, posicion lineal).
void BlendM4(const double a[16], const double b[16], double t, double o[16]) {
    if (t >= 1.0) { std::memcpy(o, b, 16 * sizeof(double)); return; }
    if (t <= 0.0) { std::memcpy(o, a, 16 * sizeof(double)); return; }
    double ra[9], sa[3], pa[3], rb[9], sb[3], pb[3], r[9], sc[3], p[3];
    ik::SplitM4(a, ra, sa, pa);
    ik::SplitM4(b, rb, sb, pb);
    ik::SlerpM3(ra, rb, t, r);
    ik::Lerp3(pa, pb, t, p);
    ik::Lerp3(sa, sb, t, sc);
    ik::ComposeM4(r, sc, p, o);
}

// Ronda 57: la clavicula del juego de este fotograma (antes de escribirla).
struct ClavCache {
    bool known;
    uintptr_t ptr;
    double time;
    double w[16];
};
ClavCache g_clavCache[2] = {};

// Hombro en reposo (kRestShoulder): clavicula = B(clavicula) x pecho y el brazo
// colgando de ella; la cadena de la animacion (brazo, codo, antebrazo, muneca,
// mano) se mueve rigida para que su brazo quede ahi. Mezclado con la animacion
// por el peso del brazo (y, en el izquierdo, por 1 - arma a dos manos cogida).
void RestShoulder(const IkShared& sh, int s, double anim[5][16]) {
    if (!sh.restShoulder[s] || sh.clav[s] < 0 || sh.chest < 0) return;
    // Ronda 58: con un arma a dos manos en la derecha (cogida o no) o con el
    // arco, el brazo izquierdo sale del hombro de la animacion: el punto de
    // agarre y el agarre fijo del arma se miden con la mano izquierda de la
    // animacion (movida, el agarre quedaba a 53 cm y no se cogia), y la pose
    // del arco adelanta el hombro (en reposo el brazo no llegaba: saturado).
    if (s == ARM_L && (sh.twoHandWeapon || sh.bowLeft)) return;
    double two = sh.twoHand < 0.0 ? 0.0 : (sh.twoHand > 1.0 ? 1.0 : sh.twoHand);
    double wr = sh.weight[s] * (s == ARM_L ? 1.0 - two : 1.0);
    if (wr <= 0.0) return;
    uintptr_t cptr = 0;
    double wc[16], wch[16], t = -1.0;
    if (!ReadNodeW(sh.nodes, sh.clav[s], wc, &cptr) || !ReadNodeW(sh.nodes, sh.chest, wch, nullptr)) return;
    RdF64(NodeRecord(sh.nodes, sh.clav[s]) + kNodeTime, &t);
    ClavCache& cc = g_clavCache[s];
    if (cc.known && cc.ptr == cptr && cc.time == t) {
        std::memcpy(wc, cc.w, sizeof(wc));   // ya escrita en este fotograma: la del juego guardada
    } else {
        cc.known = true;
        cc.ptr = cptr;
        cc.time = t;
        std::memcpy(cc.w, wc, sizeof(wc));
    }
    double cr[16], cb[16], sr[16], sb[16], inv[16], T[16], tmp[16];
    MatMul(sh.bindClav[s], wch, cr);
    BlendM4(wc, cr, wr, cb);
    MatMul(sh.bindArm[s], cb, sr);
    BlendM4(anim[0], sr, wr, sb);
    if (!InvAffine(anim[0], inv)) return;
    MatMul(inv, sb, T);
    for (int k = 0; k < 5; ++k) {
        MatMul(anim[k], T, tmp);
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(tmp[i])) return;
        std::memcpy(anim[k], tmp, sizeof(tmp));
    }
    WrDoubles(cptr, cb, 16);
}

// Ronda 48 (modo 3, pedido del usuario): antebrazos y manos flotantes, sin
// cinematica inversa. La mano va donde la pide el mando (posicion y giro) y
// el codo, el antebrazo y la muneca van pegados a ella como en la pose de
// reposo (muneca recta): nada los ata al hombro del personaje, asi que el
// escudo y el arma suben tan alto como el mando, sin el limite del brazo.
// Con el escudo en el antebrazo, muneca y mano guardan con el antebrazo la
// relacion de la animacion (la mano en la correa), como en el modo 1: el
// antebrazo, y el escudo, siguen al mando con la misma orientacion que alli.
// El brazo (hombro) se deja como en la animacion; no se dibuja.
bool FloatArm(const ik::ArmInput& in, const double rel[3][16], bool lockWrist, ik::ArmOutput* out) {
    double hr[9], hs[3], hp[3];
    ik::SplitM4(in.anim[4], hr, hs, hp);
    double F[5][16];
    std::memcpy(F[0], in.anim[0], sizeof(F[0]));
    ik::ComposeM4(in.handRot, hs, in.handPos, F[4]);
    for (int k = 1; k < 4; ++k) MatMul(rel[k - 1], F[4], F[k]);
    if (lockWrist) {
        double inv[16], loc[16];
        if (InvAffine(in.anim[2], inv)) {
            MatMul(in.anim[3], inv, loc);   // muneca en los ejes del antebrazo (animacion)
            MatMul(loc, F[2], F[3]);
            if (InvAffine(in.anim[3], inv)) {
                MatMul(in.anim[4], inv, loc);   // mano en los ejes de la muneca (animacion)
                MatMul(loc, F[3], F[4]);
            }
        }
    }
    double w = in.weight < 0.0 ? 0.0 : (in.weight > 1.0 ? 1.0 : in.weight);
    for (int k = 0; k < 5; ++k) {
        if (k == 0 || w >= 1.0) {
            std::memcpy(out->m[k], F[k], sizeof(out->m[k]));
            continue;
        }
        double ra[9], sa[3], pa[3], rf[9], sf[3], pf[3], r[9], p[3];
        ik::SplitM4(in.anim[k], ra, sa, pa);
        ik::SplitM4(F[k], rf, sf, pf);
        ik::SlerpM3(ra, rf, w, r);
        ik::Lerp3(pa, pf, w, p);
        ik::ComposeM4(r, sf, p, out->m[k]);
    }
    for (int k = 0; k < 5; ++k)
        for (int c = 0; c < 16; ++c)
            if (!std::isfinite(out->m[k][c])) return false;
    out->saturated = false;
    out->tooClose = false;
    out->hingeMeasured = false;
    for (int i = 0; i < 3; ++i) out->hingeLocal[i] = in.hingeLocal[i];
    out->reachReq = Dist3(in.anim[0] + 12, out->m[3] + 12);
    out->reachMax = Dist3(in.anim[0] + 12, in.anim[1] + 12) + Dist3(in.anim[1] + 12, in.anim[3] + 12);
    out->elbowDeg = 180.0;
    out->twistDeg = 0.0;
    out->twistRad = 0.0;
    out->handErrMm = Dist3(out->m[4] + 12, in.handPos);
    out->stretch = 1.0;
    out->pole[0] = out->pole[1] = out->pole[2] = 0.0;
    out->protractMm = 0.0;
    out->pullMm = 0.0;
    return true;
}

void SolveArmsNow(const IkShared& sh) {
    // Ronda 21: con un arma a dos manos se resuelve primero el brazo derecho y
    // la mano izquierda se pide sobre el arma: la relacion entre las dos manos
    // de la animacion (que la agarra con las dos) llevada a la mano derecha
    // resuelta.
    const bool twoHand = sh.twoHand > 0.0 && sh.weight[ARM_R] > 0.0;
    // Ronda 27: con un arma a dos manos se calcula su agarre (para cogerla al
    // acercar la mano) aunque la izquierda aun no la tenga.
    const bool twoHandWeapon = (sh.twoHandWeapon || twoHand) && sh.weight[ARM_R] > 0.0;
    const int order[2] = { twoHandWeapon ? ARM_R : ARM_L, twoHandWeapon ? ARM_L : ARM_R };
    bool haveR = false;
    double animRRot[9], animRPos[3], ikRRot[9], ikRPos[3];
    const double solveNow = NowSec();
    for (int oi = 0; oi < 2; ++oi) {
        const int s = order[oi];
        if (sh.weight[s] <= 0.0) continue;
        // La cadena entera desde la animacion de este fotograma.
        for (int k = 0; k < 5; ++k) WrF64(NodeRecord(sh.nodes, sh.node[s][k]) + kNodeTime, -1.0);
        g_origEvalNode(sh.skel, static_cast<unsigned>(sh.node[s][4]));
        ik::ArmInput in;
        uintptr_t wptr[5] = {};
        bool ok = true;
        for (int k = 0; k < 5 && ok; ++k) ok = ReadNodeW(sh.nodes, sh.node[s][k], in.anim[k], &wptr[k]);
        if (ok) RestShoulder(sh, s, in.anim);   // ronda 57
        ik::ArmOutput out;
        const bool floatNow = sh.floating && sh.floatOk[s];   // ronda 48: modo 3, sin cinematica inversa
        if (ok) {
            std::memcpy(in.bindH, sh.bindH[s], sizeof(in.bindH));
            std::memcpy(in.handRot, sh.handRot[s], sizeof(in.handRot));
            ik::Copy3(sh.handPos[s], in.handPos);
            if (s == ARM_R && twoHand && !floatNow) TwoHandReachLimit(sh, in);   // ronda 37 (sin brazo que limite en el 3)
            if (s == ARM_L && twoHandWeapon && haveR) {
                double lr[9], ls[3], lp[3], rRT[9], relRot[9], dv[3], rel[3], gripRot[9], gripPos[3], mixR[9], mixP[3];
                ik::SplitM4(in.anim[4], lr, ls, lp);
                ik::M3T(animRRot, rRT);
                ik::M3Mul(lr, rRT, relRot);          // mano izq = rel x mano dcha (filas)
                ik::Sub(lp, animRPos, dv);
                ik::VecM3(dv, rRT, rel);             // en los ejes de la mano derecha
                // Ronda 23: relacion y giro de la mano derecha filtrados (ver GripState).
                double relRotF[9], relF[3];
                bool newFrame = false;   // fotograma nuevo (no otra resolucion del mismo)
                AcquireSRWLockExclusive(&g_ikLock);
                {
                    GripState& g = g_grip;
                    double gdt = solveNow - g.lastT;
                    if (!g.valid || gdt > 0.25 || gdt < 0.0) {
                        g.valid = true;
                        g.lastT = solveNow;
                        ik::Copy3(rel, g.relF);
                        ik::QuatFromM3(relRot, g.relQ);
                        std::memcpy(g.outRelRot, relRot, sizeof(relRot));
                        ik::Copy3(rel, g.outRel);
                        g.captured = false;
                        g.accN = 0;
                        newFrame = true;
                    } else if (gdt >= kGripSameFrameSec) {
                        g.lastT = solveNow;
                        double a = ik::LowPassAlpha(kGripRelCutHz, gdt), q[4], nq[4];
                        ik::Lerp3(g.relF, rel, a, g.relF);
                        ik::QuatFromM3(relRot, q);
                        ik::QuatNlerp(g.relQ, q, a, nq);
                        for (int i = 0; i < 4; ++i) g.relQ[i] = nq[i];
                        ik::M3FromQuat(g.relQ, g.outRelRot);
                        ik::Copy3(g.relF, g.outRel);
                        newFrame = true;
                    }
                    // Ronda 26: media de la relacion en reposo (fotogramas nuevos).
                    if (!sh.gripAccum || g.captured) {
                        if (!g.captured) g.accN = 0;
                    } else if (newFrame) {
                        double q[4];
                        ik::QuatFromM3(relRot, q);
                        if (g.accN == 0) {
                            ik::Copy3(rel, g.accRel);
                            for (int i = 0; i < 4; ++i) g.accQ[i] = q[i];
                        } else {
                            ik::QuatAlign(g.accQ, q);
                            ik::Add(g.accRel, rel, g.accRel);
                            for (int i = 0; i < 4; ++i) g.accQ[i] += q[i];
                        }
                        ++g.accN;
                    }
                    if (sh.gripCapture && !g.captured && g.accN >= 20) {
                        for (int i = 0; i < 3; ++i) g.capRel[i] = g.accRel[i] / g.accN;
                        ik::M3FromQuat(g.accQ, g.capRelRot);   // normaliza
                        g.captured = true;
                    }
                    if (sh.gripFixed) {
                        // Ronda 24: el agarre del juego para este arma, fijo.
                        std::memcpy(relRotF, sh.gripRelRot, sizeof(relRotF));
                        ik::Copy3(sh.gripRel, relF);
                    } else {
                        std::memcpy(relRotF, g.outRelRot, sizeof(relRotF));
                        ik::Copy3(g.outRel, relF);
                    }
                }
                ReleaseSRWLockExclusive(&g_ikLock);
                double along[3];
                // Ronda 25: sobre la mano derecha tal cual (sin el filtro "1 euro"
                // de la ronda 23, que la dejaba atras del arma en los tajos).
                ik::M3Mul(relRotF, ikRRot, gripRot);
                ik::VecM3(relF, ikRRot, along);
                for (int k = 0; k < 3; ++k) gripPos[k] = ikRPos[k] + along[k];
                AcquireSRWLockExclusive(&g_ikLock);
                ik::Copy3(gripPos, g_gripWorld);   // ronda 27: para cogerla al acercar la mano
                g_gripWorldT = solveNow;
                ReleaseSRWLockExclusive(&g_ikLock);
                double tw = twoHand ? sh.twoHand : 0.0;
                ik::SlerpM3(in.handRot, gripRot, tw, mixR);
                ik::Lerp3(in.handPos, gripPos, tw, mixP);
                std::memcpy(in.handRot, mixR, sizeof(mixR));
                ik::Copy3(mixP, in.handPos);
            }
            in.weight = sh.weight[s];
            ik::Copy3(sh.bodyRight, in.bodyRight);
            ik::Copy3(sh.bodyFwd, in.bodyFwd);
            in.down[0] = 0.0; in.down[1] = 1.0; in.down[2] = 0.0;
            in.outSign = (s == ARM_R) ? 1.0 : -1.0;
            ik::Copy3(sh.hingeLocal[s], in.hingeLocal);
            in.poleHandWeight = kPoleHandWeight;
            in.maxStretch = sh.maxStretch[s];
            // El escudo va en el antebrazo izquierdo: que gire casi como la mano
            // (con el escudo puesto, todo el giro y la mano fija en la correa).
            in.twistShare = (s == ARM_L) ? kTwistShareLeft : kTwistShareRight;
            in.lockWrist = sh.lockWrist[s];
            if (in.lockWrist) in.twistShare = kTwistShareShield;
            in.shieldPole = in.lockWrist;
            in.foreMaxDeg = in.lockWrist ? kShieldForeMaxDeg : 0.0;
            // Ronda 34: con el escudo, el hombro se adelanta hacia la mano
            // cuando esta lejos (el codo dobla y el escudo sigue al mando).
            in.protractMaxMm = in.lockWrist ? kShieldProtractM * 1000.0 : 0.0;
            in.protractStart = kShieldProtractStart;
            if (in.lockWrist) in.poleHandWeight = kShieldPoleHandWeight;
            // Ronda 22: la mano izquierda en el arma a dos manos temblaba: el
            // objetivo queda a menudo cerca del alcance (codo a 175 grados,
            // alargado hasta x1,25) y ahi 1 mm de ruido movia el codo 5-19
            // grados. Alcance suave y el codo de la animacion (prueba T15).
            const bool grip2h = s == ARM_L && twoHand && haveR;
            in.softReach = grip2h ? kTwoHandSoftReach : 0.0;
            in.animPole = grip2h;
            if (grip2h && in.maxStretch < kTwoHandMaxStretch) in.maxStretch = kTwoHandMaxStretch;   // ronda 24
            // Ronda 23: en el arma, el giro del antebrazo desde la animacion.
            // Ronda 27: de una medida a otra poco a poco, con el peso de coger
            // el arma (antes cambiaba de golpe y el antebrazo saltaba).
            in.restTwist = sh.restTwist[s];
            in.restTwistW = (s == ARM_L && twoHandWeapon && twoHand) ? 1.0 - sh.twoHand : 1.0;
            std::memcpy(in.restRelE, sh.restRel[s][0], sizeof(in.restRelE));
            std::memcpy(in.restRelFo, sh.restRel[s][1], sizeof(in.restRelFo));
            std::memcpy(in.restRelWr, sh.restRel[s][2], sizeof(in.restRelWr));
            const double down[3] = { 0.0, 1.0, 0.0 };
            AcquireSRWLockExclusive(&g_ikLock);
            // (Ronda 27: sin borrar el giro y el codo anteriores al coger o
            // soltar el arma; con el giro fundido, borrarlos lo hacia saltar.)
            const bool smoothPole = s == ARM_L && twoHandWeapon;
            in.hasPrevTwist = g_armStat[s].twistValid;
            in.prevTwist = g_armStat[s].twistRad;
            in.hasPrevPole = false;
            in.poleAlpha = 1.0;
            if (smoothPole && g_armStat[s].poleValid) {
                double pdt = solveNow - g_armStat[s].poleT;
                if (pdt >= 0.0 && pdt < 0.25) {
                    const double* pb = g_armStat[s].poleBody;
                    for (int k = 0; k < 3; ++k) in.prevPole[k] = pb[0] * sh.bodyRight[k] + pb[1] * down[k] + pb[2] * sh.bodyFwd[k];
                    in.hasPrevPole = true;
                    in.poleAlpha = 1.0 - std::exp(-pdt / kTwoHandPoleSec);
                }
            }
            ReleaseSRWLockExclusive(&g_ikLock);
            ok = floatNow ? FloatArm(in, sh.floatRel[s], in.lockWrist, &out) : ik::SolveArm(in, &out);
            if (ok && smoothPole && !floatNow) {
                AcquireSRWLockExclusive(&g_ikLock);
                ArmStat& ps = g_armStat[s];
                if (!ps.poleValid || solveNow - ps.poleT >= kGripSameFrameSec) {
                    ps.poleBody[0] = ik::Dot(out.pole, sh.bodyRight);
                    ps.poleBody[1] = ik::Dot(out.pole, down);
                    ps.poleBody[2] = ik::Dot(out.pole, sh.bodyFwd);
                    ps.poleT = solveNow;
                    ps.poleValid = true;
                }
                ReleaseSRWLockExclusive(&g_ikLock);
            }
        }
        if (ok) {
            for (int k = 0; k < 5 && ok; ++k) ok = WrDoubles(wptr[k], out.m[k], 16);
        }
        if (ok && s == ARM_R) {
            double sc[3];
            ik::SplitM4(in.anim[4], animRRot, sc, animRPos);
            ik::SplitM4(out.m[4], ikRRot, sc, ikRPos);
            haveR = true;
        }
        if (ok) {
            AcquireSRWLockExclusive(&g_ikLock);
            ArmStat& st = g_armStat[s];
            st.valid = true;
            for (int k = 0; k < 5; ++k) ik::Copy3(in.anim[k] + 12, st.anim[k]);
            st.reachMax = out.reachMax;
            st.twistValid = true;
            st.twistRad = out.twistRad;
            ReleaseSRWLockExclusive(&g_ikLock);
        }
    }
}

void MaybeSolve(uintptr_t skel) {
    IkShared sh;
    AcquireSRWLockShared(&g_ikLock);
    sh = g_ikShared;
    ReleaseSRWLockShared(&g_ikLock);
    if (sh.skel != skel || !sh.nodes) return;
    if (!IkFrameFresh()) return;   // ronda 32: sin fotogramas de partida (muerte, cinematica)
    {
        int maxIdx = 0;
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 5; ++k)
                if (sh.node[s][k] > maxIdx) maxIdx = sh.node[s][k];
        if (!NodesStillOf(skel, sh.nodes, maxIdx)) return;   // ronda 20: esqueleto de otra partida
    }
    double t = 0.0;
    if (!RdF64(skel + kSkelTime, &t) || t < 0.0) return;   // fotograma de la raiz (+0x136d20)
    bool stale = false;
    for (int s = 0; s < 2 && !stale; ++s) {
        if (sh.weight[s] <= 0.0) continue;
        for (int k = 0; k < 5 && !stale; ++k) {
            double nt = 0.0;
            if (!RdF64(NodeRecord(sh.nodes, sh.node[s][k]) + kNodeTime, &nt) || nt < 0.0 || nt != t) stale = true;
        }
    }
    if (!stale) return;
    SolveArmsNow(sh);
}

// Raiz, pecho o cabeza del jugador con el cuerpo bajo el visor: el original
// los calcula y aqui se corrigen (solo si de verdad se recalcularon en el
// fotograma de la animacion; con el tiempo -1 de +0x136d20, que saca el
// movimiento de la raiz, no se toca nada).
uintptr_t EvalBodyNode(uintptr_t skel, unsigned node) {
    BodyShared b;
    AcquireSRWLockShared(&g_ikLock);
    b = g_body;
    ReleaseSRWLockShared(&g_ikLock);
    double t = 0.0, rt = 0.0;
    uintptr_t rec = NodeRecord(b.nodes, static_cast<int>(node));
    bool haveT = b.nodes && RdF64(skel + kSkelTime, &t) && RdF64(rec + kNodeTime, &rt);
    bool stale = haveT && (rt < 0.0 || rt != t);
    uintptr_t r = g_origEvalNode(skel, node);
    if (!stale || t < 0.0 || b.skel != skel || b.weight <= 0.0) return r;
    if (!IkFrameFresh()) return r;   // ronda 32: sin fotogramas de partida (muerte, cinematica)
    if (!NodesStillOf(skel, b.nodes, static_cast<int>(node))) return r;   // ronda 20
    double w[16];
    uintptr_t wp = 0;
    if (!ReadNodeW(b.nodes, static_cast<int>(node), w, &wp)) return r;
    const double up[3] = { 0.0, -1.0, 0.0 };
    if (static_cast<int>(node) == b.root) {
        w[12] += b.delta[0] * b.weight;
        w[13] += b.delta[1] * b.weight;   // ronda 27
        w[14] += b.delta[2] * b.weight;
        WrDoubles(wp, w, 16);
    } else if (static_cast<int>(node) == b.chest) {
        double rot[9], sc[3], pos[3], cf[3];
        ik::SplitM4(w, rot, sc, pos);
        ik::VecM3(b.chestFwdLocal, rot, cf);
        cf[1] = 0.0;
        double yaw = 0.0;
        double cfh = ik::Len(cf);   // ronda 24: casi vertical, su direccion no vale
        if (ik::Norm(cf, 1e-3)) {
            yaw = ik::SignedAngle(cf, b.fwd, up);
            const double lim = 80.0 * ik::kPi / 180.0;
            if (yaw > lim || yaw < -lim) {
                // Vista detras del pecho: se queda en el tope del lado en que
                // estaba (cerca de 180 grados saltaba de +80 a -80).
                if (g_chestClampSide == 0) g_chestClampSide = yaw > 0.0 ? 1 : -1;
                yaw = g_chestClampSide * lim;
            } else {
                g_chestClampSide = 0;
            }
            yaw *= b.weight * b.chestWeight * SmoothStep(0.15, 0.4, cfh);
        }
        double R[9];
        ik::AxisAngleM3(up, yaw, R);
        for (int k = 0; k < 3; ++k) {
            double row[3] = { w[k * 4], w[k * 4 + 1], w[k * 4 + 2] }, o[3];
            ik::VecM3(row, R, o);
            w[k * 4] = o[0]; w[k * 4 + 1] = o[1]; w[k * 4 + 2] = o[2];
        }
        WrDoubles(wp, w, 16);
        std::memcpy(g_chestRot, R, sizeof(R));
        ik::Copy3(pos, g_chestPivot);
        g_chestDelta[0] = b.delta[0] * b.weight;
        g_chestDelta[1] = b.delta[1] * b.weight;   // ronda 27
        g_chestDelta[2] = b.delta[2] * b.weight;
    } else if (static_cast<int>(node) == b.head) {
        double rt3[9];
        ik::M3T(g_chestRot, rt3);
        for (int k = 0; k < 3; ++k) {
            double row[3] = { w[k * 4], w[k * 4 + 1], w[k * 4 + 2] }, o[3];
            ik::VecM3(row, rt3, o);
            w[k * 4] = o[0]; w[k * 4 + 1] = o[1]; w[k * 4 + 2] = o[2];
        }
        double p[3] = { w[12] - g_chestPivot[0], w[13] - g_chestPivot[1], w[14] - g_chestPivot[2] }, o[3];
        ik::VecM3(p, rt3, o);
        w[12] = o[0] + g_chestPivot[0] - g_chestDelta[0];
        w[13] = o[1] + g_chestPivot[1] - g_chestDelta[1];
        w[14] = o[2] + g_chestPivot[2] - g_chestDelta[2];
        WrDoubles(wp, w, 16);
    }
    return r;
}

int g_physDepth = 0;   // ronda 39: dentro de una consulta de los pies de la fisica del juego (hilo del juego)
bool g_bodyPhysicsSafe = false;   // ronda 39: enganchados los pies de la fisica (si no, sin cuerpo bajo el visor)

void MarkLegsStale(uintptr_t skel);

uintptr_t __fastcall HookedEvalNode(uintptr_t skel, unsigned node) {
    if (g_physDepth > 0) return g_origEvalNode(skel, node);   // ronda 39: la fisica ve el esqueleto del juego
    // Ronda 41: el sistema de animacion del juego ve la raiz y los pies sin el
    // cuerpo bajo el visor (como la fisica en la ronda 39).
    if (skel && skel == g_bodySkel.load(std::memory_order_relaxed) && g_bodyPhysicsSafe) {
        uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ModuleBase();
        if (ra >= kAnimSysCallLo && ra < kAnimSysCallHi && g_frameThread != 0 && GetCurrentThreadId() == g_frameThread) {
            ++g_physDepth;
            MarkLegsStale(skel);
            uintptr_t r = g_origEvalNode(skel, node);
            --g_physDepth;
            MarkLegsStale(skel);   // el dibujo las vuelve a calcular bajo el visor
            return r;
        }
    }
    if (skel && node < 64 && skel == g_bodySkel.load(std::memory_order_relaxed) &&
        ((g_bodyNodeMask.load(std::memory_order_relaxed) >> node) & 1ull) != 0) {
        return EvalBodyNode(skel, node);
    }
    if (skel && skel == g_ikSkel.load(std::memory_order_relaxed) && !t_inSolve) {
        if (node < 64 && ((g_ikNodeMask.load(std::memory_order_relaxed) >> node) & 1ull) != 0) {
            t_inSolve = true;
            MaybeSolve(skel);
            t_inSolve = false;
        }
    }
    return g_origEvalNode(skel, node);
}

void __fastcall HookedEvalAll(uintptr_t skel) {
    if (g_physDepth > 0) {   // ronda 39
        g_origEvalAll(skel);
        return;
    }
    if (skel && skel == g_bodySkel.load(std::memory_order_relaxed)) {
        // Raiz, pecho y cabeza por el camino perezoso (con sus correcciones):
        // asi el original ya los encuentra calculados y cuelga el resto de ellos.
        int head = -1, root = -1;
        AcquireSRWLockShared(&g_ikLock);
        head = g_body.head;
        root = g_body.root;
        ReleaseSRWLockShared(&g_ikLock);
        // Ronda 39: la raiz tambien (tras una consulta de los pies de la fisica
        // queda sin calcular con la cabeza ya calculada; el original la haria
        // sin el desplazamiento y las piernas quedarian fuera del cuerpo).
        if (root >= 0 && root < 64) HookedEvalNode(skel, static_cast<unsigned>(root));
        if (head >= 0) HookedEvalNode(skel, static_cast<unsigned>(head));
    }
    if (skel && skel == g_ikSkel.load(std::memory_order_relaxed) && !t_inSolve) {
        t_inSolve = true;
        MaybeSolve(skel);
        t_inSolve = false;
    }
    g_origEvalAll(skel);
}

// Ronda 20: el jugador de la cadena sigue siendo el de la partida. Al morir y
// cargar, IkShutdown marcaba los nodos del esqueleto viejo leyendo por el
// jugador viejo, ya liberado: con la memoria reutilizada, la tabla de nodos
// salia dentro de un tipo de Python estatico de Blade.exe (+0x9dfd90) y los
// -1,0 escritos alli cerraban el juego (PyObject_IsTrue, +0x390f3b).
bool ChainPlayerIsCurrent() {
    uintptr_t app = 0, player = 0;
    return g_chain.player && RdPtr(ModuleBase() + kAppObjectPtr, &app) && app && RdPtr(app + kAppPlayer, &player) &&
           player == g_chain.player && IsPerson(player);
}

// Todos los nodos del esqueleto "sin calcular" (cuerpo bajo el visor).
void InvalidateAll(uintptr_t skel) {
    if (!skel || !g_chain.ok || g_chain.skel != skel || !ChainPlayerIsCurrent()) return;
    SkelView sv;
    if (!GetSkel(g_chain.player, &sv) || sv.skel != skel) return;
    for (int i = 0; i < sv.count; ++i) WrF64(NodeRecord(sv.nodes, i) + kNodeTime, -1.0);
}

// Marca los nodos de los brazos como "sin calcular" para este fotograma.
void InvalidateArms(uintptr_t skel, bool left, bool right) {
    if (!skel || !g_chain.ok || g_chain.skel != skel || !ChainPlayerIsCurrent()) return;
    SkelView sv;
    if (!GetSkel(g_chain.player, &sv) || sv.skel != skel) return;
    for (int s = 0; s < 2; ++s) {
        if ((s == ARM_L && !left) || (s == ARM_R && !right)) continue;
        for (int k = 0; k < 5; ++k) WrF64(NodeRecord(sv.nodes, g_chain.arm[s].node[k]) + kNodeTime, -1.0);
    }
}

// --- ronda 39: los pies de la fisica del juego, sin el cuerpo bajo el visor --------
// El cuerpo bajo el visor (ronda 5) mueve la raiz y con ella las piernas; desde
// la ronda 27 tambien en altura (suavizada: al bajar deprisa queda por encima).
// La fisica del juego lee los pies del esqueleto: con el elevador de Ragnar
// (baja a 2 m/s) los pies quedaban unos 40 cm por encima del suelo, el juego
// lo tomaba por un "paso demasiado bajo" (mas de 30 cm), empezaba una caida
// hasta el fondo del hueco (mas de DieFall) y mataba al personaje en el acto.
// Durante esas consultas la raiz y las piernas del jugador se recalculan sin
// el desplazamiento (los evaluadores no cambian nada) y despues se vuelven a
// marcar sin calcular, para que el dibujo las vuelva a poner bajo el visor.
using PFN_FootPos = void(__fastcall*)(uintptr_t person, void* a, void* b);
using PFN_FootRay = double(__fastcall*)(uintptr_t person, void* a, void* b);
PFN_FootPos g_origFootPosA = nullptr, g_origFootPosB = nullptr;
PFN_FootRay g_origFootRayA = nullptr, g_origFootRayB = nullptr;

void MarkLegsStale(uintptr_t skel) {
    if (!skel || !g_chain.bodyOk || g_chain.skel != skel || !g_chain.legMask || !ChainPlayerIsCurrent()) return;
    SkelView sv;
    if (!GetSkel(g_chain.player, &sv) || sv.skel != skel) return;
    for (int i = 0; i < sv.count && i < 64; ++i) {
        if ((g_chain.legMask >> i) & 1ull) WrF64(NodeRecord(sv.nodes, i) + kNodeTime, -1.0);
    }
}

uintptr_t BeginFootQuery(uintptr_t person) {
    if (!person || person != g_player || g_frameThread == 0 || GetCurrentThreadId() != g_frameThread) return 0;
    uintptr_t skel = g_bodySkel.load(std::memory_order_relaxed);
    if (!skel) return 0;   // sin cuerpo bajo el visor no hay nada que quitar
    if (g_physDepth++ == 0) MarkLegsStale(skel);
    return skel;
}

void EndFootQuery(uintptr_t skel) {
    if (!skel) return;
    if (--g_physDepth == 0) MarkLegsStale(skel);
}

void __fastcall HookedFootPosA(uintptr_t person, void* a, void* b) {
    uintptr_t q = BeginFootQuery(person);
    g_origFootPosA(person, a, b);
    EndFootQuery(q);
}
void __fastcall HookedFootPosB(uintptr_t person, void* a, void* b) {
    uintptr_t q = BeginFootQuery(person);
    g_origFootPosB(person, a, b);
    EndFootQuery(q);
}
double __fastcall HookedFootRayA(uintptr_t person, void* a, void* b) {
    uintptr_t q = BeginFootQuery(person);
    double r = g_origFootRayA(person, a, b);
    EndFootQuery(q);
    return r;
}
double __fastcall HookedFootRayB(uintptr_t person, void* a, void* b) {
    uintptr_t q = BeginFootQuery(person);
    double r = g_origFootRayB(person, a, b);
    EndFootQuery(q);
    return r;
}

// --- ronda 41: parada sin pie apoyado ------------------------------------------
// Al parar, el juego pasa a Rlx con la transicion "con el pie que este apoyado"
// (StopWalk -> +0x12c540 -> +0x12c160 con 5; o 3/4): ese pie se queda clavado
// mientras la postura va a la de reposo, y el cuerpo avanza o retrocede hasta
// media zancada para quedar sobre el. Solo en la parada que manda el mod al
// soltar el joystick, la transicion pasa a 0 (sin pie apoyado: la del propio
// juego cuando no hay ninguno): el personaje se queda donde esta.
using PFN_LaunchAction = unsigned long long(__fastcall*)(uintptr_t person, const char* name, int mode, double t, uintptr_t a5);
using PFN_LaunchActionB = unsigned long long(__fastcall*)(uintptr_t person, const char* name, int mode, double t, uintptr_t a5,
                                                         uintptr_t a6);
PFN_LaunchAction g_origLaunchAction = nullptr;
PFN_LaunchActionB g_origLaunchActionB = nullptr;
bool g_stopTestDispatch = false;   // dentro del RaiseEvent('StopTest') del mod (hilo del juego)

int PlainStopMode(uintptr_t person, int mode) {
    if (!g_stopTestDispatch || person != g_player || g_frameThread == 0 || GetCurrentThreadId() != g_frameThread) return mode;
    if (mode != 3 && mode != 4 && mode != 5) return mode;
    return 0;
}

unsigned long long __fastcall HookedLaunchAction(uintptr_t person, const char* name, int mode, double t, uintptr_t a5) {
    return g_origLaunchAction(person, name, PlainStopMode(person, mode), t, a5);
}
unsigned long long __fastcall HookedLaunchActionB(uintptr_t person, const char* name, int mode, double t, uintptr_t a5, uintptr_t a6) {
    return g_origLaunchActionB(person, name, PlainStopMode(person, mode), t, a5, a6);
}

// Mando (pose 3x4 del espacio de seguimiento) al mundo del juego.
bool ControllerToGame(const VrHandState& hs, const TrackingToGameMap& map, double pos[3], double axes[3][3]) {
    if (!hs.valid) return false;
    const float* m = hs.pose;
    double c[3] = { m[3], -m[7], -m[11] };
    double d[3] = { c[0] - map.hmd[0], c[1] - map.hmd[1], c[2] - map.hmd[2] };
    double cs = map.cosYaw, sn = map.sinYaw;
    double rx = d[0] * cs + d[2] * sn;
    double rz = -d[0] * sn + d[2] * cs;
    pos[0] = map.cam[0] + rx * map.unitsPerMeter;
    pos[1] = map.cam[1] + d[1] * map.unitsPerMeter;
    pos[2] = map.cam[2] + rz * map.unitsPerMeter;
    if (axes) {
        // derecha = conv(col0); abajo = -conv(col1); adelante = -conv(col2) (como el visor)
        double ax[3][3] = { { m[0], -m[4], -m[8] }, { -m[1], m[5], m[9] }, { -m[2], m[6], m[10] } };
        for (int k = 0; k < 3; ++k) {
            axes[k][0] = ax[k][0] * cs + ax[k][2] * sn;
            axes[k][1] = ax[k][1];
            axes[k][2] = -ax[k][0] * sn + ax[k][2] * cs;
        }
    }
    return true;
}

// --- cinematica inversa: hilo del juego, una vez por fotograma -------------------------
void ToCam(const TrackingToGameMap& m, const double p[3], double o[3]) {
    double d[3] = { p[0] - m.cam[0], p[1] - m.cam[1], p[2] - m.cam[2] };
    double upm = m.unitsPerMeter > 1e-6 ? m.unitsPerMeter : 1000.0;
    o[0] = ik::Dot(d, m.right) / upm;
    o[1] = ik::Dot(d, m.down) / upm;
    o[2] = ik::Dot(d, m.fwd) / upm;
}

// Ctrl+F8 (pasada la cuenta atras): brazos estirados al frente, pulgares
// arriba y mirando al frente. Se guardan los ejes de cada mando en la base del
// cuerpo llevados a la pose de reposo (deshaciendo los 90 grados de flexion
// del hombro) y el alcance del jugador desde el hombro del personaje (la vista
// se recentro al empezar la cuenta atras: los ojos del jugador estan en los
// del personaje).
void Calibrate() {
    TrackingToGameMap map;
    char line[400];
    if (!HeadTrackGetTrackingToGameMap(&map)) {
        LogLine("[IK] calibracion: sin seguimiento posicional del visor; no se calibra.");
        PostToast(L"Calibrar las manos", L"No se pudo: falta el seguimiento del visor", kToastMs);
        return;
    }
    double upm = map.unitsPerMeter > 1e-6 ? map.unitsPerMeter : 1000.0;
    const double down[3] = { 0.0, 1.0, 0.0 };
    double F[3] = { map.fwd[0], 0.0, map.fwd[2] }, R[3];
    if (!ik::Norm(F)) return;
    ik::Cross(down, F, R);
    double K[9] = { R[0], R[1], R[2], down[0], down[1], down[2], F[0], F[1], F[2] };
    double flex[9], flexT[9];
    ik::ArmsForwardBody(flex);
    ik::M3T(flex, flexT);
    ArmStat st[2];
    AcquireSRWLockShared(&g_ikLock);
    st[0] = g_armStat[0];
    st[1] = g_armStat[1];
    ReleaseSRWLockShared(&g_ikLock);
    bool done[2] = { false, false };
    for (int s = 0; s < 2; ++s) {
        VrHandState hs;
        double pos[3], ax[3][3];
        if (!GetVrHandState(s, &hs) || !ControllerToGame(hs, map, pos, ax)) continue;
        double cw[9] = { ax[0][0], ax[0][1], ax[0][2], ax[1][0], ax[1][1], ax[1][2], ax[2][0], ax[2][1], ax[2][2] };
        double kt[9], cfwd[9], hang[9];
        ik::M3T(K, kt);
        ik::M3Mul(cw, kt, cfwd);       // filas del mando en la base del cuerpo, brazo al frente
        ik::M3Orthonormalize(cfwd);
        ik::M3Mul(cfwd, flexT, hang);  // el mismo mando con el brazo colgando
        ik::M3Orthonormalize(hang);
        double def[9];
        DefaultHang(s, def);
        std::memcpy(g_ctrlHang[s], hang, sizeof(hang));
        g_ctrlHangCalibrated[s] = true;
        done[s] = true;
        // Alcance: mano pedida (mando + desplazamiento del agarre) al hombro del personaje.
        double off[3] = { kGripOffM[0] * upm, kGripOffM[1] * upm, kGripOffM[2] * upm }, offW[3], hand[3];
        ik::VecM3(off, cw, offW);
        ik::Add(pos, offW, hand);
        double reach = -1.0, charReach = -1.0;
        if (st[s].valid) {
            reach = Dist3(hand, st[s].anim[0]) / upm;
            if (g_chain.ok) charReach = (g_chain.arm[s].lenUpper + g_chain.arm[s].lenLower + g_chain.arm[s].lenHand) / upm;
            if (reach > 0.35 && reach < 1.3) g_userReachM[s] = reach;
        }
        double rel[3];
        ToCam(map, pos, rel);
        std::snprintf(line, sizeof(line),
                      "[IK] calibrado el mando %s (brazo al frente): f(%.2f,%.2f,%.2f) d(%.2f,%.2f,%.2f) | colgando a %.0f grados del de serie | "
                      "mando a (%.2f,%.2f,%.2f) m de la camara | alcance del jugador %.2f m (personaje %.2f m)%s",
                      kArmLabel[s], cfwd[6], cfwd[7], cfwd[8], cfwd[3], cfwd[4], cfwd[5], ik::M3AngleBetween(hang, def) * 180.0 / ik::kPi,
                      rel[0], rel[1], rel[2], reach, charReach,
                      (reach > 0.35 && reach < 1.3) ? "" : " (no se usa)");
        LogLine(line);
    }
    bool saved = (done[0] || done[1]) && SaveCalibration();
    for (int s = 0; s < 2; ++s) {
        if (done[s]) VrInputHaptic(s, 0.3f, 1.0f);
    }
    const wchar_t* txt = (done[0] && done[1]) ? (saved ? L"Hecho (guardado)" : L"Hecho (no se pudo guardar)")
                         : (done[0] ? L"Solo el mando izquierdo" : (done[1] ? L"Solo el mando derecho" : L"No se ven los mandos"));
    PostToast(L"Calibrar las manos", txt, kToastMs);
}

bool CallPersonAttach(uintptr_t fn, uintptr_t person) {
    __try {
        reinterpret_cast<void(__fastcall*)(uintptr_t)>(fn)(person);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void SolveNowAndAttach(uintptr_t player, uintptr_t skel) {
    if (!t_inSolve) {
        t_inSolve = true;
        MaybeSolve(skel);
        t_inSolve = false;
    }
    uintptr_t vt = 0, fn = 0;
    if (!RdPtr(player, &vt) || vt != ModuleBase() + kVtPerson || !RdPtr(vt + 2 * 8, &fn) || fn != ModuleBase() + kFnPersonAttach) return;
    CallPersonAttach(fn, player);
}

// Cuerpo bajo el visor: desplazamiento horizontal del visor respecto a la
// camara del juego y hacia donde mira el pecho (la vista, y si las dos manos
// estan delante, a medias con ellas), suavizado.
bool PersonPos(uintptr_t person, double out[3]);

void UpdateBody(bool on, uintptr_t player, uintptr_t skel, uintptr_t nodes, const TrackingToGameMap& map, const bool handOk[2],
                double dt) {
    if (!on || !skel || !nodes) {
        g_bodyYValid = false;   // ronda 36
        if (g_bodyWasActive) {
            uintptr_t old = g_bodySkel.exchange(0);
            g_bodyNodeMask.store(0, std::memory_order_relaxed);
            InvalidateAll(old);
            g_bodyWasActive = false;
            g_bodyFwdValid = false;
        }
        return;
    }
    double upm = map.unitsPerMeter > 1e-6 ? map.unitsPerMeter : 1000.0;
    {
        double vert = (map.base[1] - map.cam[1]) / upm;   // + = visor por encima (el juego tiene y hacia abajo)
        vert -= HeadTrackEyeLiftM();                        // ronda 46: sin la altura anadida a los ojos
        double t = NowSec();
        if (std::fabs(vert) <= kViewHeightWarnM) {
            g_viewHeightSince = 0.0;
        } else if (g_viewHeightSince == 0.0) {
            g_viewHeightSince = t;
        } else if (t - g_viewHeightSince > kViewHeightWarnSec && t >= g_viewHeightNextHint) {
            g_viewHeightNextHint = t + kViewHeightHintEverySec;
            int cm = static_cast<int>(std::fabs(vert) * 100.0 + 0.5);
            wchar_t txt[128];
            std::swprintf(txt, 128, L"El visor esta %d cm %ls que al recentrar: clic derecho para recentrar", cm,
                          vert > 0.0 ? L"mas alto" : L"mas bajo");
            PostToast(L"Vista desplazada", txt, kToastMs);
            char line[200];
            std::snprintf(line, sizeof(line), "[IK] visor %.2f m %s de donde se recentro durante %.0f s: aviso de recentrar (clic derecho).",
                          std::fabs(vert), vert > 0.0 ? "por encima" : "por debajo", kViewHeightWarnSec);
            LogLine(line);
        }
    }
    double delta[3] = { map.cam[0] - map.base[0], 0.0, map.cam[2] - map.base[2] };
    double len = ik::Len(delta);
    double maxLen = kBodyMaxOffsetM * upm;
    if (len > maxLen) {
        for (int i = 0; i < 3; ++i) delta[i] *= maxLen / len;
    }
    // Ronda 27: la altura de la camara va suavizada en las escaleras: el cuerpo
    // sube o baja lo mismo para seguir bajo los ojos (la cabeza lo deshace).
    // Ronda 36: con la altura de la persona suavizada, no con la de la camara.
    {
        double pp[3];
        double bdt = (dt > 0.0 && dt < 0.25) ? dt : 0.0;
        if (!PersonPos(player, pp)) {
            g_bodyYValid = false;
            delta[1] = 0.0;
        } else {
            if (!g_bodyYValid || bdt <= 0.0 || std::fabs(pp[1] - g_bodyY) > kBodyYSnapM * upm) {
                g_bodyY = pp[1];
                g_bodyYVel = 0.0;
                g_bodyYValid = true;
            } else {
                SmoothDampStep(&g_bodyY, &g_bodyYVel, pp[1], kBodyYSmoothSec, bdt);
            }
            delta[1] = g_bodyY - pp[1];
        }
    }
    // Ronda 24: adelante de la vista en horizontal tambien al mirar abajo o
    // arriba (con el "arriba" de la cabeza, como el diorama): la proyeccion
    // del adelante solo se quedaba en casi nada y su direccion bailaba.
    double camF[3];
    if (!ik::HorizontalHeading(map.fwd, map.down, camF)) return;
    double tf[3] = { camF[0], camF[1], camF[2] };
    // Con las manos delante, el pecho mira a medias hacia ellas, con un peso
    // que crece poco a poco (ronda 24: antes era si o no a 0,15 m y con el
    // producto 0,3, y podia saltar 30 grados de un fotograma a otro). Con el
    // arma a dos manos solo cuenta la derecha (la izquierda del jugador no
    // sujeta nada).
    bool twoHandMid = g_twoHandW > 0.0 && handOk[ARM_R] && g_haveLastTarget[ARM_R];
    if (twoHandMid || (handOk[0] && handOk[1] && g_haveLastTarget[0] && g_haveLastTarget[1])) {
        double mid[3];
        if (twoHandMid) {
            mid[0] = g_lastHandPos[ARM_R][0] - map.cam[0];
            mid[2] = g_lastHandPos[ARM_R][2] - map.cam[2];
        } else {
            mid[0] = (g_lastHandPos[0][0] + g_lastHandPos[1][0]) * 0.5 - map.cam[0];
            mid[2] = (g_lastHandPos[0][2] + g_lastHandPos[1][2]) * 0.5 - map.cam[2];
        }
        mid[1] = 0.0;
        double ml = ik::Len(mid);
        if (ml > 1e-6) {
            for (int i = 0; i < 3; ++i) mid[i] /= ml;
            double wl = SmoothStep(0.10, 0.25, ml / upm) * SmoothStep(0.1, 0.5, ik::Dot(mid, camF));
            if (wl > 0.0) {
                for (int i = 0; i < 3; ++i) tf[i] = camF[i] + mid[i] * wl;
                if (!ik::Norm(tf, 1e-6)) ik::Copy3(camF, tf);
            }
        }
    }
    if (!g_bodyFwdValid) {
        ik::Copy3(tf, g_bodyFwdSmooth);
        g_bodyFwdValid = true;
    } else {
        double a = 1.0 - std::exp(-(dt > 0.0 && dt < 0.1 ? dt : 0.016) / kBodyYawSmoothSec);
        double f[3];
        ik::Lerp3(g_bodyFwdSmooth, tf, a, f);
        f[1] = 0.0;
        if (ik::Norm(f)) ik::Copy3(f, g_bodyFwdSmooth);
    }
    BodyShared b = {};
    b.skel = skel;
    b.nodes = nodes;
    b.root = g_chain.root;
    b.chest = g_chain.chest;
    b.head = g_chain.head;
    ik::Copy3(delta, b.delta);
    ik::Copy3(g_bodyFwdSmooth, b.fwd);
    b.weight = g_bodyW;   // ronda 23 (antes el mayor de los dos brazos)
    {
        // Con un enemigo fijado, kChestLockedWeight (ronda 18: 1, como sin el;
        // en las rondas 10-17 era 0 y el brazo del escudo quedaba doblado).
        double target = g_lockedPub.load(std::memory_order_relaxed) ? kChestLockedWeight : 1.0;
        double a = 1.0 - std::exp(-(dt > 0.0 && dt < 0.1 ? dt : 0.016) / kChestLockSmoothSec);
        g_chestWeight += (target - g_chestWeight) * a;
        if (std::fabs(g_chestWeight - target) < 1e-3) g_chestWeight = target;
    }
    // Ronda 24: con el arma a dos manos el pecho se queda como lo pone el juego
    // (de lado, con los hombros donde hacen falta para agarrarla).
    b.chestWeight = g_chestWeight * g_comboChestW * (1.0 - g_twoHandChestW);   // ronda 27: mas despacio que la mano
    ik::Copy3(g_chain.chestFwdLocal, b.chestFwdLocal);
    AcquireSRWLockExclusive(&g_ikLock);
    g_body = b;
    ReleaseSRWLockExclusive(&g_ikLock);
    unsigned long long mask = (1ull << b.root) | (1ull << b.chest) | (1ull << b.head);
    g_bodyNodeMask.store(mask, std::memory_order_relaxed);
    g_bodySkel.store(skel, std::memory_order_relaxed);
    g_bodyWasActive = true;
}

void EndSwing(double now);
void EndPunch(uintptr_t player, double now);

// Los brazos vuelven a la animacion de golpe (diorama, menu, otro jugador).
void IkShutdown() {
    g_ikWeight[0] = g_ikWeight[1] = 0.0;
    g_bodyW = 0.0;
    g_comboChestW = 1.0;
    g_twoHandGrabbed = false;
    g_twoHandW = 0.0;
    g_twoHandChestW = 0.0;
    g_comboHoldPub.store(false, std::memory_order_relaxed);
    g_gestureReadyPub.store(false, std::memory_order_relaxed);
    g_shieldGestureOk.store(false, std::memory_order_relaxed);
    g_armsGestureOkPub.store(false, std::memory_order_relaxed);   // ronda 30
    g_twoHandWeaponPub.store(false, std::memory_order_relaxed);   // ronda 36
    EndSwing(NowSec());
    EndPunch(g_player, NowSec());   // ronda 31
    AcquireSRWLockExclusive(&g_ikLock);
    g_armStat[0].twistValid = g_armStat[1].twistValid = false;
    g_armStat[0].poleValid = g_armStat[1].poleValid = false;
    g_grip.valid = false;
    ReleaseSRWLockExclusive(&g_ikLock);
    if (!g_ikWasActive) return;
    uintptr_t old = g_ikSkel.exchange(0);
    g_ikNodeMask.store(0, std::memory_order_relaxed);
    InvalidateArms(old, true, true);
    if (g_bodyWasActive) {
        g_bodySkel.store(0, std::memory_order_relaxed);
        g_bodyNodeMask.store(0, std::memory_order_relaxed);
        InvalidateAll(old);
        g_bodyWasActive = false;
        g_bodyFwdValid = false;
    }
    g_ikWasActive = false;
}

// Ronda 26: el agarre recien fijado, contra el eje del arma (la recta de su
// area mas larga), en los ejes de la mano derecha. Con el arma ya colocada en
// la mano derecha resuelta (tras SolveNowAndAttach). Si la mano izquierda
// queda bastante mas lejos del eje que la derecha, se lleva a el (a la misma
// distancia que la derecha, por el mismo lado). Solo si el eje pasa cerca de
// la mano derecha (es el mango; en un hacha el area es el filo).
void GripShaftCheck(uintptr_t player, const SkelView& sv) {
    int gi = g_gripShaftCheck;
    g_gripShaftCheck = -1;
    if (gi < 0 || gi >= g_gripTableN) return;
    GripFixed& gf = g_gripTable[gi];
    uintptr_t weapon = InventoryItem(player, kInvRight);
    double hw[16], ww[16];
    WeaponGeom geom;
    if (!weapon || !IsWeapon(weapon)) {
        // Ronda 60: sin el arma en la mano no era su agarre: fuera, y se vuelve a medir.
        if (gi == g_gripTableN - 1) --g_gripTableN;
        else gf.kind[0] = 0;
        if (g_gripCur == gi) g_gripCur = -1;
        g_gripRlxSince = -1.0;
        AcquireSRWLockExclusive(&g_ikLock);
        g_grip.valid = false;   // la siguiente resolucion empieza de cero (sin "captured")
        ReleaseSRWLockExclusive(&g_ikLock);
        return;
    }
    if (!ReadWeaponGeom(weapon, &geom) || geom.n <= 0 || !WeaponWorld(weapon, ww) ||
        !NodeWorld(sv, g_chain.arm[ARM_R].node[4], hw, nullptr))
        return;
    int best = 0;
    double bestLen = -1.0;
    for (int i = 0; i < geom.n; ++i) {
        double l = Dist3(geom.a[i], geom.b[i]);
        if (l > bestLen) { bestLen = l; best = i; }
    }
    double rot[9], sc[3], ph[3], rotT[9], pa[3], pb[3], qa[3], qb[3], d[3];
    ik::SplitM4(hw, rot, sc, ph);
    ik::M3T(rot, rotT);
    XformPoint(ww, geom.a[best], pa);
    XformPoint(ww, geom.b[best], pb);
    ik::Sub(pa, ph, d);
    ik::VecM3(d, rotT, qa);
    ik::Sub(pb, ph, d);
    ik::VecM3(d, rotT, qb);
    double ax[3];
    ik::Sub(qb, qa, ax);
    double axLen = ik::Len(ax);
    if (axLen < 50.0 || !ik::Norm(ax)) return;
    auto proj = [&](const double p[3], double out[3]) {
        double v[3];
        ik::Sub(p, qa, v);
        double t = ik::Dot(v, ax);
        for (int k = 0; k < 3; ++k) out[k] = qa[k] + ax[k] * t;
    };
    const double origin[3] = { 0.0, 0.0, 0.0 };
    double pr[3], pl[3];
    proj(origin, pr);
    proj(gf.rel, pl);
    double dR = ik::Len(pr);
    double off[3];
    ik::Sub(gf.rel, pl, off);
    double dL = ik::Len(off);
    if (dR > kGripShaftMaxRightMm || dL <= dR + kGripShaftSnapMm) return;
    double keep = dR > 20.0 ? dR : 20.0;
    double nrel[3];
    for (int k = 0; k < 3; ++k) nrel[k] = pl[k] + (dL > 1e-6 ? off[k] * keep / dL : 0.0);
    ik::Copy3(nrel, gf.rel);
}

// Ronda 50: tensando el arco, la mano derecha se gira lo justo para que la
// flecha (su -Z, por donde la suelta el juego) apunte de la mano derecha al
// arco (la mano izquierda). Vectores fila: con la rotacion de mundo M (v' =
// v M), mano' = mano M y la flecha, pegada a la mano, flecha' = flecha M.
void BowAlignRightHand() {
    if (!g_bow.drawing || !g_bow.relValid || !g_haveLastTarget[ARM_L]) return;
    double d[3];
    ik::Sub(g_lastHandPos[ARM_L], g_lastHandPos[ARM_R], d);
    if (!ik::Norm(d, 1.0)) return;
    double arrowRot[9];
    ik::M3Mul(g_bow.rel, g_lastHandRot[ARM_R], arrowRot);
    double a[3] = { -arrowRot[6], -arrowRot[7], -arrowRot[8] };
    if (!ik::Norm(a)) return;
    double u[3];
    ik::Cross(a, d, u);
    double s = ik::Len(u), c = ik::Dot(a, d);
    if (s < 1e-6) {
        if (c > 0.0) return;   // ya apunta
        double t[3] = { 1.0, 0.0, 0.0 };
        if (std::fabs(a[0]) > 0.9) { t[0] = 0.0; t[1] = 1.0; }
        ik::Cross(a, t, u);
        s = 0.0;
        c = -1.0;
    }
    if (!ik::Norm(u)) return;
    const double th = std::atan2(s, c), ct = std::cos(th), st = std::sin(th);
    const double K[9] = { 0.0, -u[2], u[1], u[2], 0.0, -u[0], -u[1], u[0], 0.0 };
    double M[9];
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            M[r * 3 + col] = (r == col ? ct : 0.0) - st * K[r * 3 + col] + (1.0 - ct) * u[r] * u[col];
    double h2[9];
    ik::M3Mul(g_lastHandRot[ARM_R], M, h2);
    ik::M3Orthonormalize(h2);
    std::memcpy(g_lastHandRot[ARM_R], h2, sizeof(h2));
}

// Pesos, objetivos y publicacion para los evaluadores. *fpOut: primera persona.
void IkFrame(uintptr_t player, double dt, bool* fpOut) {
    g_ikFrameAt.store(NowSec(), std::memory_order_relaxed);   // ronda 32
    *fpOut = false;
    SkelView sv;
    bool chainOk = g_chain.ok && g_chain.player == player;
    bool haveSkel = chainOk && GetSkel(player, &sv) && sv.skel == g_chain.skel && sv.count == g_chain.count;
    TrackingToGameMap map;
    bool haveMap = HeadTrackGetTrackingToGameMap(&map) && GetTickCount64() - map.tick < 250;
    double upm = haveMap && map.unitsPerMeter > 1e-6 ? map.unitsPerMeter : 1000.0;
    g_ikMapValid = haveMap;
    if (haveMap) g_ikMap = map;
    bool fp = false;
    if (haveSkel && haveMap && g_chain.head >= 0) {
        double hw[16];
        if (NodeWorld(sv, g_chain.head, hw, nullptr)) {
            double vp[3];
            if (g_chain.haveView) {
                double zero[3] = { 0.0, 0.0, 0.0 }, at[16];
                MatMul(g_chain.viewAnchor, hw, at);
                XformPoint(at, zero, vp);
            } else {
                Translation(hw, vp);
            }
            const double fpDist = Dist3(vp, map.base) / upm;
            const bool wasFp = g_firstPersonNow.load(std::memory_order_relaxed);   // ronda 49: con margen para salir
            fp = fpDist < (wasFp ? kFirstPersonExitM : (g_chain.haveView ? kFirstPersonMaxM : kFirstPersonMaxM + 0.1));
        }
    }
    *fpOut = fp;
    g_firstPersonNow.store(fp, std::memory_order_relaxed);
    int life = 0;
    bool alive = RdI32(player + kPLife, &life) && life > 0;
    const int combatMode = g_combatMode.load(std::memory_order_relaxed);
    // Brazos con los mandos: con los dos evaluadores enganchados y fuera del
    // modo 0 (VR basico).
    bool enabled = g_ikAvailable && combatMode != 0;
    bool base = enabled && haveSkel && haveMap && fp && alive && !DioramaEnabled();
    // Ronda 30: armas fuera o manos libres (gesto de la espalda y punetazos).
    {
        uintptr_t invR = InventoryItem(player, kInvRight), invL = InventoryItem(player, kInvLeft);
        bool weaponsOut = (invR && IsWeapon(invR)) || invL != 0;
        g_weaponsOutPub.store(weaponsOut, std::memory_order_relaxed);
        g_unarmedPub.store(invR == 0 && invL == 0, std::memory_order_relaxed);
        g_rightEmptyPub.store(invR == 0, std::memory_order_relaxed);   // ronda 32
        char lk[48] = "";   // ronda 58: arco en la izquierda (hombro de la animacion)
        g_leftBowIk = invL && EntityKind(invL, lk, sizeof(lk)) && _strnicmp(lk, "Arco", 4) == 0;
    }
    // Ronda 23, modo 2: durante la animacion de ataque del juego (y desde que
    // se pulsa A) los brazos y el pecho son los de la animacion.
    bool comboHold = false;
    {
        char anim[48];
        ReadAnimNames(player, anim, sizeof(anim), nullptr, 0);
        double nowS = NowSec();
        if (IsAttackAnimName(anim)) g_comboLastAttackAnim = nowS;
        unsigned long long pt = g_comboPressTick.load(std::memory_order_relaxed);
        bool pressRecent = pt != 0 && GetTickCount64() - pt < static_cast<unsigned long long>(kComboPressWaitSec * 1000.0);
        bool animRecent = g_comboLastAttackAnim >= 0.0 && nowS - g_comboLastAttackAnim < kComboTailSec;
        comboHold = combatMode == 2 && base && (pressRecent || animRecent);
        g_comboHoldPub.store(comboHold, std::memory_order_relaxed);
        g_armsGestureOkPub.store(base && chainOk && !comboHold, std::memory_order_relaxed);
        double ct = comboHold ? 0.0 : 1.0;
        double cstep = (dt > 0.0 && dt < 0.1 ? dt : 0.016) / (comboHold ? kComboFadeOutSec : kComboChestInSec);
        g_comboChestW = ik::StepToward(g_comboChestW, ct, cstep);   // ronda 25 (alternaba 1 / 0,93)
    }

    double bodyR[3], bodyF[3];
    BodyFrame(haveSkel ? sv.skel : 0, haveMap ? &map : nullptr, bodyR, bodyF);
    if (g_bodyWasActive && g_bodyFwdValid) {
        // Con el cuerpo bajo el visor, el pecho mira hacia g_bodyFwdSmooth.
        const double downV[3] = { 0.0, 1.0, 0.0 };
        ik::Copy3(g_bodyFwdSmooth, bodyF);
        ik::Cross(downV, bodyF, bodyR);
    }
    // Escudo (o lo que vaya en el antebrazo): la mano se queda en su correa.
    for (int s = 0; s < 2; ++s) g_forearmItem[s] = chainOk ? ItemOnNode(player, g_chain.mesh, g_chain.arm[s].node[2]) : 0;
    bool handOk[2] = { false, false };
    for (int s = 0; s < 2; ++s) {
        VrHandState hs;
        double pos[3], ax[3][3];
        bool hand = haveMap && GetVrHandState(s, &hs) && ControllerToGame(hs, map, pos, ax);
        handOk[s] = hand;
        if (hand && chainOk) {
            double cw[9] = { ax[0][0], ax[0][1], ax[0][2], ax[1][0], ax[1][1], ax[1][2], ax[2][0], ax[2][1], ax[2][2] };
            double grip[9];
            ik::GripFrom(g_chain.arm[s].handBody, g_ctrlHang[s], grip);
            ik::M3Mul(grip, cw, g_lastHandRot[s]);
            double off[3] = { kGripOffM[0] * upm, kGripOffM[1] * upm, kGripOffM[2] * upm }, offW[3];
            ik::VecM3(off, cw, offW);
            ik::Add(pos, offW, g_lastHandPos[s]);
            g_haveLastTarget[s] = true;
            if (s == ARM_R) BowAlignRightHand();   // ronda 50 (la izquierda ya esta: s = 0 va antes)
        }
        bool wanted = base && hand && !comboHold;
        double step = (dt > 0.0 && dt < 0.1 ? dt : 0.016) / (comboHold ? kComboFadeOutSec : kIkBlendSec);
        g_ikWeight[s] = ik::StepToward(g_ikWeight[s], wanted ? 1.0 : 0.0, step);
        if (!g_haveLastTarget[s]) g_ikWeight[s] = 0.0;
        if (!alive) g_ikWeight[s] = 0.0;   // ronda 32: al morir, la animacion de muerte ya (sin fundido)
    }
    {
        // Cuerpo bajo el visor: con los brazos, y tambien durante un combo del
        // modo 2 (la raiz sigue bajo el visor; el pecho, ver g_comboChestW).
        bool wantedBody = base && (handOk[0] || handOk[1]) && (g_haveLastTarget[0] || g_haveLastTarget[1]);
        double step = (dt > 0.0 && dt < 0.1 ? dt : 0.016) / kIkBlendSec;
        g_bodyW = ik::StepToward(g_bodyW, wantedBody ? 1.0 : 0.0, step);
        if (!alive) g_bodyW = 0.0;   // ronda 32
    }

    // Ronda 21: arma a dos manos en la derecha: la izquierda va a ella.
    {
        uintptr_t right = InventoryItem(player, kInvRight);
        const uintptr_t rightNow = right;   // ronda 60: lo que hay de verdad en la mano (sin el margen de abajo)
        if (!right && g_rightItemSeen) {
            // Ronda 23: al atacar con el baston la mano derecha queda vacia un
            // momento; la izquierda no suelta el arma por eso.
            double nowS = NowSec();
            if (g_rightEmptySince <= 0.0) g_rightEmptySince = nowS;
            if (nowS - g_rightEmptySince < kTwoHandRightGraceSec) right = g_rightItemSeen;
        } else {
            g_rightEmptySince = 0.0;
        }
        if (right != g_rightItemSeen) {
            g_rightItemSeen = right;
            AcquireSRWLockExclusive(&g_ikLock);
            g_grip.valid = false;   // otra arma: otra relacion entre las manos
            g_gripWorldT = -1.0;
            ReleaseSRWLockExclusive(&g_ikLock);
            g_twoHandGrabbed = false;   // ronda 27: el arma nueva empieza solo en la derecha
            char kind[64] = "";
            bool weapon = right && IsWeapon(right);
            bool haveKind = weapon && EntityKind(right, kind, sizeof(kind));
            g_rightTwoHanded = haveKind && IsTwoHandKind(kind);
            // Ronda 24: agarre fijo ya guardado para este tipo de arma.
            g_gripCur = -1;
            g_gripRlxSince = -1.0;
            g_gripKind[0] = 0;
            if (g_rightTwoHanded) {
                std::snprintf(g_gripKind, sizeof(g_gripKind), "%s", kind);
                for (int i = 0; i < g_gripTableN; ++i)
                    if (_stricmp(g_gripTable[i].kind, g_gripKind) == 0) g_gripCur = i;
            }
        }
        // Ronda 27: la izquierda coge el arma al acercar el mando a su agarre.
        // Ronda 36: con el agarre izquierdo apretado, y la sujeta mientras se
        // mantiene (sin distancia para soltarla).
        {
            VrHandState lhs;
            bool gripL = GetVrHandState(ARM_L, &lhs) && lhs.valid && lhs.grip;
            double gw[3];
            bool gwOk = false;
            AcquireSRWLockShared(&g_ikLock);
            if (g_gripWorldT >= 0.0 && NowSec() - g_gripWorldT < 0.25) {
                ik::Copy3(g_gripWorld, gw);
                gwOk = true;
            }
            ReleaseSRWLockShared(&g_ikLock);
            if (!g_rightTwoHanded) {
                g_twoHandGrabbed = false;
            } else if (g_twoHandGrabbed && !gripL) {
                g_twoHandGrabbed = false;
                g_hapticGrab.store(2, std::memory_order_relaxed);
            } else if (!g_twoHandGrabbed && gripL && gwOk && g_haveLastTarget[ARM_L] && g_ikWeight[ARM_L] >= 0.99 &&
                       g_ikWeight[ARM_R] >= 0.99) {
                double dist = Dist3(g_lastHandPos[ARM_L], gw) / upm;
                if (dist < kGrabOnM) {
                    g_twoHandGrabbed = true;
                    g_hapticGrab.store(1, std::memory_order_relaxed);
                }
            }
        }
        g_twoHandWeaponPub.store(base && g_rightTwoHanded, std::memory_order_relaxed);   // ronda 36 (VrPadHook)
        double target = (g_rightTwoHanded && g_ikWeight[ARM_R] > 0.0 && g_twoHandGrabbed) ? 1.0 : 0.0;
        double step = (dt > 0.0 && dt < 0.1 ? dt : 0.016) / kGrabBlendSec;
        g_twoHandW = ik::StepToward(g_twoHandW, target, step);   // ronda 25 (alternaba 1 / 0,93: el temblor)
        g_twoHandChestW = ik::StepToward(g_twoHandChestW, target, (dt > 0.0 && dt < 0.1 ? dt : 0.016) / kTwoHandChestSec);
        // Ronda 24: el agarre del juego para este arma se guarda tras
        // kGripFixRlxSec en reposo (Rlx) con las dos manos en ella: la relacion
        // filtrada entre las manos de esa animacion. Desde entonces, fija.
        // Ronda 60: solo con el arma de verdad en la mano derecha. Al cargar tras
        // morir con la amazona (21:57 del 12-10) la mano estaba vacia, pero
        // g_rightTwoHanded y g_gripKind ("Bo") seguian los de antes: se fijo el
        // agarre del baston con las manos colgando, a (-248,-20,494) mm en vez de
        // (-454,-47,61), y al sacarlo la izquierda no lo podia coger.
        const bool holding = rightNow && rightNow == g_rightItemSeen;
        bool captured = false;
        double capRel[3] = { 0.0, 0.0, 0.0 }, capRot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        AcquireSRWLockExclusive(&g_ikLock);
        if (g_grip.captured && g_grip.valid) {
            captured = true;
            ik::Copy3(g_grip.capRel, capRel);
            std::memcpy(capRot, g_grip.capRelRot, sizeof(capRot));
        }
        ReleaseSRWLockExclusive(&g_ikLock);
        if (captured && holding && g_gripCur < 0 && g_rightTwoHanded && g_gripKind[0] && g_gripTableN < 32) {
            GripFixed& gf = g_gripTable[g_gripTableN];
            std::snprintf(gf.kind, sizeof(gf.kind), "%s", g_gripKind);
            ik::Copy3(capRel, gf.rel);
            std::memcpy(gf.relRot, capRot, sizeof(capRot));
            g_gripCur = g_gripTableN++;
            g_gripShaftCheck = g_gripCur;
        }
        char an[48];
        ReadAnimNames(player, an, sizeof(an), nullptr, 0);
        bool rlx = (an[0] == 'R' || an[0] == 'r') && (an[1] == 'l' || an[1] == 'L') && (an[2] == 'x' || an[2] == 'X');
        double nowG = NowSec();
        if (!rlx || !holding || !g_rightTwoHanded || g_ikWeight[ARM_L] < 0.99 || g_ikWeight[ARM_R] < 0.99) g_gripRlxSince = -1.0;
        else if (g_gripRlxSince < 0.0) g_gripRlxSince = nowG;
        double rlxFor = g_gripRlxSince >= 0.0 ? nowG - g_gripRlxSince : -1.0;
        g_gripAccumReq = g_rightTwoHanded && g_gripCur < 0 && rlxFor >= kGripFixSkipSec;
        g_gripCaptureReq = g_gripAccumReq && rlxFor >= kGripFixSkipSec + kGripFixRlxSec;
    }

    // (Descartado afinar el eje del codo con la flexion de la animacion: el
    // del brazo izquierdo se iba hasta 110 grados y retorcia codo y antebrazo.)

    bool active = haveSkel && (g_ikWeight[0] > 0.0 || g_ikWeight[1] > 0.0 || g_bodyW > 0.0);
    UpdateBody(active && g_chain.bodyOk && g_bodyPhysicsSafe && haveMap, player, haveSkel ? sv.skel : 0, haveSkel ? sv.nodes : 0, map, handOk, dt);
    if (active) {
        IkShared sh = {};
        sh.skel = sv.skel;
        sh.nodes = sv.nodes;
        unsigned long long mask = 0;
        for (int s = 0; s < 2; ++s) {
            sh.weight[s] = g_ikWeight[s];
            std::memcpy(sh.handRot[s], g_lastHandRot[s], sizeof(sh.handRot[s]));
            ik::Copy3(g_lastHandPos[s], sh.handPos[s]);
            for (int k = 0; k < 5; ++k) {
                sh.node[s][k] = g_chain.arm[s].node[k];
                if (g_ikWeight[s] > 0.0) mask |= 1ull << g_chain.arm[s].node[k];
            }
            std::memcpy(sh.bindH[s], g_chain.arm[s].bindH, sizeof(sh.bindH[s]));
            ik::Copy3(g_chain.arm[s].hingeLocal, sh.hingeLocal[s]);
            sh.maxStretch[s] = MaxStretch(s);
            sh.lockWrist[s] = g_forearmItem[s] != 0;
            sh.restTwist[s] = kRestTwist[s] && g_chain.arm[s].restOk;
            std::memcpy(sh.restRel[s][0], g_chain.arm[s].restRelE, sizeof(sh.restRel[s][0]));
            std::memcpy(sh.restRel[s][1], g_chain.arm[s].restRelFo, sizeof(sh.restRel[s][1]));
            std::memcpy(sh.restRel[s][2], g_chain.arm[s].restRelWr, sizeof(sh.restRel[s][2]));
            sh.floatOk[s] = g_chain.arm[s].floatOk;   // ronda 48
            std::memcpy(sh.floatRel[s], g_chain.arm[s].floatRel, sizeof(sh.floatRel[s]));
            sh.restShoulder[s] = kRestShoulder[s] && g_chain.arm[s].restShoulderOk && g_chain.chest >= 0;   // ronda 57
            sh.clav[s] = g_chain.arm[s].clav;
            std::memcpy(sh.bindClav[s], g_chain.arm[s].bindClav, sizeof(sh.bindClav[s]));
            std::memcpy(sh.bindArm[s], g_chain.arm[s].bindArm, sizeof(sh.bindArm[s]));
        }
        sh.floating = combatMode == 3 && CombatVRBodilessActive();   // ronda 48: manos flotantes
        sh.chest = g_chain.chest;
        ik::Copy3(bodyR, sh.bodyRight);
        ik::Copy3(bodyF, sh.bodyFwd);
        sh.twoHand = g_twoHandW;
        sh.twoHandWeapon = g_rightTwoHanded;
        sh.bowLeft = g_leftBowIk;   // ronda 58
        sh.gripFixed = g_gripCur >= 0;
        if (sh.gripFixed) {
            ik::Copy3(g_gripTable[g_gripCur].rel, sh.gripRel);
            std::memcpy(sh.gripRelRot, g_gripTable[g_gripCur].relRot, sizeof(sh.gripRelRot));
        }
        sh.gripCapture = g_gripCaptureReq;
        sh.gripAccum = g_gripAccumReq;
        AcquireSRWLockExclusive(&g_ikLock);
        g_ikShared = sh;
        ReleaseSRWLockExclusive(&g_ikLock);
        g_ikNodeMask.store(mask, std::memory_order_relaxed);
        g_ikSkel.store(sv.skel, std::memory_order_relaxed);
        // Que se recalculen (y se resuelvan) aunque la animacion no avance; se
        // resuelven ya, con los mandos de este fotograma, y se vuelve a colocar
        // lo enganchado (el juego lo coloco antes, en el tick de la persona,
        // con la mano del fotograma anterior: el arma se separaba al moverla).
        if (g_bodyWasActive) InvalidateAll(sv.skel);   // el cuerpo entero, con su desplazamiento y su giro
        else InvalidateArms(sv.skel, g_ikWeight[ARM_L] > 0.0, g_ikWeight[ARM_R] > 0.0);
        SolveNowAndAttach(player, sv.skel);
        if (g_gripShaftCheck >= 0) GripShaftCheck(player, sv);
    } else if (g_ikWasActive) {
        AcquireSRWLockExclusive(&g_ikLock);
        g_armStat[0].twistValid = g_armStat[1].twistValid = false;
        ReleaseSRWLockExclusive(&g_ikLock);
        uintptr_t old = g_ikSkel.exchange(0);
        g_ikNodeMask.store(0, std::memory_order_relaxed);
        InvalidateArms(old, true, true);   // vuelven a la animacion ya en este fotograma
    }
    g_ikWasActive = active;
    bool shieldOk = active && alive && fp && g_ikWeight[ARM_L] >= 0.99 && g_forearmItem[ARM_L] != 0;
    g_shieldGestureOk.store(shieldOk, std::memory_order_relaxed);
    {
        // Ronda 33: escudo VR (hilo del juego, lo leen +0xb2240 y +0xbded0).
        double dropM = 1e9;
        if (shieldOk && haveMap && g_haveLastTarget[ARM_L]) dropM = (g_lastHandPos[ARM_L][1] - map.cam[1]) / upm;   // y abajo
        // Ronda 48: en el modo 3 desde los ojos del personaje (la altura anadida
        // a los ojos por debajo de la camara): alzado = a la altura de su cara o
        // sus hombros, donde llegan los golpes a el.
        if (dropM < 1e8) dropM -= HeadTrackEyeLiftM();
        g_vrShieldUp = shieldOk && dropM <= (g_vrShieldUp ? kShieldLowerDropM : kShieldRaisedMaxDropM);
        g_vrShieldItem = shieldOk ? g_forearmItem[ARM_L] : 0;
        g_vrShieldPlayer = player;
        // Ronda 57: arma a dos manos alzada.
        bool twoOk = active && alive && fp && g_ikWeight[ARM_L] >= 0.99 && g_ikWeight[ARM_R] >= 0.99 && g_rightTwoHanded &&
                     g_twoHandGrabbed && g_twoHandW >= 0.99 && g_forearmItem[ARM_L] == 0 && haveMap && g_haveLastTarget[ARM_L] &&
                     g_haveLastTarget[ARM_R] && !g_swingActive.load(std::memory_order_relaxed);
        double drop2 = 1e9;
        if (twoOk) {
            double dl = (g_lastHandPos[ARM_L][1] - map.cam[1]) / upm, dr = (g_lastHandPos[ARM_R][1] - map.cam[1]) / upm;
            drop2 = (dl < dr ? dl : dr) - HeadTrackEyeLiftM();
        }
        g_vr2hBlockUp = twoOk && drop2 <= (g_vr2hBlockUp ? kShieldLowerDropM : kShieldRaisedMaxDropM);
    }
}

// Una linea por minuto (solo si hubo algo) con lo esencial para un informe de
// fallo: modo de combate y lo que se hizo con los mandos.
void LogMinuteSummary(double now) {
    if (g_swingStarts > 0 || g_punches > 0 || g_attacksPlayer > 0 || g_vrShieldStops > 0 || g_vr2hStops > 0 || g_bowShots > 0 ||
        g_energy.denied > 0) {
        const int m = g_combatMode.load();
        char line[400];
        std::snprintf(line, sizeof(line),
                      "[COMBATE] ultimo minuto: modo %d (%s) | tajos con el mando %lld (dieron %lld) | punetazos %lld (dieron %lld) | "
                      "ataques del juego (A) %lld | golpes parados: escudo %lld, arma a dos manos %lld | flechas disparadas %lld | "
                      "ataques sin resistencia %lld",
                      m, CombatModeName(m), g_swingStarts, g_swingHits, g_punches, g_punchHits, g_attacksPlayer, g_vrShieldStops,
                      g_vr2hStops, g_bowShots, g_energy.denied);
        LogLine(line);
    }
    g_swingStarts = g_swingHits = g_punches = g_punchHits = g_attacksPlayer = 0;
    g_vrShieldStops = g_vr2hStops = g_bowShots = 0;
    g_energy.denied = 0;
    g_energy.lines = 0;
    g_directLines = 0;
    g_minuteStart = now;
}

// --- tajo con el arma (ataque basico por gesto) -------------------------------------
bool CallMessage(uintptr_t fn, uintptr_t ent, int msg) {
    __try {
        reinterpret_cast<unsigned long long(__fastcall*)(uintptr_t, int, unsigned long long, unsigned long long)>(fn)(ent, msg, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// MESSAGE_START_WEAPON / STOP_WEAPON al arma por su MessageEvent (vtable[50]),
// como los eventos Start_Weapon / Stop_Weapon de las animaciones.
bool SendWeaponMessage(uintptr_t weapon, int msg) {
    uintptr_t vt = 0, fn = 0;
    if (!IsWeapon(weapon) || !RdPtr(weapon, &vt) || !RdPtr(vt + kEntVtMessage, &fn) || !fn) return false;
    return CallMessage(fn, weapon, msg);
}

void EndSwing(double now) {
    if (!g_swing.active) return;
    int state = 0, hits = 0;
    RdI32(g_swing.weapon + kWState, &state);
    RdI32(g_swing.weapon + kWHitCount, &hits);
    if (g_swing.energyDenied) hits = 0;   // ronda 59: no se encendio (la cuenta seria la del tajo anterior)
    if (g_swing.owned && state != 0) SendWeaponMessage(g_swing.weapon, kMsgStopWeapon);
    if (hits > 0 && hits <= 32) ++g_swingHits;
    g_swing.active = false;
    g_swing.owned = false;
    g_swing.cooldownUntil = now + kSwingCooldownSec;
    // Ronda 34: si toco a alguien, el arma no vuelve a encenderse hasta que
    // pase lo que dura el ataque basico del juego con ella. Ronda 59: tras el
    // ataque torpe, lo que dura el suyo.
    if (hits > 0 && hits <= 32 && g_cadW.sec > 0.0 && !g_swing.energyDenied) {
        double t = g_swing.firstHitAt >= 0.0 ? g_swing.firstHitAt : g_swing.t0;
        double cad = g_cadW.sec;
        if (g_swing.clumsySec + kCadenceAfterEndSec > cad && g_swing.clumsySec > 0.0) cad = g_swing.clumsySec + kCadenceAfterEndSec;
        g_nextWeaponDamageAt = t + cad;
    }
    g_swing.energyDenied = false;
    g_swing.clumsySec = 0.0;
    g_swingActive.store(false, std::memory_order_relaxed);
}

// Punto del mundo del juego -> espacio de seguimiento (metros, ejes del juego),
// deshaciendo ControllerToGame con el mismo mapa con que se coloco la mano.
void GameToTracking(const TrackingToGameMap& m, const double p[3], double q[3]) {
    double upm = m.unitsPerMeter > 1e-6 ? m.unitsPerMeter : 1000.0;
    double rx = (p[0] - m.cam[0]) / upm, ry = (p[1] - m.cam[1]) / upm, rz = (p[2] - m.cam[2]) / upm;
    q[0] = m.hmd[0] + rx * m.cosYaw - rz * m.sinYaw;
    q[1] = m.hmd[1] + ry;
    q[2] = m.hmd[2] + rx * m.sinYaw + rz * m.cosYaw;
}

// Velocidad (m/s) del extremo mas rapido de las areas del arma con el MANDO
// REAL: los extremos se llevan al espacio de seguimiento con el mapa del
// fotograma (el mismo con que se coloco la mano). Asi no cuentan ni el
// movimiento del personaje ni los saltos de la camara del juego (al chocar con
// una pared se recorta su desplazamiento y la mano saltaba con ella).
double g_swPrev[kMaxAreas * 2][3];
bool g_swHavePrev = false;
uintptr_t g_swPrevWeapon = 0;
double g_swPrevTime = 0.0;
double SwingSpeed(uintptr_t weapon, double now) {
    double m[16];
    if (!weapon || !g_ikMapValid || !g_trkPlayer.haveGeom || weapon != g_trkPlayer.weapon || !WeaponWorld(weapon, m)) {
        g_swHavePrev = false;
        return 0.0;
    }
    double pts[kMaxAreas * 2][3];
    int np = 0;
    for (int k = 0; k < g_trkPlayer.geom.n; ++k) {
        double w[3];
        XformPoint(m, g_trkPlayer.geom.a[k], w);
        GameToTracking(g_ikMap, w, pts[np++]);
        XformPoint(m, g_trkPlayer.geom.b[k], w);
        GameToTracking(g_ikMap, w, pts[np++]);
    }
    double dt = now - g_swPrevTime, vmax = 0.0;
    if (g_swHavePrev && weapon == g_swPrevWeapon && dt > 0.002 && dt < 0.25) {
        for (int i = 0; i < np; ++i) {
            double v = Dist3(pts[i], g_swPrev[i]) / dt;
            if (v > vmax) vmax = v;
        }
    }
    for (int i = 0; i < np; ++i) for (int j = 0; j < 3; ++j) g_swPrev[i][j] = pts[i][j];
    g_swHavePrev = true;
    g_swPrevWeapon = weapon;
    g_swPrevTime = now;
    return vmax;
}

// --- ronda 34: mano hacia el cuerpo y cadencia del juego ---------------------------
// Hilo del juego, una vez por fotograma antes de UpdateSwing y UpdatePunch:
// velocidad de cada mando real y cuanto de ella va hacia el pecho.
void UpdateHandMotion(double now) {
    float hm[12];
    bool haveHmd = GetHmdPoseMatrix34(hm);
    double chest[3] = { haveHmd ? hm[3] : 0.0, haveHmd ? hm[7] - kChestBelowEyesM : 0.0, haveHmd ? hm[11] : 0.0 };
    double dt = now - g_handMotT;
    g_handMotT = now;
    for (int s = 0; s < 2; ++s) {
        HandMot& m = g_handMot[s];
        VrHandState hs;
        if (!GetVrHandState(s, &hs) || !hs.valid) {
            m.valid = false;
            g_handHavePrev[s] = false;
            continue;
        }
        double p[3] = { hs.pose[3], hs.pose[7], hs.pose[11] };
        m.valid = false;
        if (g_handHavePrev[s] && dt > 0.002 && dt < 0.25) {
            double v[3] = { (p[0] - g_handPrev[s][0]) / dt, (p[1] - g_handPrev[s][1]) / dt, (p[2] - g_handPrev[s][2]) / dt };
            m.speed = Len3(v);
            m.toward = 0.0;
            m.towardNeck = 0.0;
            m.up = m.speed > 1e-3 ? v[1] / m.speed : 0.0;
            if (haveHmd && m.speed > 1e-3) {
                // En horizontal (y arriba en OpenVR): un tajo de arriba abajo
                // baja hacia el pecho y no es recoger el brazo.
                double rx = p[0] - chest[0], rz = p[2] - chest[2];
                double rl = std::sqrt(rx * rx + rz * rz);
                if (rl > 0.10) m.toward = -((v[0] * rx + v[2] * rz) / rl) / m.speed;
                // Ronda 42: hacia el cuello en 3D.
                double n[3] = { p[0] - hm[3], p[1] - (hm[7] - kNeckBelowEyesM), p[2] - hm[11] };
                double nl = Len3(n);
                if (nl > 0.12) m.towardNeck = -((v[0] * n[0] + v[1] * n[1] + v[2] * n[2]) / nl) / m.speed;
            }
            m.valid = true;
        }
        for (int k = 0; k < 3; ++k) g_handPrev[s][k] = p[k];
        g_handHavePrev[s] = true;
    }
}

bool HandRetracting(int s) {
    const HandMot& m = g_handMot[s];
    if (!m.valid || m.speed <= kRetractMinMs) return false;
    if (m.toward > kRetractRatio) return true;
    // Ronda 42: subir el brazo hacia uno mismo.
    return m.towardNeck > kRetractNeckRatio && m.up > kRetractUpRatio && m.toward > kRetractNeckHoriz;
}

bool CheckPrologue(uintptr_t addr, const unsigned char* bytes, size_t n);
int AttackEnergy(const char* anim, const char* what, double* clumsySec);   // ronda 59

bool CallAnimDuration(uintptr_t fn, const char* name, double* out) {
    // mov [rsp+8],rbx; mov [rsp+0x10],rsi; push rdi; sub rsp,0x30
    static const unsigned char kPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30 };
    static int s_ok = -1;
    if (s_ok < 0) s_ok = CheckPrologue(fn, kPrologue, sizeof(kPrologue)) ? 1 : 0;
    if (!s_ok) return false;
    int ok = 0;
    __try {
        ok = reinterpret_cast<int(__fastcall*)(const char*, double*)>(fn)(name, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok != 0 && *out == *out && *out > 0.05 && *out < 10.0;
}

// Duracion del ataque basico del juego del personaje con esa arma (0 = punos).
// Se recalcula al cambiar de arma o de personaje.
// Ronda 42: personaje de la animacion: prefijo ("Kgt_g_08_new") o sufijo
// ("Wlk_1h_Kgt"). Hasta la ronda 41 solo el prefijo, y si el jugador no hacia
// ninguna animacion con prefijo con el brazo derecho siguiendo al mando (las de
// andar y estar llevan sufijo; los ataques con A se hacen con el brazo de la
// animacion) la cadencia no se sabia nunca: en la prueba del 12-10 los tajos
// no tuvieron limite en toda la partida.
bool CadenceChrFromAnim(const char* full, char* out) {
    out[0] = 0;
    size_t n = std::strlen(full);
    if (n >= 4 && full[3] == '_') {
        std::memcpy(out, full, 3);
        out[3] = 0;
        if (KnownCadenceChr(out)) return true;
    }
    const char* u = std::strrchr(full, '_');
    if (u && std::strlen(u + 1) == 3) {
        std::memcpy(out, u + 1, 4);
        if (KnownCadenceChr(out)) return true;
    }
    out[0] = 0;
    return false;
}

// Ronda 42: o por el modelo del jugador (Knight_N, Barbarian_N, Amazon_N, Dwarf_N).
bool CadenceChrFromKind(uintptr_t player, char* out) {
    char kind[48];
    out[0] = 0;
    if (!EntityKind(player, kind, sizeof(kind))) return false;
    static const char* const kPre[] = { "Knight", "Barbarian", "Amazon", "Dwarf" };
    static const char* const kChr[] = { "Kgt", "Bar", "Amz", "Dwf" };
    for (int i = 0; i < 4; ++i) {
        if (SameNoCaseN(kind, kPre[i], static_cast<int>(std::strlen(kPre[i])))) {
            std::snprintf(out, 8, "%s", kChr[i]);
            return true;
        }
    }
    return false;
}

void UpdateCadence(Cadence& c, uintptr_t player, uintptr_t weapon) {
    char anim[48], fullName[48];
    ReadAnimNames(player, anim, sizeof(anim), fullName, sizeof(fullName));
    char chr[8] = "";
    if (!CadenceChrFromAnim(fullName, chr) && !CadenceChrFromKind(player, chr)) {
        // Ronda 35: Rlx_* y otras comunes no dicen el personaje: el ultimo bueno.
        if (c.chr[0]) {
            std::snprintf(chr, sizeof(chr), "%s", c.chr);
        } else {
            // Ronda 42: sin saberlo, una cadencia por defecto (una vez por arma).
            if (c.weapon == weapon && c.sec > 0.0) return;
            c.weapon = weapon;
            c.sec = kCadenceDefaultSec;
            c.animSec = 0.0;
            c.anim[0] = 0;
            return;
        }
    }
    if (c.weapon == weapon && std::strcmp(chr, c.chr) == 0 && c.anim[0]) return;
    const char* cls = "NO";
    char kind[48] = "";
    if (weapon) {
        cls = "1H";
        if (EntityKind(weapon, kind, sizeof(kind))) {
            for (const char* k : k2WKinds) if (SameNoCase(kind, k)) cls = "2W";
            for (const char* k : kAxeKinds) if (SameNoCase(kind, k)) cls = "AXE";
            for (const char* k : kSpKinds) if (SameNoCase(kind, k)) cls = "SP";
        }
    }
    const char* animName = nullptr;
    const char* fallback = nullptr;
    for (const GiEntry& e : kGiAnims) {
        if (std::strcmp(e.chr, chr) != 0) continue;
        if (std::strcmp(e.cls, cls) == 0) animName = e.anim;
        if (!fallback && std::strcmp(e.cls, "NO") != 0 && weapon) fallback = e.anim;
    }
    if (!animName) animName = fallback;
    c.weapon = weapon;
    std::snprintf(c.chr, sizeof(c.chr), "%s", chr);
    c.sec = 0.0;
    c.animSec = 0.0;
    std::snprintf(c.anim, sizeof(c.anim), "%s", animName ? animName : "-");
    double d = 0.0;
    if (animName && CallAnimDuration(ModuleBase() + kFnAnimDuration, animName, &d)) {
        c.animSec = d;
        c.sec = weapon ? d + kCadenceAfterEndSec : d * kCadenceFactor;   // ronda 43: con arma, al acabar la animacion
    }
    if (c.sec <= 0.0) c.sec = kCadenceDefaultSec;   // ronda 42: nunca sin limite
    {
        double learned = animName ? CadenceLearnedFor(animName) : -1.0;   // ronda 43
        if (learned > 0.0) c.sec = learned;
    }
}

// Hilo del juego, cada fotograma (tras colocar el arma en la mano y medir su
// velocidad en UpdateTrack). Solo con el brazo derecho siguiendo al mando y
// fuera de los ataques del propio juego (boton A), que llevan su deteccion.
void UpdateSwing(uintptr_t player, double now, bool fp) {
    uintptr_t weapon = g_trkPlayer.weapon;
    char anim[48];
    ReadAnimNames(player, anim, sizeof(anim), nullptr, 0);
    int life = 0;
    bool ready = fp && g_ikWeight[ARM_R] >= 0.99 && weapon && IsWeapon(weapon) && g_trkPlayer.haveGeom &&
                 RdI32(player + kPLife, &life) && life > 0;
    ready = ready && !g_bow.equipped;   // ronda 50: con el arco, la flecha de la derecha no da tajos
    g_weaponGestureReady = ready || g_bow.equipped;   // ronda 23: modo 1, A tapado (ronda 50: tambien con el arco VR)
    bool allowed = ready && !IsAttackAnimName(anim);
    double speed = SwingSpeed(weapon, now);
    if (ready) UpdateCadence(g_cadW, player, weapon);   // ronda 34
    if (!allowed || (g_swing.active && weapon != g_swing.weapon)) {
        g_swing.fastFrames = 0;
        EndSwing(now);
        return;
    }
    bool retract = HandRetracting(ARM_R);   // ronda 34
    if (!g_swing.active) {
        bool handMoves = g_handMot[ARM_R].valid && g_handMot[ARM_R].speed >= kSwingHandMinMs;   // ronda 72
        g_swing.fastFrames = (speed >= kSwingOnMs && handMoves && !retract) ? g_swing.fastFrames + 1 : 0;
        if (g_swing.fastFrames < kSwingOnFrames || now < g_swing.cooldownUntil) return;
        if (now < g_nextWeaponDamageAt) return;   // ronda 34: el arma aun no puede volver a dar (cadencia del juego)
        g_swing.fastFrames = 0;
        g_swing.firstHitAt = -1.0;
        g_swing.retractFrames = 0;
        int state = 0;
        RdI32(weapon + kWState, &state);
        g_swing.active = true;
        g_swing.weapon = weapon;
        g_swing.t0 = now;
        g_swing.slowFrames = 0;
        // Ronda 59: la resistencia del juego (coste del ataque basico con esta arma).
        int ecode = AttackEnergy(g_cadW.anim, "tajo", &g_swing.clumsySec);
        g_swing.energyDenied = ecode == 2 || ecode == 3;
        // Si el arma ya esta encendida (no deberia), no se toca.
        g_swing.owned = !g_swing.energyDenied && state == 0 && SendWeaponMessage(weapon, kMsgStartWeapon);
        if (!g_swing.energyDenied) {
            g_swingT0.store(now, std::memory_order_relaxed);
            g_swingActive.store(true, std::memory_order_relaxed);
            ++g_swingStarts;
        }
        return;
    }
    {
        int hits = 0;
        if (g_swing.firstHitAt < 0.0 && !g_swing.energyDenied && RdI32(weapon + kWHitCount, &hits) && hits > 0 && hits <= 32) g_swing.firstHitAt = now;
    }
    g_swing.retractFrames = retract ? g_swing.retractFrames + 1 : 0;
    g_swing.slowFrames = speed < kSwingOffMs ? g_swing.slowFrames + 1 : 0;
    if (g_swing.retractFrames >= kRetractFrames || g_swing.slowFrames >= 3 || now - g_swing.t0 > kSwingMaxSec) EndSwing(now);
}

// --- ronda 31: punetazos con las manos libres (armas naturales) ----------------------
bool SendPersonMessage(uintptr_t person, int msg) {
    uintptr_t vt = 0, fn = 0;
    if (!IsPerson(person) || !RdPtr(person, &vt) || !RdPtr(vt + kEntVtMessage, &fn) || !fn) return false;
    return CallMessage(fn, person, msg);
}

void EndPunch(uintptr_t player, double now) {
    if (!g_punch.active) return;
    int state = 0;
    if (player) RdI32(player + kPNaturalState, &state);
    if (g_punch.owned && player && state != 0) SendPersonMessage(player, kMsgStopWeapon);
    if (g_punch.hitsSeen > 0 && !g_punch.energyDenied) ++g_punchHits;
    g_punch.active = false;
    g_punch.owned = false;
    g_punch.cooldownUntil = now + kPunchCooldownSec;
    // Ronda 34: cadencia del puno del juego tras un golpe que toca (ronda 59:
    // tras el torpe, lo que dura el suyo).
    if (g_punch.hitsSeen > 0 && g_cadP.sec > 0.0 && !g_punch.energyDenied) {
        double t = g_punch.firstHitAt >= 0.0 ? g_punch.firstHitAt : g_punch.t0;
        double cad = g_cadP.sec;
        if (g_punch.clumsySec > 0.0 && g_punch.clumsySec + kCadenceAfterEndSec > cad) cad = g_punch.clumsySec + kCadenceAfterEndSec;
        g_nextPunchDamageAt = t + cad;
    }
    g_punch.energyDenied = false;
    g_punch.clumsySec = 0.0;
}

// Hilo del juego, cada fotograma (tras UpdateSwing). Con las manos libres, un
// golpe rapido de un mando (en cualquier direccion, como el tajo) enciende las
// armas naturales mientras dura; sin animacion, sin combos y sin A (en el modo
// 1 A sigue tapada: con las armas a la espalda el ataque del juego las saca).
void UpdatePunch(uintptr_t player, double now, bool fp) {
    char anim[48];
    ReadAnimNames(player, anim, sizeof(anim), nullptr, 0);
    int life = 0, n1 = 0, n2 = 0;
    bool natural = (RdI32(player + kPNaturalCount, &n1) && n1 > 0 && n1 <= 64) ||
                   (RdI32(player + kPNaturalCount2, &n2) && n2 > 0 && n2 <= 64);
    bool arms = g_ikWeight[ARM_L] >= 0.99 || g_ikWeight[ARM_R] >= 0.99;
    bool ready = fp && natural && arms && g_unarmedPub.load(std::memory_order_relaxed) &&
                 g_combatMode.load(std::memory_order_relaxed) != 0 && RdI32(player + kPLife, &life) && life > 0;
    g_gestureReadyPub.store(g_weaponGestureReady || ready, std::memory_order_relaxed);   // modo 1: A tapado
    bool allowed = ready && !IsAttackAnimName(anim);
    if (ready) UpdateCadence(g_cadP, player, 0);   // ronda 34
    // Velocidad de cada mando real (espacio de seguimiento; UpdateHandMotion).
    double sp[2] = { g_handMot[0].valid ? g_handMot[0].speed : 0.0, g_handMot[1].valid ? g_handMot[1].speed : 0.0 };
    bool retract[2] = { HandRetracting(ARM_L), HandRetracting(ARM_R) };   // ronda 34
    if (!allowed) {
        g_punch.fastFrames[0] = g_punch.fastFrames[1] = 0;
        EndPunch(player, now);
        return;
    }
    if (!g_punch.active) {
        int best = -1;
        for (int s = 0; s < 2; ++s) {
            bool fast = g_ikWeight[s] >= 0.99 && sp[s] >= kPunchOnMs && !retract[s];
            g_punch.fastFrames[s] = fast ? g_punch.fastFrames[s] + 1 : 0;
            if (g_punch.fastFrames[s] >= kPunchOnFrames && (best < 0 || sp[s] > sp[best])) best = s;
        }
        if (best < 0 || now < g_punch.cooldownUntil) return;
        if (now < g_nextPunchDamageAt) return;   // ronda 34: el puno aun no puede volver a dar (cadencia del juego)
        g_punch.fastFrames[0] = g_punch.fastFrames[1] = 0;
        g_punch.firstHitAt = -1.0;
        g_punch.retractFrames = 0;
        int state = 0;
        RdI32(player + kPNaturalState, &state);
        g_punch.active = true;
        g_punch.hand = best;
        g_punch.t0 = now;
        g_punch.slowFrames = 0;
        g_punch.hitsSeen = 0;
        // Ronda 59: la resistencia del juego (coste del punetazo basico).
        int ecode = AttackEnergy(g_cadP.anim, "punetazo", &g_punch.clumsySec);
        g_punch.energyDenied = ecode == 2 || ecode == 3;
        // Si ya estaban encendidas (un golpe del propio juego), no se tocan.
        g_punch.owned = !g_punch.energyDenied && state == 0 && SendPersonMessage(player, kMsgStartWeapon);
        if (g_punch.energyDenied) {
            int h = 0;   // la cuenta de golpeados sigue la del anterior: que no cuente como golpe
            if (RdI32(player + kPNaturalHits, &h) && h > 0 && h <= 32) g_punch.hitsSeen = h;
        }
        ++g_punches;
        return;
    }
    double v = sp[g_punch.hand];
    int hits = 0;
    if (RdI32(player + kPNaturalHits, &hits) && hits > g_punch.hitsSeen && hits <= 32) {
        if (g_punch.hitsSeen == 0) g_punch.firstHitAt = now;
        g_punch.hitsSeen = hits;
        g_hapticHit.store(1 + g_punch.hand, std::memory_order_relaxed);
    }
    g_punch.retractFrames = retract[g_punch.hand] ? g_punch.retractFrames + 1 : 0;
    g_punch.slowFrames = v < kPunchOffMs ? g_punch.slowFrames + 1 : 0;
    if (g_punch.retractFrames >= kRetractFrames || g_punch.slowFrames >= 3 || now - g_punch.t0 > kPunchMaxSec) EndPunch(player, now);
}

// --- el personaje que anda solo -------------------------------------------------
bool IsWalkAnimName(const char* a) {
    if (!a) return false;
    static const char* const kPrefix[] = { "WLK", "JOG", "RUN", "WBK" };
    for (const char* p : kPrefix) {
        if (std::strncmp(a, p, std::strlen(p)) == 0) return true;
    }
    static const char* const kExact[] = { "Attack_f", "Attack_b", "Attack_l", "Attack_r", "Shattack_f", "Shattack_b",
                                          "Shattack_l", "Shattack_r", "Attack_f_s_nc", "Attack_b_s_nc" };
    for (const char* p : kExact) {
        if (std::strcmp(a, p) == 0) return true;
    }
    return false;
}

// Ronda 38: acciones propias de andar, estar o pelear (sin distinguir
// mayusculas). Las demas (palancas lvr_*, coger tke_*, empujar, comer,
// escenas...) las lleva un script del juego: ni la parada del mod ni Supr
// tocan al personaje mientras duran. En el log de la ronda 37, con la palanca
// del elevador (lvr_d) la parada quito el giro hacia la palanca (Tl) y Supr
// mando 74-79 giros; murio al empezar a bajar.
bool IsFreeActionName(const char* a) {
    if (!a || !a[0]) return true;
    char low[48];
    int i = 0;
    for (; a[i] && i < 47; ++i) low[i] = static_cast<char>((a[i] >= 'A' && a[i] <= 'Z') ? a[i] - 'A' + 'a' : a[i]);
    low[i] = 0;
    static const char* const kPrefix[] = { "rlx", "wlk", "wbk", "jog", "run", "snk", "attack_", "shattack", "chg", "hurt",
                                           "df_", "sw_react", "d_", "jmp", "fll", "g_", "g2h_", "1tw", "clmb" };
    for (const char* p : kPrefix) {
        if (std::strncmp(low, p, std::strlen(p)) == 0) return true;
    }
    return false;
}
std::atomic<bool> g_scriptedActionPub{false};   // ronda 38: accion de script en curso (Supr no gira)

// Escalar, caer o saltar (clmb_*, Fll*, jmp*, ShortJump...), sin distinguir mayusculas.
bool IsAirAnimName(const char* a) {
    if (!a || !a[0]) return false;
    char low[48];
    int i = 0;
    for (; a[i] && i < 47; ++i) low[i] = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
    low[i] = 0;
    return std::strncmp(low, "clmb", 4) == 0 || std::strncmp(low, "fll", 3) == 0 || std::strncmp(low, "jmp", 3) == 0 ||
           std::strstr(low, "jump") != nullptr;
}

bool MovementKeysDown() {
    static const int kKeys[] = { 'W', 'A', 'S', 'D', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    for (int k : kKeys) {
        if (GetAsyncKeyState(k) & 0x8000) return true;
    }
    return false;
}

bool CallPadKey(uintptr_t fn, long long dev, int pressed, const char* key, unsigned t) {
    __try {
        reinterpret_cast<void(__fastcall*)(long long, int, const char*, unsigned)>(fn)(dev, pressed, key, t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool CallPersonPos(uintptr_t fn, uintptr_t posState, double out[3]) {
    __try {
        reinterpret_cast<void*(__fastcall*)(uintptr_t, double*)>(fn)(posState, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return out[0] == out[0] && out[2] == out[2];
}

// Python del juego (hilo del juego, fuera de cualquier script en marcha).
bool RunPython(const char* code) {
    int r = -1;
    __try {
        r = reinterpret_cast<int(__fastcall*)(const char*)>(ModuleBase() + kFnPyRunSimple)(code);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return r == 0;
}

// --- ronda 59: evaluar una expresion de Python y leer su valor --------------------------
// Valor numerico de un objeto de Python (int o float), sin tocar su cuenta.
bool PyNumberValue(uintptr_t base, uintptr_t o, double* x) {
    if (!o) return false;
    uintptr_t t = *reinterpret_cast<const uintptr_t*>(o + 8);
    if (t == base + kPyFloatType) {
        *x = *reinterpret_cast<const double*>(o + 0x10);
        return true;
    }
    if (t == base + kPyIntType) {
        *x = static_cast<double>(*reinterpret_cast<const int*>(o + 0x10));
        return true;
    }
    return false;
}

// Py_DECREF: si llega a 0, el tp_dealloc de su tipo.
void PyDecRef(uintptr_t o) {
    int* rc = reinterpret_cast<int*>(o);
    if (--*rc == 0) {
        uintptr_t t = *reinterpret_cast<const uintptr_t*>(o + 8);
        uintptr_t dealloc = *reinterpret_cast<const uintptr_t*>(t + 0x28);
        if (dealloc) reinterpret_cast<void(__fastcall*)(uintptr_t)>(dealloc)(o);
    }
}

// Como PyRun_SimpleString pero con Py_eval_input y leyendo el resultado: hasta n
// numeros (un int o float, o una tupla de ellos). -1 si fallo (error de Python
// limpiado con PyErr_Clear). Sin objetos con destructor (va bajo __try).
int PyEvalNumbersRaw(const char* expr, double* out, int n) {
    const uintptr_t b = ModuleBase();
    uintptr_t m = reinterpret_cast<uintptr_t(__fastcall*)(const char*)>(b + kFnPyImportAddModule)("__main__");
    if (!m) {
        reinterpret_cast<void(__fastcall*)()>(b + kFnPyErrClear)();
        return -1;
    }
    uintptr_t d = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t)>(b + kFnPyModuleGetDict)(m);
    if (!d) {
        reinterpret_cast<void(__fastcall*)()>(b + kFnPyErrClear)();
        return -1;
    }
    uintptr_t v = reinterpret_cast<uintptr_t(__fastcall*)(const char*, int, uintptr_t, uintptr_t)>(b + kFnPyRunString)(expr, kPyEvalInput, d, d);
    if (!v) {
        reinterpret_cast<void(__fastcall*)()>(b + kFnPyErrClear)();
        return -1;
    }
    int got = 0;
    if (*reinterpret_cast<const uintptr_t*>(v + 8) == b + kPyTupleType) {
        int size = *reinterpret_cast<const int*>(v + 0x10);
        for (int i = 0; i < size && got < n; ++i) {
            uintptr_t it = *reinterpret_cast<const uintptr_t*>(v + 0x18 + 8 * static_cast<uintptr_t>(i));
            if (!PyNumberValue(b, it, &out[got])) break;
            ++got;
        }
    } else if (n > 0 && PyNumberValue(b, v, &out[0])) {
        got = 1;
    }
    PyDecRef(v);
    return got;
}

bool PyTypeNamed(uintptr_t type, const char* name) {
    uintptr_t p = 0;
    if (!RdPtr(type + 0x18, &p) || !p) return false;
    char buf[16] = {};
    __try {
        for (int i = 0; i < 15; ++i) {
            buf[i] = reinterpret_cast<const char*>(p)[i];
            if (!buf[i]) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return std::strcmp(buf, name) == 0;
}

// Comprobado una vez: prologos de las cuatro funciones y nombres de los tipos.
bool PyEvalCheck() {
    static const unsigned char kAddModule[] = { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9 };
    static const unsigned char kGetDict[] = { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x05 };
    static const unsigned char kRunString[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x49, 0x8b, 0xd9, 0x49, 0x8b, 0xf8 };
    static const unsigned char kErrClear[] = { 0x45, 0x33, 0xc0, 0x33, 0xd2, 0x33, 0xc9, 0xe9 };
    const uintptr_t b = ModuleBase();
    return CheckPrologue(b + kFnPyImportAddModule, kAddModule, sizeof(kAddModule)) &&
           CheckPrologue(b + kFnPyModuleGetDict, kGetDict, sizeof(kGetDict)) &&
           CheckPrologue(b + kFnPyRunString, kRunString, sizeof(kRunString)) &&
           CheckPrologue(b + kFnPyErrClear, kErrClear, sizeof(kErrClear)) &&
           PyTypeNamed(b + kPyIntType, "int") && PyTypeNamed(b + kPyFloatType, "float") && PyTypeNamed(b + kPyTupleType, "tuple");
}

// Hilo del juego (fuera de la ejecucion de Python, como RunPython).
int PyEvalNumbers(const char* expr, double* out, int n) {
    if (g_energy.evalState < 0) {
        g_energy.evalState = PyEvalCheck() ? 1 : 0;
        if (!g_energy.evalState)
            LogLine("[RESISTENCIA] AVISO: las funciones de Python de Blade.exe no son las esperadas; los tajos con el mando no gastan resistencia.");
    }
    if (g_energy.evalState != 1) return -1;
    int got = -1;
    __try {
        got = PyEvalNumbersRaw(expr, out, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        got = -1;
    }
    return got;
}

// Ronda 59: resistencia de un ataque con el mando (tajo o punetazo) con el
// ataque basico "anim" del personaje (Kgt_g_08_new...). Devuelve el codigo de
// BladeVR_Fatiga: 0 normal, 1 torpe (*clumsySec = lo que dura el suyo), 2 el arma
// pide tanto como el maximo, 3 sin energia; -1 sin datos (el ataque sale, como
// hasta la ronda 58).
int AttackEnergy(const char* anim, const char* what, double* clumsySec) {
    *clumsySec = 0.0;
    if (!g_energyOn.load(std::memory_order_relaxed)) return -1;   // ronda 60: quitada con Mayus + F8
    if (g_energy.pyState != 1) return -1;
    char safe[48];
    int k = 0;
    for (const char* p = anim ? anim : ""; *p && k < static_cast<int>(sizeof(safe)) - 1; ++p) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') safe[k++] = c;
    }
    safe[k] = 0;
    if (!k) std::snprintf(safe, sizeof(safe), "%s", "-");
    char expr[96];
    std::snprintf(expr, sizeof(expr), "BladeVR_Fatiga('%s')", safe);
    double r[5] = {};
    int n = PyEvalNumbers(expr, r, 5);
    if (n < 5) {
        if (g_energy.lines++ < kMaxEnergyLinesPerMin) {
            char line[200];
            std::snprintf(line, sizeof(line), "[RESISTENCIA] ERROR al calcularla para %s (%s): el ataque sale sin gastarla.", what, safe);
            LogLine(line);
        }
        return -1;
    }
    int code = static_cast<int>(r[0]);
    if (code < 0) return -1;
    if (code == 1) *clumsySec = r[4] > 0.0 ? r[4] : 0.3;
    else if (code == 2 || code == 3) ++g_energy.denied;
    return code;
}

// Hilo del juego, cada fotograma de partida: instala kPyEnergy (al empezar cada nivel).
void UpdateEnergyInstall(uintptr_t app, double now) {
    unsigned char inGame = 0;
    if (g_energy.pyState != 0 || now < g_energy.pyAt || !RdU8(app + kAppInGame, &inGame) || !inGame) return;
    bool ok = RunPython(kPyEnergy);
    ++g_energy.pyTries;
    if (ok) {
        g_energy.pyState = 1;
        LogLine("[RESISTENCIA] instalada: los tajos y punetazos con el mando gastan la resistencia como el ataque basico del juego.");
    } else if (g_energy.pyTries >= 3) {
        g_energy.pyState = -1;
        LogLine("[RESISTENCIA] ERROR: no se pudo instalar su Python; los tajos con el mando no la gastan.");
    } else {
        g_energy.pyAt = now + 5.0;
    }
}

// Posicion de la persona como la usa el juego (no la del esqueleto).
bool PersonPos(uintptr_t person, double out[3]) {
    uintptr_t holder = 0;
    if (!RdPtr(person + kPPosState + 0x1120, &holder) || !holder) return false;
    return CallPersonPos(ModuleBase() + kFnPersonPos, person + kPPosState, out);
}

// Guinada de la entidad (hacia donde anda), como la del juego: atan2(x, z) de
// su adelante (= -eje y de la entidad).
bool EntityYaw(uintptr_t person, double* yaw) {
    SkelView sv;
    double e[16];
    if (!GetSkel(person, &sv) || !RdDoubles(sv.skel + kSkelEntity, e, 16)) return false;
    double fx = -e[4], fz = -e[6];
    if (fx * fx + fz * fz < 1e-6) return false;
    *yaw = std::atan2(fx, fz);
    return true;
}

// Hilo del juego, cada fotograma: si el personaje se mueve sin joystick.
void UpdateWalkWatch(uintptr_t player, double now) {
    double p[3];
    bool havePos = PersonPos(player, p);
    double upm = g_ikMapValid && g_ikMap.unitsPerMeter > 1e-6 ? g_ikMap.unitsPerMeter : 1000.0;
    if (havePos) {
        double dt = now - g_walkPrevT;
        if (g_walkHavePrev && dt > 0.002 && dt < 0.25) {
            double dx = p[0] - g_walkPrevPos[0], dz = p[2] - g_walkPrevPos[2];
            double v = std::sqrt(dx * dx + dz * dz) / upm / dt;
            double a = 1.0 - std::exp(-dt / 0.15);
            if (v > 20.0) g_walkSpeed = 0.0;   // salto (carga, teletransporte)
            else g_walkSpeed += (v - g_walkSpeed) * a;
        }
        for (int k = 0; k < 3; ++k) g_walkPrevPos[k] = p[k];
        g_walkPrevT = now;
        g_walkHavePrev = true;
    } else {
        g_walkHavePrev = false;
        g_walkSpeed = 0.0;
    }
    char anim[48];
    ReadAnimNames(player, anim, sizeof(anim), nullptr, 0);
    // Ronda 38: accion de script en curso (Supr no gira).
    g_scriptedActionPub.store(!IsFreeActionName(anim), std::memory_order_relaxed);
    float sent[2] = { 0, 0 };
    bool quest = VrPadGetLeftStick(sent);
    bool padFresh = now - g_padLastInput < 0.5;
    // Suelto: ningun eje llega al umbral de las teclas del joystick del juego
    // (0,405 de lo enviado; VrPadHook manda 0 en ese caso desde la ronda 15).
    bool questIdle = quest && std::fabs(sent[0]) < kSentKeyMin && std::fabs(sent[1]) < kSentKeyMin;
    if (questIdle && !g_questIdle) g_questIdleSince = now;
    g_questIdle = questIdle;
    double idleFor = questIdle ? now - g_questIdleSince : 0.0;
    bool neutralLong = quest ? (questIdle && idleFor >= kWalkNeutralSec)
                             : (padFresh && g_padNeutral && now - g_padNeutralSince >= kWalkNeutralSec);
    bool walkAnim = IsWalkAnimName(anim);

    bool keys = MovementKeysDown();
    // Ronda 67: otro mando (Xbox) moviendo al personaje cuenta como el teclado.
    bool otherPadMoving = g_movingPad != 0 && !g_movingPadIsQuest && g_movingPadAt >= 0.0 && now - g_movingPadAt < kOtherPadHoldSec;
    if (otherPadMoving) keys = true;
    // Tras escalar, caer o saltar con el joystick en reposo: en cuanto vuelve a
    // andar se le para (el ciclo de JOG ya empezado seguia ~1 s; ronda 9).
    bool air = IsAirAnimName(anim);
    if (air) g_airUntil = now + kAfterAirSec;
    // Ronda 66: de quien es el movimiento (ver g_questDriven).
    if (quest && !questIdle) {
        g_questDriven = true;
    } else if (!quest || keys || (g_walkSpeed < kWalkStopSpeedMs && !walkAnim && !air && now >= g_airUntil)) {
        g_questDriven = false;
    }
    bool otherInput = keys || !g_questDriven;   // otra entrada mueve al personaje (o esta quieto): el mod no lo para
    if (!air && now < g_airUntil && questIdle && idleFor >= 0.1 && walkAnim && !otherInput && now >= g_walkNextStop &&
        g_combatMode.load(std::memory_order_relaxed) != 0) {   // ronda 70: no en el modo 0
        g_airUntil = 0.0;
        g_walkNextStop = now + kWalkStopRetrySec;
        RunPython(kPyStopWalk);
        g_walkStuck = true;
    }

    // Parar: andando o corriendo con el joystick en reposo (lo que lee el juego),
    // o moviendose con Gof/Gob/Tr/Tl puestos sin joystick.
    int gof = 0, gob = 0, tr = 0, tl = 0;
    RdI32(player + kPGof, &gof);
    RdI32(player + kPGob, &gob);
    RdI32(player + kPTr, &tr);
    RdI32(player + kPTl, &tl);
    bool moveFlags = gof != 0 || gob != 0 || tr != 0 || tl != 0;
    // Ronda 40: parada al soltar el joystick (ver kPyStopTest). Mientras el
    // joystick siga suelto y la accion sea de andar, en cuanto el juego quita
    // las banderas de movimiento se le manda StopTest (cada kStopTestEverySec,
    // hasta kStopTestMaxSends veces): el juego pasa a Rlx en ese momento en vez
    // de esperar a su siguiente StopTest de la animacion (hasta medio ciclo).
    {
        bool idleNow = (quest ? (questIdle && g_questDriven) : (padFresh && g_padNeutral)) && !keys &&
                       g_combatMode.load(std::memory_order_relaxed) != 0;   // ronda 70: no en el modo 0
        bool stopAnim = (IsWalkAnimName(anim) || SameNoCaseN(anim, "SNK", 3)) && !SameNoCaseN(anim, "RUN", 3);
        if (idleNow && stopAnim && IsFreeActionName(anim) && !IsAirAnimName(anim)) {
            if (!g_stopTestEp) {
                g_stopTestEp = true;
                g_stopTestSends = 0;
                g_stopTestNext = 0.0;
            }
            int mf = 0;
            if (RdI32(player + kPMoveFlags, &mf) && (static_cast<unsigned>(mf) & kMoveFlagBits) == 0 &&
                g_stopTestSends < kStopTestMaxSends && now >= g_stopTestNext) {
                ++g_stopTestSends;
                g_stopTestNext = now + kStopTestEverySec;
                g_stopTestDispatch = true;   // ronda 41: sin pie apoyado
                RunPython(kPyStopTest);
                g_stopTestDispatch = false;
            }
        } else {
            g_stopTestEp = false;
        }
    }
    const bool xboxLike = g_combatMode.load(std::memory_order_relaxed) == 0;   // ronda 70
    if (questIdle && idleFor >= (xboxLike ? kWalkStopNeutralXboxSec : kWalkStopNeutralSec) && (walkAnim || moveFlags) &&
        IsFreeActionName(anim) &&   // ronda 38
        !IsAttackAnimName(anim) && !IsAirAnimName(anim) && g_walkSpeed > kWalkStopSpeedMs && !otherInput && now >= g_walkNextStop) {
        g_walkNextStop = now + kWalkStopRetrySec;
        RunPython(kPyStopWalk);
        g_walkStuck = true;   // y que el proceso del mando suelte las teclas del joystick
    }

    if (!g_walkEp) {
        if (quest && neutralLong && g_walkSpeed > kWalkSpeedMs && !IsAttackAnimName(anim) && !otherInput) {
            g_walkEp = true;
            g_walkEpT0 = now;
            g_walkEpSlowSince = 0.0;
        }
    } else {
        bool stickUsed = (quest ? !questIdle : (!g_padNeutral || !padFresh)) || MovementKeysDown();
        if (g_walkSpeed < kWalkStopMs) {
            if (g_walkEpSlowSince == 0.0) g_walkEpSlowSince = now;
        } else {
            g_walkEpSlowSince = 0.0;
        }
        if (stickUsed || (g_walkEpSlowSince > 0.0 && now - g_walkEpSlowSince > 0.3) || now - g_walkEpT0 > 30.0) {
            g_walkEp = false;
            g_walkStuck = false;
        }
    }
}

// --- comida y pociones en la mano ---------------------------------------------------
// Hilo del juego, cada fotograma: instala el Python en cada nivel y, con algo
// que no es un arma en la mano derecha (o una pocion en la izquierda, ronda
// 21), mira si ese mando esta junto a la boca (BladeVR_Comer decide si es
// comida o pocion pendiente de esa mano).
bool MandoJuntoALaBoca(int arm, const float hm[12], double now) {
    VrHandState hs;
    if (!GetVrHandState(arm, &hs)) {
        g_mouthSince[arm] = 0.0;
        return false;
    }
    // Espacio de seguimiento (OpenVR: y arriba, z atras): abajo = -col1, adelante = -col2.
    double d2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        double mouth = hm[k * 4 + 3] - kMouthDownM * hm[k * 4 + 1] - kMouthFwdM * hm[k * 4 + 2];
        double d = hs.pose[k * 4 + 3] - mouth;
        d2 += d * d;
    }
    if (std::sqrt(d2) > kMouthRadiusM || now < g_eatNextAt) {
        g_mouthSince[arm] = 0.0;
        return false;
    }
    if (g_mouthSince[arm] == 0.0) g_mouthSince[arm] = now;
    if (now - g_mouthSince[arm] < kMouthHoldSec) return false;
    g_mouthSince[arm] = 0.0;
    return true;
}

void UpdateFood(uintptr_t app, uintptr_t player, double now, bool fp) {
    unsigned char inGame = 0;
    if (!RdU8(app + kAppInGame, &inGame) || !inGame) return;
    if (g_foodInstallState == 0 && now >= g_foodInstallAt) {
        bool ok = RunPython(kPyFood);
        ++g_foodInstallTries;
        if (ok) {
            g_foodInstallState = 1;
            LogLine("[COMIDA] Lista: la comida y las pociones se quedan en la mano y se toman acercando ese mando a la boca.");
        } else if (g_foodInstallTries >= 3) {
            g_foodInstallState = -1;
            LogLine("[COMIDA] ERROR: no se pudo instalar el Python de la comida y las pociones (como en el juego).");
        } else {
            g_foodInstallAt = now + 5.0;
        }
    }
    if (g_foodInstallState != 1) return;
    {
        int want = g_combatMode.load(std::memory_order_relaxed) == 0 ? 0 : 1;
        if (want != g_foodBocaApplied && RunPython(want ? kPyFoodBocaOn : kPyFoodBocaOff)) g_foodBocaApplied = want;
    }
    uintptr_t item[2] = { InventoryItem(player, kInvLeft2), InventoryItem(player, kInvRight) };
    float hm[12];
    bool hmd = fp && GetHmdPoseMatrix34(hm);
    for (int arm = 0; arm < 2; ++arm) {
        if (!hmd || !item[arm] || IsWeapon(item[arm]) || g_ikWeight[arm] < 0.99) {
            g_mouthSince[arm] = 0.0;
            continue;
        }
        if (!MandoJuntoALaBoca(arm, hm, now)) continue;
        g_eatNextAt = now + kEatCooldownSec;
        bool ok = RunPython(arm == ARM_R ? "BladeVR_Comer('R')\n" : "BladeVR_Comer('L')\n");
        uintptr_t after = InventoryItem(player, arm == ARM_R ? kInvRight : kInvLeft2);
        if (ok && after != item[arm]) (arm == ARM_R ? g_hapticRight : g_hapticLeft).store(1, std::memory_order_relaxed);
    }
}

// --- ronda 50: arco VR -----------------------------------------------------------------
bool KindStartsWith(uintptr_t e, const char* prefix, char* kind, int n) {
    return e && EntityKind(e, kind, n) && _strnicmp(kind, prefix, std::strlen(prefix)) == 0;
}

// Giro (sin escala, filas) de una matriz de entidad o de nodo; false si no vale.
bool RotOf(const double m[16], double r[9]) {
    double sc[3], p[3];
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    ik::SplitM4(m, r, sc, p);
    if (sc[0] < 1e-6 || sc[1] < 1e-6 || sc[2] < 1e-6) return false;
    ik::M3Orthonormalize(r);
    return true;
}

void UpdateBow(uintptr_t app, uintptr_t player, double now, bool fp) {
    unsigned char inGame = 0;
    if (!RdU8(app + kAppInGame, &inGame) || !inGame) {
        g_bowZonePub.store(false, std::memory_order_relaxed);
        return;
    }
    if (g_bow.pyState == 0 && now >= g_bow.pyAt) {
        bool ok = RunPython(kPyBow);
        ++g_bow.pyTries;
        if (ok) {
            g_bow.pyState = 1;
        } else if (g_bow.pyTries >= 3) {
            g_bow.pyState = -1;
            LogLine("[ARCO] ERROR: no se pudo instalar el Python del arco VR (el arco va como en el juego).");
        } else {
            g_bow.pyAt = now + 5.0;
        }
    }
    if (g_bow.pyState != 1) {
        g_bowZonePub.store(false, std::memory_order_relaxed);
        return;
    }
    uintptr_t left = InventoryItem(player, kInvLeft), right = InventoryItem(player, kInvRight);
    char lk[48] = "", rk[48] = "";
    const bool bow = KindStartsWith(left, "Arco", lk, sizeof(lk));
    const bool arrow = KindStartsWith(right, "Flecha", rk, sizeof(rk));
    int life = 0;
    const bool alive = RdI32(player + kPLife, &life) && life > 0;
    const bool vr = CombatVRGetCombatMode() == 1 && fp && alive && g_ikWeight[ARM_L] >= 0.99 && g_ikWeight[ARM_R] >= 0.99 &&
                    g_haveLastTarget[ARM_L] && g_haveLastTarget[ARM_R];
    g_bow.equipped = bow && vr;
    g_bowEquippedPub.store(g_bow.equipped, std::memory_order_relaxed);
    // Ronda 52 (pedido del usuario): al sacar el arco el juego pone ya una
    // flecha en la mano derecha (en el mismo fotograma que el arco); con el arco
    // VR se devuelve al carcaj y hay que cogerla de la espalda.
    if (bow && !g_bow.prevBow) g_bow.stripUntil = now + 1.5;
    if (!bow) g_bow.stripUntil = 0.0;
    g_bow.prevBow = bow;
    if (g_bow.stripUntil > 0.0 && now < g_bow.stripUntil && g_bow.equipped && arrow && !g_bow.drawing) {
        g_bow.stripUntil = 0.0;
        RunPython("BladeVR_GuardarFlecha()\n");
    }

    const double upm = g_ikMapValid && g_ikMap.unitsPerMeter > 1e-6 ? g_ikMap.unitsPerMeter : 1000.0;
    // Giro de la flecha respecto a la mano derecha (los dos del fotograma anterior).
    // Tensando se conserva el ultimo bueno (si un fotograma no se puede leer, la
    // mano no vuelve a soltarse de la direccion del arco).
    if (!g_bow.drawing) g_bow.relValid = false;
    if (g_bow.equipped && arrow && g_chain.ok) {
        SkelView sv;
        double hw[16], aw[16], hr[9], ar[9], hrT[9];
        if (GetSkel(player, &sv) && sv.skel == g_chain.skel && NodeWorld(sv, g_chain.arm[ARM_R].node[4], hw, nullptr) &&
            WeaponWorld(right, aw) && RotOf(hw, hr) && RotOf(aw, ar) && Dist3(hw + 12, aw + 12) < 1.5 * upm) {   // la flecha, en la mano
            ik::M3T(hr, hrT);
            ik::M3Mul(ar, hrT, g_bow.rel);
            g_bow.relValid = true;
        }
    }
    const double dist = Dist3(g_lastHandPos[ARM_L], g_lastHandPos[ARM_R]) / upm;
    const bool zone = g_bow.equipped && arrow && dist <= kBowNockM;
    const bool held = VrPadBowTriggerHeld();
    if (!held) g_bow.startTried = false;
    if (!g_bow.drawing) {
        if (held && !g_bow.startTried) {
            g_bow.startTried = true;
            if (g_bow.equipped && arrow) {
                RunPython("BladeVR_Tensar()\n");
                g_bow.drawing = true;
                g_bow.arrow = right;
                g_hapticBow.store(1, std::memory_order_relaxed);
            }
        }
    } else {
        const bool broken = !g_bow.equipped || !arrow || right != g_bow.arrow;   // arco sin los mandos o fuera de la mano, o sin flecha
        if (broken) {
            RunPython("BladeVR_Destensar()\n");
            g_bow.drawing = false;
        } else if (!held) {
            g_bow.drawing = false;
            if (dist < kBowMinDrawM) {
                RunPython("BladeVR_Destensar()\n");   // soltado sin tensar: no dispara
            } else {
                double d[3];
                ik::Sub(g_lastHandPos[ARM_L], g_lastHandPos[ARM_R], d);
                ik::Norm(d);
                double p = (dist - kBowSlackM) / (kBowFullDrawM - kBowSlackM);
                if (p < 0.0) p = 0.0;
                if (p > 1.0) p = 1.0;
                const double speed = kBowSpeed * (kBowMinSpeedFrac + (1.0 - kBowMinSpeedFrac) * p);
                char py[160];
                std::snprintf(py, sizeof(py), "BladeVR_Disparar(%.1f, %.1f, %.1f)\n", d[0] * speed, d[1] * speed, d[2] * speed);
                RunPython(py);
                ++g_bowShots;
                g_hapticBow.store(2, std::memory_order_relaxed);
            }
        }
    }
    g_bowZonePub.store(zone || g_bow.drawing, std::memory_order_relaxed);
    // Ronda 51: la siguiente flecha del carcaj con el agarre derecho en la
    // espalda (VrPadHook); al sacar el arco el juego ya pone una en la mano.
    // Solo con el arco aun en la mano (el gesto pudo llegar en el mismo
    // fotograma en que se fue) y la mano derecha vacia.
    if (g_bowArrowReq.exchange(0, std::memory_order_relaxed) != 0 && g_bow.equipped && !right) {
        g_bow.stripUntil = 0.0;   // ronda 52: esta la coge el jugador
        RunPython("BladeVR_RecargarArco()\n");
    }
}

// --- ronda 10: correr solo con sprint, enemigo fijado, Y y B -----------------------
// Enemigo fijado y vivo (persona+0x2a48).
bool ActiveEnemyAlive(uintptr_t player, uintptr_t* enemyOut) {
    uintptr_t enemy = 0;
    int life = 0;
    bool alive = RdPtr(player + kPActiveEnemy, &enemy) && enemy != 0 && IsPerson(enemy) && RdI32(enemy + kPLife, &life) &&
                 life > 0;
    if (enemyOut) *enemyOut = alive ? enemy : 0;
    return alive;
}

// Por que el juego convertiria ahora un giro de Supr en un paso lateral (0 = no
// lo haria; ver kPStrafeMode). 1-3: las comprobaciones de +0x5ece0 / +0x5e020;
// 4: bloqueando (Rlx_block, donde se vieron los deslizamientos), por si acaso.
int TurnStrafeReason(uintptr_t player) {
    char anim[24];
    bool haveAnim = RdBName(player + kPAnimName, anim, sizeof(anim));
    if (haveAnim && (std::strcmp(anim, "B1") == 0 || std::strcmp(anim, "B2") == 0)) return 1;
    int v = 0;
    if (RdI32(player + kPStrafeMode, &v) && v != 0) return 2;
    v = 0;
    if (RdI32(player + kPStrafeMode2, &v) && v != 0) return 3;
    if (haveAnim && std::strncmp(anim, "Rlx_block", 9) == 0) return 4;
    return 0;
}

// Hilo del juego, cada fotograma: Supr sigue en pausa kStrafeHoldSec despues
// de bloquear (ver CombatVRGetPlayerFacing).
void UpdateStrafeWatch(uintptr_t player) {
    if (TurnStrafeReason(player) != 0) g_strafeHoldUntil = NowSec() + kStrafeHoldSec;
}

// Hilo del juego: fijado "con margen" (sigue fijado kLockHoldSec despues de
// perderlo, para que Supr y el pecho no cambien de modo a cada parpadeo).
bool LockHeld(uintptr_t player, double now) {
    if (ActiveEnemyAlive(player, nullptr)) g_lockSeenAt = now;
    return g_lockSeenAt >= 0.0 && now - g_lockSeenAt < kLockHoldSec;
}

// Hilo del juego: con los mandos de Quest y sin sprint, Run tiene que estar a
// 0 (andar siempre). Si se quedo a 1 (el soltar del joystick llego durante
// una escalada), se quita por Python.
void UpdateRunGuard(uintptr_t player, double now) {
    float sent[2] = { 0, 0 };
    if (!VrPadGetLeftStick(sent) || VrPadSprintActive() || MovementKeysDown() || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) return;
    if (!g_questDriven) return;   // ronda 66: corre con el teclado o con el mando de Xbox
    if (g_combatMode.load(std::memory_order_relaxed) == 0) return;   // ronda 70: en el modo 0, a fondo corre (como el Xbox)
    int run = 0;
    if (!RdI32(player + kPRunFlag, &run) || run == 0 || now < g_runGuardNext) return;
    g_runGuardNext = now + kRunGuardRetrySec;
    RunPython(kPyRunOff);
}

// Hilo del juego, cada fotograma (antes de los brazos): estado del enemigo
// fijado para Supr, el pecho y VrPadHook.
void UpdateLockWatch(uintptr_t player, double now) {
    g_lockedPub.store(LockHeld(player, now), std::memory_order_relaxed);
}

// Hilo del juego: lo que pide VrPadHook (Y = soltar al enemigo, gesto de la
// espalda, gatillo con la mano derecha vacia, B quieto = seleccionar objeto).
void HandlePadRequests(uintptr_t player) {
    if (g_unlockRequest.exchange(0, std::memory_order_relaxed) != 0) {
        uintptr_t enemy = 0;
        RdPtr(player + kPActiveEnemy, &enemy);
        if (enemy) {
            bool ok = RunPython(kPyUnlockEnemy);
            uintptr_t after = 0;
            RdPtr(player + kPActiveEnemy, &after);
            if (ok && after == 0) {
                g_lockSeenAt = -1.0;   // Supr y el pecho vuelven ya, sin el margen
                g_lockedPub.store(false, std::memory_order_relaxed);
                g_hapticLeft.store(1, std::memory_order_relaxed);
            }
        }
    }
    // Ronda 30: guardar o sacar las armas por el gesto de la espalda (si ya
    // estan como se pide, nada).
    int toggle = g_weaponToggleRequest.exchange(0, std::memory_order_relaxed);
    if (toggle != 0 && (toggle == 1) == g_weaponsOutPub.load(std::memory_order_relaxed)) {
        uintptr_t enemyBefore = 0, enemyAfter = 0;
        RdPtr(player + kPActiveEnemy, &enemyBefore);
        RunPython(kPyToggleWeapons);
        RdPtr(player + kPActiveEnemy, &enemyAfter);
        if (enemyBefore && !enemyAfter) {
            g_lockSeenAt = -1.0;   // Supr y el pecho, como al soltarlo con Y
            g_lockedPub.store(false, std::memory_order_relaxed);
        }
    }
    // Ronda 32: fijar enemigo sin sacar las armas guardadas.
    if (g_selectEnemyRequest.exchange(0, std::memory_order_relaxed) != 0) RunPython(kPySelectEnemyNoDraw);
    if (g_selectRequest.exchange(0, std::memory_order_relaxed) != 0) RunPython(kPySelectObj);
}

// --- giro directo (ronda 13) --------------------------------------------------------
double WrapPiD(double a) {
    while (a > ik::kPi) a -= 2.0 * ik::kPi;
    while (a <= -ik::kPi) a += 2.0 * ik::kPi;
    return a;
}

bool CallGetAngle(uintptr_t fn, uintptr_t person, double* out) {
    double a = 0.0;
    __try {
        a = reinterpret_cast<double(__fastcall*)(uintptr_t)>(fn)(person);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!(a == a) || a < -100.0 || a > 100.0) return false;
    *out = a;
    return true;
}

// Hilo del juego, cada fotograma: mide si Angle crece o decrece con la
// guinada de la entidad (cuando el personaje gira por cualquier motivo).
void UpdateAngleCalibration(uintptr_t player) {
    double a = 0.0, y = 0.0;
    if (!CallGetAngle(ModuleBase() + kFnGetAngle, player, &a) || !EntityYaw(player, &y)) {
        g_angleHavePrev = false;
        return;
    }
    if (g_angleHavePrev && g_angleSign == 0) {
        double dA = WrapPiD(a - g_anglePrevA), dY = WrapPiD(y - g_anglePrevY);
        double aY = std::fabs(dY);
        if (aY > 0.5 * ik::kPi / 180.0 && aY < 30.0 * ik::kPi / 180.0 && std::fabs(std::fabs(dA) - aY) < 0.25 * aY) {
            if (dA * dY > 0.0) ++g_angleVotesPos;
            else ++g_angleVotesNeg;
            if (g_angleVotesPos + g_angleVotesNeg >= kAngleVotesNeeded) {
                if (g_angleVotesNeg == 0) g_angleSign = 1;
                else if (g_angleVotesPos == 0) g_angleSign = -1;
                if (g_angleSign == 0) g_angleVotesPos = g_angleVotesNeg = 0;   // medida contradictoria: se vuelve a medir
            }
        }
    }
    g_anglePrevA = a;
    g_anglePrevY = y;
    g_angleHavePrev = true;
}

// Acciones en las que se puede poner el angulo sin estorbar al juego (estar
// quieto, con o sin armas, o bloqueando).
bool DirectTurnAnimOk(const char* anim) {
    if (!anim || !anim[0]) return false;
    char low[48];
    int i = 0;
    for (; anim[i] && i < 47; ++i) low[i] = (anim[i] >= 'A' && anim[i] <= 'Z') ? static_cast<char>(anim[i] - 'A' + 'a') : anim[i];
    low[i] = 0;
    return std::strncmp(low, "rlx", 3) == 0 || std::strcmp(low, "shattack_rlx") == 0;
}

// --- primera persona al cargar y al salir del diorama ------------------------------
std::atomic<int> g_fpRequest{0};
bool g_fpPending = false;
double g_fpWindowEnd = 0.0;

// Hilo del juego: mientras dura la ventana, la vista vuelve a primera persona
// si algo (la carga de la partida o del mapa) la cambia.
void ForceFirstPersonFrame(uintptr_t app, double now) {
    if (g_fpRequest.exchange(0) != 0) {
        g_fpPending = true;
        g_fpWindowEnd = 0.0;
    }
    if (!g_fpPending || !app) return;
    unsigned char inGame = 0;
    uintptr_t cam = 0;
    int view = -1;
    if (!RdU8(app + kAppInGame, &inGame) || !inGame) return;          // aun cargando o sin control
    if (!RdPtr(app + kAppCamera, &cam) || !cam || !RdI32(cam + kCamViewType, &view) || view < 0 || view > 3) return;
    if (g_fpWindowEnd == 0.0) g_fpWindowEnd = now + kFirstPersonForceSec;
    if (view != kViewFirstPerson) CallSetViewType(ModuleBase() + kFnSetViewType, cam + kCamViewCtl, kViewFirstPerson);
    if (now >= g_fpWindowEnd) g_fpPending = false;
}

bool CheckPrologue(uintptr_t addr, const unsigned char* bytes, size_t n) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(addr), bytes, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InstallOne(uintptr_t offset, const unsigned char* prologue, size_t n, void* detour, void** original, const char* what) {
    uintptr_t addr = ModuleBase() + offset;
    char line[256];
    if (!CheckPrologue(addr, prologue, n)) {
        std::snprintf(line, sizeof(line), "[COMBATE] AVISO: %s (Blade.exe+0x%llx) no tiene el prologo esperado; no se engancha.",
                      what, static_cast<unsigned long long>(offset));
        LogLine(line);
        return false;
    }
    void* target = reinterpret_cast<void*>(addr);
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        std::snprintf(line, sizeof(line), "[COMBATE] ERROR: no se pudo enganchar %s (Blade.exe+0x%llx).", what,
                      static_cast<unsigned long long>(offset));
        LogLine(line);
        return false;
    }
    std::snprintf(line, sizeof(line), "[COMBATE] Enganchado %s en Blade.exe+0x%llx.", what, static_cast<unsigned long long>(offset));
    LogLine(line);
    return true;
}

// Ronda 16: la puerta del mando (hilo del juego). Solo cambia lo que devuelve a
// la actualizacion del mando de Steam Input; el gestor sigue diciendo que lo
// ultimo usado es el raton. Con los mandos de Quest (fuera del diorama, o en el
// con los mandos manejando al personaje, ronda 62), si su mando tiene algo que
// entregar o el movimiento es suyo (ronda 68; ver kRetGateSteamPad). El mando
// SDL (Xbox) en uso, siempre (rondas 67 y 69).
using PFN_InputGate = int(__fastcall*)(long long mgr);
PFN_InputGate g_origInputGate = nullptr;

// Ronda 65: el mando del juego es el de los Quest (por su asa de Steam Input).
bool IsQuestPadDevice(uintptr_t dev) {
    uintptr_t h = 0;
    return dev && RdPtr(dev + kPadController, &h) && VrPadIsQuestController(static_cast<unsigned long long>(h));
}

bool RdFloats(uintptr_t a, float* out, int n) {
    if (a < 0x10000) return false;
    __try {
        const float* src = reinterpret_cast<const float*>(a);
        for (int i = 0; i < n; ++i) out[i] = src[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Ronda 67: algun stick fuera de su zona muerta (el juego guarda 0 dentro) o un
// gatillo por encima de su umbral (por debajo no hace nada).
bool PadAxesUsed(const float* v) {
    return v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f || std::fabs(v[4]) >= kPadTriggerIdleMax ||
           std::fabs(v[5]) >= kPadTriggerIdleMax;
}

// En uso ahora o en su ultimo fotograma procesado (un soltar por entregar), o
// con botones en cola (ronda 68; sin procesar el mando se tiran).
bool PadInUse(uintptr_t dev) {
    float now6[6], prev6[6];
    if (!RdFloats(dev + kPadAxesNow, now6, 6) || !RdFloats(dev + kPadAxesPrev, prev6, 6)) return false;
    if (PadAxesUsed(now6) || PadAxesUsed(prev6)) return true;
    uintptr_t queued = 0;
    return RdPtr(dev + kPadQueueCount, &queued) && queued != 0 && queued < 4096;
}

using PFN_PadAxisY = void(__fastcall*)(uintptr_t pad, float cur, float prev, float t);
PFN_PadAxisY g_origPadAxisY = nullptr;

void __fastcall HookedPadAxisY(uintptr_t pad, float cur, float prev, float t) {
    float sent[2] = { 0, 0 };
    if (pad && g_installed && cur == 0.0f && prev == 0.0f && VrPadGetLeftStick(sent)) {
        bool questMoving = sent[0] != 0.0f || sent[1] != 0.0f;
        if (questMoving || !g_questDriven) return;   // ronda 66 (ver kFnPadAxisY)
    }
    g_origPadAxisY(pad, cur, prev, t);
}

// Ronda 69: clase de un dispositivo de entrada por su tabla virtual.
uintptr_t DeviceVtRva(uintptr_t dev) {
    uintptr_t vt = 0;
    if (!dev || !RdPtr(dev, &vt) || vt < ModuleBase()) return 0;
    return vt - ModuleBase();
}

int __fastcall HookedInputGate(long long mgr) {
    int r = g_origInputGate(mgr);
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ModuleBase();
    if (ret == kRetGateSdlPad) {
        // Ronda 67: el mando de Xbox (SDL) con su stick, un gatillo o botones
        // en uso se procesa aunque lo ultimo usado sea el raton o el teclado.
        // Ronda 69: tambien sin los Quest (dormidos).
        if (r == 0 && g_installed && g_sdlDev && PadInUse(g_sdlDev)) r = 1;
        return r;
    }
    if (r != 0) return r;
    if (ret != kRetGateSteamPad || !g_installed || (DioramaEnabled() && !VrPadDioramaGameActive())) return r;
    float sent[2] = { 0, 0 };
    if (!VrPadGetLeftStick(sent)) return r;   // sin los mandos de Quest, como el juego
    if (g_questDev && !g_questDriven && !PadInUse(g_questDev)) return r;   // ronda 68: quieto, nada que entregar
    return 1;
}

// --- ronda 33: escudo VR ---------------------------------------------------------
using PFN_IsBlocking = int(__fastcall*)(uintptr_t person);
using PFN_ShieldTest = int(__fastcall*)(uintptr_t person, const double* a, const double* b, double* out);
PFN_IsBlocking g_origIsBlocking = nullptr;
PFN_ShieldTest g_origShieldTest = nullptr;

int __fastcall HookedIsBlocking(uintptr_t person) {
    if (person && person == g_vrShieldPlayer && ((g_vrShieldUp && g_vrShieldItem) || g_vr2hBlockUp) && IkFrameFresh()) return 1;   // ronda 57: tambien el arma a dos manos alzada
    return g_origIsBlocking(person);
}

// -1 = no se pudo (sigue la prueba del juego); 0 / 1 = no para / para.
int VrShieldTest(uintptr_t person, const double* a, const double* b, double* out) {
    double m[16], c[3], body[3], sa[3], sb[3];
    if (!WeaponWorld(g_vrShieldItem, m) || !PersonPos(person, body) || !RdDoubles(reinterpret_cast<uintptr_t>(a), sa, 3) ||
        !RdDoubles(reinterpret_cast<uintptr_t>(b), sb, 3))
        return -1;
    Translation(m, c);
    double upm = g_ikMapValid && g_ikMap.unitsPerMeter > 1e-6 ? g_ikMap.unitsPerMeter : 1000.0;
    // Ronda 48: en el modo 3 (manos flotantes) el escudo cuenta donde se ve
    // (la ronda 47 lo bajaba la altura anadida a los ojos): los golpes van al
    // cuerpo del personaje, que queda esa altura por debajo de los ojos, y se
    // paran poniendo el escudo en el camino del arma que se ve llegar.
    // Lado del escudo: del cuerpo al escudo, en horizontal.
    double dir[3] = { c[0] - body[0], 0.0, c[2] - body[2] };
    double dl = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    bool haveDir = dl > 1.0;
    if (haveDir) { dir[0] /= dl; dir[2] /= dl; }
    const int kSamples = 12;
    for (int i = 0; i <= kSamples; ++i) {
        double t = static_cast<double>(i) / kSamples, p[3];
        for (int k = 0; k < 3; ++k) p[k] = sa[k] + (sb[k] - sa[k]) * t;
        double dc = Dist3(p, c) / upm;
        bool hit = dc <= kShieldSphereM;
        if (!hit && haveDir) {
            double hx = p[0] - body[0], hz = p[2] - body[2];
            double horiz = std::sqrt(hx * hx + hz * hz) / upm;
            double front = (hx * dir[0] + hz * dir[2]) / upm;
            double dy = std::fabs(p[1] - c[1]) / upm;
            hit = front > 0.0 && dy <= kShieldBandM && horiz <= kShieldBodyReachM;
        }
        if (hit) {
            for (int k = 0; k < 3; ++k) out[k] = p[k];
            return 1;
        }
    }
    return 0;
}

int __fastcall HookedShieldTest(uintptr_t person, const double* a, const double* b, double* out) {
    if (person && person == g_vrShieldPlayer && g_vrShieldItem && a && b && out && IkFrameFresh()) {
        int r = VrShieldTest(person, a, b, out);
        if (r == 1) ++g_vrShieldStops;
        if (r >= 0) return r;
    }
    if (person && person == g_vrShieldPlayer && g_vr2hBlockUp && !g_vrShieldItem && IkFrameFresh()) {
        // Ronda 57: el bloqueo del arma a dos manos alzada, la prueba del juego
        // (aparte solo para contar las paradas en el resumen del minuto).
        int r = g_origShieldTest(person, a, b, out);
        if (r) ++g_vr2hStops;
        return r;
    }
    return g_origShieldTest(person, a, b, out);
}

// --- ronda 47: modo sin cuerpo (solo antebrazos y manos) -----------------------------
// (Ronda 48: es el modo de las manos flotantes; antebrazos y manos los coloca
// FloatArm, pegados al mando, y aqui solo se esconde el resto.)
// El dibujo de la persona (vtable[26] = +0xb87c0: evalua el esqueleto, coloca
// lo enganchado y dibuja el objeto 3D con la malla deformada por los huesos).
// Para el jugador en el modo 3, durante esa llamada los huesos que no son de
// antebrazo o mano se encogen a un punto (escala 1e-4): cada vertice va con un
// solo hueso, asi que su parte de la malla desaparece. Hombro y brazo se encogen
// en su codo (la union queda como una tapa) y el resto en el pecho (los
// triangulos entre ellos quedan con dos vertices en el mismo punto: sin area).
// Al volver se deja todo como estaba: la fisica, la camara, la animacion y la
// malla de los golpes (+0xb9240, que va aparte) ven el cuerpo entero.
constexpr uintptr_t kFnPersonDraw = 0xb87c0;
using PFN_PersonDraw = void(__fastcall*)(uintptr_t person, uintptr_t a2, int a3);
PFN_PersonDraw g_origPersonDraw = nullptr;
struct SavedNodeW {
    uintptr_t wp;
    double w[16];
};
SavedNodeW g_hideSaved[64];
int g_hideSavedN = 0;

bool BodyHideBegin(uintptr_t person) {
    g_hideSavedN = 0;
    SkelView sv;
    if (!g_chain.ok || !GetSkel(person, &sv) || sv.skel != g_chain.skel || sv.count > 64) return false;
    HookedEvalAll(sv.skel);   // todo calculado (con los brazos de los mandos) antes de encoger
    unsigned long long keep = 0;
    double elbow[2][3];
    int shoulder[2] = { -1, -1 };
    for (int s = 0; s < 2; ++s) {
        const ArmInfo& a = g_chain.arm[s];
        for (int k = 1; k < 5; ++k)
            if (a.node[k] >= 0 && a.node[k] < 64) keep |= 1ull << a.node[k];
        double w[16];
        if (!NodeWorld(sv, a.node[1], w, nullptr)) return false;
        elbow[s][0] = w[12]; elbow[s][1] = w[13]; elbow[s][2] = w[14];
        shoulder[s] = NodeParent(sv, a.node[0]);
    }
    double chest[3];
    {
        double w[16];
        int c = g_chain.chest >= 0 ? g_chain.chest : g_chain.root;
        if (!NodeWorld(sv, c, w, nullptr)) return false;
        chest[0] = w[12]; chest[1] = w[13]; chest[2] = w[14];
    }
    for (int i = 0; i < sv.count; ++i) {
        if ((keep >> i) & 1ull) continue;
        uintptr_t wp = 0;
        if (!RdPtr(NodeRecord(sv.nodes, i) + kNodeWorld, &wp) || !wp) continue;
        SavedNodeW& sn = g_hideSaved[g_hideSavedN];
        if (!RdDoubles(wp, sn.w, 16)) continue;
        sn.wp = wp;
        ++g_hideSavedN;
        const double* p = chest;
        for (int s = 0; s < 2; ++s)
            if (i == g_chain.arm[s].node[0] || i == shoulder[s]) p = elbow[s];
        double w[16];
        for (int k = 0; k < 16; ++k) w[k] = sn.w[k];
        for (int k = 0; k < 12; ++k) w[k] *= 1e-4;
        w[12] = p[0]; w[13] = p[1]; w[14] = p[2];
        WrDoubles(wp, w, 16);
    }
    return true;
}

void BodyHideEnd() {
    for (int i = 0; i < g_hideSavedN; ++i) WrDoubles(g_hideSaved[i].wp, g_hideSaved[i].w, 16);
    g_hideSavedN = 0;
}

void __fastcall HookedPersonDraw(uintptr_t person, uintptr_t a2, int a3) {
    if (person && person == g_player && g_frameThread != 0 && GetCurrentThreadId() == g_frameThread && CombatVRBodilessActive()) {
        bool hidden = false;
        __try {
            hidden = BodyHideBegin(person);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hidden = false;
        }
        g_origPersonDraw(person, a2, a3);
        if (hidden || g_hideSavedN > 0) BodyHideEnd();
        return;
    }
    g_origPersonDraw(person, a2, a3);
}

// Present: teclas (Ctrl+F8, Mayus+F8) y cuenta atras de la calibracion (estado
// solo de este hilo).
bool g_f8WasDownP = false;
unsigned long long g_calibStartTick = 0;
int g_calibShown = -1;

} // namespace

bool InstallCombatVRHooks() {
    // movsd [rsp+0x10],xmm1; push rbx; sub rsp,0x30
    static const unsigned char kDefencePrologue[] = { 0xf2, 0x0f, 0x11, 0x4c, 0x24, 0x10, 0x53, 0x48, 0x83, 0xec, 0x30 };
    // mov [rsp+0x18],rbx; mov [rsp+0x20],rbp; push rsi; push rdi
    static const unsigned char kEvalNodePrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57 };
    // mov [rsp+0x10],rbx; mov [rsp+0x18],rbp; push rsi; push rdi
    static const unsigned char kEvalAllPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57 };
    LoadCalibration();
    // Ronda 53: de serie el modo 1 (mandos de movimiento con el cuerpo visible).
    // Ronda 54 (pedido del usuario): el ultimo modo elegido, de BladeVR.ini.
    {
        int m = IniGetInt("modo_combate", 1);
        if (m < 0 || m > 3) m = 1;
        g_combatMode.store(m, std::memory_order_relaxed);
        HeadTrackSetWalkFollowsView(m != 0, m);   // ronda 70: en el modo 0, Supr apagado (vista libre)
        char line[220];
        std::snprintf(line, sizeof(line), "[MODO] modo de combate %d al arrancar (BladeVR.ini, el ultimo elegido): %s.", m, CombatModeName(m));
        LogLine(line);
        // Ronda 60: resistencia en los golpes con los mandos (Mayus + F8), de BladeVR.ini.
        bool en = IniGetInt("resistencia", 1) != 0;
        g_energyOn.store(en, std::memory_order_relaxed);
        LogLine(en ? "[RESISTENCIA] al arrancar (BladeVR.ini): los golpes con los mandos la gastan (Mayus + F8 para quitarla)."
                   : "[RESISTENCIA] al arrancar (BladeVR.ini): los golpes con los mandos NO la gastan (Mayus + F8 para ponerla).");
    }
    bool b = InstallOne(kFnDefenceNeeded, kDefencePrologue, sizeof(kDefencePrologue),
                        reinterpret_cast<void*>(&HookedDefenceNeeded), reinterpret_cast<void**>(&g_origDefence),
                        "la decision de defenderse de la IA (tambien del tajo por gesto)");
    // Los dos evaluadores o ninguno: con uno solo, unos consumidores verian los
    // brazos resueltos y otros los de la animacion.
    bool c = InstallOne(kFnEvalNode, kEvalNodePrologue, sizeof(kEvalNodePrologue), reinterpret_cast<void*>(&HookedEvalNode),
                        reinterpret_cast<void**>(&g_origEvalNode), "el evaluador perezoso de huesos (brazos)");
    bool d = c && InstallOne(kFnEvalAll, kEvalAllPrologue, sizeof(kEvalAllPrologue), reinterpret_cast<void*>(&HookedEvalAll),
                             reinterpret_cast<void**>(&g_origEvalAll), "el evaluador de todos los huesos (brazos)");
    if (c && !d) {
        MH_DisableHook(reinterpret_cast<void*>(ModuleBase() + kFnEvalNode));
        LogLine("[COMBATE] Sin el segundo evaluador se quita tambien el primero: brazos con los mandos desactivados.");
    }
    g_ikAvailable = c && d;
    // Ronda 39: los pies de la fisica del juego sin el cuerpo bajo el visor.
    if (g_ikAvailable) {
        // mov [rsp+8],rbx; mov [rsp+0x10],rbp; mov [rsp+0x18],rsi; push rdi
        static const unsigned char kFootPosPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10,
                                                          0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
        // mov [rsp+0x10],rbx; mov [rsp+0x18],rsi; push rdi; sub rsp,0x70
        static const unsigned char kFootRayPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                                                          0x57, 0x48, 0x83, 0xec, 0x70 };
        int n = 0;
        n += InstallOne(kFnFootPosA, kFootPosPrologue, sizeof(kFootPosPrologue), reinterpret_cast<void*>(&HookedFootPosA),
                        reinterpret_cast<void**>(&g_origFootPosA), "el pie (+0xd0) para la fisica del juego") ? 1 : 0;
        n += InstallOne(kFnFootPosB, kFootPosPrologue, sizeof(kFootPosPrologue), reinterpret_cast<void*>(&HookedFootPosB),
                        reinterpret_cast<void**>(&g_origFootPosB), "el pie (+0xd4) para la fisica del juego") ? 1 : 0;
        n += InstallOne(kFnFootRayA, kFootRayPrologue, sizeof(kFootRayPrologue), reinterpret_cast<void*>(&HookedFootRayA),
                        reinterpret_cast<void**>(&g_origFootRayA), "el suelo bajo el pie (+0xd0) para la fisica") ? 1 : 0;
        n += InstallOne(kFnFootRayB, kFootRayPrologue, sizeof(kFootRayPrologue), reinterpret_cast<void*>(&HookedFootRayB),
                        reinterpret_cast<void**>(&g_origFootRayB), "el suelo bajo el pie (+0xd4) para la fisica") ? 1 : 0;
        if (n < 4) {
            LogLine("[COMBATE] AVISO: no se engancharon los cuatro pies de la fisica: sin cuerpo bajo el visor (la fisica veria el desplazamiento).");
            g_bodyPhysicsSafe = false;
        } else {
            g_bodyPhysicsSafe = true;
        }
    }
    // Ronda 47: modo sin cuerpo (el dibujo de la persona).
    if (g_ikAvailable) {
        // mov [rsp+8],rbx; mov [rsp+0x10],rsi; push rdi; sub rsp,0x20
        static const unsigned char kPersonDrawPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
                                                             0x57, 0x48, 0x83, 0xec, 0x20 };
        InstallOne(kFnPersonDraw, kPersonDrawPrologue, sizeof(kPersonDrawPrologue), reinterpret_cast<void*>(&HookedPersonDraw),
                   reinterpret_cast<void**>(&g_origPersonDraw), "el dibujo de la persona (modo sin cuerpo)");
    }
    // Ronda 41: parada sin pie apoyado (solo la que manda el mod al soltar).
    {
        // mov rax,rsp; push rbp; push rbx; push rsi; push rdi; push r12; push r13; push r14; push r15
        static const unsigned char kLaunchPrologue[] = { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
        // mov rax,rsp; mov [rax+0x10],rbx; mov [rax+0x18],rbp; push rsi; push rdi; push r12
        static const unsigned char kLaunchBPrologue[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x56, 0x57, 0x41, 0x54 };
        InstallOne(kFnLaunchAction, kLaunchPrologue, sizeof(kLaunchPrologue), reinterpret_cast<void*>(&HookedLaunchAction),
                   reinterpret_cast<void**>(&g_origLaunchAction), "lanzar una accion con transicion (parada sin pie apoyado)");
        InstallOne(kFnLaunchActionB, kLaunchBPrologue, sizeof(kLaunchBPrologue), reinterpret_cast<void*>(&HookedLaunchActionB),
                   reinterpret_cast<void**>(&g_origLaunchActionB), "lanzar Rlx_block con transicion (parada sin pie apoyado)");
    }
    // Ronda 33: escudo VR (solo cambian algo para el jugador con el escudo en el
    // antebrazo siguiendo al mando).
    {
        // push rbx; sub rsp,0x40; mov rbx,rcx
        static const unsigned char kIsBlockingPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0xd9 };
        // mov [rsp+8],rbx; mov [rsp+0x10],rsi; mov [rsp+0x18],rdi
        static const unsigned char kShieldTestPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89 };
        bool e = InstallOne(kFnShieldTest, kShieldTestPrologue, sizeof(kShieldTestPrologue), reinterpret_cast<void*>(&HookedShieldTest),
                            reinterpret_cast<void**>(&g_origShieldTest), "la prueba del escudo (escudo VR)");
        if (e) {
            InstallOne(kFnIsBlocking, kIsBlockingPrologue, sizeof(kIsBlockingPrologue), reinterpret_cast<void*>(&HookedIsBlocking),
                       reinterpret_cast<void**>(&g_origIsBlocking), "\"esta bloqueando\" (escudo VR levantado)");
        }
    }
    // Ronda 65: el eje Y de los mandos del juego (con Y = 0 ahora y antes no se
    // procesa mientras se mueve el joystick de los Quest o si el movimiento no
    // es suyo, ronda 66; ver kFnPadAxisY).
    {
        // mov rax,rsp; mov [rax+8],rbx; mov [rax+0x10],rbp; mov [rax+0x18],rsi
        static const unsigned char kPadAxisYPrologue[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10,
                                                           0x48, 0x89, 0x70, 0x18 };
        InstallOne(kFnPadAxisY, kPadAxisYPrologue, sizeof(kPadAxisYPrologue), reinterpret_cast<void*>(&HookedPadAxisY),
                   reinterpret_cast<void**>(&g_origPadAxisY), "el eje Y de los mandos del juego (solo los Quest al moverse)");
    }
    // Ronda 16: la puerta del mando, solo si la llamada desde la actualizacion
    // del mando de Steam Input es la esperada (call; test eax,eax; jnz).
    {
        // push rbx; sub rsp,0x20; mov rbx,rcx
        static const unsigned char kGatePrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9 };
        static const unsigned char kGateRetBytes[] = { 0x85, 0xc0, 0x75, 0x3f };
        if (CheckPrologue(ModuleBase() + kRetGateSteamPad, kGateRetBytes, sizeof(kGateRetBytes))) {
            InstallOne(kFnInputGate, kGatePrologue, sizeof(kGatePrologue), reinterpret_cast<void*>(&HookedInputGate),
                       reinterpret_cast<void**>(&g_origInputGate), "la puerta del mando (procesarlo aunque lo ultimo sea el raton)");
        } else {
            LogLine("[COMBATE] AVISO: la llamada a la puerta del mando (Blade.exe+0x271572) no es la esperada; no se engancha.");
        }
    }
    g_installed = true;
    LogLine(g_ikAvailable ? "[COMBATE] Instalado: las manos del personaje siguen a los mandos en primera persona (Ctrl+F8 calibrar, "
                            "Mayus+F8 resistencia si/no)."
                          : "[COMBATE] Instalado sin brazos con los mandos (no se engancharon los evaluadores del esqueleto).");
    return b || (c && d);
}

void CombatVRRequestFirstPerson() { g_fpRequest.store(1, std::memory_order_relaxed); }

bool CombatVRShieldGestureAllowed() { return g_shieldGestureOk.load(std::memory_order_relaxed); }

bool CombatVRGetPlayerFacing(double* yaw, bool* enemyLocked) {
    if (!g_installed || !yaw || DioramaEnabled()) return false;
    uintptr_t app = 0, player = 0;
    if (!RdPtr(ModuleBase() + kAppObjectPtr, &app) || !app || !RdPtr(app + kAppPlayer, &player) || !IsPerson(player)) return false;
    if (!EntityYaw(player, yaw)) return false;
    // Con un enemigo fijado (con margen, ronda 10) o si el juego convertiria
    // los giros en pasos laterales (ronda 11), Supr no gira al personaje.
    if (enemyLocked) {
        double now = NowSec();
        *enemyLocked = LockHeld(player, now) || TurnStrafeReason(player) != 0 || now < g_strafeHoldUntil;
    }
    return true;
}

bool CombatVRDirectTurnAvailable(bool moving) {
    if (!g_installed || DioramaEnabled() || NowSec() < g_directOffUntil || g_angleSign == 0 || moving) return false;
    uintptr_t app = 0, player = 0;
    if (!RdPtr(ModuleBase() + kAppObjectPtr, &app) || !app || !RdPtr(app + kAppPlayer, &player) || !IsPerson(player)) return false;
    int life = 0;
    if (!RdI32(player + kPLife, &life) || life <= 0) return false;
    if (LockHeld(player, NowSec())) return false;   // con un enemigo fijado lo encara el juego
    char anim[48];
    ReadAnimNames(player, anim, sizeof(anim), nullptr, 0);
    return DirectTurnAnimOk(anim);
}

void CombatVRDirectTurnStep(double viewYaw) {
    uintptr_t app = 0, player = 0;
    if (!RdPtr(ModuleBase() + kAppObjectPtr, &app) || !app || !RdPtr(app + kAppPlayer, &player) || !IsPerson(player)) return;
    double a = 0.0, y = 0.0;
    if (!CallGetAngle(ModuleBase() + kFnGetAngle, player, &a) || !EntityYaw(player, &y)) return;
    double now = NowSec();
    double dt = now - g_directLastT;
    bool fresh = dt > 0.2;   // un giro nuevo
    if (dt < 0.005 || dt > 0.05) dt = 0.016;
    g_directLastT = now;
    double err = WrapPiD(viewYaw - y);
    if (fresh) {
        g_directStartErr = std::fabs(err);
        g_directSteps = 0;
    }
    double maxStep = kDirectTurnDegPerSec * ik::kPi / 180.0 * dt;
    double step = err;
    if (step > maxStep) step = maxStep;
    if (step < -maxStep) step = -maxStep;
    double na = a + g_angleSign * step;
    while (na < 0.0) na += 2.0 * ik::kPi;
    while (na >= 2.0 * ik::kPi) na -= 2.0 * ik::kPi;
    char code[200];
    std::snprintf(code, sizeof(code),
                  "import Bladex\nBladeVR_pj = Bladex.GetEntity('Player1')\nif BladeVR_pj:\n    BladeVR_pj.Angle = %.6f\n", na);
    bool ok = RunPython(code);
    // Seguro: si en 1,5 s de pasos el error no ha bajado de la mitad, el
    // angulo no hace lo que se espera: se vuelve a los giros de siempre.
    if (++g_directSteps > 90 && std::fabs(err) > 0.5 * g_directStartErr) {
        g_directOffUntil = now + kDirectRetrySec;
        g_directSteps = 0;
        if (g_directLines++ >= 5) return;
        char line[240];
        std::snprintf(line, sizeof(line),
                      "[GIRO] AVISO: el angulo puesto no acerca al personaje a la vista (error %.0f grados, Python %s): "
                      "giro directo en pausa %.0f s (mientras, los giros de siempre).",
                      std::fabs(err) * 180.0 / ik::kPi, ok ? "hecho" : "FALLO", kDirectRetrySec);
        LogLine(line);
    }
    if (!ok && g_directLines++ < 5) LogLine("[GIRO] ERROR: fallo el Python que pone el angulo del personaje.");
}

bool CombatVRGetPlayerPosition(double out[3]) {
    if (!g_installed || !out || DioramaEnabled()) return false;
    uintptr_t app = 0, player = 0;
    if (!RdPtr(ModuleBase() + kAppObjectPtr, &app) || !app || !RdPtr(app + kAppPlayer, &player) || !IsPerson(player)) return false;
    return PersonPos(player, out);
}

bool CombatVRInGameplay() {
    unsigned long long t = g_gameplayTick.load(std::memory_order_relaxed);
    return g_installed && t != 0 && GetTickCount64() - t < kGameplayFreshMs && !DioramaEnabled();
}

bool CombatVREnemyLocked() { return g_lockedPub.load(std::memory_order_relaxed); }

// --- ronda 23: modos de combate ---------------------------------------------------------
int CombatVRGetCombatMode() {
    int m = g_combatMode.load(std::memory_order_relaxed);
    return m == 3 ? 1 : m;   // ronda 47: el 3 se comporta como el 1 (salvo brazos y cuerpo, ronda 48)
}

bool CombatVRBodilessActive() {
    return g_installed && g_combatMode.load(std::memory_order_relaxed) == 3 && CombatVRInGameplay() &&
           g_firstPersonNow.load(std::memory_order_relaxed);
}

void CombatVRCycleCombatMode() {
    // Ronda 48: 1 (mandos) -> 3 (manos flotantes) -> 2 (combos) -> 0 (basico) -> 1.
    static const int kNext[4] = { 1, 3, 0, 2 };
    int cur = g_combatMode.load(std::memory_order_relaxed);
    int m = (cur >= 0 && cur <= 3) ? kNext[cur] : 1;
    g_combatMode.store(m, std::memory_order_relaxed);
    HeadTrackSetWalkFollowsView(m != 0, m);   // ronda 70: en el modo 0, Supr apagado (vista libre)
    // Ronda 74: con los mandos de movimiento (1, 3 y 2), siempre en primera
    // persona; al pasar al 0, la vista recentrada como con Insert dos veces
    // (sin Supr, adelante vuelve a ser hacia donde se mira).
    if (m != 0) CombatVRRequestFirstPerson();
    else HeadTrackRecenterLikeInsert();
    static const wchar_t* const kTitle[4] = { L"Combate: VR b\u00e1sico", L"Combate: mandos de movimiento",
                                               L"Combate: mandos + combos", L"Combate: manos flotantes" };
    static const wchar_t* const kText[4] = {
        L"Sin mandos de movimiento: A ataca y hace los combos del juego",
        L"Ataca moviendo el arma, bloquea alzando el escudo; A no ataca",
        L"A y las direcciones hacen los combos del juego; el gesto, el ataque b\u00e1sico",
        L"Sin cuerpo: antebrazos, manos y armas siguen a los mandos sin l\u00edmite del brazo; "
        L"ojos m\u00e1s altos (May\u00fas + Re P\u00e1g / Av P\u00e1g)" };
    PostToast(kTitle[m], kText[m], kToastMs);
    IniSetInt("modo_combate", m);   // ronda 54
    char line[240];
    std::snprintf(line, sizeof(line), "[MODO] modo de combate %d: %s.", m, CombatModeName(m));
    LogLine(line);
}

void CombatVRNotifyAttackButton() {
    if (g_combatMode.load(std::memory_order_relaxed) == 2) g_comboPressTick.store(GetTickCount64(), std::memory_order_relaxed);
}

// --- ronda 30: armas a la espalda y punetazos -------------------------------------------
bool CombatVRArmsGestureState(bool* weaponsOut) {
    if (weaponsOut) *weaponsOut = g_weaponsOutPub.load(std::memory_order_relaxed);
    return g_installed && CombatVRInGameplay() && g_combatMode.load(std::memory_order_relaxed) != 0 &&
           g_armsGestureOkPub.load(std::memory_order_relaxed);
}

// Ronda 38: accion de script en curso (palancas, coger, escenas): Supr no gira.
bool CombatVRScriptedAction() { return g_installed && g_scriptedActionPub.load(std::memory_order_relaxed); }

bool CombatVRLeftGripHoldsWeapon() {
    return g_installed && CombatVRInGameplay() && g_combatMode.load(std::memory_order_relaxed) != 0 &&
           g_twoHandWeaponPub.load(std::memory_order_relaxed);
}

void CombatVRRequestWeaponToggle(bool stow) { g_weaponToggleRequest.store(stow ? 1 : 2, std::memory_order_relaxed); }

bool CombatVRSelectEnemyWithoutDraw() {
    return g_installed && CombatVRInGameplay() && g_combatMode.load(std::memory_order_relaxed) != 0 &&
           g_rightEmptyPub.load(std::memory_order_relaxed);
}

void CombatVRRequestSelectEnemy() { g_selectEnemyRequest.store(1, std::memory_order_relaxed); }

bool CombatVRComboHold() { return g_installed && g_comboHoldPub.load(std::memory_order_relaxed); }

bool CombatVRGestureAttackReady() { return g_installed && g_gestureReadyPub.load(std::memory_order_relaxed); }

bool CombatVRBowWantsTrigger() { return g_installed && CombatVRInGameplay() && g_bowZonePub.load(std::memory_order_relaxed); }   // ronda 50

bool CombatVRBowArrowGesture() { return g_installed && CombatVRInGameplay() && g_bowEquippedPub.load(std::memory_order_relaxed); }   // ronda 51

void CombatVRRequestBowArrow() { g_bowArrowReq.store(1, std::memory_order_relaxed); }

void CombatVRRequestUnlockEnemy() { g_unlockRequest.store(1, std::memory_order_relaxed); }

void CombatVRRequestSelectObject() { g_selectRequest.store(1, std::memory_order_relaxed); }

bool CombatVRSkipIdlePad(long long dev) {
    if (!g_installed || !dev) return false;
    uintptr_t d = static_cast<uintptr_t>(dev);
    // Ronda 69: los mandos se apuntan siempre (la puerta del SDL los usa sin Quest).
    if (IsQuestPadDevice(d)) g_questDev = d;
    else if (DeviceVtRva(d) == kVtSdlPad) g_sdlDev = d;
    if (DioramaEnabled()) return false;
    if (g_frameThread == 0 || GetCurrentThreadId() != g_frameThread) return false;
    float sent[2] = { 0, 0 };
    if (!VrPadGetLeftStick(sent)) return false;   // sin los Quest, como el juego
    float now6[6], prev6[6];
    if (!RdFloats(d + kPadAxesNow, now6, 6) || !RdFloats(d + kPadAxesPrev, prev6, 6)) return false;
    double now = NowSec();
    if (now6[0] != 0.0f || now6[1] != 0.0f) {
        g_movingPad = d;
        g_movingPadIsQuest = IsQuestPadDevice(d);
        g_movingPadAt = now;
    }
    if (PadAxesUsed(now6)) {
        g_activePad = d;
        g_activePadAt = now;
        return false;
    }
    if (PadAxesUsed(prev6)) return false;   // lo acaba de soltar: que el juego lo vea
    if (g_activePad == 0 || g_activePad == d || g_activePadAt < 0.0 || now - g_activePadAt > kOtherPadHoldSec) return false;
    return true;
}

void CombatVROnGamepadInput(long long dev, float t) {
    if (!g_installed || !dev || DioramaEnabled()) return;
    if (g_frameThread == 0 || GetCurrentThreadId() != g_frameThread) return;
    {
        // Ronda 65: con los mandos de Quest, solo su mando (el juego procesa tambien los otros).
        float sent[2] = { 0, 0 };
        if (VrPadGetLeftStick(sent) && !IsQuestPadDevice(static_cast<uintptr_t>(dev))) return;
    }
    float x = 0.0f, y = 0.0f;
    if (!RdF32(static_cast<uintptr_t>(dev) + kPadStickX, &x) || !RdF32(static_cast<uintptr_t>(dev) + kPadStickY, &y)) return;
    double now = NowSec();
    g_padLastInput = now;
    bool neutral = x == 0.0f && y == 0.0f;
    if (neutral && !g_padNeutral) g_padNeutralSince = now;
    g_padNeutral = neutral;
    if (!neutral) g_walkStuck = false;
    if (!g_walkStuck || now < g_walkNextRelease) return;
    g_walkNextRelease = now + kWalkRetrySec;
    g_walkStuck = false;
    uintptr_t fn = ModuleBase() + kFnPadKey;
    // El tiempo, como lo pasa el juego (+0x27b2c0: cvttss2si del tercer argumento).
    unsigned ti = static_cast<unsigned>(static_cast<long long>(t));
    static const char* const kKeys[] = { "JoyUp", "JoyDown", "JoyLeft", "JoyRight" };
    for (const char* k : kKeys) CallPadKey(fn, dev, 0, k, ti);
}

void CombatVROnPresent() {
    VrPadOnPresent();   // mandos de Quest para el juego (despues del DioramaPollKeys de este fotograma)
    if (!g_installed) return;
    if (g_hapticRight.exchange(0, std::memory_order_relaxed)) VrInputHaptic(ARM_R, 0.15f, 0.7f);   // bocado
    int punchHit = g_hapticHit.exchange(0, std::memory_order_relaxed);   // ronda 31: un punetazo que toca
    if (punchHit == 1 + ARM_L || punchHit == 1 + ARM_R) VrInputHaptic(punchHit - 1, 0.06f, 0.9f);
    if (g_hapticLeft.exchange(0, std::memory_order_relaxed)) VrInputHaptic(ARM_L, 0.05f, 0.5f);    // enemigo soltado con Y
    int bowH = g_hapticBow.exchange(0, std::memory_order_relaxed);   // ronda 50: arco VR
    if (bowH == 1) VrInputHaptic(ARM_R, 0.03f, 0.4f);   // cuerda agarrada
    else if (bowH == 2) {                                // disparo
        VrInputHaptic(ARM_R, 0.05f, 0.8f);
        VrInputHaptic(ARM_L, 0.08f, 0.9f);
    }
    int grab = g_hapticGrab.exchange(0, std::memory_order_relaxed);   // ronda 27: coger / soltar el arma
    if (grab == 1) VrInputHaptic(ARM_L, 0.06f, 0.6f);
    else if (grab == 2) VrInputHaptic(ARM_L, 0.03f, 0.3f);
    OpenVRUpdateToast();
    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool edge = f8 && !g_f8WasDownP;
    g_f8WasDownP = f8;
    if (edge && g_ikAvailable) {
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift && !ctrl) {
            // Ronda 60 (pedido del usuario): resistencia en los golpes con los mandos si / no.
            bool next = !g_energyOn.load(std::memory_order_relaxed);
            g_energyOn.store(next, std::memory_order_relaxed);
            IniSetInt("resistencia", next ? 1 : 0);
            OpenVRShowToast(L"Resistencia en los golpes con los mandos",
                            next ? L"S\u00ed: gastan la barra como el ataque del juego" : L"No: los golpes con los mandos no la gastan", kToastMs);
            LogLine(next ? "[RESISTENCIA] Mayus + F8: los golpes con los mandos la gastan (guardado en BladeVR.ini)."
                         : "[RESISTENCIA] Mayus + F8: los golpes con los mandos ya NO la gastan (guardado en BladeVR.ini).");
        } else if (ctrl) {
            // Cuenta atras para soltar el teclado y estirar los brazos; la
            // vista se recentra ya (ojos del jugador = ojos del personaje).
            g_calibStartTick = GetTickCount64();
            g_calibShown = -1;
            HeadTrackRequestRecenter();
            LogLine("[IK] Ctrl+F8: calibracion en 5 s (vista recentrada).");
        }
    }
    if (g_calibStartTick) {
        unsigned long long el = GetTickCount64() - g_calibStartTick;
        if (el >= kCalibCountdownMs) {
            g_calibStartTick = 0;
            g_calibRequest.store(1, std::memory_order_relaxed);   // el aviso y la vibracion larga, al medir
        } else {
            int left = static_cast<int>((kCalibCountdownMs - el + 999) / 1000);
            if (left != g_calibShown) {
                g_calibShown = left;
                static const wchar_t* const kNums[6] = { L"0", L"1", L"2", L"3", L"4", L"5" };
                wchar_t txt[160];
                CopyW(txt, 160, L"Brazos estirados al frente, pulgares arriba, mira al frente... ");
                size_t n = std::wcslen(txt);
                CopyW(txt + n, static_cast<int>(160 - n), kNums[left < 0 ? 0 : (left > 5 ? 5 : left)]);
                OpenVRShowToast(L"Calibrar las manos", txt, 1200);
                VrInputHaptic(0, 0.04f, 0.4f);   // un toque por segundo en los dos mandos
                VrInputHaptic(1, 0.04f, 0.4f);
            }
        }
    }
    wchar_t title[64], text[200];
    unsigned ms = 0;
    bool have = false;
    AcquireSRWLockExclusive(&g_toastLock);
    if (g_toastPending) {
        CopyW(title, 64, g_toastTitle);
        CopyW(text, 200, g_toastText);
        ms = g_toastMsPending;
        g_toastPending = false;
        have = true;
    }
    ReleaseSRWLockExclusive(&g_toastLock);
    if (have) OpenVRShowToast(title, text, ms);
}

void CombatVROnGameFrame() {
    if (!g_installed) return;
    if (DioramaEnabled()) {
        IkShutdown();
        g_directOffUntil = 0.0;   // ronda 62: al salir del diorama, giro directo otra vez
        g_directSteps = 0;
        g_questDriven = false;   // ronda 66
        return;
    }
    g_frameThread = GetCurrentThreadId();
    double now = NowSec();
    double dt = (g_prevFrame > 0.0) ? now - g_prevFrame : 0.0;
    g_prevFrame = now;
    if (g_minuteStart == 0.0) g_minuteStart = now;

    uintptr_t app = 0, player = 0;
    if (!RdPtr(ModuleBase() + kAppObjectPtr, &app) || !app || !RdPtr(app + kAppPlayer, &player) || !IsPerson(player)) {
        g_player = 0;
        g_lockedPub.store(false, std::memory_order_relaxed);
        g_questDriven = false;   // ronda 66
        IkShutdown();
        return;
    }
    g_player = player;
    {
        // Partida en marcha (para VrPadHook: Y y B solo son del mod fuera de
        // los menus): nivel con control, sin el menu abierto y el jugador vivo.
        unsigned char inGame = 0;
        char mode[8] = "";
        int life = 0;
        bool menu = RdBName(app + kAppMode, mode, sizeof(mode)) && std::strcmp(mode, "Menu") == 0;
        if (RdU8(app + kAppInGame, &inGame) && inGame && !menu && RdI32(player + kPLife, &life) && life > 0) {
            g_gameplayTick.store(GetTickCount64(), std::memory_order_relaxed);
        }
    }

    // Nuevo jugador, o el mismo puntero con otro esqueleto (al cargar nivel).
    SkelView sv0;
    uintptr_t mesh0 = 0;
    bool haveSv0 = GetSkel(player, &sv0);
    MeshOf(player, &mesh0);
    if (player != g_lastPlayer || mesh0 != g_lastMesh || (haveSv0 && sv0.count != g_lastNodeCount)) {
        IkShutdown();
        g_fpRequest.store(1, std::memory_order_relaxed);   // nivel nuevo: primera persona
        BuildChain(player);
        g_cadW = {};               // ronda 38: otro personaje (con "Amz" del anterior la cadencia salia a 0)
        g_cadP = {};
        g_gripTableN = 0;          // ronda 26: otro personaje, otras animaciones (otro agarre)
        g_gripCur = -1;
        g_gripShaftCheck = -1;
        g_rightItemSeen = 0;       // que se vuelva a mirar el arma (y su agarre) con este personaje
        g_rightTwoHanded = false;  // ronda 60: y nada del arma de antes hasta verla en la mano
        g_gripKind[0] = 0;
        g_gripRlxSince = -1.0;
        g_twoHandGrabbed = false;
        g_foodInstallState = 0;
        g_foodInstallTries = 0;
        g_foodBocaApplied = -1;
        g_foodInstallAt = now + kFoodInstallDelaySec;
        g_bow = {};             // ronda 50: el Python del arco despues del de la comida
        g_bow.pyAt = now + kFoodInstallDelaySec + 0.5;
        g_bowEquippedPub.store(false, std::memory_order_relaxed);
        g_bowZonePub.store(false, std::memory_order_relaxed);
        g_energy.pyState = 0;   // ronda 59: el Python de la resistencia, despues del del arco
        g_energy.pyTries = 0;
        g_energy.pyAt = now + kFoodInstallDelaySec + 1.0;
        g_directOffUntil = 0.0;   // ronda 62: giro directo otra vez
        g_lastPlayer = player;
        g_lastMesh = mesh0;
        g_lastNodeCount = haveSv0 ? sv0.count : 0;
    }

    ForceFirstPersonFrame(app, now);
    if (g_calibRequest.exchange(0) != 0) Calibrate();
    HandlePadRequests(player);
    UpdateAngleCalibration(player);
    UpdateStrafeWatch(player);
    UpdateLockWatch(player, now);   // antes de los brazos: el pecho no gira con un enemigo fijado
    bool fp = false;
    IkFrame(player, dt, &fp);
    // Ronda 54: si se salio del juego en el diorama (BladeVR.ini), se vuelve a
    // entrar la primera vez que se juega, tras 1,5 s en primera persona.
    if (!g_dioramaIniDone) {
        if (!fp) {
            g_dioramaIniFpAt = -1.0;
        } else if (g_dioramaIniFpAt < 0.0) {
            g_dioramaIniFpAt = now;
        } else if (now - g_dioramaIniFpAt >= 1.5) {
            g_dioramaIniDone = true;
            if (IniGetInt("diorama", 0) == 1 && !DioramaEnabled()) {
                DioramaRequestToggle();
                LogLine("[MODO] BladeVR.ini: se salio del juego en el diorama: se vuelve a entrar.");
            }
        }
    }

    bool wasP = g_trkPlayer.inAttack;
    UpdateTrack(g_trkPlayer, player);
    if (!wasP && g_trkPlayer.inAttack) {
        ++g_attacksPlayer;
        // Ronda 43: el minimo entre dos ataques basicos del juego (A) iguales y
        // seguidos pasa a ser la cadencia de ese ataque; sin los de la
        // resistencia al minimo (*_bad_*, mas lentos) ni el siguiente (hay que
        // esperar a recuperarla).
        bool bad = std::strstr(g_trkPlayer.full, "_bad") != nullptr;
        if (!bad && !g_lastGameAttackBad && g_lastGameAttackAt >= 0.0 && now - g_lastGameAttackAt < kGameAttackGapSec) {
            double iv = now - g_lastGameAttackAt;
            if (std::strcmp(g_trkPlayer.full, g_lastGameAttackFull) == 0) NoteLearnedCadence(g_trkPlayer.full, iv);
        }
        g_lastGameAttackAt = now;
        g_lastGameAttackBad = bad;
        std::snprintf(g_lastGameAttackFull, sizeof(g_lastGameAttackFull), "%s", g_trkPlayer.full);
    }
    UpdateHandMotion(now);          // ronda 34: mandos hacia el cuerpo
    UpdateEnergyInstall(app, now);  // ronda 59
    UpdateSwing(player, now, fp);
    UpdatePunch(player, now, fp);   // ronda 31
    UpdateWalkWatch(player, now);
    UpdateRunGuard(player, now);
    UpdateFood(app, player, now, fp);
    UpdateBow(app, player, now, fp);   // ronda 50

    if (now - g_minuteStart >= 60.0) LogMinuteSummary(now);
}

} // namespace BladeVR
