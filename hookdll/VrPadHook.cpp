#include "VrPadHook.h"
#include "VrInput.h"
#include "CombatVRHook.h"
#include "DioramaHook.h"
#include "HeadTrackHook.h"
#include "HookLogger.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace BladeVR {

namespace {

// --- lo que el juego pide por nombre (game_actions_1710170.vdf) --------------
// Mismo orden que la tabla del juego (+0x9c4000, 15 botones).
enum PadBtn {
    P_SOUTH, P_EAST, P_WEST, P_NORTH, P_BACK, P_GUIDE, P_START, P_LSTICK, P_RSTICK, P_LSHOULDER, P_RSHOULDER,
    P_UP, P_DOWN, P_LEFT, P_RIGHT, P_BTN_COUNT
};
const char* const kBtnNames[P_BTN_COUNT] = {
    "ButtonSouth", "ButtonEast", "ButtonWest", "ButtonNorth", "ButtonBack", "ButtonGuide", "ButtonStart",
    "ButtonLeftStick", "ButtonRightStick", "ButtonLeftShoulder", "ButtonRightShoulder", "ButtonUp", "ButtonDown",
    "ButtonLeft", "ButtonRight"
};
enum PadAx { P_LJOY, P_RJOY, P_LT, P_RT, P_AX_COUNT };
const char* const kAxNames[P_AX_COUNT] = { "LeftJoystick", "RightJoystick", "ButtonLeftTrigger", "ButtonRightTrigger" };

struct PadOut {
    bool btn[P_BTN_COUNT];
    float ax[P_AX_COUNT][2];
};

constexpr float kDeadzoneMove = 0.40f;   // los joysticks de VR derivan (el del usuario hasta 0,30 en x, medido en el log; 0,40 desde la ronda 12)
constexpr float kDeadzoneLook = 0.15f;
// Ronda 71 (pedido del usuario; sustituye a la 70): en el modo de combate 0
// (VR basico) los mandos de Quest son un mando de Xbox tal cual.
// - Joysticks: los valores del mando sin tocar (solo la zona muerta del juego,
//   0,15): sin la zona muerta del mod, sin mirar el toque del pulgar, sin tope
//   de andar (a fondo corre), sin histeresis ni sprint. Un joystick que deriva
//   se nota (el del usuario); uno bien, no.
// - Botones: los del Xbox. B = saltar y seleccionar objeto, Y = darse la
//   vuelta, clic izquierdo = L3 (cambia la capa de la maqueta, como el Xbox:
//   ronda 72), clic derecho = R3 (ultima vista).
//   Lo del mod va con el pulgar en el reposapulgares: pulgar izquierdo + clic
//   derecho corto = recentrar (mantenido 1 s, diorama); pulgar derecho + clic
//   izquierdo mantenido 1 s = modo de combate; pulgar izquierdo = cruceta,
//   Start y Back (los Quest no los tienen).
// Andar y sprint: el juego corre (Run = 1, persona+0x2bd8) cuando su eje Y
// pasa de 0,91 tras su zona muerta (0,15), es decir, con nuestro valor por
// encima de ~0,92. Andando, el joystick se limita a 0,85 (nunca corre);
// con el sprint (clic del joystick izquierdo) va a fondo en su direccion.
// El sprint acaba al soltar el joystick (kSprintStopMs) o con otro clic.
constexpr float kWalkMaxMag = 0.85f;
constexpr unsigned long long kSprintArmMs = 1500;     // clic con el joystick suelto: espera a moverse
constexpr unsigned long long kSprintStopMs = 300;
// Ronda 15: con el sprint cualquier inclinacion fuera de la zona muerta se
// ponia a fondo (el pulgar apoyado tras el clic lo hacia correr solo). Para
// correr hace falta inclinarlo kSprintMinOn (tras la zona muerta, ~0,58 crudo);
// por debajo de kSprintMinOff cuenta como suelto.
constexpr float kSprintMinOn = 0.30f;
constexpr float kSprintMinOff = 0.20f;
constexpr float kDpadOn = 0.55f;
constexpr float kDpadOff = 0.35f;
constexpr unsigned long long kMinPressMs = 60;   // que el juego vea hasta el toque mas corto
constexpr unsigned kToastMs = 2500;

// Bloquear con el escudo levantando el brazo izquierdo: el mando izquierdo
// delante del visor (en su direccion horizontal) y a la altura del hombro o
// mas arriba, en el espacio de seguimiento (no depende de la camara del
// juego). Histeresis y 80 ms para entrar; solo con escudo en el antebrazo y
// el brazo siguiendo al mando (CombatVRShieldGestureAllowed).
// Ronda 14: solo el escudo alzado delante (a la altura de los hombros o mas
// arriba y de cara hacia delante), como el bloqueo del juego, que lo sube
// delante; el modo "de frente" a cualquier altura se quita y al bajar el
// brazo (mas de 0,32 m bajo los ojos) se deja de bloquear (antes 0,45).
constexpr float kShieldFwdOn = 0.20f;          // m delante del visor
constexpr float kShieldFwdOff = 0.15f;
constexpr float kShieldDropOn = 0.25f;         // m como mucho por debajo de los ojos
constexpr float kShieldDropOff = 0.32f;
constexpr float kShieldRaisedFaceOn = 0.50f;   // cara del escudo a menos de 60 grados de la vista
constexpr float kShieldRaisedFaceOff = 0.30f;
constexpr float kShieldSideOn = 0.45f;         // m a un lado como mucho
constexpr float kShieldSideOff = 0.55f;
constexpr unsigned long long kShieldConfirmMs = 80;
// (La cara del escudo mira como el dorso de la mano izquierda: eje -X del
// mando izquierdo. Quitado en la ronda 14: el escudo "de frente" a cualquier
// altura, hasta la cadera.)
// Ejes del joystick izquierdo con histeresis alrededor del umbral de las teclas
// del juego (JoyUp/Down/Left/Right: 0,3 tras su zona muerta de 0,15, es decir
// 0,405 de lo que se manda). Andando en diagonal un eje rondaba ese umbral: la
// tecla se soltaba y se volvia a pulsar en menos de 0,125 s y el juego lo toma
// por un doble toque (Actions.FrwdDown: Run = 1), o daba tirones.
constexpr float kAxisOn = 0.50f;               // la tecla del juego entra por encima de esto
constexpr float kAxisOff = 0.30f;              // y sale por debajo
constexpr float kAxisHeldMin = 0.46f;          // pulsada: nunca por debajo del umbral del juego
constexpr float kAxisIdleMax = 0.36f;          // suelta: nunca por encima
// Ronda 64: el sprint dejaba al personaje clavado. Con el eje Y por encima de
// su umbral de correr (+0x68, 0,91) el juego (+0x27bfd0) no mira los flancos de
// JoyUp/JoyDown si Gof o Gob estan puestos: solo pone Run = 1. El sprint pasaba
// de golpe de 0 (o del bache del pulgar al hacer clic) a 1,0; con Gof aun a 1
// (recien parado) JoyUp no se pulsaba nunca mientras el joystick siguiera a
// fondo. Un mando de verdad pasa por valores intermedios: aqui, cuando la tecla
// de ese lado no esta pulsada, primero kRunRampY durante kRunRampMs (pulsa la
// tecla por el camino normal) y despues a fondo (Run = 1).
constexpr float kRunRampY = 0.85f;             // por debajo del umbral de correr del juego
constexpr unsigned long long kRunRampMs = 100;
constexpr float kRunRampKeyOn = 0.41f;         // lo mandado con la tecla del juego pulsada (entra a 0,405)

// --- Steam Input ---------------------------------------------------------------
// Asa del mando de los Quest y asas de accion propias (Steam da 0 si no hay
// ningun mando suyo encendido: aun no tiene la configuracion del juego).
constexpr unsigned long long kFakeHandle = 0x0B1ADE5000000001ull;
constexpr unsigned long long kSynthDigital = 0x0B1ADE5100000000ull;
constexpr unsigned long long kSynthAnalog = 0x0B1ADE5200000000ull;
constexpr unsigned long long kSynthMask = 0xFFFFFFFF00000000ull;
constexpr int kSteamInputTypeXBoxOne = 3;        // ESteamInputType
constexpr int kSourceModeJoystickMove = 6;       // EInputSourceMode
constexpr int kSourceModeTrigger = 10;

// Entradas de la tabla virtual de SteamInput006 que usa el juego.
constexpr int kSlotGetConnected = 0x30 / 8;
constexpr int kSlotDigitalHandle = 0x80 / 8;
constexpr int kSlotDigitalData = 0x88 / 8;
constexpr int kSlotAnalogHandle = 0xa0 / 8;
constexpr int kSlotAnalogData = 0xa8 / 8;
constexpr int kSlotVibration = 0xf0 / 8;
constexpr int kSlotInputType = 0x128 / 8;

struct SteamDigital { bool bState; bool bActive; };
struct SteamAnalog { int eMode; float x; float y; bool bActive; };
using PFN_SteamGetHSteamUser = int(__cdecl*)();
using PFN_SteamFindOrCreateUserInterface = void*(__cdecl*)(int, const char*);
using PFN_GetConnected = int(__fastcall*)(void*, unsigned long long*);
using PFN_GetHandle = unsigned long long(__fastcall*)(void*, const char*);
using PFN_GetDigital = SteamDigital*(__fastcall*)(void*, SteamDigital*, unsigned long long, unsigned long long);
using PFN_GetAnalog = SteamAnalog*(__fastcall*)(void*, SteamAnalog*, unsigned long long, unsigned long long);
using PFN_Vibration = void(__fastcall*)(void*, unsigned long long, unsigned short, unsigned short);
using PFN_GetType = int(__fastcall*)(void*, unsigned long long);

PFN_GetConnected g_oGetConnected = nullptr;
PFN_GetHandle g_oDigitalHandle = nullptr;
PFN_GetDigital g_oDigital = nullptr;
PFN_GetHandle g_oAnalogHandle = nullptr;
PFN_GetAnalog g_oAnalog = nullptr;
PFN_Vibration g_oVibration = nullptr;
PFN_GetType g_oGetType = nullptr;
bool g_installTried = false;

SRWLOCK g_padLock = SRWLOCK_INIT;
PadOut g_pad = {};                                  // lo que ve el juego (bajo g_padLock)
std::atomic<bool> g_present{false};                 // hay mandos de Quest: el juego ve un mando mas
std::atomic<unsigned long long> g_hBtn[P_BTN_COUNT];
std::atomic<unsigned long long> g_hAx[P_AX_COUNT];
std::atomic<unsigned> g_vib[2];                     // 0..65535 (motor izq. / der.)
std::atomic<unsigned long long> g_vibTick{0};
std::atomic<bool> g_seenByGame{false};              // el juego ya pidio el tipo del mando de los Quest

// --- hilo de Present ------------------------------------------------------------
bool g_prevClick[2] = { false, false };
// Ronda 37: al diorama hay que mantener el clic derecho kDioramaHoldMs con el
// pulgar izquierdo en reposo (con el agarre izquierdo apretado el pulgar queda
// en el reposapulgares y un clic para recentrar lo metia en el diorama: el
// juego "iba lento y se quedaba parado"). Un clic corto recentra.
constexpr unsigned long long kDioramaHoldMs = 1000;
unsigned long long g_rClickAt = 0;   // clic derecho apretado (0 = no, o ya resuelto)
// Ronda 53 (pedido del usuario): el modo de combate (pulgar derecho en reposo +
// clic izquierdo) tambien hay que mantenerlo kDioramaHoldMs; un clic corto es
// el sprint de siempre.
unsigned long long g_lClickAt = 0;   // clic izquierdo con el pulgar derecho en reposo (0 = no, o ya resuelto)
bool g_mask[2] = { false, false };
bool g_l3Held = false, g_r3Held = false;            // ronda 71: L3 / R3 en el modo 0 (el L3 lo toma la capa de la maqueta)
// Ronda 62 (pedido del usuario): en el diorama, pulgar derecho en reposo + clic
// del joystick izquierdo mantenido kDioramaHoldMs: los mandos de Quest manejan
// al personaje como el mando del juego (y el diorama no los ve); otra vez,
// vuelven a la maqueta. Al salir del diorama se vuelve a la maqueta.
std::atomic<bool> g_dioGame{false};
unsigned long long g_dioClickAt = 0;
bool g_aPrev = false, g_bPrev = false, g_yPrev = false;
int g_aRoute = -1, g_bRoute = -1, g_yRoute = -1;    // boton que se manda mientras se mantiene (-1 ninguno)
bool g_dpad[4] = { false, false, false, false };
bool g_rawPrev[P_BTN_COUNT] = {};
unsigned long long g_holdUntil[P_BTN_COUNT] = {};
unsigned long long g_lastPulse[2] = { 0, 0 };
bool g_actionsLogged = false;
bool g_presentLogged = false;
wchar_t g_toastTitle[64];
wchar_t g_toastText[160];
bool g_toastPending = false;
bool g_sprint = false;                              // sprint (clic del joystick izquierdo)
std::atomic<bool> g_sprintPub{false};               // el sprint para CombatVR (hilo del juego)
unsigned long long g_sprintArmedUntil = 0;
unsigned long long g_sprintNeutralSince = 0;
unsigned long long g_sprintClickAt = 0;             // ronda 15: el clic no acaba el sprint en los primeros 0,6 s
bool g_sprintStick = false;                         // ronda 15: joystick inclinado lo bastante para correr
float g_lastSentY = 0.0f;                           // ronda 64: eje Y izquierdo mandado al juego el fotograma anterior
int g_rampSign = 0;                                 // ronda 64: lado hacia el que corre (0 = no corre)
unsigned long long g_rampUntil = 0;
// Ronda 17: el joystick izquierdo solo cuenta con el pulgar encima (toque
// capacitivo del joystick, accion left_stick_touch). El del usuario deriva en
// reposo hasta 0,6 en x: con un enemigo fijado eso es un paso lateral y el
// personaje daba vueltas a su alrededor. Si el toque no llega (enlaces viejos
// guardados en SteamVR), con el pulgar en el reposapulgares.
bool g_lTouchSeen = false;                          // el toque llego alguna vez (enlace que funciona)
bool g_shieldUp = false;                            // bloqueo por gesto activo
bool g_axisOn[2] = { false, false };                // joystick izquierdo: tecla del juego pulsada (x, y)
unsigned long long g_shieldCandidate = 0;           // desde cuando esta el mando en la zona
// --- ronda 30 (pedido del usuario): gesto de la espalda ----------------------
// Zona de la espalda: el mando por encima del hombro y detras de los ojos, en
// el espacio de seguimiento respecto al visor (su adelante en horizontal).
// Apretar el agarre de esa mano alli guarda las armas (si estan fuera) o las
// saca (ronda 31: sin esperar quieta; los punetazos los ve CombatVR). Ese
// agarre no va al juego (LB bloquear / RB lanzar) hasta soltarlo.
constexpr float kBackBehindOn = 0.08f;     // m detras de los ojos
constexpr float kBackBehindOff = 0.04f;
constexpr float kBackDropOn = 0.30f;       // m como mucho por debajo de los ojos (hombros ~0,25)
constexpr float kBackDropOff = 0.36f;
constexpr float kBackSideOn = 0.50f;       // m a un lado como mucho
constexpr float kBackSideOff = 0.58f;
constexpr unsigned long long kToggleCooldownMs = 1200;
bool g_backIn[2] = { false, false };
bool g_gripPrev[2] = { false, false };
bool g_gripMod[2] = { false, false };          // agarre apretado en la zona: del gesto hasta soltarlo
bool g_lGripTwoHand = false;                   // ronda 36: agarre izquierdo para sujetar el arma a dos manos (hasta soltarlo)
bool g_lGripPrev2h = false;
unsigned long long g_toggleCooldownUntil = 0;
constexpr unsigned long long kArrowCooldownMs = 400;   // ronda 51: sacar flecha con el gesto de la espalda
unsigned long long g_arrowCooldownUntil = 0;
// --- ronda 32 (pedido del usuario): lanzar con el gesto (modo 1) ---------------
// El juego lanza con "mantener lanzar (RB) + atacar (A)" (TestThrowRight):
// con lanzar mantenido menos de THROW_TIME_MIN (0,4 s) A solo suelta el
// objeto; despues lo lanza con la fuerza de la barra (llena a 1 s). En el
// modo 1 A no va al juego, asi que con el agarre derecho mantenido al menos
// kThrowMinHoldMs, bajar el brazo derecho deprisa despues de tenerlo a la
// altura de la cabeza (como mucho kThrowRaisedDrop bajo los ojos en los
// ultimos kThrowRaisedValidMs) manda A. Una vez por pulsacion del agarre.
constexpr unsigned long long kThrowMinHoldMs = 450;
constexpr float kThrowRaisedDrop = 0.15f;
constexpr unsigned long long kThrowRaisedValidMs = 1500;
constexpr float kThrowSpeed = 2.0f;           // m/s del mando
constexpr float kThrowDownSpeed = 0.8f;       // m/s hacia abajo
constexpr float kThrowBackMax = 0.3f;         // m/s hacia atras como mucho (no al echar el brazo atras)
constexpr int kThrowFrames = 2;
constexpr unsigned long long kThrowPressMs = 80;
unsigned long long g_rbHeldSince = 0;
unsigned long long g_throwRaisedAt = 0;
bool g_throwDone = false;                      // ya se lanzo con esta pulsacion
int g_throwFast = 0;
float g_throwPrev[3];
double g_throwPrevT = 0.0;
bool g_throwHavePrev = false;
unsigned long long g_throwAUntil = 0;
// Ronda 32: gatillo derecho (fijar enemigo) con la mano derecha vacia: del mod.
// Ronda 33: de quien es la pulsacion se decide al empezar a apretar
// (kRtClaim, por debajo del umbral del juego, 0,2): con kRtOn el juego veia el
// gatillo un fotograma al pasar por 0,2 y su "fijar" sacaba las armas.
constexpr float kRtClaim = 0.08f;
constexpr float kRtRelease = 0.04f;
constexpr float kRtOn = 0.55f;
constexpr float kRtOff = 0.30f;
bool g_rtOn = false;
bool g_rtPressed = false;                      // el gatillo paso de kRtClaim (pulsacion en curso)
bool g_rtMod = false;                          // la pulsacion en curso es del mod (no va al juego)
// Ronda 50: arco VR. Con el arco en la izquierda y la mano derecha (con
// flecha) junto a el, el gatillo derecho es la cuerda: no va al juego.
bool g_rtBow = false;
std::atomic<bool> g_rtBowHeldPub{false};
// Joystick izquierdo para CombatVR y HeadTrack (VrPadGetLeftStick): lo enviado
// al juego.
std::atomic<float> g_leftSent[2];

void Log(const std::string& s) { HookLogger::Instance().Line(s); }

void CopyW(wchar_t* dst, int n, const wchar_t* src) {
    int i = 0;
    for (; src && src[i] && i < n - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
}

void Toast(const wchar_t* title, const wchar_t* text) {
    CopyW(g_toastTitle, 64, title);
    CopyW(g_toastText, 160, text);
    g_toastPending = true;
}

int IndexOf(const char* name, const char* const* table, int n) {
    if (!name) return -1;
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(name, table[i]) == 0) return i;
    }
    return -1;
}

int BtnByHandle(unsigned long long h) {
    if ((h & kSynthMask) == kSynthDigital) {
        unsigned long long i = h & ~kSynthMask;
        return i < P_BTN_COUNT ? static_cast<int>(i) : -1;
    }
    for (int i = 0; i < P_BTN_COUNT; ++i) {
        if (h != 0 && g_hBtn[i].load(std::memory_order_relaxed) == h) return i;
    }
    return -1;
}

int AxByHandle(unsigned long long h) {
    if ((h & kSynthMask) == kSynthAnalog) {
        unsigned long long i = h & ~kSynthMask;
        return i < P_AX_COUNT ? static_cast<int>(i) : -1;
    }
    for (int i = 0; i < P_AX_COUNT; ++i) {
        if (h != 0 && g_hAx[i].load(std::memory_order_relaxed) == h) return i;
    }
    return -1;
}

// --- envolturas de ISteamInput (hilo del juego) -----------------------------------
int __fastcall WGetConnected(void* self, unsigned long long* out) {
    int n = g_oGetConnected(self, out);
    {
        // Ronda 67: cuantos mandos de verdad ve Steam Input (en el log al cambiar).
        static int s_lastReal = -1;
        if (n != s_lastReal && n >= 0) {
            s_lastReal = n;
            char line[120];
            std::snprintf(line, sizeof(line), "[MANDO] Steam Input: %d mando(s) de verdad conectado(s) (sin contar los Quest).", n);
            Log(line);
        }
    }
    if (out && n >= 0 && n < 16 && g_present.load(std::memory_order_relaxed)) {
        bool have = false;
        for (int i = 0; i < n; ++i) {
            if (out[i] == kFakeHandle) have = true;
        }
        if (!have) out[n++] = kFakeHandle;
    }
    return n;
}

unsigned long long __fastcall WDigitalHandle(void* self, const char* name) {
    unsigned long long h = g_oDigitalHandle(self, name);
    int i = IndexOf(name, kBtnNames, P_BTN_COUNT);
    if (i >= 0) {
        if (h == 0) h = kSynthDigital + static_cast<unsigned long long>(i);
        g_hBtn[i].store(h, std::memory_order_relaxed);
    }
    return h;
}

unsigned long long __fastcall WAnalogHandle(void* self, const char* name) {
    unsigned long long h = g_oAnalogHandle(self, name);
    int i = IndexOf(name, kAxNames, P_AX_COUNT);
    if (i >= 0) {
        if (h == 0) h = kSynthAnalog + static_cast<unsigned long long>(i);
        g_hAx[i].store(h, std::memory_order_relaxed);
    }
    return h;
}

// Ronda 72: en el modo 0 los botones de los Quest pasan por la capa de la
// maqueta del mando del juego (DioramaHook), como los de un mando de Xbox por
// Steam Input: L3 la cambia y nunca llega al juego; con ella puesta, el juego
// no recibe botones ni joysticks (salvo Start, Back y Guia). Indice en el orden
// de DioramaHook (A, B, X, Y, LB, RB, arriba, abajo, izq, dcha, L3, R3).
int DioramaPadIndex(int b) {
    static const int kMap[P_BTN_COUNT] = { 0, 1, 2, 3, -1, -1, -1, 10, 11, 4, 5, 6, 7, 8, 9 };
    return b >= 0 && b < P_BTN_COUNT ? kMap[b] : -1;
}

bool QuestUsesPadLayer() { return CombatVRGetCombatMode() == 0 && !DioramaEnabled(); }

SteamDigital* __fastcall WDigital(void* self, SteamDigital* out, unsigned long long controller, unsigned long long action) {
    if (controller == kFakeHandle) {
        int i = BtnByHandle(action);
        bool down = false;
        if (i >= 0) {
            AcquireSRWLockShared(&g_padLock);
            down = g_pad.btn[i];
            ReleaseSRWLockShared(&g_padLock);
            int d = DioramaPadIndex(i);
            if (d >= 0 && QuestUsesPadLayer()) down = DioramaQuestPadButton(d, down);
        }
        if (out) {
            out->bState = down;
            out->bActive = i >= 0;
        }
        return out;
    }
    if ((action & kSynthMask) == kSynthDigital) {   // asa nuestra con un mando de verdad: nada
        if (out) { out->bState = false; out->bActive = false; }
        return out;
    }
    return g_oDigital(self, out, controller, action);
}

SteamAnalog* __fastcall WAnalog(void* self, SteamAnalog* out, unsigned long long controller, unsigned long long action) {
    if (controller == kFakeHandle) {
        int i = AxByHandle(action);
        float x = 0.0f, y = 0.0f;
        if (i >= 0) {
            AcquireSRWLockShared(&g_padLock);
            x = g_pad.ax[i][0];
            y = g_pad.ax[i][1];
            ReleaseSRWLockShared(&g_padLock);
            if (QuestUsesPadLayer() && DioramaPadLayerActive()) x = y = 0.0f;   // ronda 72: capa de la maqueta
        }
        if (out) {
            out->eMode = (i == P_LT || i == P_RT) ? kSourceModeTrigger : kSourceModeJoystickMove;
            out->x = x;
            out->y = y;
            out->bActive = i >= 0;
        }
        return out;
    }
    if ((action & kSynthMask) == kSynthAnalog) {
        if (out) { out->eMode = 0; out->x = 0.0f; out->y = 0.0f; out->bActive = false; }
        return out;
    }
    return g_oAnalog(self, out, controller, action);
}

void __fastcall WVibration(void* self, unsigned long long controller, unsigned short left, unsigned short right) {
    if (controller == kFakeHandle) {
        g_vib[0].store(left, std::memory_order_relaxed);
        g_vib[1].store(right, std::memory_order_relaxed);
        g_vibTick.store(GetTickCount64(), std::memory_order_relaxed);
        return;
    }
    g_oVibration(self, controller, left, right);
}

int __fastcall WGetType(void* self, unsigned long long controller) {
    if (controller == kFakeHandle) {
        g_seenByGame.store(true, std::memory_order_relaxed);
        return kSteamInputTypeXBoxOne;
    }
    return g_oGetType(self, controller);
}

// --- instalacion (hilo de Present) --------------------------------------------------
bool ReadVtable(void* iface, void*** vt) {
    __try {
        *vt = *reinterpret_cast<void***>(iface);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *vt != nullptr;
}

bool ReadSlot(void** vt, int slot, void** out) {
    __try {
        *out = vt[slot];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *out != nullptr;
}

bool WriteSlot(void** vt, int slot, void* fn) {
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    bool ok = true;
    __try {
        vt[slot] = fn;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DWORD tmp = 0;
    VirtualProtect(&vt[slot], sizeof(void*), old, &tmp);
    return ok;
}

std::string ModuleOf(void* p) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(p), &m) && m) {
        DWORD n = GetModuleFileNameA(m, name, MAX_PATH);
        std::string s(name, n);
        size_t slash = s.find_last_of("\\/");
        return slash == std::string::npos ? s : s.substr(slash + 1);
    }
    return "?";
}

void TryInstall() {
    if (g_installTried) return;
    HMODULE steam = GetModuleHandleA("steam_api64.dll");
    if (!steam) return;
    auto getUser = reinterpret_cast<PFN_SteamGetHSteamUser>(GetProcAddress(steam, "SteamAPI_GetHSteamUser"));
    auto findIface = reinterpret_cast<PFN_SteamFindOrCreateUserInterface>(
        GetProcAddress(steam, "SteamInternal_FindOrCreateUserInterface"));
    if (!getUser || !findIface) {
        g_installTried = true;
        Log("[MANDO] AVISO: steam_api64.dll sin las funciones esperadas; los mandos de Quest no llegan al juego.");
        return;
    }
    int user = getUser();
    if (user == 0) return;   // Steam aun no: se reintenta
    g_installTried = true;
    void* iface = findIface(user, "SteamInput006");
    void** vt = nullptr;
    if (!iface || !ReadVtable(iface, &vt)) {
        Log("[MANDO] AVISO: no se encontro ISteamInput (SteamInput006); los mandos de Quest no llegan al juego.");
        return;
    }
    void *gc = nullptr, *dh = nullptr, *dd = nullptr, *ah = nullptr, *ad = nullptr, *vb = nullptr, *ty = nullptr;
    if (!ReadSlot(vt, kSlotGetConnected, &gc) || !ReadSlot(vt, kSlotDigitalHandle, &dh) || !ReadSlot(vt, kSlotDigitalData, &dd) ||
        !ReadSlot(vt, kSlotAnalogHandle, &ah) || !ReadSlot(vt, kSlotAnalogData, &ad) || !ReadSlot(vt, kSlotVibration, &vb) ||
        !ReadSlot(vt, kSlotInputType, &ty)) {
        Log("[MANDO] AVISO: tabla de ISteamInput ilegible; los mandos de Quest no llegan al juego.");
        return;
    }
    g_oGetConnected = reinterpret_cast<PFN_GetConnected>(gc);
    g_oDigitalHandle = reinterpret_cast<PFN_GetHandle>(dh);
    g_oDigital = reinterpret_cast<PFN_GetDigital>(dd);
    g_oAnalogHandle = reinterpret_cast<PFN_GetHandle>(ah);
    g_oAnalog = reinterpret_cast<PFN_GetAnalog>(ad);
    g_oVibration = reinterpret_cast<PFN_Vibration>(vb);
    g_oGetType = reinterpret_cast<PFN_GetType>(ty);
    // Primero las asas y los datos; lo ultimo, anunciar el mando.
    bool ok = WriteSlot(vt, kSlotDigitalHandle, reinterpret_cast<void*>(&WDigitalHandle)) &&
              WriteSlot(vt, kSlotAnalogHandle, reinterpret_cast<void*>(&WAnalogHandle)) &&
              WriteSlot(vt, kSlotDigitalData, reinterpret_cast<void*>(&WDigital)) &&
              WriteSlot(vt, kSlotAnalogData, reinterpret_cast<void*>(&WAnalog)) &&
              WriteSlot(vt, kSlotVibration, reinterpret_cast<void*>(&WVibration)) &&
              WriteSlot(vt, kSlotInputType, reinterpret_cast<void*>(&WGetType)) &&
              WriteSlot(vt, kSlotGetConnected, reinterpret_cast<void*>(&WGetConnected));
    Log(std::string(ok ? "[MANDO] Steam Input enganchado: los mandos de Quest seran un mando de Xbox One para el juego "
                         "(tabla en "
                       : "[MANDO] ERROR: no se pudo cambiar la tabla de ISteamInput (") +
        ModuleOf(dd) + ").");
}

float Deadzone(float* x, float* y, float dz) {
    float m = std::sqrt((*x) * (*x) + (*y) * (*y));
    if (m <= dz || m <= 0.0f) {
        *x = 0.0f;
        *y = 0.0f;
        return 0.0f;
    }
    float s = (m - dz) / (1.0f - dz);
    if (s > 1.0f) s = 1.0f;
    *x *= s / m;
    *y *= s / m;
    return s;
}

bool Hyst(bool prev, float v) { return prev ? v > kDpadOff : v > kDpadOn; }

float AxisHyst(float v, bool* on) {
    float a = std::fabs(v);
    if (*on) {
        if (a < kAxisOff) *on = false;
    } else if (a > kAxisOn) {
        *on = true;
    }
    if (*on) {
        if (a < kAxisHeldMin) v = v < 0.0f ? -kAxisHeldMin : kAxisHeldMin;
    } else if (a > kAxisIdleMax) {
        v = v < 0.0f ? -kAxisIdleMax : kAxisIdleMax;
    }
    return v;
}

// Ronda 64: eje Y a fondo (sprint) con la tecla de ese lado sin pulsar: antes
// kRunRampY (ver kRunRampY).
float RunRampY(float y, unsigned long long now) {
    if (std::fabs(y) <= kRunRampY) return y;
    int s = y > 0.0f ? 1 : -1;
    if (s != g_rampSign) {
        g_rampSign = s;
        bool keyOn = g_lastSentY * static_cast<float>(s) >= kRunRampKeyOn;
        g_rampUntil = keyOn ? 0 : now + kRunRampMs;
    }
    return now < g_rampUntil ? static_cast<float>(s) * kRunRampY : y;
}

// Hilo de Present. true mientras el escudo esta levantado delante.
bool ShieldGesture(const VrHandState& L, unsigned long long now) {
    float hm[12];
    if (!L.valid || !CombatVRShieldGestureAllowed() || !GetHmdPoseMatrix34(hm)) {
        g_shieldCandidate = 0;
        g_shieldUp = false;
        return false;
    }
    // OpenVR: x derecha, y arriba, z atras. Adelante del visor en horizontal.
    float fx = -hm[2], fz = -hm[10];
    float fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 1e-3f) return g_shieldUp;   // mirando recto arriba o abajo: se queda como estaba
    fx /= fl;
    fz /= fl;
    float rx = -fz, rz = fx;             // derecha horizontal
    float dx = L.pose[3] - hm[3], dy = L.pose[7] - hm[7], dz = L.pose[11] - hm[11];
    float fwd = dx * fx + dz * fz;
    float side = std::fabs(dx * rx + dz * rz);
    float drop = -dy;
    // Cara del escudo = dorso de la mano izquierda = -X del mando (columna 0).
    float face = -(L.pose[0] * fx + L.pose[8] * fz);
    bool raisedOn = fwd > kShieldFwdOn && drop < kShieldDropOn && side < kShieldSideOn && face > kShieldRaisedFaceOn;
    bool raisedOff = fwd > kShieldFwdOff && drop < kShieldDropOff && side < kShieldSideOff && face > kShieldRaisedFaceOff;
    bool inZone = g_shieldUp ? raisedOff : raisedOn;
    if (g_shieldUp) {
        if (!inZone) {
            g_shieldCandidate = 0;
            g_shieldUp = false;
        }
        return g_shieldUp;
    }
    if (!inZone) {
        g_shieldCandidate = 0;
        return false;
    }
    if (g_shieldCandidate == 0) g_shieldCandidate = now;
    if (now - g_shieldCandidate < kShieldConfirmMs) return false;
    g_shieldUp = true;
    VrInputHaptic(0, 0.04f, 0.4f);
    return true;
}

double QpcNowSec() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

// Ronda 32. Hilo de Present, fuera del diorama. true = mandar A ahora (lanzar).
// rbHeld = el juego ve el agarre derecho (lanzar) apretado.
bool ThrowGesture(const VrHandState& R, unsigned long long now, bool rbHeld, bool allowed) {
    double t = QpcNowSec();
    float p[3] = { R.pose[3], R.pose[7], R.pose[11] };
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    double dt = t - g_throwPrevT;
    bool haveV = R.valid && g_throwHavePrev && dt > 0.004 && dt < 0.1;
    if (haveV) {
        vx = static_cast<float>((p[0] - g_throwPrev[0]) / dt);
        vy = static_cast<float>((p[1] - g_throwPrev[1]) / dt);
        vz = static_cast<float>((p[2] - g_throwPrev[2]) / dt);
    }
    for (int k = 0; k < 3; ++k) g_throwPrev[k] = p[k];
    g_throwPrevT = t;
    g_throwHavePrev = R.valid;
    if (!rbHeld) {
        g_rbHeldSince = 0;
        g_throwRaisedAt = 0;
        g_throwDone = false;
        g_throwFast = 0;
        return false;
    }
    if (g_rbHeldSince == 0) g_rbHeldSince = now;
    float hm[12];
    if (!allowed || g_throwDone || !R.valid || !GetHmdPoseMatrix34(hm)) {
        g_throwFast = 0;
        return false;
    }
    float fx = -hm[2], fz = -hm[10];
    float fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 1e-3f) return false;
    fx /= fl;
    fz /= fl;
    float drop = hm[7] - R.pose[7];   // m por debajo de los ojos
    if (drop < kThrowRaisedDrop) g_throwRaisedAt = now;
    bool raised = g_throwRaisedAt != 0 && now - g_throwRaisedAt < kThrowRaisedValidMs;
    bool charged = now - g_rbHeldSince >= kThrowMinHoldMs;
    float vm = std::sqrt(vx * vx + vy * vy + vz * vz);
    float vf = vx * fx + vz * fz;
    bool fast = haveV && vm > kThrowSpeed && -vy > kThrowDownSpeed && vf > -kThrowBackMax;
    g_throwFast = fast ? g_throwFast + 1 : 0;
    if (!(raised && charged && g_throwFast >= kThrowFrames)) return false;
    g_throwDone = true;
    g_throwFast = 0;
    VrInputHaptic(1, 0.06f, 0.8f);
    return true;
}

void ResetArmsGestures() {
    g_backIn[0] = g_backIn[1] = false;
}

// Ronda 30. Hilo de Present, fuera del diorama: gesto de la espalda (guardar o
// sacar las armas con el agarre). gripMod[h] = el agarre de esa mano es del
// gesto (no se manda al juego).
void ArmsGestures(const VrHandState hands[2], unsigned long long now, bool gripMod[2]) {
    bool weaponsOut = false;
    bool ok = CombatVRArmsGestureState(&weaponsOut);
    float hm[12];
    float fx = 0.0f, fz = 0.0f;
    bool haveFwd = false;
    if (GetHmdPoseMatrix34(hm)) {
        // OpenVR: x derecha, y arriba, z atras. Adelante del visor en horizontal.
        fx = -hm[2];
        fz = -hm[10];
        float fl = std::sqrt(fx * fx + fz * fz);
        if (fl > 1e-3f) {
            fx /= fl;
            fz /= fl;
            haveFwd = true;
        }
    }
    for (int h = 0; h < 2; ++h) {
        const VrHandState& H = hands[h];
        bool grip = H.valid && H.grip;
        bool gripPress = grip && !g_gripPrev[h];
        g_gripPrev[h] = grip;
        if (!grip) g_gripMod[h] = false;
        gripMod[h] = g_gripMod[h];
        if (!ok || !H.valid || !haveFwd) {
            g_backIn[h] = false;
            continue;
        }
        float rx = -fz, rz = fx;   // derecha horizontal
        float dx = H.pose[3] - hm[3], dy = H.pose[7] - hm[7], dz = H.pose[11] - hm[11];
        float ahead = dx * fx + dz * fz;
        float side = std::fabs(dx * rx + dz * rz);
        float drop = -dy;
        bool inOn = -ahead > kBackBehindOn && drop < kBackDropOn && side < kBackSideOn;
        bool inOff = -ahead > kBackBehindOff && drop < kBackDropOff && side < kBackSideOff;
        bool in = g_backIn[h] ? inOff : inOn;
        g_backIn[h] = in;
        if (!in || !gripPress) continue;
        g_gripMod[h] = true;
        gripMod[h] = true;
        // Ronda 51 (pedido del usuario): con el arco VR, el agarre derecho en la
        // espalda saca una flecha del carcaj (va a la espalda derecha); el
        // izquierdo sigue guardando y sacando las armas.
        if (h == 1 && CombatVRBowArrowGesture()) {
            if (now < g_arrowCooldownUntil) continue;
            g_arrowCooldownUntil = now + kArrowCooldownMs;
            CombatVRRequestBowArrow();
            VrInputHaptic(h, 0.04f, 0.5f);
            continue;
        }
        if (now < g_toggleCooldownUntil) continue;
        CombatVRRequestWeaponToggle(weaponsOut);   // fuera: guardar; guardadas: sacar
        g_toggleCooldownUntil = now + kToggleCooldownMs;
        VrInputHaptic(h, 0.08f, 0.8f);
    }
}

// Ronda 62. Hilo de Present, en el diorama con g_dioGame: los mandos de Quest
// como el mando del juego, con los botones de fuera del diorama: joystick
// izquierdo andar (clic: sprint), derecho camara, A atacar, B saltar /
// seleccionar objeto, X usar, Y darse la vuelta, agarres bloquear (LB) y
// lanzar (RB), gatillos sigilo (LT) y fijar (RT); con el pulgar izquierdo en
// reposo, el joystick derecho es la cruceta y A / B son Start / Back. Clic
// derecho: recolocar la maqueta (recentrar). Sin gestos (en el diorama no hay
// brazos con los mandos).
void DioramaGamePad(const VrHandState& L, const VrHandState& R, bool shift, bool lPress, bool rPress, bool aPress, bool bPress,
                    bool aRaw, bool bRaw, unsigned long long now, PadOut& out) {
    if (lPress) {
        g_mask[0] = true;
        g_sprint = !g_sprint;
        g_sprintArmedUntil = g_sprint ? now + kSprintArmMs : 0;
        g_sprintClickAt = now;
        g_sprintNeutralSince = 0;
        if (g_sprint) VrInputHaptic(0, 0.03f, 0.5f);
    }
    if (rPress && !shift) {
        g_mask[1] = true;
        HeadTrackRequestRecenter();
    }
    if (aPress) g_aRoute = shift ? P_START : P_SOUTH;
    if (!aRaw) g_aRoute = -1;
    if (g_aRoute >= 0) out.btn[g_aRoute] = true;
    if (bPress) g_bRoute = shift ? P_BACK : P_EAST;
    if (!bRaw) g_bRoute = -1;
    if (g_bRoute >= 0) out.btn[g_bRoute] = true;
    out.btn[P_WEST] = L.valid && L.buttonA;    // X
    out.btn[P_NORTH] = L.valid && L.buttonB;   // Y
    out.btn[P_LSHOULDER] = L.valid && L.grip;
    out.btn[P_RSHOULDER] = R.valid && R.grip;
    out.ax[P_LT][0] = L.valid ? L.triggerValue : 0.0f;
    out.ax[P_RT][0] = R.valid ? R.triggerValue : 0.0f;
    float lx = L.valid ? L.stickX : 0.0f, ly = L.valid ? L.stickY : 0.0f;
    float rx = R.valid ? R.stickX : 0.0f, ry = R.valid ? R.stickY : 0.0f;
    if (shift) {
        g_dpad[0] = Hyst(g_dpad[0], ry);
        g_dpad[1] = Hyst(g_dpad[1], -ry);
        g_dpad[2] = Hyst(g_dpad[2], -rx);
        g_dpad[3] = Hyst(g_dpad[3], rx);
        rx = ry = 0.0f;
    } else {
        g_dpad[0] = g_dpad[1] = g_dpad[2] = g_dpad[3] = false;
    }
    out.btn[P_UP] = g_dpad[0];
    out.btn[P_DOWN] = g_dpad[1];
    out.btn[P_LEFT] = g_dpad[2];
    out.btn[P_RIGHT] = g_dpad[3];
    // Joystick izquierdo como fuera del diorama: solo con el pulgar encima,
    // andar hasta kWalkMaxMag y a fondo con el sprint.
    if (L.valid && L.stickTouchKnown && L.stickTouch) g_lTouchSeen = true;
    bool touchGate = L.valid && L.stickTouchKnown && g_lTouchSeen;
    bool thumbOff = touchGate ? !L.stickTouch : shift;
    if (thumbOff) {
        lx = 0.0f;
        ly = 0.0f;
    }
    Deadzone(&lx, &ly, kDeadzoneMove);
    Deadzone(&rx, &ry, kDeadzoneLook);
    bool moving = lx != 0.0f || ly != 0.0f;
    if (g_sprint) {
        float sm = std::sqrt(lx * lx + ly * ly);
        g_sprintStick = g_sprintStick ? sm >= kSprintMinOff : sm >= kSprintMinOn;
        if (!g_sprintStick) {
            lx = 0.0f;
            ly = 0.0f;
            moving = false;
        }
        if (moving) {
            g_sprintNeutralSince = 0;
            if (now - g_sprintClickAt > 600) g_sprintArmedUntil = 0;
        } else if (!(g_sprintArmedUntil && now < g_sprintArmedUntil)) {
            if (g_sprintNeutralSince == 0) g_sprintNeutralSince = now;
            else if (now - g_sprintNeutralSince >= kSprintStopMs) g_sprint = false;
        }
    } else {
        g_sprintStick = false;
    }
    if (moving) {
        float m = std::sqrt(lx * lx + ly * ly);
        if (g_sprint) {
            float big = std::fabs(lx) > std::fabs(ly) ? std::fabs(lx) : std::fabs(ly);
            lx /= big;
            ly /= big;
        } else if (m > kWalkMaxMag) {
            lx *= kWalkMaxMag / m;
            ly *= kWalkMaxMag / m;
        }
    }
    lx = AxisHyst(lx, &g_axisOn[0]);
    ly = AxisHyst(ly, &g_axisOn[1]);
    if (!g_axisOn[0] && !g_axisOn[1]) {
        lx = 0.0f;
        ly = 0.0f;
    }
    ly = RunRampY(ly, now);   // ronda 64
    g_leftSent[0].store(lx, std::memory_order_relaxed);
    g_leftSent[1].store(ly, std::memory_order_relaxed);
    out.ax[P_LJOY][0] = lx;
    out.ax[P_LJOY][1] = ly;
    out.ax[P_RJOY][0] = rx;
    out.ax[P_RJOY][1] = ry;
}

} // namespace

bool VrPadDioramaGameActive() { return g_dioGame.load(std::memory_order_relaxed) && DioramaEnabled(); }

void VrPadOnHands(VrHandState hands[2], bool /*actions*/) {
    const VrHandState L = hands[0];
    const VrHandState R = hands[1];
    bool lc = L.valid && L.stickClick;
    bool rc = R.valid && R.stickClick;
    bool lPress = lc && !g_prevClick[0];
    bool rPress = rc && !g_prevClick[1];
    g_prevClick[0] = lc;
    g_prevClick[1] = rc;
    if (!lc) g_mask[0] = false;
    if (!rc) g_mask[1] = false;
    bool shift = L.valid && L.thumbrest;
    bool rShift = R.valid && R.thumbrest;   // ronda 23: pulgar derecho en reposo (+ clic izquierdo = modo de combate)
    bool aRaw = R.valid && R.buttonA;
    bool bRaw = R.valid && R.buttonB;
    bool yRaw = L.valid && L.buttonB;
    bool aPress = aRaw && !g_aPrev;
    bool bPress = bRaw && !g_bPrev;
    bool yPress = yRaw && !g_yPrev;
    g_aPrev = aRaw;
    g_bPrev = bRaw;
    g_yPrev = yRaw;
    unsigned long long now = GetTickCount64();
    PadOut out = {};

    if (DioramaEnabled()) {
        // Mandos del diorama como en la 1.6. Pulgar izquierdo en reposo + clic
        // del joystick derecho: de vuelta al juego en primera persona (el
        // diorama no ve ese clic). Ronda 17 (antes pulgar derecho + clic
        // izquierdo).
        g_rClickAt = 0;   // ronda 37
        g_lClickAt = 0;   // ronda 53
        if (rPress && shift) {
            g_mask[1] = true;
            g_dioGame.store(false, std::memory_order_relaxed);   // ronda 62
            g_dioClickAt = 0;
            DioramaRequestToggle();
            CombatVRRequestFirstPerson();
            Log("[MANDO] pulgar izquierdo + clic derecho: fuera del diorama, mandos al juego en primera persona.");
        }
        // Ronda 62: pulgar derecho en reposo + clic izquierdo mantenido 1 s:
        // mandos al personaje / a la maqueta (el diorama no ve ese clic).
        if (lPress && rShift) {
            g_mask[0] = true;
            g_dioClickAt = now;
        }
        bool dioGame = g_dioGame.load(std::memory_order_relaxed);
        if (g_dioClickAt != 0 && lc && rShift && now - g_dioClickAt >= kDioramaHoldMs) {
            g_dioClickAt = 0;
            dioGame = !dioGame;
            g_dioGame.store(dioGame, std::memory_order_relaxed);
            g_sprint = false;
            g_aRoute = g_bRoute = g_yRoute = -1;
            g_axisOn[0] = g_axisOn[1] = false;
            VrInputHaptic(0, 0.05f, 0.6f);
            VrInputHaptic(1, 0.05f, 0.6f);
            Toast(L"Mandos", dioGame ? L"Manejan al personaje, como el mando del juego (pulgar derecho en reposo + clic izq.: maqueta)"
                                     : L"Manejan la maqueta");
            Log(dioGame ? "[MANDO] diorama: pulgar derecho + clic izquierdo mantenido 1 s: los mandos manejan al personaje."
                        : "[MANDO] diorama: pulgar derecho + clic izquierdo mantenido 1 s: los mandos vuelven a la maqueta.");
        } else if (g_dioClickAt != 0 && (!lc || !rShift)) {
            g_dioClickAt = 0;
        }
        if (dioGame) {
            DioramaGamePad(L, R, shift, lPress && !rShift, rPress, aPress, bPress, aRaw, bRaw, now, out);
        } else {
            g_sprint = false;
            g_aRoute = g_bRoute = g_yRoute = -1;
            g_dpad[0] = g_dpad[1] = g_dpad[2] = g_dpad[3] = false;
        }
        g_shieldCandidate = 0;
        g_shieldUp = false;
        ResetArmsGestures();   // ronda 30
        g_rbHeldSince = 0;     // ronda 32
        g_throwDone = false;
        g_throwHavePrev = false;
        g_rtOn = g_rtMod = g_rtPressed = false;
    } else {
        g_dioGame.store(false, std::memory_order_relaxed);   // ronda 62: fuera del diorama, la maqueta al volver
        g_dioClickAt = 0;
        // Pulgar izquierdo en reposo + clic derecho: al diorama (el mismo
        // gesto que para volver; ronda 17). Pulgar derecho en reposo + clic
        // izquierdo: modo de combate (ronda 23). Clic izquierdo solo: sprint.
        // Clic derecho solo: recentrar. Los clics no se mandan al juego, salvo
        // en el modo 0 (ronda 71): solos son L3 y R3 del Xbox, y recentrar es
        // pulgar izquierdo en reposo + clic derecho corto.
        bool sprintClick = false;
        const bool xboxLike = CombatVRGetCombatMode() == 0;   // ronda 71: los Quest como un mando de Xbox
        if (lPress) {
            g_mask[0] = true;
            if (rShift) {
                g_lClickAt = now;   // ronda 53: modo de combate si se mantiene; si no, sprint al soltar
            } else {
                g_lClickAt = 0;
                if (xboxLike) g_l3Held = true;   // L3 al juego
                else sprintClick = true;
            }
        }
        if (!lc || !xboxLike) g_l3Held = false;
        if (g_lClickAt != 0 && lc && rShift && now - g_lClickAt >= kDioramaHoldMs) {
            g_lClickAt = 0;
            CombatVRCycleCombatMode();   // da su propio aviso en el visor
            VrInputHaptic(0, 0.05f, 0.6f);
            VrInputHaptic(1, 0.05f, 0.6f);
            Log("[MANDO] pulgar derecho + clic izquierdo mantenido 1 s: otro modo de combate.");
        } else if (g_lClickAt != 0 && !lc) {
            g_lClickAt = 0;
            if (!xboxLike) sprintClick = true;   // clic corto: el sprint de siempre (no en el modo 0)
        }
        if (xboxLike && g_sprint) {
            g_sprint = false;   // ronda 71: en el modo 0 a fondo corre, como el Xbox
            g_sprintArmedUntil = 0;
        }
        if (sprintClick) {
            g_sprint = !g_sprint;
            g_sprintArmedUntil = g_sprint ? now + kSprintArmMs : 0;
            g_sprintClickAt = now;
            g_sprintNeutralSince = 0;
            if (g_sprint) VrInputHaptic(0, 0.03f, 0.5f);
        }
        if (rPress) {
            g_mask[1] = true;
            if (shift) {
                g_rClickAt = now;   // ronda 37: diorama si se mantiene; si no, recentrar al soltar
            } else if (xboxLike) {
                g_rClickAt = 0;
                g_r3Held = true;   // ronda 71: R3 al juego
            } else {
                g_rClickAt = 0;
                HeadTrackRequestRecenter();
                Toast(L"Vista", L"Recentrada");
                Log("[MANDO] clic derecho: vista recentrada.");
            }
        }
        if (!rc || !xboxLike) g_r3Held = false;
        out.btn[P_LSTICK] = g_l3Held;
        out.btn[P_RSTICK] = g_r3Held;
        if (g_rClickAt != 0 && rc && shift && now - g_rClickAt >= kDioramaHoldMs) {
            g_rClickAt = 0;
            g_sprint = false;
            DioramaRequestToggle();   // el diorama da su propio aviso
            Log("[MANDO] pulgar izquierdo + clic derecho mantenido 1 s: al diorama (mandos del diorama).");
        } else if (g_rClickAt != 0 && !rc) {
            g_rClickAt = 0;
            HeadTrackRequestRecenter();
            Toast(L"Vista", L"Recentrada");
            Log("[MANDO] clic derecho: vista recentrada.");
        }
        bool gameplay = CombatVRInGameplay();
        // A: con el pulgar izquierdo en reposo al pulsar, Start. Ronda 23: en
        // el modo de combate 1 (mandos de movimiento) A no va al juego en
        // partida mientras el ataque por gesto este listo (sin ataques ni
        // combos del juego); en el 2, A va al juego y CombatVR suelta los
        // brazos para que el combo mueva el arma.
        if (aPress) {
            int cm = CombatVRGetCombatMode();
            if (shift) {
                g_aRoute = P_START;
            } else if (gameplay && cm == 1 && CombatVRGestureAttackReady()) {
                g_aRoute = -2;   // del mod: no se manda nada al juego
            } else {
                g_aRoute = P_SOUTH;
                if (gameplay && cm == 2) CombatVRNotifyAttackButton();
            }
        }
        if (!aRaw) g_aRoute = -1;
        if (g_aRoute >= 0) out.btn[g_aRoute] = true;
        out.btn[P_WEST] = L.valid && L.buttonA;    // X
        // Y: en partida suelta al enemigo fijado (el juego no tiene boton para
        // eso; su "darse la vuelta" no sirve en primera persona). En los menus
        // y en el modo 0, Y del mando.
        if (yPress) {
            if (gameplay && !xboxLike) {
                g_yRoute = -2;   // del mod: no se manda nada al juego
                CombatVRRequestUnlockEnemy();
            } else {
                g_yRoute = P_NORTH;   // en el modo 0, Y del Xbox (darse la vuelta)
            }
        }
        if (!yRaw) g_yRoute = -1;
        if (g_yRoute >= 0) out.btn[g_yRoute] = true;
        // Bloquear: agarre izquierdo, o levantar el escudo delante (gesto).
        // Ronda 30: el agarre apretado con la mano en la espalda es del gesto
        // de sacar o guardar las armas (no va al juego).
        bool shieldGesture = ShieldGesture(L, now);
        bool gripMod[2] = { false, false };
        ArmsGestures(hands, now, gripMod);
        // Ronda 36: con un arma a dos manos el agarre izquierdo es para
        // sujetarla con la izquierda (CombatVR); no va al juego. Se decide al
        // apretarlo y vale hasta soltarlo.
        bool lgrip = L.valid && L.grip;
        if (lgrip && !g_lGripPrev2h) g_lGripTwoHand = CombatVRLeftGripHoldsWeapon();
        if (!lgrip) g_lGripTwoHand = false;
        g_lGripPrev2h = lgrip;
        // Ronda 56 (pedido del usuario): en partida con los modos de mandos de
        // movimiento (1, 2 y 3) el agarre izquierdo ya no bloquea: se bloquea
        // alzando el escudo. En el VR basico y en los menus, como siempre.
        const bool gripBlocks = !gameplay || CombatVRGetCombatMode() == 0;
        out.btn[P_LSHOULDER] = (lgrip && gripBlocks && !gripMod[0] && !g_lGripTwoHand) || shieldGesture;
        out.btn[P_RSHOULDER] = R.valid && R.grip && !gripMod[1];
        // Ronda 32: lanzar con el gesto en el modo 1 (A tapado).
        {
            bool weaponsOutT = false;
            bool throwOk = gameplay && CombatVRGetCombatMode() == 1 && CombatVRArmsGestureState(&weaponsOutT);
            if (ThrowGesture(R, now, out.btn[P_RSHOULDER], throwOk)) g_throwAUntil = now + kThrowPressMs;
            if (now < g_throwAUntil) out.btn[P_SOUTH] = true;
        }
        float lx = L.valid ? L.stickX : 0.0f, ly = L.valid ? L.stickY : 0.0f;
        float rx = R.valid ? R.stickX : 0.0f, ry = R.valid ? R.stickY : 0.0f;
        if (shift) {
            // Cruceta con el joystick derecho; la camara se queda quieta.
            g_dpad[0] = Hyst(g_dpad[0], ry);
            g_dpad[1] = Hyst(g_dpad[1], -ry);
            g_dpad[2] = Hyst(g_dpad[2], -rx);
            g_dpad[3] = Hyst(g_dpad[3], rx);
            rx = ry = 0.0f;
        } else {
            g_dpad[0] = g_dpad[1] = g_dpad[2] = g_dpad[3] = false;
        }
        out.btn[P_UP] = g_dpad[0];
        out.btn[P_DOWN] = g_dpad[1];
        out.btn[P_LEFT] = g_dpad[2];
        out.btn[P_RIGHT] = g_dpad[3];
        // Ronda 17: sin el pulgar sobre el joystick izquierdo, lo que marque es
        // deriva y se ignora.
        if (L.valid && L.stickTouchKnown && L.stickTouch) g_lTouchSeen = true;
        bool touchGate = L.valid && L.stickTouchKnown && g_lTouchSeen && !xboxLike;   // ronda 71: no en el modo 0
        bool thumbOff = touchGate ? !L.stickTouch : shift;
        if (thumbOff) {
            lx = 0.0f;
            ly = 0.0f;
        }
        if (!xboxLike) {   // ronda 71: en el modo 0, solo la zona muerta del juego
            Deadzone(&lx, &ly, kDeadzoneMove);
            Deadzone(&rx, &ry, kDeadzoneLook);
        }
        // Andar siempre; correr solo con el sprint (en el modo 0, a fondo, como el Xbox).
        bool moving = lx != 0.0f || ly != 0.0f;
        if (g_sprint) {
            float sm = std::sqrt(lx * lx + ly * ly);
            g_sprintStick = g_sprintStick ? sm >= kSprintMinOff : sm >= kSprintMinOn;
            if (!g_sprintStick) {
                lx = 0.0f;
                ly = 0.0f;
                moving = false;
            }
        } else {
            g_sprintStick = false;
        }
        if (g_sprint) {
            if (moving) {
                g_sprintNeutralSince = 0;
                // Al hacer clic el pulgar puede volver un momento al centro:
                // la espera sigue los primeros 0,6 s.
                if (now - g_sprintClickAt > 600) g_sprintArmedUntil = 0;
            } else if (!(g_sprintArmedUntil && now < g_sprintArmedUntil)) {
                if (g_sprintNeutralSince == 0) g_sprintNeutralSince = now;
                else if (now - g_sprintNeutralSince >= kSprintStopMs) g_sprint = false;
            }
        }
        if (moving) {
            float m = std::sqrt(lx * lx + ly * ly);
            if (g_sprint) {
                float big = std::fabs(lx) > std::fabs(ly) ? std::fabs(lx) : std::fabs(ly);
                lx /= big;   // a fondo en su direccion (el eje mayor a 1)
                ly /= big;
            } else if (!xboxLike && m > kWalkMaxMag) {
                lx *= kWalkMaxMag / m;
                ly *= kWalkMaxMag / m;
            }
        }
        // Ronda 72 (pedido del usuario): quitado el giro del joystick por
        // cuartos de la vista con un enemigo fijado (ronda 49). Fijado, el
        // joystick va respecto al enemigo, como en el juego: adelante hacia el,
        // atras alejandose y a los lados rodeandolo.
        if (!xboxLike) {
            lx = AxisHyst(lx, &g_axisOn[0]);
            ly = AxisHyst(ly, &g_axisOn[1]);
        } else {
            g_axisOn[0] = g_axisOn[1] = false;
        }
        // Ronda 15: sin ninguna tecla del joystick pulsada (los dos ejes por
        // debajo del umbral del juego) se manda 0: el juego no anda con eso y
        // asi lee el joystick en reposo (la parada de CombatVR lo necesita).
        // Ronda 70: en el modo 0 no (el giro analogico con poca inclinacion).
        float hx = lx, hy = ly;
        if (!xboxLike && !g_axisOn[0] && !g_axisOn[1]) {
            lx = 0.0f;
            ly = 0.0f;
        }
        ly = RunRampY(ly, now);   // ronda 64: el sprint pulsa antes la tecla del joystick
        // B: con el pulgar izquierdo en reposo, Back. En partida, saltar (y
        // subirse a mesas y cajas) solo empujando el joystick hacia delante o
        // atras (o hacia cualquier lado con un enemigo fijado: esquivas); con
        // el joystick en reposo o solo girando, B solo alterna entre los
        // objetos cercanos (SelectObj, sin "Jump"). En los menus, B del mando.
        if (bPress) {
            bool jumpStick = moving && (std::fabs(hy) >= 0.5f * std::fabs(hx) || CombatVREnemyLocked());
            if (shift) {
                g_bRoute = P_BACK;
            } else if (gameplay && !jumpStick && !xboxLike) {   // ronda 71: en el modo 0, B del Xbox
                g_bRoute = -2;   // del mod
                CombatVRRequestSelectObject();
            } else {
                g_bRoute = P_EAST;
            }
        }
        if (!bRaw) g_bRoute = -1;
        if (g_bRoute >= 0) out.btn[g_bRoute] = true;
        g_leftSent[0].store(lx, std::memory_order_relaxed);
        g_leftSent[1].store(ly, std::memory_order_relaxed);
        out.ax[P_LJOY][0] = lx;
        out.ax[P_LJOY][1] = ly;
        out.ax[P_RJOY][0] = rx;
        out.ax[P_RJOY][1] = ry;
        out.ax[P_LT][0] = L.valid ? L.triggerValue : 0.0f;
        out.ax[P_RT][0] = R.valid ? R.triggerValue : 0.0f;
        // Ronda 32: con la mano derecha vacia el gatillo derecho (fijar
        // enemigo) es del mod: fija sin sacar las armas guardadas.
        {
            float rt = R.valid ? R.triggerValue : 0.0f;
            if (!g_rtPressed && rt > kRtClaim) {
                g_rtPressed = true;
                g_rtBow = gameplay && CombatVRBowWantsTrigger();   // ronda 50: la cuerda del arco
                g_rtMod = g_rtBow || CombatVRSelectEnemyWithoutDraw();   // se decide antes de que el juego lo vea
            } else if (g_rtPressed && rt < kRtRelease) {
                g_rtPressed = false;
                g_rtMod = false;
                g_rtBow = false;
            }
            bool on = g_rtOn ? rt > kRtOff : rt > kRtOn;
            if (on && !g_rtOn && g_rtMod && !g_rtBow) CombatVRRequestSelectEnemy();
            g_rtOn = on;
            g_rtBowHeldPub.store(g_rtBow && on, std::memory_order_relaxed);
            if (g_rtMod) out.ax[P_RT][0] = 0.0f;
        }
    }
    if (g_mask[0]) hands[0].stickClick = false;
    if (g_mask[1]) hands[1].stickClick = false;
    g_sprintPub.store(g_sprint, std::memory_order_relaxed);

    // Cada pulsacion dura al menos kMinPressMs para el juego.
    for (int b = 0; b < P_BTN_COUNT; ++b) {
        bool raw = out.btn[b];
        if (raw && !g_rawPrev[b]) g_holdUntil[b] = now + kMinPressMs;
        g_rawPrev[b] = raw;
        out.btn[b] = raw || now < g_holdUntil[b];
    }
    // Ronda 64: lo que se manda en este fotograma, para la rampa del sprint (en
    // los menus, en la maqueta o con el joystick por debajo de correr, la
    // siguiente vez que vaya a fondo vuelve a empezar por la rampa).
    g_lastSentY = out.ax[P_LJOY][1];
    if (std::fabs(g_lastSentY) <= kRunRampY) g_rampSign = 0;
    AcquireSRWLockExclusive(&g_padLock);
    g_pad = out;
    ReleaseSRWLockExclusive(&g_padLock);
    g_present.store(L.valid || R.valid, std::memory_order_relaxed);

    // Vibracion que pide el juego: pulsos de 100 ms como mucho cada 80 ms.
    bool vibFresh = now - g_vibTick.load(std::memory_order_relaxed) < 300;
    for (int h = 0; h < 2; ++h) {
        float amp = vibFresh ? static_cast<float>(g_vib[h].load(std::memory_order_relaxed)) / 65535.0f : 0.0f;
        if (amp > 0.03f && now - g_lastPulse[h] >= 80) {
            VrInputHaptic(h, 0.1f, amp);
            g_lastPulse[h] = now;
        }
    }
}

bool VrPadSprintActive() { return g_present.load(std::memory_order_relaxed) && g_sprintPub.load(std::memory_order_relaxed); }

bool VrPadIsQuestController(unsigned long long controllerHandle) { return controllerHandle == kFakeHandle; }

bool VrPadGetLeftStick(float sent[2]) {
    if (!g_present.load(std::memory_order_relaxed)) return false;
    sent[0] = g_leftSent[0].load(std::memory_order_relaxed);
    sent[1] = g_leftSent[1].load(std::memory_order_relaxed);
    return true;
}

void VrPadOnPresent() {
    TryInstall();
    if (g_toastPending) {
        g_toastPending = false;
        OpenVRShowToast(g_toastTitle, g_toastText, kToastMs);
    }
    if (!g_actionsLogged && VrInputManifestActive()) {
        g_actionsLogged = true;
        Log("[MANDO] Reposapulgares disponibles (acciones de OpenVR): cruceta y Start / Back con el pulgar izquierdo en reposo.");
    }
    if (!g_presentLogged && g_seenByGame.load(std::memory_order_relaxed)) {
        g_presentLogged = true;
        Log("[MANDO] El juego ya ve el mando de los Quest (Xbox One por Steam Input).");
    }
}

// Ronda 50: el gatillo derecho es la cuerda del arco y esta apretado.
bool VrPadBowTriggerHeld() { return g_rtBowHeldPub.load(std::memory_order_relaxed); }

} // namespace BladeVR
