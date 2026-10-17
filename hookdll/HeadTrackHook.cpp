#include "HeadTrackHook.h"
#include "BladeVRIni.h"
#include "HookLogger.h"
#include "OpenVRHook.h"
#include "FromWorldLocator.h"
#include "FovHook.h"
#include "Dx11Hook.h"
#include "StereoHook.h"
#include "DioramaHook.h"
#include "CombatVRHook.h"
#include "VrPadHook.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <cwchar>
#include <intrin.h>

namespace BladeVR {

// =====================================================================
// Seguimiento de cabeza 6DOF.
//
// Como funciona (ver HeadTrackHook.h para el resumen):
//  * La matriz de vista global fromWorld (4x4 doubles, FromWorldLocator)
//    la escribe UNA funcion por fotograma (la que contiene Blade.exe+0x72e55;
//    su inicio se localiza en tiempo de ejecucion con la tabla de unwind),
//    llamada 3 veces por fotograma en partida; la ultima (retorno
//    +0x5a81d) deja la matriz definitiva. Se hookea esa funcion y, tras la
//    ultima llamada, se reescribe fromWorld entera.
//  * Convencion de fromWorld: p_vista = (p_mundo - c) * M, vectores fila;
//    las COLUMNAS de M son los ejes de la camara en mundo (col0 derecha,
//    col1 abajo, col2 adelante) y t = M[12..14] = -(c * M). Mundo: X
//    derecha, Y ABAJO, Z adelante (diestro). OpenVR: X derecha, Y arriba,
//    Z atras (diestro) -> cambio de base (x, -y, -z).
//  * Rotacion: ejes del visor girados en Y para que la guinada de
//    recentrado coincida con la de la camara del juego (columna 2);
//    cabeceo y alabeo solo del visor. Traslacion: c del juego +
//    desplazamiento del visor (metros * unidades/m), recortado contra la
//    geometria y los objetos con el trazado de rayos del propio juego.
//  * Pose: la de WaitGetPoses (render pose), que ademas se adjunta al
//    Submit (Submit_TextureWithPose) para que el compositor reproyecte
//    respecto a la pose exacta con la que se dibujo.
//  * Submodo "camina hacia donde mira" (Supr): la vista es absoluta y un
//    lazo cerrado gira al personaje hasta la guinada del visor con
//    movimiento de raton (solo X) que llega UNICAMENTE al juego: un WM_INPUT
//    propio cuyo contenido sirve GetRawInputData, enganchada en la tabla de
//    importaciones de Blade.exe. El raton de Windows no se toca.
//  * Modo diorama (F5, DioramaHook.cpp): la camara de partida pasa a ser la
//    cabeza del jugador llevada a la maqueta (sin colision ni limite de
//    desplazamiento); Inicio vuelve a colocar la maqueta. El personaje se
//    mueve relativo a la mirada sobre la maqueta (Supr lo apaga; en el modo
//    "por la espalda" no hace falta): el joystick izquierdo se reinterpreta
//    con el esquema de la "camara 2" del propio juego (ver
//    HookedActionMove): sin enemigo activo, el personaje va hacia donde se
//    empuja respecto a la vista sobre la maqueta; con enemigo activo, el
//    control clasico del juego (combos). Si esos ganchos no se pueden poner,
//    el joystick se reinterpreta como direccion sobre la maqueta
//    (HookedGamepadProcess) y el lazo de Supr gira la camara del juego.
//  * 2.0: con un enemigo fijado la posicion de la camara del juego se
//    ancla a la posicion del jugador (StabilizeGameCamera: la cabeza respecto
//    a el suavizada y la camara detras segun hacia donde mira).
// =====================================================================

// --- offsets del juego -------------------------------------------------
static constexpr uintptr_t kFromWorldWriterRipOffset = 0x72e55;      // dentro de la funcion que escribe fromWorld
static constexpr uintptr_t kFromWorldFinalCallReturnOffset = 0x5a81d; // retorno de la ultima llamada por fotograma (partida)
// Camara clasica (la usan las cinematicas): Camera::Update (+0x5b440) sin la
// camara de persecucion nueva (+0x592d0, camara+0x258) termina con +0x72c00
// / +0x72a90 / +0x72b00 y la ultima retorna aqui. Solo sirve para el diorama
// (sin colision).
static constexpr uintptr_t kClassicCameraFinalReturnOffset = 0x5ba80;
// Proceso del mando del juego (Steam Input en +0x271000.. y SDL): ejes ya
// leidos en el dispositivo (+0x80 izq. X, +0x84 izq. Y con Y positiva hacia
// atras) -> acciones. Con la camara clasica el joystick izquierdo da
// direcciones digitales: arriba/abajo = avanzar/retroceder, izquierda/
// derecha = GIRAR (+0x27bfd0).
static constexpr uintptr_t kGamepadProcessOffset = 0x27b2c0;
static constexpr int kPadLeftX = 0x80;
static constexpr int kPadLeftY = 0x84;
// Control de la "camara 2" del juego (app+0x77c == 2, SetReworkedCamera(2)).
// +0x4f030 (app, persona) = movimiento relativo a la camara permitido: modo 2,
// la persona no esta en un estado especial (+0xb7130), sin enemigo activo
// (persona+0x2a48), la aplicacion no esta en "Menu", persona+0x2b74 == 0 y
// camara+0x4c4 != 3. Lo consultan el proceso del mando (+0x27b2c0: con "si"
// el joystick va como ejes analogicos; con "no", como las direcciones
// digitales de siempre, lo que da los combos), los manejadores de avanzar,
// retroceder y girar (con "si" escriben los ejes en persona+0x2b60+0x8c/+0x90:
// X derecha +, Y atras +) y +0x149950 (control de la persona, llamado desde
// +0x1484a0), que con los ejes calcula el rumbo atan2(-Y, X) + AngY - pi/2
// (AngY = camara+0x520, angulo de la direccion de la mirada en el convenio de
// +0x73a00: -atan2(x, z); "adelante" = AngY) y gira/avanza al personaje. La
// camara tambien llama a +0x4f030 (retornos 0x504c1 y 0x59577): esas
// llamadas conservan la respuesta del juego.
static constexpr uintptr_t kActionAllowedOffset = 0x4f030;
static constexpr uintptr_t kActionMoveOffset = 0x149950;
static constexpr uintptr_t kActionAllowedCameraReturn1 = 0x504c1;
static constexpr uintptr_t kActionAllowedCameraReturn2 = 0x59577;
static constexpr uintptr_t kAppPointerOffset = 0xF4A6A8;
static constexpr int kAppCameraMode = 0x77c;          // int
static constexpr int kAppCameraRotated = 0x3f;        // byte (lo borra +0x149950)
static constexpr int kAppCameraEntity = 0xe8;
static constexpr int kCameraAngY = 0x520;             // double
static constexpr int kCameraFollowYaw = 0x458;        // double (+0x149950 la acerca al personaje)
static constexpr int kCameraFollowPitch = 0x460;      // double
static constexpr int kAppPlayerEntity = 0xc8;          // -> "Player1"
// Las dos primeras llamadas por fotograma en partida (estados intermedios de
// la camara de seguimiento): no se reescribe tras ellas. Cualquier otro
// llamante (seleccion de personaje, cinematicas) se trata como definitivo.
static constexpr uintptr_t kFromWorldIntermediateReturn1 = 0x5a7d3;
static constexpr uintptr_t kFromWorldIntermediateReturn2 = 0x5a7f8;
// Las dos primeras de la camara clasica (y cinematicas): mismo patron.
static constexpr uintptr_t kClassicIntermediateReturn1 = 0x5ba38;
static constexpr uintptr_t kClassicIntermediateReturn2 = 0x5ba5c;
static constexpr uintptr_t kPoseFromWorldOffset = 0x98;   // fromWorld dentro del objeto de pose (param_1 del escritor)
static constexpr uintptr_t kGetLevelOffset = 0x7ceb0;       // FUN_14007ceb0() -> nivel
static constexpr uintptr_t kPointSectorOffset = 0x7b020;    // FUN_14007b020(nivel, p) -> sector o 0
static constexpr uintptr_t kRayCastOffset = 0x7ce20;        // FUN_14007ce20(nivel, desde, hasta, &impacto, 2, 1, filtro)

// --- parametros ----------------------------------------------------------
static constexpr double kPi = 3.14159265358979323846;
static constexpr double kWallMarginUnits = 200.0;       // 20 cm entre la camara y cualquier superficie
static constexpr double kMaxOffsetStepUnits = 25.0;     // limite de cambio del desplazamiento por fotograma
// (Se probo una distancia minima al personaje en tercera persona y se
// descarto: brazos y torso siguen atravesandose igualmente.)
static constexpr double kFollowGainPerFrame = 0.25;
static constexpr double kFollowDeadbandDeg = 0.3;
static constexpr long kFollowMaxCountsPerFrame = 400;
// Supr (camina hacia donde mira): el lazo compara la vista con hacia donde
// mira el PERSONAJE (la entidad, que es hacia donde anda), no con la camara
// del juego (la cabeza se mece con la animacion y en guardia va de lado).
// Con histeresis: quieto, el personaje solo se gira si la vista se aparta mas
// de kFollowStillEnterDeg (cada giro es una peticion de giro del juego, y
// pedirlas sin parar le hacia dar pasos); andando, sigue a la vista de cerca.
// Con un enemigo fijado no se gira (el juego lo encara al enemigo).
static constexpr double kFollowStillEnterDeg = 20.0;
static constexpr double kFollowStillExitDeg = 3.0;
static constexpr double kFollowMoveEnterDeg = 2.0;
static constexpr double kFollowMoveExitDeg = 0.7;
// El juego (SDL) solo acepta el raton con su ventana en primer plano: sin
// foco, los giros se pierden. Se avisa y, al recentrar o al usar el joystick,
// se intenta devolverle el foco.
static constexpr uint64_t kNoFocusWarnMs = 1000;
static constexpr uint64_t kRefocusRetryMs = 5000;
// En diorama el joystick izquierdo se lee como direccion sobre la maqueta
// (relativa a la mirada); el juego recibe "adelante" con la misma fuerza y el
// lazo gira al personaje hacia esa direccion (histeresis en grados).
static constexpr float kStickRemapMin = 0.25f;
static constexpr double kDioramaStickEnterDeg = 2.0;
static constexpr double kDioramaStickExitDeg = 0.5;
static constexpr uint64_t kStickHeadingTimeoutMs = 150;
static constexpr int kProbeDirCount = 14;
static const double kProbeDirs[kProbeDirCount][3] = {
    {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1},
    {0.57735,0.57735,0.57735}, {-0.57735,0.57735,0.57735}, {0.57735,-0.57735,0.57735}, {-0.57735,-0.57735,0.57735},
    {0.57735,0.57735,-0.57735}, {-0.57735,0.57735,-0.57735}, {0.57735,-0.57735,-0.57735}, {-0.57735,-0.57735,-0.57735} };

using PFN_GetLevel = long long(__fastcall*)();
using PFN_PointSector = long long(__fastcall*)(long long, const double*);
using PFN_RayCast = int(__fastcall*)(long long, const double*, const double*, double*, int, int, void*);
using PFN_Generic4 = unsigned long long(__fastcall*)(unsigned long long, unsigned long long, unsigned long long, unsigned long long);

// --- estado --------------------------------------------------------------
static std::atomic<int> g_mode{1};                 // 0 apagado, 1 seguimiento (activo por defecto)
static std::atomic<int> g_walkFollowsView{1};      // Supr (2.0: por defecto el personaje camina hacia donde se mira)
static std::atomic<int> g_dioramaSteer{1};         // Supr en diorama: moverse relativo a la mirada (activo por defecto)
static std::atomic<double> g_dioramaViewYaw{0.0};  // guinada de la vista sobre la maqueta (hilo del juego)
static std::atomic<bool> g_dioramaViewYawValid{false};
static std::atomic<double> g_stickHeading{0.0};    // direccion pedida con el joystick (camara clasica)
static std::atomic<uint64_t> g_stickHeadingTick{0};
static std::atomic<int> g_unitsPerMeter{1000};     // escala 6DOF
// Ronda 26: ojos adelantados (cm) sobre la camara de primera persona del juego.
constexpr int kEyeForwardDefaultCm = 10;
constexpr int kEyeForwardMaxCm = 20;
static std::atomic<int> g_eyeForwardCm{kEyeForwardDefaultCm};
// Ronda 46: altura anadida a los ojos (cm), guardada en BladeVR_altura.txt (ronda 54: BladeVR.ini).
// Ronda 47: solo en el modo sin cuerpo (modo de combate 3, desde la ronda 48 el
// de las manos flotantes); de serie 45 cm (lo
// que el usuario subio con la cama y se sentia a su altura).
// Ronda 49 (pedido del usuario): de serie 30 cm, para no ser mas alto que los
// caballeros. Ronda 54 (pedido del usuario): de serie 0, la altura del modo con
// el cuerpo visible; se guarda en BladeVR.ini.
static constexpr int kEyeLiftMaxCm = 80;
static constexpr int kEyeLiftDefaultCm = 0;
static std::atomic<int> g_eyeLiftCm{kEyeLiftDefaultCm};
static std::atomic<bool> g_eyeLiftLoaded{false};
static std::atomic<int> g_mouseCountsPerDegreeX10{120};
static std::atomic<bool> g_insWasDown{false};
static std::atomic<bool> g_delWasDown{false};
static std::atomic<bool> g_homeWasDown{false};
// Recentrado de posicion (Inicio). Se pide desde el hilo de Present y se
// aplica en el hilo del juego, dentro del hook de fromWorld.
static std::atomic<int> g_recenterRequested{0};
// La ultima reescritura de fromWorld fue la camara del diorama.
static std::atomic<bool> g_dioramaCamera{false};

static std::mutex g_stateMutex;
static double g_yawRef = 0.0;
static bool g_haveYawRef = false;
static double g_posRef[3] = {0.0, 0.0, 0.0};
static bool g_havePosRef = false;
static double g_appliedOff[3] = {0.0, 0.0, 0.0};
static bool g_haveAppliedOff = false;
static bool g_followHaveOffset = false;
static double g_followYawOffset = 0.0;
static double g_followYawError = 0.0;
static double g_followAccX = 0.0;
static bool g_followErrorFresh = false;   // error recalculado desde el ultimo paso
static bool g_followTurning = false;      // Supr: girando (fuera de la banda de histeresis)
static bool g_followUsesEntity = false;   // el error se mide con la guinada de la entidad
static uint64_t g_followFarSince = 0;      // girando con mas de 15 grados de error desde... (seguro)
// Ronda 29: camaras que no son la de partida (cinematicas, seleccion de
// personaje, menu): con Supr la vista era absoluta (visor + desfase fijo) y no
// seguia a la camara de la escena, que miraba al personaje desde otro lado; y
// Supr intentaba girar al personaje (789 giros en un minuto de cinematica).
// Ahora la vista sigue la guinada de esa camara mas el giro del visor desde
// que empezo la escena (al empezar, el visor mira hacia donde mira la camara;
// Inicio lo vuelve a alinear). Solo si no hubo camara de partida en
// kFixedCamAfterMs (alguna reescritura suelta en partida no cuenta).
constexpr uint64_t kFixedCamAfterMs = 200;
static uint64_t g_lastGameplayCamMs = 0;   // ultima camara de partida recien escrita
static bool g_fixedCamActive = false;
static double g_fixedCamYawRef = 0.0;      // guinada del visor al empezar la escena

static bool g_entityYawDisabled = false;  // la guinada de la entidad no responde: se vuelve a la camara
// Ronda 14: el seguro saltaba en falso (muriendo, cargando o esprintando) y
// dejaba Supr midiendo con la camara del juego el resto de la partida (y sin
// giro directo). Ahora solo cuenta quieto, en partida y con giros de raton, y
// se vuelve a la entidad a los 10 s o con una escena nueva.
static uint64_t g_entityYawDisabledAt = 0;
static constexpr uint64_t kEntityYawRetryMs = 10000;
static bool g_dioramaSteering = false;    // diorama: corrigiendo (fuera de la banda muerta)
static bool g_applyFromWriter = false;    // hilo del juego: la llamada viene del escritor (camara recien escrita)
static double g_lastOffsetM[3] = {0.0, 0.0, 0.0};
// Ultima transformacion seguimiento -> juego (para los mandos, ver
// HeadTrackGetTrackingToGameMap). Se escribe con g_stateMutex tomado.
static std::mutex g_mapMutex;
static TrackingToGameMap g_trackMap = {};
static bool g_trackMapValid = false;

static std::mutex g_renderPoseMutex;
static float g_renderPose[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
static bool g_renderPoseValid = false;

static PFN_Generic4 g_originalFromWorldWriter = nullptr;
static uintptr_t g_fromWorldWriterStart = 0;
static bool g_fromWorldWriterHookInstalled = false;

// contadores del resumen del log (un minuto)
static std::atomic<uint64_t> g_fwCallsSec{0};
static std::atomic<uint64_t> g_fwWritesSec{0};
static std::mutex g_lastWrittenMutex;
static double g_lastWritten[16] = {};                   // ultima fromWorld que escribimos (para detectar si el juego la piso)
static bool g_haveLastWritten = false;
// Matriz del juego sobre la que se aplico esa ultima escritura, con que
// modo y en que fotograma. Si el juego no vuelve a escribir fromWorld
// (menu principal: camara estatica), la matriz sigue siendo "nuestra" pero
// con la pose del visor de hace muchos fotogramas; hay que rehacerla desde
// la base con la pose actual.
static double g_lastBase[16] = {};
static bool g_lastPositional = false;
static bool g_lastDioramaOk = false;
static unsigned long long g_lastWrittenFrame = 0;

static bool IsOurLastWrittenMatrix(const double fw[16]) {
    std::lock_guard<std::mutex> lock(g_lastWrittenMutex);
    if (!g_haveLastWritten) return false;
    for (int i = 0; i < 16; ++i) { if (fw[i] != g_lastWritten[i]) return false; }
    return true;
}
static std::atomic<uint64_t> g_fwLateRewritesSec{0};  // reescrituras hechas en la raiz de culling
// Diorama con la camara de las cinematicas: ademas de la camara de partida
// (+0x5a81d) y la clasica (+0x5ba80), las cinematicas llaman al escritor
// desde otros sitios o reescriben fromWorld despues (camaras fijas, p. ej.
// +0x58d34). Con el diorama puesto vale cualquier camara definitiva. Con la
// camara de una cinematica (escritor llamado desde otro sitio o desde la
// clasica, que solo usan las cinematicas, en el ultimo medio segundo) la
// vista del diorama sigue esa camara (DioramaMarkCinematicCall; ver
// DioramaComputeCamera).
static constexpr uint64_t kCineWriterRecentMs = 500;
static uint64_t g_cineWriterTick = 0;         // ultimo escritor de cinematica (hilo del juego)
static bool g_cineCall = false;               // la llamada en curso es de una cinematica

static bool CinematicRecent() {
    return g_cineWriterTick != 0 && GetTickCount64() - g_cineWriterTick < kCineWriterRecentMs;
}

static std::atomic<uint64_t> g_fwStaleRefreshSec{0};  // rehechas por ser de un fotograma anterior (camara estatica)
static std::atomic<uint64_t> g_cullCallsSec{0};       // llamadas a la raiz de culling
static std::atomic<uint64_t> g_cullOtherPoseSec{0};   // ... cuyo bloque NO es el objeto de pose global
static std::atomic<uint64_t> g_noPose{0};
static std::atomic<uint64_t> g_rayClampSec{0};
// Camara del juego estable con un enemigo fijado (ver StabilizeGameCamera).
static constexpr double kCamFiltMinCutoffHz = 1.0;
static constexpr double kCamFiltBeta = 0.01;           // Hz por mm/s
static constexpr double kCamFiltDCutoffHz = 1.0;
static constexpr double kCamFiltResetUnits = 1000.0;   // salto de mas de 1 m: carga, teletransporte
static constexpr double kCamFiltBlendSec = 0.3;
// Ronda 19: al fijar, la estabilizacion entra en 0,06 s (con 0,3 s el temblor
// de la camara del juego, que empieza al fijar, se veia el primer medio segundo).
static constexpr double kCamFiltBlendInSec = 0.06;
static bool g_camFiltInit = false;
static double g_camFilt[3] = { 0, 0, 0 };
static double g_camFiltD[3] = { 0, 0, 0 };
static double g_camFiltT = 0.0;
static double g_camFiltW = 0.0;                        // 0 = la del juego, 1 = estabilizada
static double g_camFiltOff[3] = { 0, 0, 0 };           // ultimo ajuste aplicado (para rehacer una camara vieja)
// Ronda 12: con un enemigo fijado el temblor era horizontal (hasta 0,4 m de un
// fotograma a otro): la camara de persecucion gira alrededor de su objetivo
// (TPos) con una guinada que apunta al enemigo. La posicion se "clava" detras
// del objetivo segun hacia donde mira el personaje (estable), con la distancia
// y la altura del juego, y despues pasa por el filtro.
static constexpr int kCameraViewType = 0x4c4;          // int (Camera.PViewType): 3 = primera persona
static constexpr int kCameraTargetPos = 0x568;         // double[3]: TPos, objetivo de la camara de persecucion (+0x300 de +0x268)
static double g_pinDist = 0.0;                         // distancia horizontal camara-objetivo suavizada (unidades)
static bool g_pinValid = false;
static bool g_pinFwdValid = false;   // ronda 37: adelante del personaje con que se ancla (para los ojos adelantados)
static double g_pinFwd[2] = { 0.0, 1.0 };
// Ronda 13: clavar la camara no basto (temblaba igual): el que salta es el
// propio objetivo (TPos = ViewPoint de la cabeza, +0x5ca30). Con un enemigo
// fijado la posicion es la del jugador en el juego (estable) + la de la cabeza
// respecto a el muy suavizada (0,4 s) + la distancia de la camara detras.
static constexpr double kHeadOffsetSmoothSec = 0.4;
static bool g_headOffValid = false;
static double g_headOff[3] = { 0, 0, 0 };              // (camara del juego - jugador) sin la orbita, suavizado
// Ronda 14: quieto la anclada ya no temblaba (1-5 mm), pero moviendose en
// combate la posicion del jugador en el juego avanza a tirones (30-130 mm de
// segunda diferencia a 2-5 m/s: el juego la mueve a saltos, no en cada
// fotograma). Se suaviza con una media de los 4 ultimos fotogramas con pesos
// 1-2-2-1 (quita del todo los saltos de 20, 30 y 40 por segundo, sin pasarse
// al parar; 1,5 fotogramas de retraso).
static bool g_perHistValid = false;
static double g_perHist[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };   // [0] anterior, [1] y [2] mas viejos

static std::atomic<uint64_t> g_pushOutSec{0};
static std::atomic<uint64_t> g_turnsSentSec{0};
// Foco de la ventana del juego (hilo de Present).
static uint64_t g_noFocusSince = 0;
static bool g_noFocusWarned = false;
static uint64_t g_lastRefocusTry = 0;
static std::atomic<bool> g_gameFocused{true};
static std::atomic<int> g_refocusRequest{0};
static std::atomic<uint64_t> g_lastLogTick{0};

static uintptr_t ModuleBase() {
    static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    return base;
}

static std::string DescribeAddress(uintptr_t addr) {
    std::ostringstream o;
    uintptr_t base = ModuleBase();
    if (addr >= base && addr < base + 0x4000000) {
        o << "Blade.exe+0x" << std::hex << (addr - base) << std::dec;
        return o.str();
    }
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(addr), &h);
    char modName[MAX_PATH] = {};
    if (h && GetModuleFileNameA(h, modName, MAX_PATH)) {
        const char* slash = strrchr(modName, '\\');
        o << (slash ? slash + 1 : modName) << "+0x" << std::hex << (addr - reinterpret_cast<uintptr_t>(h)) << std::dec;
    } else {
        o << "0x" << std::hex << addr << std::dec;
    }
    return o.str();
}

// --- lecturas/escrituras seguras (solo POD + __try, regla C2712) ---------
static bool SafeReadMatrix16(const double* src, double out[16]) {
    __try {
        for (int i = 0; i < 16; ++i) out[i] = src[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeWriteMatrix16(double* dst, const double src[16]) {
    __try {
        for (int i = 0; i < 16; ++i) dst[i] = src[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafePointSector(const double p[3], long long* outSector) {
    __try {
        uintptr_t base = ModuleBase();
        PFN_GetLevel getLevel = reinterpret_cast<PFN_GetLevel>(base + kGetLevelOffset);
        PFN_PointSector pointSector = reinterpret_cast<PFN_PointSector>(base + kPointSectorOffset);
        long long level = getLevel();
        if (!level) return false;
        *outSector = pointSector(level, p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafePointInsideLevel(const double p[3], bool* outInside) {
    long long sector = 0;
    if (!SafePointSector(p, &sector)) return false;
    *outInside = (sector != 0);
    return true;
}

static bool SafeRayCast(const double from[3], const double to[3], bool* outHit, double hit[3]) {
    __try {
        uintptr_t base = ModuleBase();
        PFN_GetLevel getLevel = reinterpret_cast<PFN_GetLevel>(base + kGetLevelOffset);
        PFN_RayCast rayCast = reinterpret_cast<PFN_RayCast>(base + kRayCastOffset);
        long long level = getLevel();
        if (!level) return false;
        unsigned long long filter[3] = {0, 0, 0};
        double h[3] = {0.0, 0.0, 0.0};
        int r = rayCast(level, from, to, h, 2, 1, filter);
        *outHit = (r != 0);
        hit[0] = h[0];
        hit[1] = h[1];
        hit[2] = h[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ProbeClearAt(const double c[3], const double off[3], double s) {
    double p[3] = { c[0] + off[0] * s, c[1] + off[1] * s, c[2] + off[2] * s };
    // El destino debe estar dentro del nivel. Hasta la ronda 28 tambien en el
    // MISMO sector que la camara ("si la cabeza asoma al otro lado de un
    // portal el culling raiz sigue en el sector viejo y la imagen se va a
    // negro"), pero los sectores de partida salen de la posicion de cada ojo
    // (ver InsideLevelFraction): cada recorte en otro sector era un tiron en
    // puertas y escaleras (145-596 por minuto, todos de este tipo).
    long long sc = 0, sp = 0;
    if (SafePointSector(c, &sc) && SafePointSector(p, &sp)) {
        if (sp == 0) return false;
    }
    bool hit = false;
    double h[3];
    if (SafeRayCast(c, p, &hit, h) && hit) {
        // impacto antes de llegar al destino (con tolerancia)
        double dx = h[0] - c[0], dy = h[1] - c[1], dz = h[2] - c[2];
        double ox = p[0] - c[0], oy = p[1] - c[1], oz = p[2] - c[2];
        if (dx * dx + dy * dy + dz * dz < ox * ox + oy * oy + oz * oz - 1e-6) return false;
    }
    for (int k = 0; k < kProbeDirCount; ++k) {
        const double* d = kProbeDirs[k];
        double q[3] = { p[0] + d[0] * kWallMarginUnits, p[1] + d[1] * kWallMarginUnits, p[2] + d[2] * kWallMarginUnits };
        if (SafeRayCast(p, q, &hit, h) && hit) return false;
    }
    return true;
}

static void PushOutFromObstacles(double p[3]) {
    for (int pass = 0; pass < 2; ++pass) {
        double push[3] = {0.0, 0.0, 0.0};
        bool any = false;
        for (int k = 0; k < kProbeDirCount; ++k) {
            const double* d = kProbeDirs[k];
            double q[3] = { p[0] + d[0] * kWallMarginUnits, p[1] + d[1] * kWallMarginUnits, p[2] + d[2] * kWallMarginUnits };
            bool hit = false;
            double h[3];
            if (SafeRayCast(p, q, &hit, h) && hit) {
                double dist = (h[0] - p[0]) * d[0] + (h[1] - p[1]) * d[1] + (h[2] - p[2]) * d[2];
                if (dist < 0.0) dist = 0.0;
                double need = kWallMarginUnits - dist;
                if (need > 0.0) {
                    push[0] -= d[0] * need;
                    push[1] -= d[1] * need;
                    push[2] -= d[2] * need;
                    any = true;
                }
            }
        }
        if (!any) return;
        double np[3] = { p[0] + push[0], p[1] + push[1], p[2] + push[2] };
        bool inside = false;
        if (SafePointInsideLevel(np, &inside) && !inside) return;
        p[0] = np[0]; p[1] = np[1]; p[2] = np[2];
        g_pushOutSec.fetch_add(1, std::memory_order_relaxed);
    }
}

// Devuelve la fraccion del desplazamiento que queda (1 = sin recortar).
static double ClampOffsetToLevel(const double c[3], double off[3]) {
    double len = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
    if (len < 1e-3) return 1.0;
    if (ProbeClearAt(c, off, 1.0)) return 1.0;
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 6; ++i) {
        double mid = 0.5 * (lo + hi);
        if (ProbeClearAt(c, off, mid)) lo = mid; else hi = mid;
    }
    off[0] *= lo; off[1] *= lo; off[2] *= lo;
    g_rayClampSec.fetch_add(1, std::memory_order_relaxed);
    return lo;
}

// Ronda 29: fraccion (0..1) de 'off' desde 'from' que queda DENTRO DEL NIVEL
// (en algun sector). La ronda 28 exigia el MISMO sector que la camara del
// juego (SameSectorFraction) y frenaba los ojos adelantados en cada portal
// (165-307 fotogramas por minuto): al cruzar una puerta los ojos se paraban
// y luego saltaban 10 cm, tirones al andar en llano. Otro sector no es un
// problema: B_Map::Render busca los sectores de partida con la posicion del
// bloque de pose (+0x7aa90 con pose+0x68, llamada en +0x81017), y esa
// posicion es la de cada ojo (HeadTrackSyncCullingCamera y las pasadas por
// ojo la escriben). Fuera del nivel (sector 0, dentro de una pared o de un
// escalon) si: ahi se para en el borde.
static double InsideLevelFraction(const double from[3], const double off[3]) {
    long long sp = 0;
    double p[3] = { from[0] + off[0], from[1] + off[1], from[2] + off[2] };
    if (!SafePointSector(p, &sp) || sp != 0) return 1.0;
    long long s0 = 0;
    if (!SafePointSector(from, &s0) || s0 == 0) return 1.0;   // el origen ya esta fuera: no hay borde que buscar
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 6; ++i) {
        double mid = 0.5 * (lo + hi);
        double q[3] = { from[0] + off[0] * mid, from[1] + off[1] * mid, from[2] + off[2] * mid };
        if (SafePointSector(q, &sp) && sp != 0) lo = mid;
        else hi = mid;
    }
    return lo;
}

// Muelle criticamente amortiguado (SmoothDamp): x hacia target en ~smoothSec.
static void SmoothDampStep(double* x, double* vel, double target, double smoothSec, double dt) {
    double omega = 2.0 / smoothSec;
    double t = omega * dt;
    double ex = 1.0 / (1.0 + t + 0.48 * t * t + 0.235 * t * t * t);
    double change = *x - target;
    double temp = (*vel + omega * change) * dt;
    *vel = (*vel - omega * temp) * ex;
    *x = target + (change + temp) * ex;
}

// Ronda 28: al recortarse el desplazamiento del visor (pared, otro sector) la
// fraccion baja al momento (seguridad); al soltarse, vuelve con un muelle
// (antes, con el tope de 25 mm por fotograma: una rampa de 1,5 m/s que
// arrancaba y paraba de golpe, en escaleras y puertas).
static double QpcSeconds();
constexpr double kClampReleaseSec = 0.15;
static double g_clampF = 1.0, g_clampFVel = 0.0, g_clampFT = 0.0;

static double EasedClampFactor(double lo) {
    if (g_applyFromWriter) {
        double now = QpcSeconds();
        double dt = now - g_clampFT;
        g_clampFT = now;
        if (lo <= g_clampF || dt <= 0.0 || dt > 0.25) {
            g_clampF = lo;
            g_clampFVel = 0.0;
        } else {
            SmoothDampStep(&g_clampF, &g_clampFVel, lo, kClampReleaseSec, dt);
            if (g_clampF > lo) { g_clampF = lo; g_clampFVel = 0.0; }
            if (lo - g_clampF < 1e-4) { g_clampF = lo; g_clampFVel = 0.0; }
        }
    }
    return g_clampF < lo ? g_clampF : lo;
}

static void RateLimitOffset(double off[3]) {
    if (!g_haveAppliedOff) {
        g_appliedOff[0] = off[0]; g_appliedOff[1] = off[1]; g_appliedOff[2] = off[2];
        g_haveAppliedOff = true;
        return;
    }
    double dx = off[0] - g_appliedOff[0], dy = off[1] - g_appliedOff[1], dz = off[2] - g_appliedOff[2];
    double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len > kMaxOffsetStepUnits) {
        double s = kMaxOffsetStepUnits / len;
        dx *= s; dy *= s; dz *= s;
    }
    g_appliedOff[0] += dx; g_appliedOff[1] += dy; g_appliedOff[2] += dz;
    off[0] = g_appliedOff[0]; off[1] = g_appliedOff[1]; off[2] = g_appliedOff[2];
}

static void RotateY(double v[3], double c, double s) {
    double x = v[0] * c + v[2] * s;
    double z = -v[0] * s + v[2] * c;
    v[0] = x;
    v[2] = z;
}

static bool SafeReadCullBlock(long long param_2, double outRot[12], double outPos[6]) {
    __try {
        const double* rot = reinterpret_cast<const double*>(param_2 + 0x98);
        const double* pos = reinterpret_cast<const double*>(param_2 + 0x68);
        for (int i = 0; i < 12; ++i) outRot[i] = rot[i];
        for (int i = 0; i < 6; ++i) outPos[i] = pos[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static double g_savedCullPos[6] = {};      // posicion (+0x68) y angulos (+0x80..) del juego, solo hilo del juego
static bool g_savedCullPosValid = false;

static bool SafeWritePos6(long long param_2, const double pos[6]) {
    __try {
        double* dPos = reinterpret_cast<double*>(param_2 + 0x68);
        for (int i = 0; i < 6; ++i) dPos[i] = pos[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeWriteCullBlock(long long param_2, const double rot[12], const double pos[6]) {
    __try {
        double* dRot = reinterpret_cast<double*>(param_2 + 0x98);
        double* dPos = reinterpret_cast<double*>(param_2 + 0x68);
        for (int i = 0; i < 12; ++i) dRot[i] = rot[i];
        for (int i = 0; i < 6; ++i) dPos[i] = pos[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


static double WrapPi(double a) {
    while (a > kPi) a -= 2.0 * kPi;
    while (a < -kPi) a += 2.0 * kPi;
    return a;
}

// --- giro del personaje sin tocar el raton de Windows -----------------------
// SDL (enlazado en Blade.exe) lee el raton en modo relativo con WM_INPUT +
// GetRawInputData (Blade.exe+0x40bea6, dentro de su WindowProc) y lo pasa a
// SDL_SendMouseMotion. Para girar al personaje se manda a la ventana del
// juego un WM_INPUT con un "handle" propio (etiqueta en los 32 bits altos,
// desplazamiento X en los bajos) y la GetRawInputData enganchada en la tabla
// de importaciones de Blade.exe devuelve para el un movimiento relativo de
// raton. Solo lo ve el juego: el cursor de Windows no se mueve. SDL lo ignora
// por si solo cuando no esta en modo relativo (menus) o la ventana no tiene
// el foco.
static constexpr unsigned long long kFakeRawInputTag = 0xB1ADull;
using PFN_GetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static PFN_GetRawInputData g_originalGetRawInputData = nullptr;
static PFN_GetRawInputData* g_rawInputImportSlot = nullptr;
static std::atomic<bool> g_rawTurnReady{false};

static UINT WINAPI HookedGetRawInputData(HRAWINPUT hRawInput, UINT command, LPVOID data, PUINT size, UINT headerSize) {
    unsigned long long v = reinterpret_cast<unsigned long long>(hRawInput);
    if ((v >> 32) != kFakeRawInputTag) {
        return g_originalGetRawInputData(hRawInput, command, data, size, headerSize);
    }
    if (!size || headerSize != sizeof(RAWINPUTHEADER) || (command != RID_INPUT && command != RID_HEADER)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return static_cast<UINT>(-1);
    }
    UINT need = (command == RID_HEADER) ? static_cast<UINT>(sizeof(RAWINPUTHEADER))
                                        : static_cast<UINT>(sizeof(RAWINPUT));
    if (!data) { *size = need; return 0; }
    if (*size < need) {
        *size = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return static_cast<UINT>(-1);
    }
    RAWINPUT ri;
    std::memset(&ri, 0, sizeof(ri));
    ri.header.dwType = RIM_TYPEMOUSE;
    ri.header.dwSize = sizeof(RAWINPUT);
    ri.header.wParam = RIM_INPUTSINK;
    ri.data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
    ri.data.mouse.lLastX = static_cast<LONG>(static_cast<int32_t>(static_cast<uint32_t>(v & 0xFFFFFFFFull)));
    std::memcpy(data, &ri, need);
    return need;
}

// Hueco de la tabla de importaciones de 'module' para dll!func (por nombre).
static void** FindImportSlot(uintptr_t module, const char* dll, const char* func) {
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const IMAGE_NT_HEADERS64* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    const IMAGE_IMPORT_DESCRIPTOR* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(module + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(module + desc->Name), dll) != 0) continue;
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        const IMAGE_THUNK_DATA64* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(module + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA64* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(module + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const IMAGE_IMPORT_BY_NAME* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(module + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), func) == 0) {
                return reinterpret_cast<void**>(&slots->u1.Function);
            }
        }
    }
    return nullptr;
}

static bool InstallRawTurnHook() {
    if (g_rawTurnReady.load(std::memory_order_relaxed)) return true;
    uintptr_t base = ModuleBase();
    void** slot = base ? FindImportSlot(base, "USER32.dll", "GetRawInputData") : nullptr;
    if (!slot) {
        HookLogger::Instance().Line("[HEAD_TRACK] ERROR: no se encontro GetRawInputData en las importaciones del juego (Supr no girara al personaje).");
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLogger::Instance().Line("[HEAD_TRACK] ERROR: no se pudo escribir en la tabla de importaciones (Supr no girara al personaje).");
        return false;
    }
    g_originalGetRawInputData = reinterpret_cast<PFN_GetRawInputData>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HookedGetRawInputData));
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    g_rawInputImportSlot = reinterpret_cast<PFN_GetRawInputData*>(slot);
    g_rawTurnReady.store(true, std::memory_order_relaxed);
    HookLogger::Instance().Line("[HEAD_TRACK] Giro del personaje (Supr) por entrada interna del juego, sin mover el raton de Windows.");
    return true;
}

static void UninstallRawTurnHook() {
    if (!g_rawTurnReady.exchange(false, std::memory_order_relaxed) || !g_rawInputImportSlot) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_rawInputImportSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        InterlockedExchangePointer(reinterpret_cast<void**>(g_rawInputImportSlot),
                                   reinterpret_cast<void*>(g_originalGetRawInputData));
        VirtualProtect(g_rawInputImportSlot, sizeof(void*), oldProtect, &oldProtect);
    }
}

static bool SendTurnToGame(long dx) {
    if (!g_rawTurnReady.load(std::memory_order_relaxed)) return false;
    HWND hwnd = static_cast<HWND>(GetGameWindow());
    if (!hwnd) return false;
    unsigned long long tagged = (kFakeRawInputTag << 32) | static_cast<unsigned long long>(static_cast<uint32_t>(static_cast<int32_t>(dx)));
    return PostMessageW(hwnd, WM_INPUT, RIM_INPUTSINK, static_cast<LPARAM>(tagged)) != 0;
}

// --- submodo "camina hacia donde mira": lazo cerrado -----------------------
// Solo actua cuando el juego acaba de escribir su camara (el error es de este
// fotograma): en menus y pausas no se acumula nada.
static void WalkFollowStep() {
    if (!g_followErrorFresh) return;
    g_followErrorFresh = false;
    double errDeg = g_followYawError * 180.0 / kPi;
    if (std::abs(errDeg) < kFollowDeadbandDeg) return;
    double k = g_mouseCountsPerDegreeX10.load(std::memory_order_relaxed) / 10.0;
    // Girar la cabeza a la izquierda => guinada decrece => raton a la izquierda (dx < 0): dx = +err * k.
    g_followAccX += errDeg * kFollowGainPerFrame * k;
    long dx = static_cast<long>(g_followAccX);
    if (dx == 0) return;
    if (dx > kFollowMaxCountsPerFrame) dx = kFollowMaxCountsPerFrame;
    if (dx < -kFollowMaxCountsPerFrame) dx = -kFollowMaxCountsPerFrame;
    g_followAccX -= dx;
    if (SendTurnToGame(dx)) g_turnsSentSec.fetch_add(1, std::memory_order_relaxed);
}

// --- foco de la ventana del juego ---------------------------------------------
// Hilo aparte: restaurar la ventana si esta minimizada y ponerla en primer
// plano (enganchando la entrada al hilo de la ventana que tiene el foco, que
// es lo que Windows pide para dejar cambiar el primer plano). Los mensajes
// van a la ventana desde este hilo, nunca desde dentro de Present.
static DWORD WINAPI RefocusThread(LPVOID) {
    HWND hwnd = static_cast<HWND>(GetGameWindow());
    if (!hwnd) return 0;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
    return 0;
}

static void TryRefocus() {
    HANDLE h = CreateThread(nullptr, 0, &RefocusThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

// Hilo de Present, cada fotograma: si la ventana del juego no esta en primer
// plano, SDL descarta el raton (WM_INPUT solo con foco) y Supr no puede girar.
static void CheckGameFocus() {
    uint64_t now = GetTickCount64();
    HWND hwnd = static_cast<HWND>(GetGameWindow());
    bool focused = !hwnd || GetForegroundWindow() == hwnd;
    g_gameFocused.store(focused, std::memory_order_relaxed);
    bool wantRefocus = g_refocusRequest.exchange(0, std::memory_order_relaxed) != 0;
    if (focused) {
        if (g_noFocusWarned) {
            HookLogger::Instance().Line("[HEAD_TRACK] Foco: la ventana del juego vuelve a estar en primer plano (Supr gira otra vez).");
        }
        g_noFocusSince = 0;
        g_noFocusWarned = false;
        return;
    }
    if (g_noFocusSince == 0) g_noFocusSince = now;
    bool follow = g_mode.load(std::memory_order_relaxed) == 1 && g_walkFollowsView.load(std::memory_order_relaxed) == 1;
    if (!g_noFocusWarned && follow && now - g_noFocusSince >= kNoFocusWarnMs) {
        g_noFocusWarned = true;
        HookLogger::Instance().Line("[HEAD_TRACK] Foco: la ventana del juego NO esta en primer plano: el juego ignora los giros de Supr "
                                    "(el personaje no sigue a la vista). Recentrar (clic del joystick derecho) le devuelve el foco.");
        OpenVRShowToast(L"El juego no tiene el foco", L"Clic en el joystick derecho para recuperarlo", 4000);
    }
    // Al recentrar, o jugando con el joystick izquierdo, se le devuelve el foco.
    float sent[2];
    bool stick = VrPadGetLeftStick(sent) && (std::fabs(sent[0]) > 0.3f || std::fabs(sent[1]) > 0.3f);
    if ((wantRefocus || (stick && now - g_noFocusSince > 2000)) && now - g_lastRefocusTry > (wantRefocus ? 500 : kRefocusRetryMs)) {
        g_lastRefocusTry = now;
        TryRefocus();
    }
}

// --- teclas y log (hilo de Present) --------------------------------------
static void ResetReferences() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_haveYawRef = false;
    g_havePosRef = false;
    g_haveAppliedOff = false;
    g_followHaveOffset = false;
    g_followYawError = 0.0;
    g_followAccX = 0.0;
    g_followErrorFresh = false;
    g_entityYawDisabled = false;
    g_followFarSince = 0;
}
bool HeadTrackGetCenterYawRad(double* outYaw) {
    if (!outYaw) return false;
    if (g_mode.load(std::memory_order_relaxed) != 1) return false;
    if (g_walkFollowsView.load(std::memory_order_relaxed) == 1) return false;
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_haveYawRef) return false;
    *outYaw = g_yawRef;
    return true;
}

void HeadTrackResetReferences() {
    ResetReferences();
    DioramaRequestAnchor();   // nivel nuevo: la maqueta se vuelve a colocar
    HookLogger::Instance().Line("[HEAD_TRACK] Nueva escena: referencias del visor recentradas.");
}

// Ronda 74: Insert dos veces = ResetReferences dos veces con el seguimiento
// apagado y vuelto a encender; las referencias se toman igual en el siguiente
// fotograma, asi que basta con una.
void HeadTrackRecenterLikeInsert() {
    ResetReferences();
    HookLogger::Instance().Line("[HEAD_TRACK] Vista recentrada (como Insert dos veces).");
}

static void CheckKeys() {
    bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (ins && !g_insWasDown.exchange(ins, std::memory_order_relaxed)) {
        int next = 1 - g_mode.load(std::memory_order_relaxed);
        ResetReferences();
        g_mode.store(next, std::memory_order_relaxed);
        HookLogger::Instance().Line(next ? "[HEAD_TRACK] Insert: seguimiento de cabeza ACTIVO (recentrado)"
                                         : "[HEAD_TRACK] Insert: seguimiento de cabeza APAGADO");
    } else if (!ins) {
        g_insWasDown.store(false, std::memory_order_relaxed);
    }

    bool home = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (home && !g_homeWasDown.exchange(home, std::memory_order_relaxed)) {
        g_recenterRequested.store(1, std::memory_order_relaxed);
        g_refocusRequest.store(1, std::memory_order_relaxed);
    } else if (!home) {
        g_homeWasDown.store(false, std::memory_order_relaxed);
    }

    bool del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    if (del && !g_delWasDown.exchange(del, std::memory_order_relaxed)) {
        if (DioramaEnabled()) {
            // En diorama, Supr cambia su propio ajuste (moverse relativo a la
            // mirada sobre la maqueta) y deja el de la vista normal como estaba.
            int next = 1 - g_dioramaSteer.load(std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_dioramaSteering = false;
                g_followYawError = 0.0;
                g_followAccX = 0.0;
                g_followErrorFresh = false;
            }
            g_dioramaSteer.store(next, std::memory_order_relaxed);
            HookLogger::Instance().Line(next ? "[HEAD_TRACK] Supr (diorama): el personaje se mueve RELATIVO A LA MIRADA sobre la maqueta"
                                             : "[HEAD_TRACK] Supr (diorama): el personaje se mueve relativo a la camara del juego");
        } else {
            int next = 1 - g_walkFollowsView.load(std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_followHaveOffset = false;
                g_followYawError = 0.0;
                g_followAccX = 0.0;
                g_followErrorFresh = false;
            }
            g_walkFollowsView.store(next, std::memory_order_relaxed);
            HookLogger::Instance().Line(next ? "[HEAD_TRACK] Supr: el personaje CAMINA HACIA DONDE MIRA el visor"
                                             : "[HEAD_TRACK] Supr: personaje INDEPENDIENTE de la camara (vista libre)");
        }
    } else if (!del) {
        g_delWasDown.store(false, std::memory_order_relaxed);
    }

}

// Una linea por minuto para los informes de fallos.
static void MaybeLogRate() {
    uint64_t now = GetTickCount64();
    uint64_t last = g_lastLogTick.load(std::memory_order_relaxed);
    if (last == 0) { g_lastLogTick.store(now, std::memory_order_relaxed); return; }
    if (now - last < 60000) return;
    g_lastLogTick.store(now, std::memory_order_relaxed);
    std::ostringstream o;
    o << "[HEAD_TRACK] ultimo minuto: modo=" << g_mode.load(std::memory_order_relaxed)
      << " caminaHaciaVista=" << g_walkFollowsView.load(std::memory_order_relaxed)
      << " desplazamiento(m)=[" << g_lastOffsetM[0] << "," << g_lastOffsetM[1] << "," << g_lastOffsetM[2] << "]"
      << " unidades/m=" << g_unitsPerMeter.load(std::memory_order_relaxed)
      << " | ojos adelantados " << g_eyeForwardCm.load(std::memory_order_relaxed) << " cm"
      << " | fromWorld llamadas=" << g_fwCallsSec.exchange(0, std::memory_order_relaxed)
      << " reescrita=" << g_fwWritesSec.exchange(0, std::memory_order_relaxed)
      << " tardias=" << g_fwLateRewritesSec.exchange(0, std::memory_order_relaxed)
      << " rehechas=" << g_fwStaleRefreshSec.exchange(0, std::memory_order_relaxed)
      << " sinPose=" << g_noPose.exchange(0, std::memory_order_relaxed)
      << " | culling llamadas=" << g_cullCallsSec.exchange(0, std::memory_order_relaxed)
      << " otraPose=" << g_cullOtherPoseSec.exchange(0, std::memory_order_relaxed)
      << " | colision recortes=" << g_rayClampSec.exchange(0, std::memory_order_relaxed)
      << " empujes=" << g_pushOutSec.exchange(0, std::memory_order_relaxed)
      << " | giros enviados=" << g_turnsSentSec.exchange(0, std::memory_order_relaxed);
    HookLogger::Instance().Line(o.str());
}

// --- API -------------------------------------------------------------------
int HeadTrackGetMode() {
    return g_mode.load(std::memory_order_relaxed);
}

bool HeadTrackDioramaCameraActive() {
    return g_mode.load(std::memory_order_relaxed) == 1 && g_dioramaCamera.load(std::memory_order_relaxed);
}

// Ronda 46: altura anadida a los ojos guardada junto a Blade.exe. Ronda 54:
// en BladeVR.ini (altura_ojos_cm, de serie 0: la del personaje, como con el
// cuerpo visible); BladeVR_altura.txt ya no se lee.
static void LoadEyeLiftOnce() {
    if (g_eyeLiftLoaded.exchange(true)) return;
    int v = IniGetInt("altura_ojos_cm", kEyeLiftDefaultCm);
    if (v < 0) v = 0;
    if (v > kEyeLiftMaxCm) v = kEyeLiftMaxCm;
    g_eyeLiftCm.store(v, std::memory_order_relaxed);
    std::ostringstream o;
    o << "[HEAD_TRACK] altura anadida a los ojos en el modo de manos flotantes: " << v << " cm (BladeVR.ini; Mayus + Re Pag / Av Pag)";
    HookLogger::Instance().Line(o.str());
}

double HeadTrackEyeLiftM() {
    if (!CombatVRBodilessActive()) return 0.0;   // ronda 47: solo sin cuerpo
    return g_eyeLiftCm.load(std::memory_order_relaxed) * 0.01;
}

void HeadTrackAdjustEyeLift(int deltaCm) {
    LoadEyeLiftOnce();
    int v = g_eyeLiftCm.load(std::memory_order_relaxed) + deltaCm;
    if (v < 0) v = 0;
    if (v > kEyeLiftMaxCm) v = kEyeLiftMaxCm;
    g_eyeLiftCm.store(v, std::memory_order_relaxed);
    bool saved = IniSetInt("altura_ojos_cm", v);   // ronda 54
    std::ostringstream o;
    o << "[HEAD_TRACK] altura anadida a los ojos " << v << " cm sobre la camara del juego (Mayus + Re Pag / Av Pag; "
      << (saved ? "guardada en BladeVR.ini" : "NO se pudo guardar") << ")";
    HookLogger::Instance().Line(o.str());
    wchar_t txt[120];
    std::swprintf(txt, 120, CombatVRBodilessActive() ? L"+%d cm sobre el personaje" : L"+%d cm (solo con las manos flotantes)", v);
    OpenVRShowToast(L"Altura de los ojos", txt, 1500);
}

void HeadTrackAdjustEyeForward(int deltaCm) {
    int v = g_eyeForwardCm.load(std::memory_order_relaxed) + deltaCm;
    if (v < 0) v = 0;
    if (v > kEyeForwardMaxCm) v = kEyeForwardMaxCm;
    g_eyeForwardCm.store(v, std::memory_order_relaxed);
    std::ostringstream o;
    o << "[HEAD_TRACK] ojos adelantados " << v << " cm sobre la camara del juego (Ctrl + Re Pag / Av Pag; de serie "
      << kEyeForwardDefaultCm << ")";
    HookLogger::Instance().Line(o.str());
    wchar_t txt[64];
    std::swprintf(txt, 64, L"%d cm por delante (de serie %d)", v, kEyeForwardDefaultCm);
    OpenVRShowToast(L"Ojos adelantados", txt, 1500);
}

void HeadTrackSetWalkFollowsView(bool on, int combatMode) {
    int next = on ? 1 : 0;
    if (g_walkFollowsView.load(std::memory_order_relaxed) == next) return;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_followHaveOffset = false;
        g_followYawError = 0.0;
        g_followAccX = 0.0;
        g_followErrorFresh = false;
    }
    g_walkFollowsView.store(next, std::memory_order_relaxed);
    char line[200];
    std::snprintf(line, sizeof(line), on ? "[HEAD_TRACK] Supr (modo de combate %d): el personaje CAMINA HACIA DONDE MIRA el visor"
                                         : "[HEAD_TRACK] Supr (modo de combate %d): vista libre, como en el juego",
                  combatMode);
    HookLogger::Instance().Line(line);
}

void HeadTrackRequestRecenter() {
    g_recenterRequested.store(1, std::memory_order_relaxed);
    g_refocusRequest.store(1, std::memory_order_relaxed);
}

bool HeadTrackGetRenderPose(float* out12) {
    if (!out12) return false;
    if (g_mode.load(std::memory_order_relaxed) != 1) return false;
    std::lock_guard<std::mutex> lock(g_renderPoseMutex);
    if (!g_renderPoseValid) return false;
    for (int i = 0; i < 12; ++i) out12[i] = g_renderPose[i];
    return true;
}

void HeadTrackOnPresent() {
    CheckKeys();
    CheckGameFocus();
    bool dioramaSteer = g_dioramaCamera.load(std::memory_order_relaxed) && DioramaEnabled() &&
                        g_dioramaSteer.load(std::memory_order_relaxed) == 1;
    if (g_mode.load(std::memory_order_relaxed) == 1 &&
        (g_walkFollowsView.load(std::memory_order_relaxed) == 1 || dioramaSteer)) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        WalkFollowStep();
    }
    MaybeLogRate();
}

// --- camara del juego estable con un enemigo fijado (2.0, ronda 11) ---------
// En primera persona la camara del juego es una camara de persecucion a 150 mm
// (+0x592d0) y, con un enemigo fijado, su guinada y su cabeceo apuntan al
// enemigo: con uno en lo alto de una escalera la imagen temblaba y al soltarlo
// dejaba de temblar. Con un enemigo fijado su posicion pasa por un filtro
// "1 euro" (suaviza el temblor de poca amplitud y sigue el movimiento real casi
// sin retraso); sin enemigo, la del juego tal cual (mezcla de 0,3 s). El visor
// se suma despues, sin filtrar.
static double QpcSeconds() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

static double FiltAlpha(double cutoffHz, double dt) {
    double tau = 1.0 / (2.0 * kPi * cutoffHz);
    return 1.0 / (1.0 + tau / dt);
}

// Objetivo de la camara de persecucion (TPos) si la vista es la primera persona.
static bool SafeReadChaseTarget(double out[3]) {
    __try {
        uintptr_t app = *reinterpret_cast<const uintptr_t*>(ModuleBase() + kAppPointerOffset);
        if (!app) return false;
        uintptr_t cam = *reinterpret_cast<const uintptr_t*>(app + kAppCameraEntity);
        if (!cam) return false;
        if (*reinterpret_cast<const int*>(cam + kCameraViewType) != 3) return false;
        const double* t = reinterpret_cast<const double*>(cam + kCameraTargetPos);
        out[0] = t[0];
        out[1] = t[1];
        out[2] = t[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return out[0] == out[0] && out[1] == out[1] && out[2] == out[2];
}

// Hilo del juego, fuera del diorama: c = posicion de la camara del juego
// (unidades), se devuelve la que se usa.
static void StabilizeGameCamera(double c[3]) {
    if (!g_applyFromWriter) {
        // Rehacer una camara vieja: el mismo ajuste que se le dio.
        for (int k = 0; k < 3; ++k) c[k] += g_camFiltOff[k];
        return;
    }
    double t = QpcSeconds();
    double dt = g_camFiltInit ? t - g_camFiltT : 0.0;
    g_camFiltT = t;
    bool dtOk = dt > 0.0 && dt < 0.1;

    // Posicion anclada al jugador: su posicion en el juego + la de la cabeza
    // respecto a el (objetivo de la camara, suavizado) + la distancia de la
    // camara detras de hacia donde mira el personaje, con su altura.
    double eye[3] = { c[0], c[1], c[2] };
    double target[3], person[3], entYaw = 0.0;
    bool locked = false;
    bool pinned = false;
    bool haveTarget = SafeReadChaseTarget(target);
    bool havePerson = CombatVRGetPlayerPosition(person);
    // Posicion del jugador suavizada (media 1-2-2-1) para la anclada.
    double personF[3] = { person[0], person[1], person[2] };
    if (havePerson) {
        double jump2 = 0.0;
        for (int k = 0; k < 3; ++k) jump2 += (person[k] - g_perHist[0][k]) * (person[k] - g_perHist[0][k]);
        if (!g_perHistValid || !dtOk || jump2 > 500.0 * 500.0) {
            for (int h = 0; h < 3; ++h)
                for (int k = 0; k < 3; ++k) g_perHist[h][k] = person[k];
            g_perHistValid = true;
        }
        for (int k = 0; k < 3; ++k) {
            personF[k] = (person[k] + 2.0 * g_perHist[0][k] + 2.0 * g_perHist[1][k] + g_perHist[2][k]) / 6.0;
            g_perHist[2][k] = g_perHist[1][k];
            g_perHist[1][k] = g_perHist[0][k];
            g_perHist[0][k] = person[k];
        }
    } else {
        g_perHistValid = false;
    }
    if (haveTarget && havePerson && CombatVRGetPlayerFacing(&entYaw, &locked)) {
        double ox = c[0] - target[0], oz = c[2] - target[2];
        double dist = std::sqrt(ox * ox + oz * oz);
        if (dist > 30.0 && dist < 400.0) {
            if (!g_pinValid || !dtOk) g_pinDist = dist;
            else g_pinDist += (dist - g_pinDist) * (1.0 - std::exp(-dt / 1.0));
            g_pinValid = true;
            // Cabeza respecto al jugador (horizontal: objetivo; vertical: la camara).
            double off[3] = { target[0] - person[0], c[1] - person[1], target[2] - person[2] };
            double jump2 = 0.0;
            for (int k = 0; k < 3; ++k) jump2 += (off[k] - g_headOff[k]) * (off[k] - g_headOff[k]);
            if (!g_headOffValid || !dtOk || jump2 > 500.0 * 500.0) {
                for (int k = 0; k < 3; ++k) g_headOff[k] = off[k];
                g_headOffValid = true;
            } else {
                double a = 1.0 - std::exp(-dt / kHeadOffsetSmoothSec);
                for (int k = 0; k < 3; ++k) g_headOff[k] += (off[k] - g_headOff[k]) * a;
            }
            double fx = std::sin(entYaw), fz = std::cos(entYaw);
            // Ronda 37: siempre detras. Antes, si la camara del juego quedaba
            // delante (girando deprisa junto al enemigo su guinada se queda
            // atras de la del personaje), los ojos saltaban 30 cm adelante y
            // atras: la vista "pillada dando vueltas" cerca del enemigo fijado.
            eye[0] = personF[0] + g_headOff[0] - g_pinDist * fx;
            eye[1] = personF[1] + g_headOff[1];
            eye[2] = personF[2] + g_headOff[2] - g_pinDist * fz;
            g_pinFwd[0] = fx;
            g_pinFwd[1] = fz;
            g_pinFwdValid = true;
            pinned = true;
        }
    }
    if (!pinned) {
        g_pinValid = false;
        g_headOffValid = false;
        g_pinFwdValid = false;
    }
    // Filtro "1 euro" sobre la posicion de la camara del juego si no se pudo
    // anclar; la anclada ya va suavizada.
    double dist2 = 0.0;
    for (int k = 0; k < 3; ++k) dist2 += (eye[k] - g_camFilt[k]) * (eye[k] - g_camFilt[k]);
    if (!g_camFiltInit || dt <= 0.0 || dt > 0.25 || dist2 > kCamFiltResetUnits * kCamFiltResetUnits) {
        for (int k = 0; k < 3; ++k) {
            g_camFilt[k] = eye[k];
            g_camFiltD[k] = 0.0;
        }
        g_camFiltInit = true;
    } else {
        double aD = FiltAlpha(kCamFiltDCutoffHz, dt), speed2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            double raw = (eye[k] - g_camFilt[k]) / dt;
            g_camFiltD[k] += (raw - g_camFiltD[k]) * aD;
            speed2 += g_camFiltD[k] * g_camFiltD[k];
        }
        double a = FiltAlpha(kCamFiltMinCutoffHz + kCamFiltBeta * std::sqrt(speed2), dt);
        for (int k = 0; k < 3; ++k) g_camFilt[k] += (eye[k] - g_camFilt[k]) * a;
    }
    if (pinned) {
        for (int k = 0; k < 3; ++k) {
            g_camFilt[k] = eye[k];   // el filtro sigue a la anclada (sin salto si deja de poder anclarse)
            g_camFiltD[k] = 0.0;
        }
    }
    // Peso: 1 con un enemigo fijado (CombatVR, con margen de 1 s).
    double targetW = CombatVREnemyLocked() ? 1.0 : 0.0;
    if (dt > 0.0 && dt < 0.25) {
        double tc = targetW > g_camFiltW ? kCamFiltBlendInSec : kCamFiltBlendSec;
        g_camFiltW += (targetW - g_camFiltW) * (1.0 - std::exp(-dt / tc));
        if (std::abs(g_camFiltW - targetW) < 1e-3) g_camFiltW = targetW;
    } else {
        g_camFiltW = targetW;
    }
    for (int k = 0; k < 3; ++k) {
        g_camFiltOff[k] = (g_camFilt[k] - c[k]) * g_camFiltW;
        c[k] += g_camFiltOff[k];
    }
}

// Ronda 27: altura de la camara suavizada. Al bajar algunas escaleras el
// personaje cae cada escalon de golpe (la camara del juego baja 15-20 cm en
// uno o dos fotogramas) y con el visor se sentia brusco y tembloroso. Muelle
// criticamente amortiguado (kCamYSmoothSec) sobre la altura; mas de kCamYSnapM
// de diferencia (teletransporte, carga) va directo. El cuerpo bajo el visor
// sigue bajo los ojos (desde la ronda 36 CombatVR suaviza la altura de la
// persona con el mismo muelle).
constexpr double kCamYSmoothSec = 0.20;   // ronda 28 (0,08 en la 27: cada escalon seguia dando un golpe)
constexpr double kCamYSnapM = 1.5;
static bool g_camYValid = false;
static double g_camY = 0.0, g_camYVel = 0.0, g_camYT = 0.0, g_camYOff = 0.0;

// Ronda 40: plataformas que suben o bajan (ascensores) y caidas. La camara
// del juego persigue a la cabeza con retraso y a saltos (bajando la
// plataforma de Ragnar a 2 m/s: hasta 1,1 m en un fotograma), mientras el
// cuerpo bajo el visor sigue a la persona (CombatVR, ronda 36): los hombros se
// movian respecto a los ojos (19 mm por fotograma de media, hasta 19 cm) y el
// brazo del escudo, con la muneca fija al mando, se retorcia. Con la persona
// moviendose en vertical y casi quieta en horizontal, la altura de la que salen
// los ojos es la de la persona + la de la camara respecto a ella de antes de
// empezar: la misma entrada y el mismo muelle que el cuerpo.
constexpr double kLiftEnterVyMs = 0.6;     // velocidad vertical para empezar...
constexpr double kLiftMaxVhMs = 0.6;       // ...con la horizontal por debajo de esta (no las escaleras)
constexpr double kLiftEnterSec = 0.12;
constexpr double kLiftExitVyMs = 0.25;     // y para acabar, por debajo de esta
constexpr double kLiftExitSec = 0.30;
constexpr double kLiftVelSmoothSec = 0.10;
constexpr double kLiftRelSmoothSec = 0.50; // altura de la camara respecto a la persona fuera de ellas
static bool g_liftOn = false, g_liftHavePrev = false, g_liftRelValid = false;
static double g_liftPrevP[3] = { 0, 0, 0 };
static double g_liftPrevT = 0.0, g_liftVy = 0.0, g_liftVh = 0.0, g_liftEnterSince = 0.0, g_liftExitSince = 0.0;
static double g_liftRel = 0.0;

// Altura de la que salen los ojos: la de la camara del juego, o en una
// plataforma la de la persona + la de la camara respecto a ella de antes.
static double LiftSourceY(double rawY, double now, double upm) {
    double p[3];
    if (!CombatVRGetPlayerPosition(p)) {
        g_liftHavePrev = false;
        g_liftRelValid = false;
        g_liftOn = false;
        g_liftEnterSince = g_liftExitSince = 0.0;
        return rawY;
    }
    double dt = now - g_liftPrevT;
    bool dtOk = g_liftHavePrev && dt > 0.0 && dt < 0.1;
    if (dtOk) {
        double dx = p[0] - g_liftPrevP[0], dy = p[1] - g_liftPrevP[1], dz = p[2] - g_liftPrevP[2];
        if (std::fabs(dy) > 1.5 * upm || std::sqrt(dx * dx + dz * dz) > 1.5 * upm) {
            dtOk = false;   // salto (carga, teletransporte)
            g_liftOn = false;
            g_liftRelValid = false;
            g_liftVy = g_liftVh = 0.0;
        } else {
            double a = 1.0 - std::exp(-dt / kLiftVelSmoothSec);
            g_liftVy += (dy / upm / dt - g_liftVy) * a;
            g_liftVh += (std::sqrt(dx * dx + dz * dz) / upm / dt - g_liftVh) * a;
        }
    } else if (!g_liftHavePrev) {
        g_liftVy = g_liftVh = 0.0;
    }
    for (int k = 0; k < 3; ++k) g_liftPrevP[k] = p[k];
    g_liftPrevT = now;
    g_liftHavePrev = true;
    if (!g_liftOn) {
        double rel = rawY - p[1];
        if (!g_liftRelValid || !dtOk) {
            g_liftRel = rel;
            g_liftRelValid = true;
        } else {
            g_liftRel += (rel - g_liftRel) * (1.0 - std::exp(-dt / kLiftRelSmoothSec));
        }
        if (dtOk && std::fabs(g_liftVy) > kLiftEnterVyMs && g_liftVh < kLiftMaxVhMs) {
            if (g_liftEnterSince == 0.0) g_liftEnterSince = now;
            if (now - g_liftEnterSince >= kLiftEnterSec) {
                g_liftOn = true;
                g_liftExitSince = 0.0;
            }
        } else {
            g_liftEnterSince = 0.0;
        }
    } else {
        if (std::fabs(g_liftVy) < kLiftExitVyMs) {
            if (g_liftExitSince == 0.0) g_liftExitSince = now;
        } else {
            g_liftExitSince = 0.0;
        }
        double src = p[1] + g_liftRel;
        if (g_liftExitSince == 0.0 || now - g_liftExitSince < kLiftExitSec) return src;
        // Fin: la camara del juego vuelve a mandar (el muelle de la altura
        // suaviza lo que se hubiera apartado).
        g_liftOn = false;
        g_liftEnterSince = 0.0;
    }
    return rawY;
}

static double SmoothCameraY(const double c[3]) {
    double rawY = c[1];
    if (!g_applyFromWriter) return g_camYValid ? rawY + g_camYOff : rawY;   // matriz vieja: el mismo desfase
    double upm = static_cast<double>(g_unitsPerMeter.load(std::memory_order_relaxed));
    if (upm < 1.0) upm = 1000.0;
    double now = QpcSeconds();
    double dt = now - g_camYT;
    g_camYT = now;
    double srcY = LiftSourceY(rawY, now, upm);   // ronda 40
    if (!g_camYValid || dt <= 0.0 || dt > 0.25 || std::fabs(srcY - g_camY) > kCamYSnapM * upm) {
        g_camY = srcY;
        g_camYVel = 0.0;
        g_camYValid = true;
    } else {
        SmoothDampStep(&g_camY, &g_camYVel, srcY, kCamYSmoothSec, dt);
        // Ronda 29: sin salir del nivel (subiendo, la altura suavizada va por
        // debajo de la del juego y podria quedar dentro de un escalon). La
        // ronda 28 la paraba en el borde del sector de la camara del juego.
        double off[3] = { 0.0, g_camY - rawY, 0.0 };
        double fr = InsideLevelFraction(c, off);
        if (fr < 1.0) {
            g_camY = rawY + off[1] * fr;
            g_camYVel = 0.0;
        }
    }
    g_camYOff = g_camY - rawY;
    return g_camY;
}

// Ronda 23 (sin Supr): compensacion de guinada de la vista durante un combo
// del modo de combate 2 (la vista no gira con el personaje) y su vuelta a 0.
static double g_comboComp = 0.0;
static double g_comboCompT = 0.0;
static bool g_comboViewHoldWas = false;
static double g_comboFrozenYaw = 0.0;
constexpr double kComboViewReturnSec = 0.35;

static double ComboViewComp(double baseYaw) {
    if (!g_applyFromWriter) return g_comboComp;   // matriz vieja: la misma que en el ultimo fotograma
    double now = QpcSeconds();
    double dt = now - g_comboCompT;
    g_comboCompT = now;
    if (dt <= 0.0 || dt > 0.1) dt = 0.016;
    bool hold = CombatVRComboHold();
    if (hold) {
        if (!g_comboViewHoldWas) g_comboFrozenYaw = WrapPi(baseYaw + g_comboComp);
        g_comboComp = WrapPi(g_comboFrozenYaw - baseYaw);
    } else if (g_comboComp != 0.0) {
        g_comboComp *= std::exp(-dt / kComboViewReturnSec);
        if (std::fabs(g_comboComp) < 1e-3) g_comboComp = 0.0;
    }
    g_comboViewHoldWas = hold;
    return g_comboComp;
}

// Reescribe fromWorld (la matriz de la camara del juego, recien escrita por el
// juego) con la orientacion y posicion del visor. Se llama desde el hilo del
// juego: tras el escritor de fromWorld y, si el juego la ha vuelto a pisar
// despues (camaras fijas de la seleccion de personaje y cinematicas), justo
// antes del culling. positional=false: solo orientacion, sin desplazamiento
// ni colision (camaras guiadas por el juego: la colision contra un recorrido
// que atraviesa geometria produce temblores).
static void ApplyHeadTrackingToFromWorld(double* fromWorld, bool positional, bool dioramaOk) {
    double game[16];
    if (!SafeReadMatrix16(fromWorld, game)) return;
    if (std::abs(game[15] - 1.0) > 1e-6) return;

    float m[12];
    if (!GetHmdPoseMatrix34(m)) {
        g_noPose.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_renderPoseMutex);
        for (int i = 0; i < 12; ++i) g_renderPose[i] = m[i];
        g_renderPoseValid = true;
    }

    // Modo diorama (F5): la camara es la cabeza del jugador llevada a la
    // maqueta (DioramaHook.cpp); Inicio vuelve a colocar la maqueta en vez de
    // recentrar.
    if (dioramaOk && DioramaEnabled()) {
        bool recenter = g_recenterRequested.exchange(0, std::memory_order_relaxed) == 1;
        if (g_cineCall) DioramaMarkCinematicCall();
        double dio[16];
        if (DioramaComputeCamera(m, game, recenter, dio)) {
            // Moverse relativo a la mirada: error de guinada entre la vista
            // sobre la maqueta (direccion horizontal: adelante de la cabeza
            // mas su "arriba", que sigue valiendo al mirar hacia abajo) y la
            // camara del juego, que es la que decide hacia donde avanza el
            // personaje. Lo corrige WalkFollowStep con giros de raton.
            double hx = dio[2] - dio[1];     // columna 1 = abajo de la camara (Y del juego hacia abajo)
            double hz = dio[10] - dio[9];
            // Solo con la camara recien escrita por el juego (no al rehacer
            // una matriz vieja en pausa: los giros se acumularian).
            bool viewOk = (hx * hx + hz * hz) > 1e-6;
            double viewYaw = viewOk ? std::atan2(hx, hz) : 0.0;
            if (viewOk) g_dioramaViewYaw.store(viewYaw, std::memory_order_relaxed);
            g_dioramaViewYawValid.store(viewOk, std::memory_order_relaxed);
            // El objetivo es la direccion pedida con el joystick izquierdo
            // sobre la maqueta (HookedGamepadProcess), solo mientras se
            // empuja: el juego recibe "adelante" y aqui se gira su camara (y
            // con ella el personaje) hacia esa direccion. Sin joystick no se
            // gira nada, asi que el juego conserva su control.
            uint64_t t = g_stickHeadingTick.load(std::memory_order_relaxed);
            bool haveTarget = viewOk && t != 0 && GetTickCount64() - t < kStickHeadingTimeoutMs;
            double target = g_stickHeading.load(std::memory_order_relaxed);
            bool steer = g_applyFromWriter && g_dioramaSteer.load(std::memory_order_relaxed) == 1 && haveTarget &&
                         DioramaViewSteeringAllowed();
            double err = steer ? WrapPi(target - std::atan2(game[2], game[10])) : 0.0;
            double enterDeg = kDioramaStickEnterDeg;
            double exitDeg = kDioramaStickExitDeg;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                double errDeg = std::abs(err) * 180.0 / kPi;
                if (!steer) g_dioramaSteering = false;
                else if (!g_dioramaSteering && errDeg > enterDeg) g_dioramaSteering = true;
                else if (g_dioramaSteering && errDeg < exitDeg) g_dioramaSteering = false;
                g_followYawError = g_dioramaSteering ? err : 0.0;
                g_followErrorFresh = g_dioramaSteering;
                if (!g_dioramaSteering) g_followAccX = 0.0;
                g_followHaveOffset = false;   // al salir, Supr normal recalcula su desfase
                g_haveAppliedOff = false;
            }
            if (SafeWriteMatrix16(fromWorld, dio)) {
                g_fwWritesSec.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(g_lastWrittenMutex);
                for (int i = 0; i < 16; ++i) { g_lastWritten[i] = dio[i]; g_lastBase[i] = game[i]; }
                g_haveLastWritten = true;
                g_lastPositional = positional;
                g_lastDioramaOk = true;
                g_lastWrittenFrame = GetPresentCallCount();
            }
            g_dioramaCamera.store(true, std::memory_order_relaxed);
            return;
        }
    }
    g_dioramaCamera.store(false, std::memory_order_relaxed);

    // Posicion de la camara del juego: c[r] = -(t . fila_r); adelante = columna 2.
    double c[3];
    for (int rr = 0; rr < 3; ++rr) {
        c[rr] = -(game[12] * game[rr * 4 + 0] + game[13] * game[rr * 4 + 1] + game[14] * game[rr * 4 + 2]);
    }
    StabilizeGameCamera(c);   // con un enemigo fijado (ronda 11)
    double baseYaw = std::atan2(game[2], game[10]);
    if (positional) {
        // Ronda 26: la camara de primera persona del juego persigue a su punto
        // de la cabeza desde 0,15 m por detras (en su direccion): con el visor
        // los ojos quedaban dentro de la cabeza y al mirar abajo se veia el
        // cuello. Los ojos se adelantan en horizontal en esa direccion; el
        // cuerpo bajo el visor se coloca respecto a este punto (base del mapa),
        // asi que queda por detras de los ojos.
        double fx = game[2], fz = game[10];
        double fl = std::sqrt(fx * fx + fz * fz);
        if (fl > 1e-6 && g_pinFwdValid && g_camFiltW > 0.0) {
            // Ronda 37: con la camara anclada (enemigo fijado), en la direccion
            // del personaje, la misma con que se pone detras (la camara del
            // juego gira mas despacio y apuntaba a otro lado: con el personaje
            // girando deprisa los ojos quedaban hasta 25 cm fuera de la cabeza).
            double w = g_camFiltW;
            fx = fx / fl * (1.0 - w) + g_pinFwd[0] * w;
            fz = fz / fl * (1.0 - w) + g_pinFwd[1] * w;
            fl = std::sqrt(fx * fx + fz * fz);
        }
        if (fl > 1e-6) {
            double e = g_eyeForwardCm.load(std::memory_order_relaxed) * 0.01 *
                       static_cast<double>(g_unitsPerMeter.load(std::memory_order_relaxed));
            double eo[3] = { fx / fl * e, 0.0, fz / fl * e };
            // Ronda 29: solo sin salir del nivel (la 28 los frenaba en cada
            // portal: tirones al andar).
            double fr = InsideLevelFraction(c, eo);
            c[0] += eo[0] * fr;
            c[2] += eo[2] * fr;
        }
        c[1] = SmoothCameraY(c);   // ronda 27: escaleras (28: 0,20 s; 29: solo sin salir del nivel)
    } else {
        g_camYValid = false;
        g_camYOff = 0.0;
    }

    // Inicio = recentrar la POSICION: la referencia del visor pasa a ser la
    // actual, de modo que la camara vuelve exactamente a los ojos del
    // personaje aunque el cuerpo se haya movido desde que se cargo la
    // partida. (Se probo calibrar la escala/IPD con la altura del visor y se
    // descarto: en tercera persona mide la altura de la camara de
    // persecucion, no la de los ojos, y cambiaba la separacion de ojos.)
    if (g_recenterRequested.exchange(0, std::memory_order_relaxed) == 1) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_havePosRef = false;
            g_haveAppliedOff = false;
            g_fixedCamActive = false;   // ronda 29: en una cinematica, la vista se vuelve a alinear con su camara
        }
        HookLogger::Instance().Line("[HEAD_TRACK] Inicio: posicion recentrada (la camara vuelve a los ojos del personaje).");
    }

    // Ejes del visor en ejes del juego (cambio de base (x, -y, -z)).
    double r0[3] = { m[0], -m[4], -m[8] };
    double u0[3] = { -m[1], m[5], m[9] };
    double f0[3] = { -m[2], m[6], m[10] };
    double hp[3] = { m[3], -m[7], -m[11] };
    // Ronda 24: guinada del visor tambien al mirar abajo o arriba: al adelante
    // se le suma el "arriba" de la cabeza (abajo si se mira arriba) en la
    // medida en que se mira hacia abajo. Igual que la de siempre con la cabeza
    // recta; mirando las manos, la proyeccion del adelante sola se quedaba en
    // casi nada y bailaba con cualquier giro de la cabeza (Supr y el pecho).
    double hmdYawGame = std::atan2(f0[0] - f0[1] * u0[0], f0[2] - f0[1] * u0[2]);

    double out[16];
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_haveYawRef) { g_yawRef = hmdYawGame; g_haveYawRef = true; }
        if (!g_havePosRef) { g_posRef[0] = hp[0]; g_posRef[1] = hp[1]; g_posRef[2] = hp[2]; g_havePosRef = true; }

        // Ronda 29: camara de una cinematica o de la seleccion de personaje.
        uint64_t nowMs = GetTickCount64();
        if (positional && g_applyFromWriter) g_lastGameplayCamMs = nowMs;
        bool fixedCam = !positional && (g_lastGameplayCamMs == 0 || nowMs - g_lastGameplayCamMs > kFixedCamAfterMs);
        if (!fixedCam) {
            g_fixedCamActive = false;
        } else if (!g_fixedCamActive) {
            g_fixedCamYawRef = hmdYawGame;
            g_fixedCamActive = true;
        }

        // Delta de guinada: vista libre = guinada del juego + relativa del visor;
        // "camina hacia donde mira" = vista absoluta (visor + desfase fijo).
        double delta;
        if (fixedCam) {
            // La guinada de la camara de la escena + lo que ha girado el visor
            // desde que empezo; Supr no gira a nadie (su desfase se conserva).
            delta = baseYaw - g_fixedCamYawRef;
            g_followYawError = 0.0;
            g_followErrorFresh = false;
            g_followAccX = 0.0;
            g_followTurning = false;
        } else if (g_walkFollowsView.load(std::memory_order_relaxed) == 1) {
            if (!g_followHaveOffset) { g_followYawOffset = WrapPi(baseYaw - hmdYawGame); g_followHaveOffset = true; g_followTurning = false; }
            delta = g_followYawOffset;
            // Hacia donde mira el personaje (la entidad) y si tiene enemigo
            // fijado; si no se sabe, la camara del juego como antes. Solo con
            // la camara recien escrita por el juego (no al rehacer una vieja).
            if (g_applyFromWriter) {
                double charYaw = baseYaw;
                bool locked = false;
                double entYaw = 0.0;
                if (g_entityYawDisabled && GetTickCount64() - g_entityYawDisabledAt > kEntityYawRetryMs) {
                    g_entityYawDisabled = false;
                    g_followFarSince = 0;
                    HookLogger::Instance().Line("[HEAD_TRACK] Supr: se vuelve a medir con la entidad.");
                }
                g_followUsesEntity = !g_entityYawDisabled && CombatVRGetPlayerFacing(&entYaw, &locked);
                if (g_followUsesEntity) charYaw = entYaw;
                double err = WrapPi((hmdYawGame + g_followYawOffset) - charYaw);
                double errDeg = std::abs(err) * 180.0 / kPi;
                float sent[2];
                bool moving = (VrPadGetLeftStick(sent) && (std::fabs(sent[0]) > 0.05f || std::fabs(sent[1]) > 0.05f)) ||
                              (GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState('S') & 0x8000) ||
                              (GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState(VK_DOWN) & 0x8000);
                // 2.0, ronda 13: quieto, el personaje se gira poniendo su
                // angulo (CombatVR), sin peticiones de giro (daban pasos).
                // Ronda 23: durante un combo del juego (modo de combate 2)
                // Supr no gira: el combo gira al personaje como en el juego y
                // la vista (absoluta) se queda al frente; al acabar, Supr lo
                // vuelve a poner mirando hacia la vista.
                // Ronda 38: tampoco durante las acciones de script del juego
                // (palancas, coger, escenas): el juego coloca al personaje.
                bool comboHold = CombatVRComboHold() || CombatVRScriptedAction();
                bool direct = !comboHold && g_followUsesEntity && CombatVRDirectTurnAvailable(moving);
                if (comboHold) {
                    g_followTurning = false;
                } else if (direct) {
                    if (!g_followTurning && errDeg > kFollowStillEnterDeg) g_followTurning = true;
                    else if (g_followTurning && errDeg < kFollowStillExitDeg) g_followTurning = false;
                    if (g_followTurning) CombatVRDirectTurnStep(WrapPi(hmdYawGame + g_followYawOffset));
                } else if (locked) {
                    g_followTurning = false;
                } else {
                    double enter = moving ? kFollowMoveEnterDeg : kFollowStillEnterDeg;
                    double exit = moving ? kFollowMoveExitDeg : kFollowStillExitDeg;
                    if (!g_followTurning && errDeg > enter) g_followTurning = true;
                    else if (g_followTurning && errDeg < exit) g_followTurning = false;
                }
                bool mouseTurn = g_followTurning && !direct;
                g_followYawError = mouseTurn ? err : 0.0;
                g_followErrorFresh = mouseTurn;
                if (!mouseTurn) g_followAccX = 0.0;
                // Seguro: si girando con foco el error no baja de 15 grados en
                // 4 s, la guinada de la entidad no sirve: se usa la camara.
                uint64_t nowMs = GetTickCount64();
                bool tooFar = mouseTurn && !moving && g_followUsesEntity && errDeg > 15.0 &&
                              g_gameFocused.load(std::memory_order_relaxed) && CombatVRInGameplay();
                if (!tooFar) g_followFarSince = 0;
                else if (g_followFarSince == 0) g_followFarSince = nowMs;
                else if (nowMs - g_followFarSince > 4000 && !g_entityYawDisabled) {
                    g_entityYawDisabled = true;
                    g_entityYawDisabledAt = nowMs;
                    HookLogger::Instance().Line("[HEAD_TRACK] Supr: el personaje no se acerca a la vista en 4 s midiendo con la entidad; "
                                                "se mide con la camara del juego durante 10 s.");
                }
            }
        } else {
            g_followHaveOffset = false;
            // Ronda 23: sin Supr la vista sigue a la camara del juego; durante
            // un combo del modo 2 se queda donde estaba y despues alcanza a la
            // del juego poco a poco.
            delta = baseYaw + ComboViewComp(baseYaw) - g_yawRef;
        }
        double cD = std::cos(delta), sD = std::sin(delta);
        double r[3] = { r0[0], r0[1], r0[2] };
        double u[3] = { u0[0], u0[1], u0[2] };
        double f[3] = { f0[0], f0[1], f0[2] };
        RotateY(r, cD, sD);
        RotateY(u, cD, sD);
        RotateY(f, cD, sD);

        double off[3] = { hp[0] - g_posRef[0], hp[1] - g_posRef[1], hp[2] - g_posRef[2] };
        RotateY(off, cD, sD);
        // Ronda 46: altura anadida a los ojos (y del juego hacia abajo).
        if (positional) {
            LoadEyeLiftOnce();
            off[1] -= HeadTrackEyeLiftM();
        }
        double cNew[3] = { c[0], c[1], c[2] };
        if (positional) {
            double upm = static_cast<double>(g_unitsPerMeter.load(std::memory_order_relaxed));
            double offU[3] = { off[0] * upm, off[1] * upm, off[2] * upm };
            double offRaw[3] = { offU[0], offU[1], offU[2] };
            double lo = ClampOffsetToLevel(c, offU);
            double fa = EasedClampFactor(lo);   // ronda 28
            for (int k = 0; k < 3; ++k) offU[k] = offRaw[k] * fa;
            cNew[0] = c[0] + offU[0]; cNew[1] = c[1] + offU[1]; cNew[2] = c[2] + offU[2];
            PushOutFromObstacles(cNew);
            double fin[3] = { cNew[0] - c[0], cNew[1] - c[1], cNew[2] - c[2] };
            RateLimitOffset(fin);
            cNew[0] = c[0] + fin[0]; cNew[1] = c[1] + fin[1]; cNew[2] = c[2] + fin[2];
        } else {
            off[0] = 0.0; off[1] = 0.0; off[2] = 0.0;
        }

        for (int i = 0; i < 16; ++i) out[i] = game[i];
        out[0] = r[0]; out[4] = r[1]; out[8] = r[2];
        out[1] = u[0]; out[5] = u[1]; out[9] = u[2];
        out[2] = f[0]; out[6] = f[1]; out[10] = f[2];
        out[12] = -(cNew[0] * r[0] + cNew[1] * r[1] + cNew[2] * r[2]);
        out[13] = -(cNew[0] * u[0] + cNew[1] * u[1] + cNew[2] * u[2]);
        out[14] = -(cNew[0] * f[0] + cNew[1] * f[1] + cNew[2] * f[2]);

        g_lastOffsetM[0] = off[0]; g_lastOffsetM[1] = off[1]; g_lastOffsetM[2] = off[2];

        {
            std::lock_guard<std::mutex> mapLock(g_mapMutex);
            g_trackMapValid = positional;
            if (positional) {
                for (int i = 0; i < 3; ++i) {
                    g_trackMap.cam[i] = cNew[i];
                    g_trackMap.hmd[i] = hp[i];
                    g_trackMap.base[i] = c[i];
                    g_trackMap.right[i] = r[i];
                    g_trackMap.down[i] = u[i];
                    g_trackMap.fwd[i] = f[i];
                }
                g_trackMap.cosYaw = cD;
                g_trackMap.sinYaw = sD;
                g_trackMap.unitsPerMeter = static_cast<double>(g_unitsPerMeter.load(std::memory_order_relaxed));
                g_trackMap.tick = GetTickCount64();
            }
        }
    }
    if (SafeWriteMatrix16(fromWorld, out)) {
        g_fwWritesSec.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_lastWrittenMutex);
        for (int i = 0; i < 16; ++i) { g_lastWritten[i] = out[i]; g_lastBase[i] = game[i]; }
        g_haveLastWritten = true;
        g_lastPositional = positional;
        g_lastDioramaOk = dioramaOk;
        g_lastWrittenFrame = GetPresentCallCount();
    }
}

// Si fromWorld sigue siendo la que escribimos en un fotograma anterior (el
// juego no la ha tocado), la vuelve a construir desde la matriz base del
// juego con la pose actual del visor. Devuelve true si lo ha hecho.
static bool RefreshStaleFromWorld(double* fromWorld) {
    double base[16];
    bool positional;
    bool dioramaOk;
    {
        std::lock_guard<std::mutex> lock(g_lastWrittenMutex);
        if (!g_haveLastWritten || g_lastWrittenFrame == GetPresentCallCount()) return false;
        for (int i = 0; i < 16; ++i) base[i] = g_lastBase[i];
        positional = g_lastPositional;
        dioramaOk = g_lastDioramaOk;
    }
    if (!SafeWriteMatrix16(fromWorld, base)) return false;
    g_cineCall = dioramaOk && CinematicRecent();
    ApplyHeadTrackingToFromWorld(fromWorld, positional, dioramaOk);
    g_cineCall = false;
    return true;
}


// --- el hook: reescritura de fromWorld -----------------------------------
static unsigned long long __fastcall HookedFromWorldWriter(unsigned long long a0, unsigned long long a1, unsigned long long a2, unsigned long long a3) {
    void* ret = _ReturnAddress();
    unsigned long long result = g_originalFromWorldWriter(a0, a1, a2, a3);
    g_fwCallsSec.fetch_add(1, std::memory_order_relaxed);

    FovClampOnGameThread();              // FOV minimo (hilo del juego)

    // Solo interesa la escritura de la matriz de la CAMARA (a0 es el objeto de
    // pose cuyo fromWorld esta en a0+0x98) y solo la definitiva del fotograma:
    // en partida, la tercera llamada (retorno +0x5a81d); en la seleccion de
    // personaje y las cinematicas los llamantes son otros y cada llamada es
    // definitiva.
    double* fromWorld = TryGetOrResolveFromWorldPointer();
    if (!fromWorld) return result;
    uintptr_t retOff = reinterpret_cast<uintptr_t>(ret) - ModuleBase();
    bool isCameraPose = (a0 + kPoseFromWorldOffset == reinterpret_cast<uintptr_t>(fromWorld));
    bool isIntermediate = (retOff == kFromWorldIntermediateReturn1 || retOff == kFromWorldIntermediateReturn2 ||
                           retOff == kClassicIntermediateReturn1 || retOff == kClassicIntermediateReturn2);
    if (!isCameraPose || isIntermediate) return result;
    StereoNotifyCameraPose();
    // Combate con mandos (CombatVRHook.cpp): una vez por fotograma de partida,
    // en el hilo del juego, con la camara de partida recien escrita.
    const bool combatFrame = (retOff == kFromWorldFinalCallReturnOffset);

    if (g_mode.load(std::memory_order_relaxed) != 1) {
        if (combatFrame) CombatVROnGameFrame();
        return result;
    }
    // Desplazamiento posicional y colision solo con la camara de partida (su
    // llamada definitiva retorna a +0x5a81d). Las cinematicas llaman al
    // escritor desde otros sitios, tres veces por fotograma y con un
    // recorrido que atraviesa geometria: ahi solo orientacion.
    // El diorama no usa colision: vale tambien la camara clasica.
    bool positional = (retOff == kFromWorldFinalCallReturnOffset);
    bool classicFinal = (retOff == kClassicCameraFinalReturnOffset);
    bool dioramaOk = positional || classicFinal;
    bool cine = false;
    if (!dioramaOk && DioramaEnabled()) {
        dioramaOk = true;
        cine = true;
    }
    if (classicFinal && DioramaEnabled()) cine = true;   // la clasica: solo cinematicas
    if (cine) g_cineWriterTick = GetTickCount64();
    g_applyFromWriter = true;
    g_cineCall = cine;
    ApplyHeadTrackingToFromWorld(fromWorld, positional, dioramaOk);
    g_cineCall = false;
    g_applyFromWriter = false;
    if (combatFrame) CombatVROnGameFrame();
    return result;
}

// --- joystick relativo a la mirada (diorama) --------------------------------
// El juego convierte el joystick izquierdo en avanzar/retroceder y girar. En
// diorama, con "relativo a la mirada" activo (no en el modo "por la
// espalda"), el joystick se lee como una direccion sobre la maqueta (arriba =
// hacia donde se mira): el juego recibe solo "adelante" con la misma fuerza
// y el lazo de WalkFollowStep gira su camara (y al personaje) hacia esa
// direccion. Soltar el joystick = el personaje se queda como este.
// Solo si no estan los ganchos del control de la camara 2 (que ademas
// permite los combos: ver HookedActionMove).
using PFN_GamepadProcess = void(__fastcall*)(long long, long long, float);
static PFN_GamepadProcess g_originalGamepadProcess = nullptr;
static bool g_gamepadHookInstalled = false;
static bool g_actionHooksInstalled = false;

static bool ReadPadLeftStick(long long dev, float* x, float* y) {
    __try {
        *x = *reinterpret_cast<const float*>(dev + kPadLeftX);
        *y = *reinterpret_cast<const float*>(dev + kPadLeftY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static void WritePadLeftStick(long long dev, float x, float y) {
    __try {
        *reinterpret_cast<float*>(dev + kPadLeftX) = x;
        *reinterpret_cast<float*>(dev + kPadLeftY) = y;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void __fastcall HookedGamepadProcess(long long dev, long long a2, float a3) {
    if (dev && CombatVRSkipIdlePad(dev)) return;   // ronda 67: mando quieto con otro moviendose
    bool remap = dev && !g_actionHooksInstalled && g_mode.load(std::memory_order_relaxed) == 1 && DioramaEnabled() &&
                 g_dioramaCamera.load(std::memory_order_relaxed) &&
                 g_dioramaSteer.load(std::memory_order_relaxed) == 1 &&
                 g_dioramaViewYawValid.load(std::memory_order_relaxed) && DioramaViewSteeringAllowed();
    float x = 0.0f, y = 0.0f;
    if (remap && ReadPadLeftStick(dev, &x, &y) && x == x && y == y) {
        double m = std::sqrt(static_cast<double>(x) * x + static_cast<double>(y) * y);
        if (m >= kStickRemapMin) {
            if (m > 1.0) m = 1.0;
            // 0 = arriba (adelante), positivo = derecha; la guinada del juego
            // crece hacia la derecha (atan2(x, z)).
            double a = std::atan2(static_cast<double>(x), -static_cast<double>(y));
            g_stickHeading.store(WrapPi(g_dioramaViewYaw.load(std::memory_order_relaxed) + a), std::memory_order_relaxed);
            g_stickHeadingTick.store(GetTickCount64(), std::memory_order_relaxed);
            WritePadLeftStick(dev, 0.0f, -static_cast<float>(m));
        }
    }
    g_originalGamepadProcess(dev, a2, a3);
    // Fuera del diorama: si el personaje sigue andando con el joystick en
    // reposo, CombatVR suelta "adelante" / "atras".
    if (dev) CombatVROnGamepadInput(dev, a3);
}

// ---------------------------------------------------------------------
// Control de la "camara 2" del juego en el diorama (modos 1-3, Supr activo)
//
// El juego ya tiene el control que se busca: en la camara 2, sin enemigo
// activo, el personaje va hacia donde se empuja el joystick respecto a la
// camara (+0x149950) y, con enemigo activo, vuelve al control clasico, que es
// el que hace los combos. En el diorama se usa ese mismo control con la
// camara 1: +0x4f030 contesta como en la camara 2 (salvo a la camara) y
// +0x149950 se ejecuta con el modo 2 y con AngY = la direccion de la vista
// sobre la maqueta; despues se devuelven el modo, AngY y lo que esa funcion
// toca de la camara (seguimiento +0x458/+0x460 y app+0x3f), asi que la
// camara 1 no cambia.
// ---------------------------------------------------------------------
using PFN_ActionAllowed = bool(__fastcall*)(void*, void*);
using PFN_ActionMove = void(__fastcall*)(void*, void*, double);
static PFN_ActionAllowed g_originalActionAllowed = nullptr;
static PFN_ActionMove g_originalActionMove = nullptr;

static bool DioramaGameActionControl() {
    return g_actionHooksInstalled && g_mode.load(std::memory_order_relaxed) == 1 && DioramaEnabled() &&
           g_dioramaCamera.load(std::memory_order_relaxed) &&
           g_dioramaSteer.load(std::memory_order_relaxed) == 1 &&
           g_dioramaViewYawValid.load(std::memory_order_relaxed) && DioramaViewSteeringAllowed();
}

// +0x1484a0 (y con el +0x149950) corre para cada persona del nivel; el
// control de la camara 2 se aplica solo al jugador (la IA no cambia).
static bool IsPlayer(void* app, void* person) {
    return app && person && *reinterpret_cast<void* const*>(static_cast<char*>(app) + kAppPlayerEntity) == person;
}

static bool __fastcall HookedActionAllowed(void* app, void* person) {
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ModuleBase();
    if (app && ret != kActionAllowedCameraReturn1 && ret != kActionAllowedCameraReturn2 &&
        IsPlayer(app, person) && DioramaGameActionControl()) {
        int* mode = reinterpret_cast<int*>(static_cast<char*>(app) + kAppCameraMode);
        int saved = *mode;
        *mode = 2;
        bool r = g_originalActionAllowed(app, person);
        *mode = saved;
        return r;
    }
    return g_originalActionAllowed(app, person);
}

static void __fastcall HookedActionMove(void* ctrl, void* person, double t) {
    char* app = DioramaGameActionControl() ? *reinterpret_cast<char**>(ModuleBase() + kAppPointerOffset) : nullptr;
    char* cam = app && IsPlayer(app, person) ? *reinterpret_cast<char**>(app + kAppCameraEntity) : nullptr;
    if (!cam) {
        g_originalActionMove(ctrl, person, t);
        return;
    }
    int* mode = reinterpret_cast<int*>(app + kAppCameraMode);
    unsigned char* rotated = reinterpret_cast<unsigned char*>(app + kAppCameraRotated);
    double* angY = reinterpret_cast<double*>(cam + kCameraAngY);
    double* followYaw = reinterpret_cast<double*>(cam + kCameraFollowYaw);
    double* followPitch = reinterpret_cast<double*>(cam + kCameraFollowPitch);
    int savedMode = *mode;
    unsigned char savedRotated = *rotated;
    double savedAngY = *angY, savedYaw = *followYaw, savedPitch = *followPitch;
    // Direccion de la vista sobre la maqueta (guinada atan2(x, z)) en el
    // convenio de +0x73a00, en [0, 2pi).
    double a = std::fmod(-g_dioramaViewYaw.load(std::memory_order_relaxed), 2.0 * kPi);
    if (a < 0.0) a += 2.0 * kPi;
    *mode = 2;
    *angY = a;
    g_originalActionMove(ctrl, person, t);
    *mode = savedMode;
    *rotated = savedRotated;
    *angY = savedAngY;
    *followYaw = savedYaw;
    *followPitch = savedPitch;
}

static void InstallActionControlHooks() {
    if (g_actionHooksInstalled) return;
    uintptr_t base = ModuleBase();
    if (!base) return;
    static const unsigned char kAllowedBytes[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static const unsigned char kMoveBytes[13] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56 };
    void* allowed = reinterpret_cast<void*>(base + kActionAllowedOffset);
    void* move = reinterpret_cast<void*>(base + kActionMoveOffset);
    if (std::memcmp(allowed, kAllowedBytes, sizeof(kAllowedBytes)) != 0 ||
        std::memcmp(move, kMoveBytes, sizeof(kMoveBytes)) != 0) {
        HookLogger::Instance().Line("[HEAD_TRACK] AVISO: bytes inesperados en el control de la camara 2 (+0x4f030/+0x149950); en diorama se usa el joystick reinterpretado.");
        return;
    }
    if (MH_CreateHook(allowed, reinterpret_cast<void*>(&HookedActionAllowed),
                      reinterpret_cast<void**>(&g_originalActionAllowed)) != MH_OK) {
        HookLogger::Instance().Line("[HEAD_TRACK] AVISO: no se pudo enganchar +0x4f030.");
        return;
    }
    if (MH_CreateHook(move, reinterpret_cast<void*>(&HookedActionMove),
                      reinterpret_cast<void**>(&g_originalActionMove)) != MH_OK) {
        MH_RemoveHook(allowed);
        HookLogger::Instance().Line("[HEAD_TRACK] AVISO: no se pudo enganchar +0x149950.");
        return;
    }
    if (MH_EnableHook(allowed) != MH_OK || MH_EnableHook(move) != MH_OK) {
        MH_DisableHook(allowed);
        MH_DisableHook(move);
        HookLogger::Instance().Line("[HEAD_TRACK] AVISO: no se pudieron activar los ganchos del control de la camara 2.");
        return;
    }
    g_actionHooksInstalled = true;
}

static void InstallGamepadHook() {
    if (g_gamepadHookInstalled) return;
    uintptr_t base = ModuleBase();
    if (!base) return;
    static const unsigned char kExpected[7] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10 };
    void* target = reinterpret_cast<void*>(base + kGamepadProcessOffset);
    if (std::memcmp(target, kExpected, sizeof(kExpected)) != 0) {
        HookLogger::Instance().Line("[HEAD_TRACK] AVISO: bytes inesperados en el proceso del mando (+0x27b2c0); en diorama con la camara clasica el joystick no sera relativo a la mirada.");
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedGamepadProcess),
                      reinterpret_cast<void**>(&g_originalGamepadProcess)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        HookLogger::Instance().Line("[HEAD_TRACK] AVISO: no se pudo enganchar el proceso del mando (+0x27b2c0).");
        return;
    }
    g_gamepadHookInstalled = true;
}

static bool InstallFromWorldWriterHook() {
    if (g_fromWorldWriterHookInstalled) return true;
    uintptr_t base = ModuleBase();
    if (!base) return false;
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(static_cast<DWORD64>(base + kFromWorldWriterRipOffset), &imageBase, nullptr);
    if (!entry) {
        HookLogger::Instance().Line("[HEAD_TRACK] ERROR: RtlLookupFunctionEntry no encontro la funcion de Blade.exe+0x72e55.");
        return false;
    }
    for (int hops = 0; hops < 8; ++hops) {
        const uint8_t* unwind = reinterpret_cast<const uint8_t*>(imageBase + entry->UnwindData);
        uint8_t flags = unwind[0] >> 3;
        if ((flags & 0x4) == 0) break; // UNW_FLAG_CHAININFO
        uint8_t countOfCodes = unwind[2];
        const RUNTIME_FUNCTION* parent = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + 4 + ((countOfCodes + 1) & ~1) * 2);
        entry = const_cast<PRUNTIME_FUNCTION>(parent);
    }
    g_fromWorldWriterStart = static_cast<uintptr_t>(imageBase + entry->BeginAddress);
    void* target = reinterpret_cast<void*>(g_fromWorldWriterStart);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedFromWorldWriter),
                      reinterpret_cast<void**>(&g_originalFromWorldWriter)) != MH_OK) {
        HookLogger::Instance().Line("[HEAD_TRACK] ERROR: MH_CreateHook fallo (escritor de fromWorld).");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        HookLogger::Instance().Line("[HEAD_TRACK] ERROR: MH_EnableHook fallo (escritor de fromWorld).");
        return false;
    }
    g_fromWorldWriterHookInstalled = true;
    std::ostringstream o;
    o << "[HEAD_TRACK] Hook de la funcion que escribe fromWorld instalado en " << DescribeAddress(g_fromWorldWriterStart);
    HookLogger::Instance().Line(o.str());
    return true;
}

bool HeadTrackGetTrackingToGameMap(TrackingToGameMap* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(g_mapMutex);
    if (!g_trackMapValid) return false;
    *out = g_trackMap;
    return true;
}

bool InstallHeadTrackHooks() {
    bool ok = InstallFromWorldWriterHook();
    InstallRawTurnHook();   // si falla, solo Supr deja de girar al personaje
    InstallGamepadHook();   // si falla, solo el joystick relativo a la mirada (camara clasica)
    InstallActionControlHooks();   // si falla, el joystick reinterpretado de antes
    return ok;
}

void UninstallHeadTrackHooks() {
    UninstallRawTurnHook();
    if (g_gamepadHookInstalled) {
        MH_DisableHook(reinterpret_cast<void*>(ModuleBase() + kGamepadProcessOffset));
        g_gamepadHookInstalled = false;
    }
    if (g_actionHooksInstalled) {
        g_actionHooksInstalled = false;
        MH_DisableHook(reinterpret_cast<void*>(ModuleBase() + kActionMoveOffset));
        MH_DisableHook(reinterpret_cast<void*>(ModuleBase() + kActionAllowedOffset));
    }
    if (g_fromWorldWriterHookInstalled && g_fromWorldWriterStart) {
        MH_DisableHook(reinterpret_cast<void*>(g_fromWorldWriterStart));
        g_fromWorldWriterHookInstalled = false;
    }
}

// --- bloque de camara del culling (+0x80f20) ----------------------------
void HeadTrackSyncCullingCamera(long long param_2) {
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    double* fromWorld = TryGetOrResolveFromWorldPointer();
    if (!fromWorld) return;
    g_cullCallsSec.fetch_add(1, std::memory_order_relaxed);
    if (static_cast<uintptr_t>(param_2) + kPoseFromWorldOffset != reinterpret_cast<uintptr_t>(fromWorld)) {
        g_cullOtherPoseSec.fetch_add(1, std::memory_order_relaxed);
    }
    double fw[16];
    if (!SafeReadMatrix16(fromWorld, fw)) return;
    // Camaras fijas (seleccion de personaje, cinematicas): el juego vuelve a
    // escribir fromWorld despues del escritor enganchado (p. ej. +0x58d34),
    // asi que aqui, justo antes del culling, se aplica el seguimiento sobre
    // esa matriz definitiva. En partida la matriz ya es la nuestra y no se
    // toca (evita aplicar el giro dos veces).
    if (!IsOurLastWrittenMatrix(fw)) {
        g_fwLateRewritesSec.fetch_add(1, std::memory_order_relaxed);
        StereoNotifyCameraPose();   // el juego ha tocado la camara: logica viva
        // Con el diorama puesto, tambien la maqueta (cinematicas con camara fija).
        bool dioramaOk = DioramaEnabled();
        g_cineCall = dioramaOk && CinematicRecent();
        ApplyHeadTrackingToFromWorld(fromWorld, false, dioramaOk);
        g_cineCall = false;
        if (!SafeReadMatrix16(fromWorld, fw)) return;
    } else if (RefreshStaleFromWorld(fromWorld)) {
        // Camara estatica (menu principal): la matriz era nuestra pero de un
        // fotograma anterior; ya esta rehecha con la pose actual.
        g_fwStaleRefreshSec.fetch_add(1, std::memory_order_relaxed);
        if (!SafeReadMatrix16(fromWorld, fw)) return;
    }
    double rot[12], pos[6];
    if (!SafeReadCullBlock(param_2, rot, pos)) return;
    // Se guardan posicion y angulos del juego para devolverselos tras el
    // culling: los controladores de las cinematicas los leen para seguir su
    // recorrido (con nuestros angulos el giro final hacia el personaje se
    // quedaba a medias).
    for (int i = 0; i < 6; ++i) g_savedCullPos[i] = pos[i];
    g_savedCullPosValid = true;
    // El objeto de pose guarda posicion (+0x68) y los tres angulos con los
    // que el juego construye fromWorld = T(-c) * Ry(guinada) * Rx(-cabeceo)
    // * Rz(-alabeo) (vectores fila): +0x80 guinada, +0x88 cabeceo, +0x90
    // alabeo. Se extraen los tres de la matriz reescrita (el alabeo del visor
    // no es cero) para que todo lo que el juego derive de los angulos sea
    // coherente con la matriz.
    double newPos[6];
    for (int rr = 0; rr < 3; ++rr) {
        newPos[rr] = -(fw[12] * fw[rr * 4 + 0] + fw[13] * fw[rr * 4 + 1] + fw[14] * fw[rr * 4 + 2]);
    }
    newPos[3] = std::atan2(-fw[2], fw[10]);        // guinada: R[0][2] = -sin(a)cos(b), R[2][2] = cos(a)cos(b)
    double s = fw[6];                              // R[1][2] = -sin(b)
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    newPos[4] = -std::asin(s);                     // cabeceo
    newPos[5] = std::atan2(fw[4], fw[5]);          // alabeo: R[1][0] = sin(g)cos(b), R[1][1] = cos(g)cos(b)
    SafeWriteCullBlock(param_2, fw, newPos);
}

void HeadTrackRestoreCullingCamera(long long param_2) {
    if (!g_savedCullPosValid) return;
    g_savedCullPosValid = false;
    SafeWritePos6(param_2, g_savedCullPos);
}

} // namespace BladeVR
