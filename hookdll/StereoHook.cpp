#include "StereoHook.h"
#include "HookLogger.h"
#include "ProjectionHook.h"
#include "Dx11Hook.h"
#include "OpenVRHook.h"
#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>
#include <atomic>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace BladeVR {

// =====================================================================
// ESTEREO POR VIEWPORT PARTIDO
//
// Hechos sobre el motor en los que se apoya todo esto:
//   * Los vertices llegan a la GPU YA en espacio de camara (T&L en CPU).
//   * Toda la geometria 3D pasa por UNA matriz de proyeccion perspectiva en
//     VS slot 0 (ver ProjectionHook). Desplazar el ojo es un off-axis:
//     m[12] = -dx * m[0].
//   * Re-ejecutar el fotograma del juego dos veces (una por ojo) provoca
//     cuelgues y no es viable: el juego renderiza UNA sola vez, como siempre.
//
// Lo que se hace: cada draw call de geometria 3D se emite DOS veces
// seguidas, con TODO el estado tal como lo dejo el juego (vertex buffers,
// texturas, shaders, render target, profundidad), cambiando solo dos cosas:
//   1) el viewport: mitad izquierda del render target para el ojo izquierdo,
//      mitad derecha para el derecho;
//   2) el constant buffer de VS slot 0: dos buffers PROPIOS con la matriz de
//      cada ojo (frustum asimetrico del visor + desplazamiento de ojo).
// El resultado es un render target en formato lado a lado (SBS), que
// Dx11Hook entrega a SteamVR con bounds 0..0.5 y 0.5..1 (ver OpenVRHook).
//
// Los draws 2D se tratan aparte segun la fase del fotograma:
//   * composicion final / post-proceso (leen un render target): UNA vez a
//     viewport completo, porque la escena que copian ya esta en SBS;
//   * sprites del mundo (fuego, particulas; van antes de la composicion y
//     tras geometria 3D): rectangulo del FOV del juego mapeado al frustum de
//     cada ojo, con scissor para no invadir el otro ojo;
//   * interfaz (tras la composicion, o fotogramas sin 3D como los menus):
//     pantalla virtual a kUiDistanceM metros vista desde cada ojo, para que
//     se fusione a una distancia comoda.
// Los draws 3D sin test de profundidad (el cielo, geometria pegada a la
// camara) se dibujan SIN desplazamiento de ojo, es decir, al infinito.
// =====================================================================

namespace ContextVTable {
    constexpr int DrawIndexed = 12;
    constexpr int Draw = 13;
}

using PFN_DrawIndexed = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);

static PFN_DrawIndexed g_originalDrawIndexed = nullptr;
static PFN_Draw g_originalDraw = nullptr;
static bool g_installed = false;

// --- ajustes de usuario ---------------------------------------------------
static std::atomic<bool> g_enabled{false};
static std::atomic<float> g_eyeSeparationUnits{54.0f};   // valor calibrado con el visor puesto
static std::atomic<int> g_uiSizePreset{1};
static constexpr float kEyeSeparationStep = 4.0f;
static constexpr float kEyeSeparationMax = 400.0f;

// Arranque automatico: el video de introduccion es 2D y se veria duplicado,
// asi que el estereo se activa solo cuando han pasado kAutoStartMs Y ya se
// ha visto un fotograma con una escena 3D de verdad (menu o partida).
static constexpr unsigned long long kAutoStartMs = 2000;
static constexpr uint32_t k3DDrawsPerFrameForScene = 30;
static std::atomic<bool> g_autoStartDone{false};
static std::atomic<unsigned long long> g_firstPollTick{0};
static std::atomic<bool> g_sceneSeen{false};
static std::atomic<unsigned long long> g_sceneFrameId{0};
static std::atomic<uint32_t> g_sceneDrawsThisFrame{0};

static std::atomic<bool> g_f5WasDown{false};
static std::atomic<bool> g_pgUpWasDown{false};
static std::atomic<bool> g_pgDnWasDown{false};
static std::atomic<bool> g_endWasDown{false};

// --- constant buffers propios de cada ojo ----------------------------------
static ID3D11Buffer* g_cbLeft = nullptr;
static ID3D11Buffer* g_cbRight = nullptr;
static ID3D11Buffer* g_cbLeftNoEye = nullptr;    // mismas matrices SIN desplazamiento (cielo)
static ID3D11Buffer* g_cbRightNoEye = nullptr;
static bool g_cbCreateFailed = false;
static float g_cbSourceMatrix[16] = {};
static float g_cbSourceSeparation = -1.0f;
static bool g_cbSourceHadHmd = false;   // OpenVR disponible cuando se rellenaron
static bool g_cbFilled = false;

// Frustum de cada ojo del visor (tangentes crudos de OpenVR).
struct EyeFrustum {
    bool valid = false;
    float l = -1.0f, r = 1.0f, t = -1.0f, b = 1.0f;
    // sub-rectangulo del FOV del juego en NDC del ojo, sin recortar
    float rawLeft = -1.0f, rawRight = 1.0f, rawTop = 1.0f, rawBottom = -1.0f;
};
static EyeFrustum g_eye[2];
static std::atomic<bool> g_hmdFrustumLogged{false};

// --- fase del fotograma para la capa 2D -----------------------------------
static std::atomic<uint64_t> g_compositionFrame{~0ull};
static std::atomic<uint64_t> g_last3DFrame{~0ull};
static constexpr float kUiDistanceM = 1.8f;     // distancia de la pantalla virtual de la interfaz
static constexpr float kUiIpdM = 0.064f;

// --- estado de reentrada y contadores ---------------------------------------
static std::atomic<bool> g_inReplay{false};     // true mientras re-emitimos un draw (evita recursion)
static std::atomic<uint64_t> g_draws3D{0};
static std::atomic<uint64_t> g_draws2D{0};
static std::atomic<uint64_t> g_drawsComposition{0};
static std::atomic<uint64_t> g_lastLogMs{0};

// Estados de rasterizador con scissor, cacheados por el estado del juego.
static std::mutex g_rsMutex;
static std::unordered_map<uintptr_t, ID3D11RasterizerState*> g_scissorStates;

// ---------------------------------------------------------------------
// Ajustes y teclas
// ---------------------------------------------------------------------
bool StereoIsEnabled() { return g_enabled.load(std::memory_order_relaxed); }
float StereoGetEyeSeparationUnits() { return g_eyeSeparationUnits.load(std::memory_order_relaxed); }
int StereoGetUiSizePreset() { return g_uiSizePreset.load(std::memory_order_relaxed); }

static bool KeyPressedOnce(int vk, std::atomic<bool>& wasDown) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool was = wasDown.exchange(down, std::memory_order_relaxed);
    return down && !was;
}

void StereoPollKeys() {
    bool f5 = KeyPressedOnce(VK_F5, g_f5WasDown);

    if (!g_autoStartDone.load(std::memory_order_relaxed)) {
        unsigned long long now = GetTickCount64();
        unsigned long long first = g_firstPollTick.load(std::memory_order_relaxed);
        if (first == 0) {
            g_firstPollTick.store(now, std::memory_order_relaxed);
        } else if (f5) {
            g_autoStartDone.store(true, std::memory_order_relaxed);   // el usuario manda
        } else if (now - first >= kAutoStartMs && g_sceneSeen.load(std::memory_order_relaxed)) {
            g_autoStartDone.store(true, std::memory_order_relaxed);
            g_enabled.store(true, std::memory_order_relaxed);
            HookLogger::Instance().Line("[STEREO] Arranque automatico: estereo activado al detectar la primera escena 3D.");
        }
    }
    if (f5) {
        bool next = !g_enabled.load(std::memory_order_relaxed);
        g_enabled.store(next, std::memory_order_relaxed);
        HookLogger::Instance().Line(next ? "[STEREO] F5: estereo ACTIVADO" : "[STEREO] F5: estereo APAGADO (imagen plana)");
    }

    float delta = 0.0f;
    if (KeyPressedOnce(VK_PRIOR, g_pgUpWasDown)) delta += kEyeSeparationStep;   // Re Pag
    if (KeyPressedOnce(VK_NEXT, g_pgDnWasDown)) delta -= kEyeSeparationStep;    // Av Pag
    if (delta != 0.0f) {
        float v = g_eyeSeparationUnits.load(std::memory_order_relaxed) + delta;
        if (v < 0.0f) v = 0.0f;
        if (v > kEyeSeparationMax) v = kEyeSeparationMax;
        g_eyeSeparationUnits.store(v, std::memory_order_relaxed);
        std::ostringstream o;
        o << "[STEREO] separacion entre ojos = " << v << " unidades (Re Pag sube, Av Pag baja)";
        HookLogger::Instance().Line(o.str());
    }

    if (KeyPressedOnce(VK_END, g_endWasDown)) {
        int next = (g_uiSizePreset.load(std::memory_order_relaxed) + 1) % 3;
        g_uiSizePreset.store(next, std::memory_order_relaxed);
        HookLogger::Instance().Line(std::string("[STEREO] Fin: tamano de la interfaz = ") +
            (next == 0 ? "0 (53 grados)" : next == 1 ? "1 (75 grados)" : "2 (90 grados)"));
    }
}

// Semiancho de la pantalla virtual de la interfaz segun el preset:
// ~53, ~75 y ~90 grados aparentes.
static float UiHalfWidthM() {
    switch (StereoGetUiSizePreset()) {
        case 0:  return kUiDistanceM * 0.50f;
        case 2:  return kUiDistanceM * 1.00f;
        default: return kUiDistanceM * 0.77f;
    }
}

// ---------------------------------------------------------------------
// Frustums y viewports
// ---------------------------------------------------------------------
static void RefreshEyeFrustums(const float gameMatrix[16]) {
    float gameTanX = (gameMatrix[0] != 0.0f) ? (1.0f / gameMatrix[0]) : 1.0f;         // semi-FOV horizontal del juego
    float gameTanY = (gameMatrix[5] != 0.0f) ? (1.0f / fabsf(gameMatrix[5])) : 1.0f;  // semi-FOV vertical
    for (int eye = 0; eye < 2; ++eye) {
        EyeFrustum& f = g_eye[eye];
        f.valid = GetEyeProjectionRaw(eye == 1, &f.l, &f.r, &f.t, &f.b) &&
                  (f.r - f.l) > 1e-4f && (f.b - f.t) > 1e-4f;
        if (!f.valid) continue;
        // Convencion de OpenVR (ver ComposeProjection del SDK): el borde
        // derecho es x/z = r, el izquierdo x/z = l; el SUPERIOR es y/z = b
        // y el inferior y/z = t.
        float idx = 1.0f / (f.r - f.l);
        float idy = 1.0f / (f.b - f.t);
        f.rawLeft   = (2.0f * (-gameTanX) - (f.r + f.l)) * idx;
        f.rawRight  = (2.0f * ( gameTanX) - (f.r + f.l)) * idx;
        f.rawTop    = (2.0f * ( gameTanY) - (f.b + f.t)) * idy;
        f.rawBottom = (2.0f * (-gameTanY) - (f.b + f.t)) * idy;
    }
    if (g_eye[0].valid && !g_hmdFrustumLogged.exchange(true)) {
        std::ostringstream o;
        o << "[STEREO] frustum del visor -- izq l=" << g_eye[0].l << " r=" << g_eye[0].r
          << " t=" << g_eye[0].t << " b=" << g_eye[0].b
          << " | der l=" << g_eye[1].l << " r=" << g_eye[1].r
          << " t=" << g_eye[1].t << " b=" << g_eye[1].b
          << " | FOV del juego tanX=" << gameTanX << " tanY=" << gameTanY;
        HookLogger::Instance().Line(o.str());
    }
}

// Mitad del render target que corresponde a un ojo.
static D3D11_VIEWPORT EyeHalfViewport(const D3D11_VIEWPORT& full, int eye) {
    D3D11_VIEWPORT vp = full;
    vp.Width = full.Width * 0.5f;
    vp.TopLeftX = full.TopLeftX + (eye == 1 ? vp.Width : 0.0f);
    return vp;
}

// Viewport de un ojo para los sprites proyectados en CPU: el rectangulo del
// FOV del juego mapeado por el frustum del ojo, SIN recortar (a FOV alto se
// sale de la mitad; por eso se dibuja con scissor). 'halfRect' recibe la
// mitad del ojo.
static D3D11_VIEWPORT SpriteViewport(const D3D11_VIEWPORT& full, int eye, D3D11_RECT* halfRect) {
    D3D11_VIEWPORT half = EyeHalfViewport(full, eye);
    if (halfRect) {
        halfRect->left = static_cast<LONG>(half.TopLeftX);
        halfRect->top = static_cast<LONG>(half.TopLeftY);
        halfRect->right = static_cast<LONG>(half.TopLeftX + half.Width);
        halfRect->bottom = static_cast<LONG>(half.TopLeftY + half.Height);
    }
    const EyeFrustum& f = g_eye[eye];
    if (!f.valid) return half;
    D3D11_VIEWPORT vp = half;
    vp.TopLeftX = half.TopLeftX + (f.rawLeft + 1.0f) * 0.5f * half.Width;
    vp.Width    = (f.rawRight - f.rawLeft) * 0.5f * half.Width;
    vp.TopLeftY = half.TopLeftY + (1.0f - f.rawTop) * 0.5f * half.Height;
    vp.Height   = (f.rawTop - f.rawBottom) * 0.5f * half.Height;
    return vp;
}

// Viewport de un ojo para la interfaz: pantalla virtual a kUiDistanceM
// metros, de 2*UiHalfWidthM de ancho y con la proporcion de la ventana del
// juego, vista desde cada ojo (desplazado +-IPD/2).
static D3D11_VIEWPORT UiViewport(const D3D11_VIEWPORT& full, int eye) {
    D3D11_VIEWPORT half = EyeHalfViewport(full, eye);
    const EyeFrustum& f = g_eye[eye];
    if (!f.valid) return half;
    float eyeX = (eye == 0) ? -kUiIpdM * 0.5f : kUiIpdM * 0.5f;
    float aspect = (full.Width > 0.0f) ? (full.Height / full.Width) : 0.5625f;
    float halfW = UiHalfWidthM();
    float halfH = halfW * aspect;
    float tanL = (-halfW - eyeX) / kUiDistanceM;
    float tanR = ( halfW - eyeX) / kUiDistanceM;
    float tanT =  halfH / kUiDistanceM;
    float tanB = -halfH / kUiDistanceM;
    float idx = 1.0f / (f.r - f.l);
    float idy = 1.0f / (f.b - f.t);
    float ndcL = (2.0f * tanL - (f.r + f.l)) * idx;
    float ndcR = (2.0f * tanR - (f.r + f.l)) * idx;
    float ndcT = (2.0f * tanT - (f.b + f.t)) * idy;
    float ndcB = (2.0f * tanB - (f.b + f.t)) * idy;
    D3D11_VIEWPORT vp = half;
    vp.TopLeftX = half.TopLeftX + (ndcL + 1.0f) * 0.5f * half.Width;
    vp.Width    = (ndcR - ndcL) * 0.5f * half.Width;
    vp.TopLeftY = half.TopLeftY + (1.0f - ndcT) * 0.5f * half.Height;
    vp.Height   = (ndcT - ndcB) * 0.5f * half.Height;
    return vp;
}

// ---------------------------------------------------------------------
// Consultas de estado del pipeline
// ---------------------------------------------------------------------

// true si el draw actual tiene el test de profundidad desactivado: asi
// dibuja Blade el cielo (geometria pegada a la camara vista por los huecos).
static bool DepthTestDisabled(ID3D11DeviceContext* ctx) {
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&dss, &ref);
    if (!dss) return false;
    D3D11_DEPTH_STENCIL_DESC d{};
    dss->GetDesc(&d);
    dss->Release();
    return !d.DepthEnable;
}

// true si alguno de los primeros SRV del pixel shader es un RENDER TARGET:
// identifica la composicion final y el post-proceso, que leen la escena ya
// renderizada (y ya en SBS).
static bool DrawSamplesRenderTarget(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srvs[4] = {};
    ctx->PSGetShaderResources(0, 4, srvs);
    bool samplesRt = false;
    for (int i = 0; i < 4; ++i) {
        if (!srvs[i]) continue;
        ID3D11Resource* res = nullptr;
        srvs[i]->GetResource(&res);
        if (res) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                if (d.BindFlags & D3D11_BIND_RENDER_TARGET) samplesRt = true;
                tex->Release();
            }
            res->Release();
        }
        srvs[i]->Release();
    }
    return samplesRt;
}

static ID3D11RasterizerState* ScissorStateFor(ID3D11RasterizerState* original) {
    uintptr_t key = reinterpret_cast<uintptr_t>(original);
    {
        std::lock_guard<std::mutex> lock(g_rsMutex);
        auto it = g_scissorStates.find(key);
        if (it != g_scissorStates.end()) return it->second;
    }
    ID3D11Device* device = GetGameDevice();
    if (!device) return nullptr;
    D3D11_RASTERIZER_DESC d{};
    if (original) {
        original->GetDesc(&d);
    } else {
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_BACK;
        d.DepthClipEnable = TRUE;
    }
    d.ScissorEnable = TRUE;
    ID3D11RasterizerState* created = nullptr;
    if (FAILED(device->CreateRasterizerState(&d, &created)) || !created) return nullptr;
    std::lock_guard<std::mutex> lock(g_rsMutex);
    auto it = g_scissorStates.find(key);
    if (it != g_scissorStates.end()) { created->Release(); return it->second; }
    g_scissorStates[key] = created;
    return created;
}

// ---------------------------------------------------------------------
// Matrices de cada ojo
// ---------------------------------------------------------------------
static bool EnsureEyeConstantBuffers() {
    if (g_cbLeft && g_cbRight && g_cbLeftNoEye && g_cbRightNoEye) return true;
    if (g_cbCreateFailed) return false;
    ID3D11Device* device = GetGameDevice();
    if (!device) return false;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 64;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bool ok = SUCCEEDED(device->CreateBuffer(&desc, nullptr, &g_cbLeft)) &&
              SUCCEEDED(device->CreateBuffer(&desc, nullptr, &g_cbRight)) &&
              SUCCEEDED(device->CreateBuffer(&desc, nullptr, &g_cbLeftNoEye)) &&
              SUCCEEDED(device->CreateBuffer(&desc, nullptr, &g_cbRightNoEye));
    if (!ok || !g_cbLeft || !g_cbRight || !g_cbLeftNoEye || !g_cbRightNoEye) {
        g_cbCreateFailed = true;
        HookLogger::Instance().Line("[STEREO] ERROR: no se pudieron crear los constant buffers de cada ojo.");
        return false;
    }
    return true;
}

// Matriz de un ojo a partir de la del juego (vector fila, w = z):
//   * con visor: m[0] = 2/(r-l), m[8] = -(r+l)/(r-l) (el termino que
//     multiplica a z desplaza la imagen una constante en NDC: eso ES la
//     asimetria del frustum), |m[5]| = 2/(b-t) conservando el signo del juego,
//     m[9] = -(b+t)/(b-t); near/far (m[10], m[14]) del juego;
//   * sin visor: FOV del juego en media anchura (m[0] doblado);
//   * en ambos, el desplazamiento de ojo es un off-axis: m[12] = -dx*m[0].
static void FillEyeMatrix(float* dst, const float* src, float dx, int eye) {
    memcpy(dst, src, 64);
    const EyeFrustum& f = g_eye[eye];
    if (!f.valid) {
        dst[0] = src[0] * 2.0f;
    } else {
        float idx = 1.0f / (f.r - f.l);
        float idy = 1.0f / (f.b - f.t);
        dst[0] = 2.0f * idx;
        dst[8] = -(f.r + f.l) * idx;
        dst[5] = (src[5] < 0.0f ? -2.0f : 2.0f) * idy;
        dst[9] = -(f.b + f.t) * idy;
    }
    dst[12] = -dx * dst[0];
}

static bool UpdateEyeConstantBuffers(ID3D11DeviceContext* ctx, const float gameMatrix[16]) {
    float separation = StereoGetEyeSeparationUnits();
    // OpenVR se inicializa de forma asincrona: cuando pase a estar disponible
    // hay que volver a pedir los frustums aunque la matriz no haya cambiado.
    bool hmdAvailable = IsOpenVRAvailable();
    if (g_cbFilled && separation == g_cbSourceSeparation && hmdAvailable == g_cbSourceHadHmd &&
        memcmp(gameMatrix, g_cbSourceMatrix, 64) == 0) return true;

    RefreshEyeFrustums(gameMatrix);

    float half = separation * 0.5f;
    float left[16], right[16], leftNoEye[16], rightNoEye[16];
    FillEyeMatrix(left, gameMatrix, -half, 0);    // ojo izquierdo: camara a -IPD/2
    FillEyeMatrix(right, gameMatrix, +half, 1);   // ojo derecho:   camara a +IPD/2
    FillEyeMatrix(leftNoEye, gameMatrix, 0.0f, 0);
    FillEyeMatrix(rightNoEye, gameMatrix, 0.0f, 1);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    auto upload = [&](ID3D11Buffer* cb, const float* m) -> bool {
        if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) return false;
        memcpy(mapped.pData, m, 64);
        ctx->Unmap(cb, 0);
        return true;
    };
    if (!upload(g_cbLeft, left) || !upload(g_cbRight, right) ||
        !upload(g_cbLeftNoEye, leftNoEye) || !upload(g_cbRightNoEye, rightNoEye)) return false;

    memcpy(g_cbSourceMatrix, gameMatrix, 64);
    g_cbSourceSeparation = separation;
    g_cbSourceHadHmd = hmdAvailable;
    g_cbFilled = true;

    std::ostringstream o;
    o << "[STEREO] matrices de ojo actualizadas -- juego m00=" << gameMatrix[0] << " m11=" << gameMatrix[5]
      << " -> ojo m00=" << left[0] << " m8_izq=" << left[8] << " m8_der=" << right[8]
      << " m12_izq=" << left[12] << " m12_der=" << right[12]
      << " separacion=" << separation << " visor=" << (g_eye[0].valid ? "si" : "no");
    HookLogger::Instance().Line(o.str());
    return true;
}

static void MaybeLogRate() {
    uint64_t now = GetTickCount64();
    uint64_t last = g_lastLogMs.load(std::memory_order_relaxed);
    if (now - last < 5000) return;
    if (!g_lastLogMs.compare_exchange_strong(last, now)) return;
    std::ostringstream o;
    o << "[STEREO] ultimos 5 s: draws 3D=" << g_draws3D.exchange(0)
      << " draws 2D=" << g_draws2D.exchange(0)
      << " composicion/post=" << g_drawsComposition.exchange(0);
    HookLogger::Instance().Line(o.str());
}

// ---------------------------------------------------------------------
// Nucleo: emitir el draw una o dos veces
// ---------------------------------------------------------------------

// Con el estereo apagado solo se cuenta cuantos draws 3D hay por fotograma,
// para detectar la primera escena de verdad (arranque automatico).
static void NoteDrawWhileDisabled() {
    if (g_sceneSeen.load(std::memory_order_relaxed)) return;
    float m[16];
    if (!ProjectionGetBoundVSSlot0Matrix(m)) return;
    unsigned long long frame = GetPresentCallCount();
    if (g_sceneFrameId.load(std::memory_order_relaxed) != frame) {
        g_sceneFrameId.store(frame, std::memory_order_relaxed);
        g_sceneDrawsThisFrame.store(0, std::memory_order_relaxed);
    }
    if (g_sceneDrawsThisFrame.fetch_add(1, std::memory_order_relaxed) + 1 >= k3DDrawsPerFrameForScene) {
        g_sceneSeen.store(true, std::memory_order_relaxed);
    }
}

// 'draw' es una lambda que llama a la funcion original con los argumentos
// exactos del draw call.
template <typename DrawFn>
static void EmitDraw(ID3D11DeviceContext* ctx, DrawFn&& draw) {
    if (!g_enabled.load(std::memory_order_relaxed) || g_inReplay.load(std::memory_order_relaxed)) {
        NoteDrawWhileDisabled();
        draw();
        return;
    }

    UINT numViewports = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&numViewports, &vp);
    if (numViewports == 0 || vp.Width < 2.0f) { draw(); return; }

    float gameMatrix[16];
    bool isPerspective = ProjectionGetBoundVSSlot0Matrix(gameMatrix);
    uint64_t frame = GetPresentCallCount();

    if (!isPerspective) {
        // ---- draw 2D ----
        if (DrawSamplesRenderTarget(ctx)) {
            g_compositionFrame.store(frame, std::memory_order_relaxed);
            g_drawsComposition.fetch_add(1, std::memory_order_relaxed);
            draw();
            return;
        }
        bool afterComposition = (g_compositionFrame.load(std::memory_order_relaxed) == frame);
        bool no3DYet = (g_last3DFrame.load(std::memory_order_relaxed) != frame);
        bool isUi = afterComposition || no3DYet;

        g_inReplay.store(true, std::memory_order_relaxed);
        if (isUi || !g_eye[0].valid) {
            for (int eye = 0; eye < 2; ++eye) {
                D3D11_VIEWPORT eyeVp = g_eye[eye].valid ? UiViewport(vp, eye) : EyeHalfViewport(vp, eye);
                ctx->RSSetViewports(1, &eyeVp);
                draw();
            }
        } else {
            ID3D11RasterizerState* gameRs = nullptr;
            ctx->RSGetState(&gameRs);
            ID3D11RasterizerState* scissorRs = ScissorStateFor(gameRs);
            UINT numRects = 1;
            D3D11_RECT gameScissor{};
            ctx->RSGetScissorRects(&numRects, &gameScissor);
            if (scissorRs) ctx->RSSetState(scissorRs);
            for (int eye = 0; eye < 2; ++eye) {
                D3D11_RECT halfRect{};
                D3D11_VIEWPORT eyeVp = SpriteViewport(vp, eye, &halfRect);
                ctx->RSSetScissorRects(1, &halfRect);
                ctx->RSSetViewports(1, &eyeVp);
                draw();
            }
            ctx->RSSetState(gameRs);   // nullptr restaura el estado por defecto
            if (numRects > 0) ctx->RSSetScissorRects(1, &gameScissor);
            if (gameRs) gameRs->Release();
        }
        ctx->RSSetViewports(1, &vp);
        g_inReplay.store(false, std::memory_order_relaxed);
        g_draws2D.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ---- draw 3D ----
    if (!EnsureEyeConstantBuffers() || !UpdateEyeConstantBuffers(ctx, gameMatrix)) { draw(); return; }

    g_last3DFrame.store(frame, std::memory_order_relaxed);
    bool noEye = DepthTestDisabled(ctx);            // cielo: al infinito
    ID3D11Buffer* gameCb = ProjectionGetBoundVSSlot0Buffer();
    g_inReplay.store(true, std::memory_order_relaxed);

    D3D11_VIEWPORT eyeVp = EyeHalfViewport(vp, 0);
    ProjectionBindVSSlot0Raw(ctx, noEye ? g_cbLeftNoEye : g_cbLeft);
    ctx->RSSetViewports(1, &eyeVp);
    draw();

    eyeVp = EyeHalfViewport(vp, 1);
    ProjectionBindVSSlot0Raw(ctx, noEye ? g_cbRightNoEye : g_cbRight);
    ctx->RSSetViewports(1, &eyeVp);
    draw();

    ctx->RSSetViewports(1, &vp);
    ProjectionBindVSSlot0Raw(ctx, gameCb);
    g_inReplay.store(false, std::memory_order_relaxed);
    g_draws3D.fetch_add(1, std::memory_order_relaxed);
    MaybeLogRate();
}

static void STDMETHODCALLTYPE HookedDrawIndexed(ID3D11DeviceContext* self, UINT IndexCount,
                                                  UINT StartIndexLocation, INT BaseVertexLocation) {
    EmitDraw(self, [&]() { g_originalDrawIndexed(self, IndexCount, StartIndexLocation, BaseVertexLocation); });
}

static void STDMETHODCALLTYPE HookedDraw(ID3D11DeviceContext* self, UINT VertexCount, UINT StartVertexLocation) {
    EmitDraw(self, [&]() { g_originalDraw(self, VertexCount, StartVertexLocation); });
}

// ---------------------------------------------------------------------
// Instalacion
// ---------------------------------------------------------------------
bool InstallStereoHook(ID3D11DeviceContext* context) {
    if (g_installed || !context) return g_installed;
    void** vtable = *reinterpret_cast<void***>(context);
    void* drawIndexedAddr = vtable[ContextVTable::DrawIndexed];
    void* drawAddr = vtable[ContextVTable::Draw];

    bool ok = true;
    ok &= (MH_CreateHook(drawIndexedAddr, &HookedDrawIndexed, reinterpret_cast<void**>(&g_originalDrawIndexed)) == MH_OK);
    ok &= (MH_CreateHook(drawAddr, &HookedDraw, reinterpret_cast<void**>(&g_originalDraw)) == MH_OK);
    ok &= (MH_EnableHook(drawIndexedAddr) == MH_OK);
    ok &= (MH_EnableHook(drawAddr) == MH_OK);

    g_installed = ok;
    HookLogger::Instance().Line(ok ? "[STEREO] Hooks de DrawIndexed/Draw instalados."
                                   : "[STEREO] ERROR: fallo al instalar los hooks de Draw.");
    return ok;
}

void UninstallStereoHook() {
    // Los hooks se deshabilitan en bloque en UninstallDx11Hook.
    g_installed = false;
}

void LogStereoSummary() {
    HookLogger::Instance().Line(std::string("STEREO_SUMMARY enabled=") + (StereoIsEnabled() ? "true" : "false") +
                                " separacion=" + std::to_string(StereoGetEyeSeparationUnits()) +
                                " interfaz=" + std::to_string(StereoGetUiSizePreset()));
}

} // namespace BladeVR
