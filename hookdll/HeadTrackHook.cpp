#include "HeadTrackHook.h"
#include "HookLogger.h"
#include "OpenVRHook.h"
#include "FromWorldLocator.h"
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
static constexpr uintptr_t kFromWorldFinalCallReturnOffset = 0x5a81d; // retorno de la ultima llamada por fotograma
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
static std::atomic<bool> g_spaceWasDown{false};
// Recentrado de posicion (Espacio). Se pide desde el hilo de Present y se
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
static std::atomic<uint64_t> g_noPose{0};
static std::atomic<uint64_t> g_rayClampSec{0};
static std::atomic<uint64_t> g_pushOutSec{0};
static std::atomic<uint64_t> g_mouseSentSec{0};
static std::atomic<long long> g_mouseDxSec{0};
static std::atomic<uint64_t> g_lastLogTick{0};

// diagnostico de crashes: fase (0 = codigo del juego) y VEH
static std::atomic<int> g_gamePhase{0};
static std::atomic<uint64_t> g_crashLogged{0};
static void* g_crashVehHandle = nullptr;

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

static LONG WINAPI CrashDiagVectoredHandler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != 0xC0000005 && code != 0xC0000094 && code != 0xC00000FD && code != 0xC000001D &&
        code != 0xC0000409 && code != 0xC0000374) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_crashLogged.fetch_add(1, std::memory_order_relaxed) < 5) {
        uintptr_t rip = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        std::ostringstream o;
        o << "[CRASH] excepcion 0x" << std::hex << code << std::dec << " rip=" << DescribeAddress(rip)
          << " tid=" << GetCurrentThreadId() << " fase=" << g_gamePhase.load(std::memory_order_relaxed);
        if (code == 0xC0000005 && info->ExceptionRecord->NumberParameters >= 2) {
            unsigned long long kind = info->ExceptionRecord->ExceptionInformation[0];
            o << " tipo=" << (kind == 0 ? "lectura" : (kind == 1 ? "escritura" : "ejecucion"))
              << " dir=0x" << std::hex << info->ExceptionRecord->ExceptionInformation[1] << std::dec;
        }
        // pila de retornos (los primeros 8 qwords legibles de la pila)
        o << " pila=[";
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(info->ContextRecord->Rsp);
        int shown = 0;
        for (int i = 0; i < 64 && shown < 8; ++i) {
            uintptr_t v = 0;
            if (IsBadReadPtr(sp + i, sizeof(uintptr_t))) break;
            v = sp[i];
            HMODULE h = nullptr;
            if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                  reinterpret_cast<LPCSTR>(v), &h) && h) {
                if (shown) o << " ";
                o << DescribeAddress(v);
                ++shown;
            }
        }
        o << "]";
        HookLogger::Instance().Line(o.str());
    }
    return EXCEPTION_CONTINUE_SEARCH;
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

static bool SafeWriteCullBlock(long long param_2, const double rot[12], const double pos[5]) {
    __try {
        double* dRot = reinterpret_cast<double*>(param_2 + 0x98);
        double* dPos = reinterpret_cast<double*>(param_2 + 0x68);
        for (int i = 0; i < 12; ++i) dRot[i] = rot[i];
        for (int i = 0; i < 5; ++i) dPos[i] = pos[i];
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

    bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    if (space && !g_spaceWasDown.exchange(space, std::memory_order_relaxed)) {
        g_recenterRequested.store(1, std::memory_order_relaxed);
    } else if (!space) {
        g_spaceWasDown.store(false, std::memory_order_relaxed);
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
      << " sinPose=" << g_noPose.exchange(0, std::memory_order_relaxed)
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

// --- el hook: reescritura de fromWorld -----------------------------------
static unsigned long long __fastcall HookedFromWorldWriter(unsigned long long a0, unsigned long long a1, unsigned long long a2, unsigned long long a3) {
    void* ret = _ReturnAddress();
    unsigned long long result = g_originalFromWorldWriter(a0, a1, a2, a3);
    g_gamePhase.store(20, std::memory_order_relaxed);
    struct PhaseReset { ~PhaseReset() { g_gamePhase.store(0, std::memory_order_relaxed); } } phaseReset;
    g_fwCallsSec.fetch_add(1, std::memory_order_relaxed);

    if (g_mode.load(std::memory_order_relaxed) != 1) return result;
    if (reinterpret_cast<uintptr_t>(ret) != ModuleBase() + kFromWorldFinalCallReturnOffset) return result;

    double* fromWorld = TryGetOrResolveFromWorldPointer();
    if (!fromWorld) return result;
    double game[16];
    if (!SafeReadMatrix16(fromWorld, game)) return result;
    if (std::abs(game[15] - 1.0) > 1e-6) return result;

    float m[12];
    if (!GetHmdPoseMatrix34(m)) {
        g_noPose.fetch_add(1, std::memory_order_relaxed);
        return result;
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

    // Espacio = recentrar la POSICION: la referencia del visor pasa a ser la
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
        HookLogger::Instance().Line("[HEAD_TRACK] Espacio: posicion recentrada (la camara vuelve a los ojos del personaje).");
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
        double upm = static_cast<double>(g_unitsPerMeter.load(std::memory_order_relaxed));
        double offU[3] = { off[0] * upm, off[1] * upm, off[2] * upm };
        ClampOffsetToLevel(c, offU);
        double cNew[3] = { c[0] + offU[0], c[1] + offU[1], c[2] + offU[2] };
        PushOutFromObstacles(cNew);
        {
            double fin[3] = { cNew[0] - c[0], cNew[1] - c[1], cNew[2] - c[2] };
            RateLimitOffset(fin);
            cNew[0] = c[0] + fin[0]; cNew[1] = c[1] + fin[1]; cNew[2] = c[2] + fin[2];
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
    if (SafeWriteMatrix16(fromWorld, out)) g_fwWritesSec.fetch_add(1, std::memory_order_relaxed);
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
    if (!g_crashVehHandle) {
        g_crashVehHandle = AddVectoredExceptionHandler(1, &CrashDiagVectoredHandler);
    }
    return InstallFromWorldWriterHook();
}

void UninstallHeadTrackHooks() {
    if (g_fromWorldWriterHookInstalled && g_fromWorldWriterStart) {
        MH_DisableHook(reinterpret_cast<void*>(g_fromWorldWriterStart));
        g_fromWorldWriterHookInstalled = false;
    }
    if (g_crashVehHandle) {
        RemoveVectoredExceptionHandler(g_crashVehHandle);
        g_crashVehHandle = nullptr;
    }
}

// --- bloque de camara del culling (+0x80f20) ----------------------------
void HeadTrackSyncCullingCamera(long long param_2) {
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    g_gamePhase.store(30, std::memory_order_relaxed);
    struct PhaseReset { ~PhaseReset() { g_gamePhase.store(0, std::memory_order_relaxed); } } phaseReset;
    double* fromWorld = TryGetOrResolveFromWorldPointer();
    if (!fromWorld) return;
    double fw[16];
    if (!SafeReadMatrix16(fromWorld, fw)) return;
    double rot[12], pos[6];
    if (!SafeReadCullBlock(param_2, rot, pos)) return;
    double newPos[5];
    for (int rr = 0; rr < 3; ++rr) {
        newPos[rr] = -(fw[12] * fw[rr * 4 + 0] + fw[13] * fw[rr * 4 + 1] + fw[14] * fw[rr * 4 + 2]);
    }
    newPos[3] = std::atan2(fw[8], fw[10]);
    double s = fw[6];
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    newPos[4] = -std::asin(s);
    SafeWriteCullBlock(param_2, fw, newPos);
}

} // namespace BladeVR
