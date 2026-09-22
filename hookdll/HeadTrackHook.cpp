#include "HeadTrackHook.h"
#include "HookLogger.h"
#include "OpenVRHook.h"
#include "FromWorldLocator.h"
#include "FovHook.h"
#include "Dx11Hook.h"
#include "StereoHook.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <sstream>
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
//    lazo cerrado envia raton (solo X) para llevar la guinada del juego
//    hacia la del visor.
// =====================================================================

// --- offsets del juego -------------------------------------------------
static constexpr uintptr_t kCameraGlobalOffset = 0xF4A6A8;   // -> ptr1 (objeto de app)
static constexpr uintptr_t kCameraEntityOffset = 0xE8;       // ptr1+0xE8 -> entidad Camera
static constexpr uintptr_t kCameraPositionOffset = 0x550;    // Position (3 doubles)
static constexpr uintptr_t kCameraTPosOffset = 0x568;        // TPos (3 doubles)
static constexpr uintptr_t kFromWorldWriterRipOffset = 0x72e55;      // dentro de la funcion que escribe fromWorld
static constexpr uintptr_t kFromWorldFinalCallReturnOffset = 0x5a81d; // retorno de la ultima llamada por fotograma (partida)
// Las dos primeras llamadas por fotograma en partida (estados intermedios de
// la camara de seguimiento): no se reescribe tras ellas. Cualquier otro
// llamante (seleccion de personaje, cinematicas) se trata como definitivo.
static constexpr uintptr_t kFromWorldIntermediateReturn1 = 0x5a7d3;
static constexpr uintptr_t kFromWorldIntermediateReturn2 = 0x5a7f8;
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
static std::atomic<int> g_walkFollowsView{0};      // Supr
static std::atomic<int> g_unitsPerMeter{1000};     // escala 6DOF
static std::atomic<int> g_mouseCountsPerDegreeX10{120};
static std::atomic<bool> g_insWasDown{false};
static std::atomic<bool> g_delWasDown{false};
static std::atomic<bool> g_homeWasDown{false};
// Recentrado de posicion (Inicio). Se pide desde el hilo de Present y se
// aplica en el hilo del juego, dentro del hook de fromWorld.
static std::atomic<int> g_recenterRequested{0};

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
static double g_lastYawDeg = 0.0;
static double g_lastPitchDeg = 0.0;
static double g_lastOffsetM[3] = {0.0, 0.0, 0.0};
static double g_lastCam[3] = {0.0, 0.0, 0.0};

static std::mutex g_renderPoseMutex;
static float g_renderPose[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
static bool g_renderPoseValid = false;

static PFN_Generic4 g_originalFromWorldWriter = nullptr;
static uintptr_t g_fromWorldWriterStart = 0;
static bool g_fromWorldWriterHookInstalled = false;

// contadores del log por segundo
static std::atomic<uint64_t> g_fwCallsSec{0};
static std::atomic<uint64_t> g_fwWritesSec{0};
static std::atomic<double> g_lastWrittenYawDeg{0.0};   // guinada de la ultima fromWorld que escribimos
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
static unsigned long long g_lastWrittenFrame = 0;

static bool IsOurLastWrittenMatrix(const double fw[16]) {
    std::lock_guard<std::mutex> lock(g_lastWrittenMutex);
    if (!g_haveLastWritten) return false;
    for (int i = 0; i < 16; ++i) { if (fw[i] != g_lastWritten[i]) return false; }
    return true;
}
static std::atomic<uint64_t> g_fwLateRewritesSec{0};  // reescrituras hechas en la raiz de culling
static std::atomic<uint64_t> g_fwStaleRefreshSec{0};  // rehechas por ser de un fotograma anterior (camara estatica)
static std::atomic<uint64_t> g_cullCallsSec{0};       // llamadas a la raiz de culling
static std::atomic<uint64_t> g_cullOtherPoseSec{0};   // ... cuyo bloque NO es el objeto de pose global
static std::atomic<uint64_t> g_noPose{0};
static std::atomic<uint64_t> g_rayClampSec{0};
static std::atomic<uint64_t> g_pushOutSec{0};
static std::atomic<uint64_t> g_mouseSentSec{0};
static std::atomic<long long> g_mouseDxSec{0};
static std::atomic<uint64_t> g_lastLogTick{0};

// Fase del mod en la que esta el hilo del juego (0 = dentro del codigo del
// juego). Solo se usa para el log periodico.
static std::atomic<int> g_gamePhase{0};

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
static bool SafeResolveCameraDoubles(uintptr_t moduleBase, double** outPosition, double** outTPos) {
    __try {
        uintptr_t ptr1 = *reinterpret_cast<uintptr_t*>(moduleBase + kCameraGlobalOffset);
        if (!ptr1) return false;
        uintptr_t ptr2 = *reinterpret_cast<uintptr_t*>(ptr1 + kCameraEntityOffset);
        if (!ptr2) return false;
        double* pos = reinterpret_cast<double*>(ptr2 + kCameraPositionOffset);
        double* tpos = reinterpret_cast<double*>(ptr2 + kCameraTPosOffset);
        volatile double touch = pos[0] + tpos[2];
        (void)touch;
        *outPosition = pos;
        *outTPos = tpos;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadVec3(const double* src, double out[3]) {
    __try {
        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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
    g_gamePhase.store(11, std::memory_order_relaxed);
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
    g_gamePhase.store(10, std::memory_order_relaxed);
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
    // El destino debe estar en el MISMO sector que la camara del
    // juego. Si la cabeza asoma al otro lado de un portal antes que la
    // entidad (escaleras, puertas), el culling raiz sigue en el sector viejo
    // y la imagen se va a negro un par de fotogramas.
    long long sc = 0, sp = 0;
    if (SafePointSector(c, &sc) && SafePointSector(p, &sp)) {
        if (sp == 0) return false;
        if (sc != 0 && sp != sc) return false;
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

static void ClampOffsetToLevel(const double c[3], double off[3]) {
    double len = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
    if (len < 1e-3) return;
    if (ProbeClearAt(c, off, 1.0)) return;
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 6; ++i) {
        double mid = 0.5 * (lo + hi);
        if (ProbeClearAt(c, off, mid)) lo = mid; else hi = mid;
    }
    off[0] *= lo; off[1] *= lo; off[2] *= lo;
    g_rayClampSec.fetch_add(1, std::memory_order_relaxed);
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

// --- submodo "camina hacia donde mira": lazo cerrado con raton ----------
static void WalkFollowStep() {
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
    INPUT in;
    std::memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    if (SendInput(1, &in, sizeof(INPUT)) == 1) {
        g_mouseSentSec.fetch_add(1, std::memory_order_relaxed);
        g_mouseDxSec.fetch_add(dx, std::memory_order_relaxed);
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
    HookLogger::Instance().Line("[HEAD_TRACK] Nueva escena: referencias del visor recentradas.");
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
    } else if (!home) {
        g_homeWasDown.store(false, std::memory_order_relaxed);
    }

    bool del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    if (del && !g_delWasDown.exchange(del, std::memory_order_relaxed)) {
        int next = 1 - g_walkFollowsView.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_followHaveOffset = false;
            g_followYawError = 0.0;
            g_followAccX = 0.0;
        }
        g_walkFollowsView.store(next, std::memory_order_relaxed);
        HookLogger::Instance().Line(next ? "[HEAD_TRACK] Supr: el personaje CAMINA HACIA DONDE MIRA el visor"
                                         : "[HEAD_TRACK] Supr: personaje INDEPENDIENTE de la camara (vista libre)");
    } else if (!del) {
        g_delWasDown.store(false, std::memory_order_relaxed);
    }

}

// Guinada de la fromWorld global en este instante (para ver si el juego la
// ha vuelto a escribir despues de nuestra reescritura).
static double CurrentFromWorldYawDeg() {
    double* fromWorld = TryGetOrResolveFromWorldPointer();
    double fw[16];
    if (!fromWorld || !SafeReadMatrix16(fromWorld, fw)) return 0.0;
    return std::atan2(fw[2], fw[10]) * 180.0 / kPi;
}

static void MaybeLogRate() {
    uint64_t now = GetTickCount64();
    uint64_t last = g_lastLogTick.load(std::memory_order_relaxed);
    if (now - last < 1000) return;
    g_lastLogTick.store(now, std::memory_order_relaxed);
    std::ostringstream o;
    o << "[HEAD_TRACK] faseJuego=" << g_gamePhase.load(std::memory_order_relaxed)
      << " modo=" << g_mode.load(std::memory_order_relaxed)
      << " caminaHaciaVista=" << g_walkFollowsView.load(std::memory_order_relaxed)
      << " errorGuinada=" << (g_followYawError * 180.0 / kPi)
      << " guinada=" << g_lastYawDeg << " cabeceo=" << g_lastPitchDeg
      << " desplazamiento(m)=[" << g_lastOffsetM[0] << "," << g_lastOffsetM[1] << "," << g_lastOffsetM[2] << "]"
      << " camJuego=[" << g_lastCam[0] << "," << g_lastCam[1] << "," << g_lastCam[2] << "]"
      << " unidades/m=" << g_unitsPerMeter.load(std::memory_order_relaxed)
      << " | fromWorld llamadas=" << g_fwCallsSec.exchange(0, std::memory_order_relaxed)
      << " reescrita=" << g_fwWritesSec.exchange(0, std::memory_order_relaxed)
      << " tardias=" << g_fwLateRewritesSec.exchange(0, std::memory_order_relaxed)
      << " rehechas=" << g_fwStaleRefreshSec.exchange(0, std::memory_order_relaxed)
      << " sinPose=" << g_noPose.exchange(0, std::memory_order_relaxed)
      << " | fwEscrita guinada=" << g_lastWrittenYawDeg.load(std::memory_order_relaxed)
      << " fwAhora guinada=" << CurrentFromWorldYawDeg()
      << " | culling llamadas=" << g_cullCallsSec.exchange(0, std::memory_order_relaxed)
      << " otraPose=" << g_cullOtherPoseSec.exchange(0, std::memory_order_relaxed)
      << " | colision recortes=" << g_rayClampSec.exchange(0, std::memory_order_relaxed)
      << " empujes=" << g_pushOutSec.exchange(0, std::memory_order_relaxed)
      << " | raton enviados=" << g_mouseSentSec.exchange(0, std::memory_order_relaxed)
      << " dx=" << g_mouseDxSec.exchange(0, std::memory_order_relaxed);
    HookLogger::Instance().Line(o.str());
}

// --- API -------------------------------------------------------------------
int HeadTrackGetMode() {
    return g_mode.load(std::memory_order_relaxed);
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
    if (g_mode.load(std::memory_order_relaxed) == 1 && g_walkFollowsView.load(std::memory_order_relaxed) == 1) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        WalkFollowStep();
    }
    MaybeLogRate();
}

// Reescribe fromWorld (la matriz de la camara del juego, recien escrita por el
// juego) con la orientacion y posicion del visor. Se llama desde el hilo del
// juego: tras el escritor de fromWorld y, si el juego la ha vuelto a pisar
// despues (camaras fijas de la seleccion de personaje y cinematicas), justo
// antes del culling. positional=false: solo orientacion, sin desplazamiento
// ni colision (camaras guiadas por el juego: la colision contra un recorrido
// que atraviesa geometria produce temblores).
static void ApplyHeadTrackingToFromWorld(double* fromWorld, bool positional) {
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

    // Posicion de la camara del juego: c[r] = -(t . fila_r); adelante = columna 2.
    double c[3];
    for (int rr = 0; rr < 3; ++rr) {
        c[rr] = -(game[12] * game[rr * 4 + 0] + game[13] * game[rr * 4 + 1] + game[14] * game[rr * 4 + 2]);
    }
    double baseYaw = std::atan2(game[2], game[10]);

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
        }
        HookLogger::Instance().Line("[HEAD_TRACK] Inicio: posicion recentrada (la camara vuelve a los ojos del personaje).");
    }

    // Ejes del visor en ejes del juego (cambio de base (x, -y, -z)).
    double r0[3] = { m[0], -m[4], -m[8] };
    double u0[3] = { -m[1], m[5], m[9] };
    double f0[3] = { -m[2], m[6], m[10] };
    double hp[3] = { m[3], -m[7], -m[11] };
    double hmdYawGame = std::atan2(f0[0], f0[2]);

    double out[16];
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_haveYawRef) { g_yawRef = hmdYawGame; g_haveYawRef = true; }
        if (!g_havePosRef) { g_posRef[0] = hp[0]; g_posRef[1] = hp[1]; g_posRef[2] = hp[2]; g_havePosRef = true; }

        // Delta de guinada: vista libre = guinada del juego + relativa del visor;
        // "camina hacia donde mira" = vista absoluta (visor + desfase fijo).
        double delta;
        if (g_walkFollowsView.load(std::memory_order_relaxed) == 1) {
            if (!g_followHaveOffset) { g_followYawOffset = WrapPi(baseYaw - hmdYawGame); g_followHaveOffset = true; }
            delta = g_followYawOffset;
            g_followYawError = WrapPi((hmdYawGame + g_followYawOffset) - baseYaw);
        } else {
            g_followHaveOffset = false;
            delta = baseYaw - g_yawRef;
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
        double cNew[3] = { c[0], c[1], c[2] };
        if (positional) {
            double upm = static_cast<double>(g_unitsPerMeter.load(std::memory_order_relaxed));
            double offU[3] = { off[0] * upm, off[1] * upm, off[2] * upm };
            ClampOffsetToLevel(c, offU);
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

        g_lastYawDeg = WrapPi(hmdYawGame - g_yawRef) * 180.0 / kPi;
        double fy = f[1] > 1.0 ? 1.0 : (f[1] < -1.0 ? -1.0 : f[1]);
        g_lastPitchDeg = -std::asin(fy) * 180.0 / kPi;
        g_lastOffsetM[0] = off[0]; g_lastOffsetM[1] = off[1]; g_lastOffsetM[2] = off[2];
        g_lastCam[0] = c[0]; g_lastCam[1] = c[1]; g_lastCam[2] = c[2];
    }
    if (SafeWriteMatrix16(fromWorld, out)) {
        g_fwWritesSec.fetch_add(1, std::memory_order_relaxed);
        g_lastWrittenYawDeg.store(std::atan2(out[2], out[10]) * 180.0 / kPi, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_lastWrittenMutex);
        for (int i = 0; i < 16; ++i) { g_lastWritten[i] = out[i]; g_lastBase[i] = game[i]; }
        g_haveLastWritten = true;
        g_lastPositional = positional;
        g_lastWrittenFrame = GetPresentCallCount();
    }
}

// Si fromWorld sigue siendo la que escribimos en un fotograma anterior (el
// juego no la ha tocado), la vuelve a construir desde la matriz base del
// juego con la pose actual del visor. Devuelve true si lo ha hecho.
static bool RefreshStaleFromWorld(double* fromWorld) {
    double base[16];
    bool positional;
    {
        std::lock_guard<std::mutex> lock(g_lastWrittenMutex);
        if (!g_haveLastWritten || g_lastWrittenFrame == GetPresentCallCount()) return false;
        for (int i = 0; i < 16; ++i) base[i] = g_lastBase[i];
        positional = g_lastPositional;
    }
    if (!SafeWriteMatrix16(fromWorld, base)) return false;
    ApplyHeadTrackingToFromWorld(fromWorld, positional);
    return true;
}


// --- el hook: reescritura de fromWorld -----------------------------------
static unsigned long long __fastcall HookedFromWorldWriter(unsigned long long a0, unsigned long long a1, unsigned long long a2, unsigned long long a3) {
    void* ret = _ReturnAddress();
    unsigned long long result = g_originalFromWorldWriter(a0, a1, a2, a3);
    g_gamePhase.store(20, std::memory_order_relaxed);
    struct PhaseReset { ~PhaseReset() { g_gamePhase.store(0, std::memory_order_relaxed); } } phaseReset;
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
    bool isIntermediate = (retOff == kFromWorldIntermediateReturn1 || retOff == kFromWorldIntermediateReturn2);
    if (!isCameraPose || isIntermediate) return result;
    StereoNotifyCameraPose();

    if (g_mode.load(std::memory_order_relaxed) != 1) return result;
    // Desplazamiento posicional y colision solo con la camara de partida (su
    // llamada definitiva retorna a +0x5a81d). Las cinematicas llaman al
    // escritor desde otros sitios, tres veces por fotograma y con un
    // recorrido que atraviesa geometria: ahi solo orientacion.
    bool positional = (retOff == kFromWorldFinalCallReturnOffset);
    ApplyHeadTrackingToFromWorld(fromWorld, positional);
    return result;
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

bool InstallHeadTrackHooks() {
    return InstallFromWorldWriterHook();
}

void UninstallHeadTrackHooks() {
    if (g_fromWorldWriterHookInstalled && g_fromWorldWriterStart) {
        MH_DisableHook(reinterpret_cast<void*>(g_fromWorldWriterStart));
        g_fromWorldWriterHookInstalled = false;
    }
}

// --- bloque de camara del culling (+0x80f20) ----------------------------
void HeadTrackSyncCullingCamera(long long param_2) {
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    g_gamePhase.store(30, std::memory_order_relaxed);
    struct PhaseReset { ~PhaseReset() { g_gamePhase.store(0, std::memory_order_relaxed); } } phaseReset;
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
        ApplyHeadTrackingToFromWorld(fromWorld, false);
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
