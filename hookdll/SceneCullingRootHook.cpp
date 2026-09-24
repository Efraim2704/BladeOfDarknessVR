#include "SceneCullingRootHook.h"
#include "HeadTrackHook.h"
#include "FovHook.h"
#include "StereoHook.h"
#include "HookLogger.h"
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace BladeVR {

// =====================================================================
// EL MUNDO SE DIBUJA UNA VEZ DESDE CADA OJO
//
// +0x80f20 es B_Map::Render (vtable de B_Map +0x7a2db8, ranura 1): todo el
// mundo 3D pasa por aqui. Lleva su propio contador de fotograma
// ([mapa+0x40]++ en cada llamada) y todas las caches por fotograma del motor
// (planos de superficie, T&L de caras, personajes, sombras de objetos,
// luces) se comparan con el, asi que una segunda llamada lo rehace todo
// desde la otra camara: cada ojo tiene su propio recorte por portales, sus
// caras traseras, sus sombras y sus reflejos. Cuesta 0,5-1 ms por pasada.
//
// Lo que dibuja el mundo se acumula en la lista de lotes del raster y se
// envia a bgfx en +0x74af20 (al final del fotograma, o antes si se llena el
// buffer de 64K vertices). Cada lote lleva su vista de bgfx en E+0x44 (-1 =
// por defecto: 22 para los tipos 1 y 3, 0 para el resto). El mundo va a las
// vistas 0 "World Pass", 1 "Depth Pass" y 13 "Glow Pass".
//
// Por fotograma:
//   * se vacian los lotes pendientes (van a sus vistas de siempre);
//   * pasada IZQUIERDA con la camara movida -IPD/2 por su eje derecho; sus
//     lotes de las vistas 0/1/13 van a copias de esas vistas con la MITAD
//     IZQUIERDA del render target; se vacian;
//   * pasada DERECHA igual, con +IPD/2 y la mitad derecha. Sus lotes 2D de
//     la vista 22 se descartan (ya salen una vez de la pasada izquierda);
//   * lo que el juego dibuje despues en 0/1/13 va a una tercera copia con el
//     rectangulo completo, que va detras.
// Las copias son la vista original entera (framebuffer, modo secuencial,
// transformacion) sin clear, ordenadas justo detras de su original.
// StereoHook reconoce los draws de las mitades por su viewport y los dibuja
// una vez con la proyeccion de su ojo.
//
// Luces que parpadean (antorchas): ver HookedOmniLightUpdate.
// Fase plana (menus, videos, cargas): una sola pasada, como el juego.
// =====================================================================

static constexpr uintptr_t kSceneCullingRootOffset = 0x80f20;

using PFN_SceneCullingRoot = void(__fastcall*)(long long, long long, long long);
static PFN_SceneCullingRoot g_originalSceneCullingRoot = nullptr;
static bool g_installed = false;
static uintptr_t g_base = 0;

// --- lotes del raster ------------------------------------------------------
static constexpr uintptr_t kBatchBeginOffset = 0x74c030;       // abrir lote (tipo, vertices)
static constexpr uintptr_t kBatchFlushOffset = 0x74af20;       // enviar la lista a bgfx
static constexpr uintptr_t kFrameEndFlushReturn = 0x74bdab;    // retorno de la llamada de +0x74bb50
static constexpr uintptr_t kListBeginOffset = 0x104aa90;       // qword: primera entrada
static constexpr uintptr_t kListEndOffset = 0x104aa98;         // qword: fin
static constexpr uintptr_t kVertexCountOffset = 0x109ab58;     // int: vertices del buffer actual
static constexpr uintptr_t kBatchStartOffset = 0x109aba0;      // int: inicio del lote abierto
static constexpr uintptr_t kBatchStart2Offset = 0x109abac;     // int
static constexpr uintptr_t kVertexBufferIndexOffset = 0x109ab24; // int: buffer dinamico (0..31)
static constexpr int kVertexBufferCount = 32;
static constexpr int kEntrySize = 0x60;
static constexpr int kEntryType = 0x14;
static constexpr int kEntryView = 0x44;

using PFN_BatchBegin = int(__fastcall*)(int, int);
using PFN_BatchFlush = void(__fastcall*)();
static PFN_BatchFlush g_originalBatchFlush = nullptr;
static bool g_flushHookInstalled = false;

// --- bgfx: estado de las vistas (s_ctx) ----------------------------------------
// Vistas en s_ctx+0x3325900, 0xC0 bytes cada una: clear +0x00 (flags u16 en
// +0x0E), rect +0x10 (x, y, w, h u16), scissor +0x18, view +0x20, proj
// +0x60, framebuffer +0xA0, modo +0xA2. Orden de vistas (m_viewRemap:
// posicion -> vista, se invierte en Frame::sort +0x655160) en
// s_ctx+0x33252d0. bgfx copia todo esto al cerrar el fotograma (+0x656260),
// en el mismo hilo del juego, asi que escribirlo aqui es lo mismo que llamar
// a setViewRect / setViewClear / setViewOrder.
static constexpr uintptr_t kBgfxCtxOffset = 0xF6B658;
static constexpr uintptr_t kBgfxViewsOffset = 0x3325900;
static constexpr uintptr_t kBgfxViewRemapOffset = 0x33252d0;
static constexpr int kViewStride = 0xC0;
static constexpr int kViewClearFlags = 0x0E;
static constexpr int kViewRect = 0x10;

static constexpr int kClonedViewCount = 3;
static const uint16_t kClonedViews[kClonedViewCount] = { 0, 1, 13 };   // World, Depth, Glow
static constexpr int kCloneBase = 200;           // 200 + 3*i + {0 izq, 1 der, 2 despues}
static constexpr int kOrderCount = kCloneBase + 3 * kClonedViewCount;   // 209
static uint16_t g_viewOrder[kOrderCount];
static bool g_viewOrderBuilt = false;
static bool g_remapInstalled = false;

// Mitades publicadas para el hilo de render: x<<48 | y<<32 | w<<16 | h.
static std::atomic<uint64_t> g_halfRect[kClonedViewCount][2];
static std::atomic<bool> g_eyeViewsActive{false};

enum Phase { kPhasePre = 0, kPhaseLeft = 1, kPhaseRight = 2, kPhasePost = 3 };
static int g_phase = kPhasePre;                   // solo hilo del juego
static bool g_eyeFrameActive = false;             // hubo pasadas por ojo en este fotograma

// --- tiempo de las pasadas (para el resumen del log) -------------------------------
static long long g_qpcFrequency = 0;
static std::atomic<uint64_t> g_passMicrosTotal{0};
static std::atomic<uint64_t> g_passMicrosMax{0};
static std::atomic<uint64_t> g_passCount{0};

// --- avisos (cada uno se escribe pocas veces) ---------------------------------------
static int g_failureLogs = 0;
static bool g_vbWrapLogged = false;

static void LogFailure(const char* what) {
    if (g_failureLogs >= 5) return;
    ++g_failureLogs;
    HookLogger::Instance().Line(std::string("[ESCENA] AVISO: ") + what);
}

static void NotePass(long long t0, long long t1) {
    if (g_qpcFrequency <= 0) return;
    uint64_t us = static_cast<uint64_t>((t1 - t0) * 1000000 / g_qpcFrequency);
    g_passMicrosTotal.fetch_add(us, std::memory_order_relaxed);
    g_passCount.fetch_add(1, std::memory_order_relaxed);
    uint64_t prev = g_passMicrosMax.load(std::memory_order_relaxed);
    while (us > prev &&
           !g_passMicrosMax.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
    }
}

// --- vistas partidas ----------------------------------------------------------------
static void BuildViewOrder() {
    if (g_viewOrderBuilt) return;
    int n = 0;
    for (int v = 0; v < kCloneBase; ++v) {
        g_viewOrder[n++] = static_cast<uint16_t>(v);
        for (int i = 0; i < kClonedViewCount; ++i) {
            if (kClonedViews[i] != v) continue;
            g_viewOrder[n++] = static_cast<uint16_t>(kCloneBase + 3 * i + 0);
            g_viewOrder[n++] = static_cast<uint16_t>(kCloneBase + 3 * i + 1);
            g_viewOrder[n++] = static_cast<uint16_t>(kCloneBase + 3 * i + 2);
        }
    }
    g_viewOrderBuilt = (n == kOrderCount);
}

static uint64_t PackRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    return (static_cast<uint64_t>(x) << 48) | (static_cast<uint64_t>(y) << 32) |
           (static_cast<uint64_t>(w) << 16) | static_cast<uint64_t>(h);
}

static void LogEyeViewRects(const uint64_t halves[kClonedViewCount][2]) {
    std::ostringstream o;
    o << "[ESCENA] vistas partidas por ojo (x,y,w,h):";
    for (int i = 0; i < kClonedViewCount; ++i) {
        o << " v" << kClonedViews[i] << " ->";
        for (int eye = 0; eye < 2; ++eye) {
            uint64_t p = halves[i][eye];
            o << (eye == 0 ? " izq " : " der ") << ((p >> 48) & 0xFFFF) << "," << ((p >> 32) & 0xFFFF)
              << "," << ((p >> 16) & 0xFFFF) << "," << (p & 0xFFFF);
        }
    }
    HookLogger::Instance().Line(o.str());
}

// Copia las vistas 0/1/13 en sus tres clones (sin clear; izquierda, derecha,
// rectangulo completo) y pone el orden. Sin destructores: __try.
static bool ConfigureEyeViewsRaw(uint64_t halves[kClonedViewCount][2]) {
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(g_base + kBgfxCtxOffset);
        if (!ctx) return false;
        char* views = reinterpret_cast<char*>(ctx + kBgfxViewsOffset);
        for (int i = 0; i < kClonedViewCount; ++i) {
            const char* src = views + kClonedViews[i] * kViewStride;
            const uint16_t* rect = reinterpret_cast<const uint16_t*>(src + kViewRect);
            uint16_t x = rect[0], y = rect[1], w = rect[2], h = rect[3];
            if (w < 2 || h < 2) return false;
            uint16_t wl = static_cast<uint16_t>(w / 2);
            uint16_t wr = static_cast<uint16_t>(w - wl);
            for (int k = 0; k < 3; ++k) {
                char* dst = views + (kCloneBase + 3 * i + k) * kViewStride;
                memcpy(dst, src, kViewStride);
                *reinterpret_cast<uint16_t*>(dst + kViewClearFlags) = 0;
                uint16_t* r = reinterpret_cast<uint16_t*>(dst + kViewRect);
                if (k == 0) { r[0] = x;                              r[1] = y; r[2] = wl; r[3] = h; }
                if (k == 1) { r[0] = static_cast<uint16_t>(x + wl); r[1] = y; r[2] = wr; r[3] = h; }
                if (k == 2) { r[0] = x;                              r[1] = y; r[2] = w;  r[3] = h; }
            }
            halves[i][0] = PackRect(x, y, wl, h);
            halves[i][1] = PackRect(static_cast<uint16_t>(x + wl), y, wr, h);
        }
        memcpy(reinterpret_cast<void*>(ctx + kBgfxViewRemapOffset), g_viewOrder, sizeof(g_viewOrder));
        g_remapInstalled = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool ConfigureEyeViews() {
    BuildViewOrder();
    if (!g_viewOrderBuilt) return false;
    uint64_t halves[kClonedViewCount][2] = {};
    if (!ConfigureEyeViewsRaw(halves)) return false;
    bool changed = false;
    for (int i = 0; i < kClonedViewCount; ++i) {
        if (g_halfRect[i][0].load(std::memory_order_relaxed) != halves[i][0] ||
            g_halfRect[i][1].load(std::memory_order_relaxed) != halves[i][1]) changed = true;
        g_halfRect[i][0].store(halves[i][0], std::memory_order_relaxed);
        g_halfRect[i][1].store(halves[i][1], std::memory_order_relaxed);
    }
    g_eyeViewsActive.store(true, std::memory_order_release);
    if (changed) LogEyeViewRects(halves);
    return true;
}

static void RestoreViewOrder() {
    g_eyeViewsActive.store(false, std::memory_order_release);
    if (!g_remapInstalled) return;
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(g_base + kBgfxCtxOffset);
        if (ctx) {
            uint16_t* remap = reinterpret_cast<uint16_t*>(ctx + kBgfxViewRemapOffset);
            for (int i = 0; i < kOrderCount; ++i) remap[i] = static_cast<uint16_t>(i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_remapInstalled = false;
}

// --- lotes ---------------------------------------------------------------------
static int EffectiveView(const char* e) {
    int v = *reinterpret_cast<const int*>(e + kEntryView);
    if (v != -1) return v;
    int type = *reinterpret_cast<const int*>(e + kEntryType);
    return (type == 1 || type == 3) ? 22 : 0;
}

static int ClonedIndex(int view) {
    for (int i = 0; i < kClonedViewCount; ++i) {
        if (kClonedViews[i] == view) return i;
    }
    return -1;
}

// Manda los lotes pendientes de una pasada a la copia de su vista. Sin
// destructores: __try.
static bool RouteEntries(int phase) {
    __try {
        char** pBegin = reinterpret_cast<char**>(g_base + kListBeginOffset);
        char** pEnd = reinterpret_cast<char**>(g_base + kListEndOffset);
        char* b = *pBegin;
        char* e = *pEnd;
        if (!b || e <= b) return true;
        char* w = b;
        for (char* r = b; r < e; r += kEntrySize) {
            int eff = EffectiveView(r);
            if (phase == kPhaseRight && eff == 22) continue;   // descartado
            int ci = ClonedIndex(eff);
            if (ci >= 0) *reinterpret_cast<int*>(r + kEntryView) = kCloneBase + 3 * ci + (phase - 1);
            if (w != r) memmove(w, r, kEntrySize);
            w += kEntrySize;
        }
        *pEnd = w;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static void __fastcall HookedBatchFlush() {
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    bool frameEnd = (ret == g_base + kFrameEndFlushReturn);
    if (g_eyeFrameActive && g_phase != kPhasePre && !RouteEntries(g_phase)) {
        LogFailure("no se pudieron repartir los lotes por ojo.");
    }
    g_originalBatchFlush();
    if (frameEnd) {
        g_phase = kPhasePre;
        g_eyeFrameActive = false;
    }
}

// Vacia los lotes pendientes a bgfx en mitad del fotograma, igual que el
// motor cuando se le llena el buffer de 64K vertices (+0x74c030): cerrar el
// lote abierto (tipo 0x80000000, como el fin de fotograma), enviar la lista,
// pasar al siguiente buffer dinamico y empezar de cero.
static bool ForceFlushRaw(bool* wrapped) {
    __try {
        int* vcount = reinterpret_cast<int*>(g_base + kVertexCountOffset);
        char* b = *reinterpret_cast<char**>(g_base + kListBeginOffset);
        char* e = *reinterpret_cast<char**>(g_base + kListEndOffset);
        if (*vcount <= 0 && e <= b) return true;
        reinterpret_cast<PFN_BatchBegin>(g_base + kBatchBeginOffset)(static_cast<int>(0x80000000u), 0);
        reinterpret_cast<PFN_BatchFlush>(g_base + kBatchFlushOffset)();   // pasa por HookedBatchFlush
        int* vb = reinterpret_cast<int*>(g_base + kVertexBufferIndexOffset);
        int next = *vb + 1;
        if (next >= kVertexBufferCount) { next = 0; *wrapped = true; }
        *vb = next;
        *reinterpret_cast<int*>(g_base + kBatchStartOffset) = 0;
        *vcount = 0;
        *reinterpret_cast<int*>(g_base + kBatchStart2Offset) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static void ForceFlush() {
    bool wrapped = false;
    if (!ForceFlushRaw(&wrapped)) LogFailure("fallo al vaciar los lotes entre pasadas.");
    if (wrapped && !g_vbWrapLogged) {
        g_vbWrapLogged = true;
        HookLogger::Instance().Line("[ESCENA] AVISO: se agotaron los 32 buffers de vertices de un fotograma.");
    }
}

// --- luces que parpadean -------------------------------------------------------------
// B_OmniLight::vtable[2] = +0x1af240 (luz, camara, fotograma): una vez por
// fotograma del mapa ([luz+0x28] != fotograma) pasa su posicion a espacio de
// camara y, si la luz parpadea ([luz+0x38] != 0, las antorchas), sortea la
// intensidad con rand(): [luz+0x20] = [luz+0x30] + aleatorio. Como cada
// pasada del mundo es un "fotograma" nuevo, cada ojo recibia una intensidad
// distinta. En la pasada derecha se apaga el sorteo (se sigue calculando la
// posicion) y la luz se queda con la intensidad de la izquierda; de paso
// rand() se consume como en el juego original, una vez por fotograma.
static constexpr uintptr_t kOmniLightUpdateOffset = 0x1af240;
static constexpr int kLightFlickEnabled = 0x38;
using PFN_OmniLightUpdate = void(__fastcall*)(char*, void*, unsigned int);
static PFN_OmniLightUpdate g_originalOmniLightUpdate = nullptr;
static bool g_lightHookInstalled = false;

static void __fastcall HookedOmniLightUpdate(char* light, void* camera, unsigned int frame) {
    if (g_phase == kPhaseRight && light) {
        int* flick = reinterpret_cast<int*>(light + kLightFlickEnabled);
        int saved = *flick;
        if (saved) {
            *flick = 0;
            g_originalOmniLightUpdate(light, camera, frame);
            *flick = saved;
            return;
        }
    }
    g_originalOmniLightUpdate(light, camera, frame);
}

// --- camara de cada ojo ----------------------------------------------------------
// Bloque de pose (param_2): posicion en +0x68 (3 doubles) y fromWorld en
// +0x98 (4x4 doubles, vectores fila, t = M[12..14]); las columnas de M son
// los ejes de la camara (col0 = derecha). Mover la camara 'off' unidades por
// su eje derecho: c' = c + off*R y t' = t - (off, 0, 0).
struct EyeSaved {
    double pos[3];
    double t0;
    double right[3];
};

static bool SaveEyeBlock(long long pose, EyeSaved* s) {
    __try {
        const double* pos = reinterpret_cast<const double*>(pose + 0x68);
        const double* fw = reinterpret_cast<const double*>(pose + 0x98);
        for (int i = 0; i < 3; ++i) s->pos[i] = pos[i];
        s->t0 = fw[12];
        s->right[0] = fw[0];
        s->right[1] = fw[4];
        s->right[2] = fw[8];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool WriteEyeBlock(long long pose, const EyeSaved* s, double off) {
    __try {
        double* pos = reinterpret_cast<double*>(pose + 0x68);
        double* fw = reinterpret_cast<double*>(pose + 0x98);
        for (int i = 0; i < 3; ++i) pos[i] = s->pos[i] + off * s->right[i];
        fw[12] = s->t0 - off;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool g_loggedEyeMode = false;

static void __fastcall HookedSceneCullingRoot(long long param_1, long long param_2, long long param_3) {
    FovClampOnGameThread();
    StereoNotifyCullingPass();
    HeadTrackSyncCullingCamera(param_2);

    bool eyeMode = g_flushHookInstalled && !StereoInFlatPhase();
    EyeSaved saved{};
    if (eyeMode) eyeMode = SaveEyeBlock(param_2, &saved) && ConfigureEyeViews();
    if (!eyeMode && g_remapInstalled) RestoreViewOrder();
    if (eyeMode != g_loggedEyeMode) {
        g_loggedEyeMode = eyeMode;
        HookLogger::Instance().Line(eyeMode ? "[ESCENA] mundo dibujado desde cada ojo."
                                            : "[ESCENA] mundo dibujado una vez (fase plana).");
    }

    LARGE_INTEGER t0, t1;
    if (eyeMode) {
        g_phase = kPhasePre;
        ForceFlush();
        g_eyeFrameActive = true;
        double half = 0.5 * static_cast<double>(StereoGetEyeSeparationUnits());
        for (int pass = 0; pass < 2; ++pass) {
            g_phase = (pass == 0) ? kPhaseLeft : kPhaseRight;
            if (!WriteEyeBlock(param_2, &saved, pass == 0 ? -half : +half)) {
                LogFailure("no se pudo mover la camara al ojo.");
            }
            QueryPerformanceCounter(&t0);
            g_originalSceneCullingRoot(param_1, param_2, param_3);
            QueryPerformanceCounter(&t1);
            NotePass(t0.QuadPart, t1.QuadPart);
            ForceFlush();
        }
        WriteEyeBlock(param_2, &saved, 0.0);
        g_phase = kPhasePost;
    } else {
        g_phase = kPhasePre;
        g_eyeFrameActive = false;
        QueryPerformanceCounter(&t0);
        g_originalSceneCullingRoot(param_1, param_2, param_3);
        QueryPerformanceCounter(&t1);
        NotePass(t0.QuadPart, t1.QuadPart);
    }

    HeadTrackRestoreCullingCamera(param_2);
}

bool SceneEyeModeActive() { return g_eyeViewsActive.load(std::memory_order_acquire); }

int SceneEyeForViewport(float x, float y, float w, float h) {
    if (!g_eyeViewsActive.load(std::memory_order_acquire)) return -1;
    for (int i = 0; i < kClonedViewCount; ++i) {
        for (int eye = 0; eye < 2; ++eye) {
            uint64_t p = g_halfRect[i][eye].load(std::memory_order_relaxed);
            float rx = static_cast<float>((p >> 48) & 0xFFFF);
            float ry = static_cast<float>((p >> 32) & 0xFFFF);
            float rw = static_cast<float>((p >> 16) & 0xFFFF);
            float rh = static_cast<float>(p & 0xFFFF);
            if (rw < 1.0f) continue;
            if (x > rx - 0.5f && x < rx + 0.5f && y > ry - 0.5f && y < ry + 0.5f &&
                w > rw - 0.5f && w < rw + 0.5f && h > rh - 0.5f && h < rh + 0.5f) return eye;
        }
    }
    return -1;
}

void SceneTakePassStats(double* avgMs, double* maxMs) {
    uint64_t total = g_passMicrosTotal.exchange(0, std::memory_order_relaxed);
    uint64_t count = g_passCount.exchange(0, std::memory_order_relaxed);
    uint64_t maxUs = g_passMicrosMax.exchange(0, std::memory_order_relaxed);
    if (avgMs) *avgMs = count ? static_cast<double>(total) / static_cast<double>(count) / 1000.0 : 0.0;
    if (maxMs) *maxMs = static_cast<double>(maxUs) / 1000.0;
}

// Engancha una funcion del juego comprobando antes sus primeros bytes: las
// dos son funciones primarias en .pdata (sin CHAININFO) que empiezan por
// "mov [rsp+8], rbx" (48 89 5C 24 08).
static bool HookGameFunction(uintptr_t offset, void* detour, void** original, const char* name) {
    void* target = reinterpret_cast<void*>(g_base + offset);
    const unsigned char* p = static_cast<const unsigned char*>(target);
    if (!(p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x5C && p[3] == 0x24 && p[4] == 0x08)) {
        HookLogger::Instance().Line(std::string("[ESCENA] ERROR: ") + name +
                                    ": bytes inesperados (otra version de Blade.exe?).");
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        HookLogger::Instance().Line(std::string("[ESCENA] ERROR: no se pudo enganchar ") + name + ".");
        return false;
    }
    return true;
}

bool InstallSceneCullingRootHook() {
    if (g_installed) return true;
    HMODULE hMain = GetModuleHandleA(nullptr);
    if (!hMain) return false;
    g_base = reinterpret_cast<uintptr_t>(hMain);
    void* target = reinterpret_cast<void*>(g_base + kSceneCullingRootOffset);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedSceneCullingRoot),
                      reinterpret_cast<void**>(&g_originalSceneCullingRoot)) != MH_OK) {
        HookLogger::Instance().Line("[SCENE_CULLING_ROOT] ERROR: MH_CreateHook fallo.");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        HookLogger::Instance().Line("[SCENE_CULLING_ROOT] ERROR: MH_EnableHook fallo.");
        return false;
    }
    g_installed = true;

    LARGE_INTEGER freq;
    if (QueryPerformanceFrequency(&freq)) g_qpcFrequency = freq.QuadPart;
    for (int i = 0; i < kClonedViewCount; ++i) {
        g_halfRect[i][0].store(0, std::memory_order_relaxed);
        g_halfRect[i][1].store(0, std::memory_order_relaxed);
    }

    // Sin el envio de lotes no se puede dibujar el mundo por ojo: StereoHook
    // se queda con la duplicacion de cada draw (estereo con costuras).
    g_flushHookInstalled = HookGameFunction(kBatchFlushOffset, reinterpret_cast<void*>(&HookedBatchFlush),
                                            reinterpret_cast<void**>(&g_originalBatchFlush),
                                            "envio de lotes (+0x74af20)");
    g_lightHookInstalled = HookGameFunction(kOmniLightUpdateOffset, reinterpret_cast<void*>(&HookedOmniLightUpdate),
                                            reinterpret_cast<void**>(&g_originalOmniLightUpdate),
                                            "luces (+0x1af240)");
    HookLogger::Instance().Line(g_flushHookInstalled
        ? "[ESCENA] Mundo por ojo disponible."
        : "[ESCENA] AVISO: mundo por ojo NO disponible; se usa la duplicacion de draws.");
    return true;
}

void UninstallSceneCullingRootHook() {
    if (!g_installed) return;
    MH_DisableHook(reinterpret_cast<void*>(g_base + kSceneCullingRootOffset));
    if (g_flushHookInstalled) {
        MH_DisableHook(reinterpret_cast<void*>(g_base + kBatchFlushOffset));
        g_flushHookInstalled = false;
    }
    if (g_lightHookInstalled) {
        MH_DisableHook(reinterpret_cast<void*>(g_base + kOmniLightUpdateOffset));
        g_lightHookInstalled = false;
    }
    g_installed = false;
}

} // namespace BladeVR
