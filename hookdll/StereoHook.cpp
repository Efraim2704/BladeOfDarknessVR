#include "StereoHook.h"
#include "HookLogger.h"
#include "ProjectionHook.h"
#include "Dx11Hook.h"
#include "OpenVRHook.h"
#include "HeadTrackHook.h"
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
static std::atomic<float> g_eyeSeparationUnits{54.0f};   // valor calibrado con el visor puesto
static std::atomic<int> g_uiSizePreset{1};
static constexpr float kEyeSeparationStep = 4.0f;
static constexpr float kEyeSeparationMax = 400.0f;

// Fase plana: el video de introduccion, las pantallas de carga y los menus
// (principal con su escenario de fondo, y pausa si usa el mismo fondo) se
// muestran como una imagen normal (sin duplicar) en una pantalla anclada al
// mundo (overlay de SteamVR, ver Dx11Hook.cpp). Se decide con lo que se ha
// DIBUJADO en el fotograma anterior, en el propio hilo de render:
//  - menos de kScene3DDrawsMin draws con perspectiva: no hay escena 3D
//    (video, pantallas de carga, negro);
//  - una mascara de menu a pantalla completa (ver kFullMaskMinW): menu
//    principal y menu de pausa, en el mismo fotograma en que aparece;
//  - un fondo ancho cualquiera (algunas paginas del menu principal usan uno
//    de 640x480 en vez de la mascara) con la logica del juego parada.
// Los paneles que se abren SOBRE el juego (F1, ficha de la seleccion de
// personaje) no pasan a plano: van por la interfaz anclada.
// La decision se fija UNA vez por Present (StereoPollKeys) y vale para todo
// el fotograma siguiente (sus draws) y su entrega a SteamVR, de modo que
// nunca se muestra en la pantalla plana un fotograma dibujado en estereo ni
// al reves.
// Descartado (parpadeaba o llegaba tarde): SetFOV + temporizador; Presents o
// pasadas de culling sin pose de camara (en las cinematicas la camara se
// actualiza a ~24 Hz y hay planos fijos de segundos); el reloj del mundo
// (Bladex.GetTime, mundo+0x20c0): en las cinematicas se para medio segundo
// si y medio no, y en las cargas avanza a ratos.
static constexpr uint32_t kScene3DDrawsMin = 8;
// Los fondos son un unico cuadrilatero (<= 6 vertices) con una textura no
// cuadrada que no es render target; el HUD usa atlas cuadrados (1024x1024,
// 512x512) o texturas pequenas (128x64, 256x64).
static constexpr UINT kMenuQuadMaxVerts = 6;
// Los tamanos son los de las TEXTURAS del juego (ficheros de DATA), no los
// de la ventana, asi que no dependen de la resolucion a la que se juegue; y
// la proporcion se compara con valores fijos, no con la de la ventana, para
// que tambien valga en pantallas que no sean 16:9.
static constexpr UINT kWideBgMinW = 640;        // fondo ancho cualquiera
// Mascara de menu a pantalla completa (menu principal y de pausa: 1920x1080):
// ancha y con proporcion de pantalla.
static constexpr UINT kFullMaskMinW = 1280;
static constexpr float kFullMaskAspectMin = 1.60f;
static constexpr float kFullMaskAspectMax = 2.10f;
// Panel sobre el juego (combos de F1: 2560x1920, proporcion 1.33; ficha de
// la seleccion: 1920x512, proporcion 3.75): ancho pero con una proporcion
// que no es la de una pantalla. El juego sigue en estereo detras y el panel
// va a un overlay anclado ("interfaz anclada").
static constexpr UINT kPanelTexMinW = 1024;
static constexpr int kPanelEnterFrames = 3;
static int g_panelFrames = 0;
static bool g_uiOverlayActive = false;
// El menu principal necesita ademas saber si la logica del juego esta parada
// (pasadas de culling seguidas sin que el juego toque la camara), porque
// algunas de sus paginas no dibujan la mascara.
static constexpr int kFlatAfterCulls = 4;
// (Descartado: decidir el menu por "logica parada" -- pasadas de culling sin
// que el juego toque la camara. F1 y la seleccion tambien la paran o la
// dejan a medias, y la deteccion tardaba varios fotogramas, con el menu
// visible mientras tanto pegado a la mirada.)
// Al volver a haber escena tras kNoSceneFramesForNewLevel fotogramas sin
// ella (pantalla de carga) se recentran las referencias del visor.
static constexpr int kNoSceneFramesForNewLevel = 30;
static int g_noSceneFrames = 0;
static std::atomic<int> g_cullsSinceCameraPose{1000};
static std::atomic<bool> g_flatLatched{true};
static std::atomic<bool> g_flatPrev{true};   // valor con el que se dibujo el fotograma recien terminado
static std::atomic<bool> g_uiOverlayLatched{false};
static std::atomic<bool> g_uiOverlayPrev{false};
static ID3D11Texture2D* g_uiOverlayTex = nullptr;      // solo hilo de render
static ID3D11RenderTargetView* g_uiOverlayRtv = nullptr;
static uint64_t g_uiOverlayClearedFrame = ~0ull;
static std::atomic<uint32_t> g_frame3DDraws{0};
static std::atomic<bool> g_frameFullMaskSeen{false};   // mascara de menu a pantalla completa, fotograma en curso
static std::atomic<bool> g_framePanelSeen{false};      // panel ancho sobre el juego, fotograma en curso
static std::atomic<bool> g_frameWideBgSeen{false};     // cualquier fondo ancho, fotograma en curso
static std::atomic<bool> g_frameUiRedirected{false};   // se ha desviado interfaz al overlay en este fotograma

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
static std::atomic<uint64_t> g_drawsFlareSkipped{0};

// El destello del sol es un sprite 2D que el juego dibuja, tras la primera
// pasada de composicion, sobre el render target HDR de la escena (formato
// R16G16B16A16_FLOAT; la interfaz va al backbuffer R8G8B8A8). Su posicion y
// su oclusion las calcula el juego desde SU camara, no desde el visor, asi
// que en VR aparece donde no toca y a traves de las paredes: se omite.
static constexpr bool kHideSunFlare = true;
static std::atomic<uint64_t> g_lastLogMs{0};

// Estados de rasterizador con scissor, cacheados por el estado del juego.
static std::mutex g_rsMutex;
static std::unordered_map<uintptr_t, ID3D11RasterizerState*> g_scissorStates;

// ---------------------------------------------------------------------
// Ajustes y teclas
// ---------------------------------------------------------------------
bool StereoInFlatPhase() { return g_flatLatched.load(std::memory_order_relaxed); }
bool StereoFrameWasFlat() { return g_flatPrev.load(std::memory_order_relaxed); }
void StereoNotifyCameraPose() { g_cullsSinceCameraPose.store(0, std::memory_order_relaxed); }
void StereoNotifyCullingPass() {
    int n = g_cullsSinceCameraPose.load(std::memory_order_relaxed);
    if (n < 1000) g_cullsSinceCameraPose.store(n + 1, std::memory_order_relaxed);
}
ID3D11Texture2D* StereoUiOverlayTextureOfFrame() {
    return g_uiOverlayPrev.load(std::memory_order_relaxed) ? g_uiOverlayTex : nullptr;
}

// Una vez por Present: fija la decision de fase plana para el fotograma
// siguiente a partir de lo dibujado en el que acaba de terminar.
static void LatchFlatPhase() {
    uint32_t draws3D = g_frame3DDraws.exchange(0, std::memory_order_relaxed);
    bool fullMask = g_frameFullMaskSeen.exchange(false, std::memory_order_relaxed);
    bool panel = g_framePanelSeen.exchange(false, std::memory_order_relaxed);
    bool wideBg = g_frameWideBgSeen.exchange(false, std::memory_order_relaxed);
    bool logicStopped = g_cullsSinceCameraPose.load(std::memory_order_relaxed) >= kFlatAfterCulls;
    bool redirected = g_frameUiRedirected.exchange(false, std::memory_order_relaxed);
    bool scene = draws3D >= kScene3DDrawsMin;
    if (!scene) {
        ++g_noSceneFrames;
    } else {
        if (g_noSceneFrames >= kNoSceneFramesForNewLevel) HeadTrackResetReferences();
        g_noSceneFrames = 0;
    }
    bool flat = !scene || fullMask || (wideBg && !panel && logicStopped);
    g_panelFrames = (panel && scene && !fullMask) ? g_panelFrames + 1 : 0;
    if (g_uiOverlayActive) {
        if (!panel || !scene || fullMask) g_uiOverlayActive = false;
    } else if (g_panelFrames >= kPanelEnterFrames) {
        g_uiOverlayActive = true;
    }
    bool uiOverlay = g_uiOverlayActive;
    // El fotograma recien terminado lleva su interfaz en el overlay si de
    // verdad se desvio algun draw (tambien en el primer fotograma de un menu
    // a pantalla completa, ver EmitDraw) Y ese fotograma sigue teniendo el
    // fondo de la pantalla. Al cerrar F1 el fondo ya no esta pero el estado
    // aun venia activo, asi que la interfaz (el HUD) se habia desviado: se
    // pierde un fotograma de HUD, que no se nota, en vez de ensenarlo un
    // instante en la pantalla anclada, que si se notaba.
    g_uiOverlayPrev.store(redirected && (panel || fullMask), std::memory_order_relaxed);
    if (g_uiOverlayLatched.exchange(uiOverlay, std::memory_order_relaxed) != uiOverlay) {
        HookLogger::Instance().Line(uiOverlay ? "[STEREO] Interfaz anclada (panel sobre el juego): activada."
                                              : "[STEREO] Interfaz anclada: terminada.");
    }
    g_flatPrev.store(g_flatLatched.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (g_flatLatched.exchange(flat, std::memory_order_relaxed) != flat) {
        std::ostringstream o;
        o << "[STEREO] Fase plana: " << (flat ? "activada" : "terminada (estereo 3D)")
          << " (draws 3D=" << draws3D << (fullMask ? ", mascara a pantalla completa" : "")
          << (panel ? ", panel" : "") << (wideBg ? ", fondo ancho" : "")
          << (logicStopped ? ", logica parada" : ", logica viva") << ")";
        HookLogger::Instance().Line(o.str());
    }
}

// Se llama para cada draw del juego (no para las repeticiones por ojo):
// cuenta los draws 3D y busca la textura de fondo de menu en los 2D.
static void NoteDrawForSceneDetection(ID3D11DeviceContext* ctx, bool isPerspective, UINT vertexCount) {
    if (isPerspective) {
        g_frame3DDraws.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if ((d.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
            if (vertexCount <= kMenuQuadMaxVerts && d.Width != d.Height && d.Height > 0 &&
                d.Width >= kWideBgMinW) {
                float aspect = static_cast<float>(d.Width) / static_cast<float>(d.Height);
                bool screenAspect = (aspect >= kFullMaskAspectMin && aspect <= kFullMaskAspectMax);
                g_frameWideBgSeen.store(true, std::memory_order_relaxed);
                if (d.Width >= kFullMaskMinW && screenAspect) {
                    g_frameFullMaskSeen.store(true, std::memory_order_relaxed);
                } else if (d.Width >= kPanelTexMinW && !screenAspect) {
                    g_framePanelSeen.store(true, std::memory_order_relaxed);
                }
            }
        }
        tex->Release();
    }
    if (res) res->Release();
    srv->Release();
}
float StereoGetEyeSeparationUnits() { return g_eyeSeparationUnits.load(std::memory_order_relaxed); }
int StereoGetUiSizePreset() { return g_uiSizePreset.load(std::memory_order_relaxed); }

static bool KeyPressedOnce(int vk, std::atomic<bool>& wasDown) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool was = wasDown.exchange(down, std::memory_order_relaxed);
    return down && !was;
}

void StereoEndFrame() { LatchFlatPhase(); }

void StereoPollKeys() {
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
float StereoUiDistanceM() { return kUiDistanceM; }
float StereoUiHalfWidthM() { return UiHalfWidthM(); }

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
// juego, vista desde cada ojo (desplazado +-IPD/2); va con la cabeza.
// halfRect: la mitad del ojo, para el scissor.
static D3D11_VIEWPORT UiViewport(const D3D11_VIEWPORT& full, int eye, D3D11_RECT* halfRect) {
    D3D11_VIEWPORT half = EyeHalfViewport(full, eye);
    if (halfRect) {
        halfRect->left = static_cast<LONG>(half.TopLeftX);
        halfRect->top = static_cast<LONG>(half.TopLeftY);
        halfRect->right = static_cast<LONG>(half.TopLeftX + half.Width);
        halfRect->bottom = static_cast<LONG>(half.TopLeftY + half.Height);
    }
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

// Los sprites del mundo (la llama de una antorcha, chispas) se dibujan
// tambien con el test de profundidad apagado, igual que el cielo, pero NO
// estan al infinito: son un cuadrado pequeno a unos metros de distancia.
// Tratarlos como cielo los dejaba sin desplazamiento de ojo, y al girar la
// cabeza se separaban de la antorcha: el circulo de fuego parecia crecer.
// Se reconocen por su firma: un cuadrilatero (<=6 vertices) con mezcla
// activada y una textura pequena. El cielo del juego es geometria del nivel,
// con muchos mas vertices y texturas grandes.
static constexpr UINT kWorldSpriteMaxVerts = 6;
static constexpr UINT kWorldSpriteMaxTex = 128;

static bool LooksLikeWorldSprite(ID3D11DeviceContext* ctx, UINT vertexCount) {
    if (vertexCount > kWorldSpriteMaxVerts) return false;

    ID3D11BlendState* bs = nullptr;
    float blendFactor[4] = {};
    UINT sampleMask = 0;
    ctx->OMGetBlendState(&bs, blendFactor, &sampleMask);
    bool blended = false;
    if (bs) {
        D3D11_BLEND_DESC bd{};
        bs->GetDesc(&bd);
        blended = bd.RenderTarget[0].BlendEnable != FALSE;
        bs->Release();
    }
    if (!blended) return false;

    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    bool smallTexture = false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        smallTexture = (d.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 &&
                       d.Width <= kWorldSpriteMaxTex && d.Height <= kWorldSpriteMaxTex;
        tex->Release();
    }
    if (res) res->Release();
    srv->Release();
    return smallTexture;
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

// Formato del render target vinculado en el slot 0 (DXGI_FORMAT_UNKNOWN si no hay).
static DXGI_FORMAT CurrentRenderTargetFormat(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return DXGI_FORMAT_UNKNOWN;
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    rtv->GetDesc(&d);
    rtv->Release();
    return d.Format;
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
      << " composicion/post=" << g_drawsComposition.exchange(0)
      << " destello omitido=" << g_drawsFlareSkipped.exchange(0)
      << " fase plana=" << (StereoInFlatPhase() ? "si" : "no");
    HookLogger::Instance().Line(o.str());
}

// ---------------------------------------------------------------------
// Nucleo: emitir el draw una o dos veces
// ---------------------------------------------------------------------

// Textura RGBA (tamano del render target del juego) donde se dibuja la
// interfaz en modo interfaz anclada. Se limpia a transparente en el primer
// draw de interfaz de cada fotograma.
static bool EnsureUiOverlayTexture(ID3D11DeviceContext* ctx, ID3D11Texture2D* like) {
    D3D11_TEXTURE2D_DESC d{};
    like->GetDesc(&d);
    if (g_uiOverlayTex) {
        D3D11_TEXTURE2D_DESC cur{};
        g_uiOverlayTex->GetDesc(&cur);
        if (cur.Width == d.Width && cur.Height == d.Height) return true;
        g_uiOverlayRtv->Release(); g_uiOverlayRtv = nullptr;
        g_uiOverlayTex->Release(); g_uiOverlayTex = nullptr;
    }
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (!device) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = d.Width;
    desc.Height = d.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &g_uiOverlayTex)) && g_uiOverlayTex &&
              SUCCEEDED(device->CreateRenderTargetView(g_uiOverlayTex, nullptr, &g_uiOverlayRtv)) && g_uiOverlayRtv;
    device->Release();
    if (!ok) {
        if (g_uiOverlayRtv) { g_uiOverlayRtv->Release(); g_uiOverlayRtv = nullptr; }
        if (g_uiOverlayTex) { g_uiOverlayTex->Release(); g_uiOverlayTex = nullptr; }
        HookLogger::Instance().Line("[STEREO] ERROR: no se pudo crear la textura de la interfaz anclada.");
        return false;
    }
    std::ostringstream o;
    o << "[STEREO] Textura de la interfaz anclada creada: " << d.Width << "x" << d.Height;
    HookLogger::Instance().Line(o.str());
    return true;
}

// Dibuja un draw de interfaz en la textura aparte (una sola vez, viewport
// del juego) en vez de en el render target del juego. false si no se pudo
// (se dibuja entonces por el camino normal).
template <typename DrawFn>
static bool DrawUiIntoOverlayTexture(ID3D11DeviceContext* ctx, uint64_t frame, DrawFn&& draw) {
    ID3D11RenderTargetView* gameRtv = nullptr;
    ID3D11DepthStencilView* gameDsv = nullptr;
    ctx->OMGetRenderTargets(1, &gameRtv, &gameDsv);
    if (!gameRtv) { if (gameDsv) gameDsv->Release(); return false; }
    ID3D11Resource* res = nullptr;
    gameRtv->GetResource(&res);
    ID3D11Texture2D* rtTex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rtTex));
    bool ok = rtTex && EnsureUiOverlayTexture(ctx, rtTex);
    if (rtTex) rtTex->Release();
    if (res) res->Release();
    if (ok) {
        if (g_uiOverlayClearedFrame != frame) {
            const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            ctx->ClearRenderTargetView(g_uiOverlayRtv, transparent);
            g_uiOverlayClearedFrame = frame;
        }
        ctx->OMSetRenderTargets(1, &g_uiOverlayRtv, nullptr);
        draw();
        ctx->OMSetRenderTargets(1, &gameRtv, gameDsv);
    }
    gameRtv->Release();
    if (gameDsv) gameDsv->Release();
    return ok;
}

// 'draw' es una lambda que llama a la funcion original con los argumentos
// exactos del draw call.
template <typename DrawFn>
static void EmitDraw(ID3D11DeviceContext* ctx, UINT vertexCount, DrawFn&& draw) {
    uint64_t frame = GetPresentCallCount();

    if (g_inReplay.load(std::memory_order_relaxed)) { draw(); return; }
    {
        float m[16];
        bool persp = ProjectionGetBoundVSSlot0Matrix(m);
        NoteDrawForSceneDetection(ctx, persp, vertexCount);
        if (g_flatLatched.load(std::memory_order_relaxed)) {
            // fase plana: imagen normal, sin duplicar
            draw();
            return;
        }
    }

    UINT numViewports = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&numViewports, &vp);
    if (numViewports == 0 || vp.Width < 2.0f) { draw(); return; }

    float gameMatrix[16];
    bool isPerspective = ProjectionGetBoundVSSlot0Matrix(gameMatrix);
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
        if (kHideSunFlare && afterComposition && CurrentRenderTargetFormat(ctx) == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            g_drawsFlareSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Tambien en el PRIMER fotograma de un menu a pantalla completa (la
        // mascara ya se ha visto en este mismo fotograma, antes de decidir la
        // fase plana): asi el menu aparece ya en su sitio, sin un fotograma
        // pegado a la mirada.
        bool toOverlay = g_uiOverlayLatched.load(std::memory_order_relaxed) ||
                         g_frameFullMaskSeen.load(std::memory_order_relaxed) ||
                         g_framePanelSeen.load(std::memory_order_relaxed);
        if (isUi && toOverlay && DrawUiIntoOverlayTexture(ctx, frame, draw)) {
            g_frameUiRedirected.store(true, std::memory_order_relaxed);
            g_draws2D.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        g_inReplay.store(true, std::memory_order_relaxed);
        bool useUiScreen = isUi || !g_eye[0].valid;
        ID3D11RasterizerState* gameRs = nullptr;
        ctx->RSGetState(&gameRs);
        ID3D11RasterizerState* scissorRs = ScissorStateFor(gameRs);
        UINT numRects = 1;
        D3D11_RECT gameScissor{};
        ctx->RSGetScissorRects(&numRects, &gameScissor);
        if (scissorRs) ctx->RSSetState(scissorRs);
        for (int eye = 0; eye < 2; ++eye) {
            D3D11_RECT halfRect{};
            D3D11_VIEWPORT eyeVp = useUiScreen ? UiViewport(vp, eye, &halfRect) : SpriteViewport(vp, eye, &halfRect);
            ctx->RSSetScissorRects(1, &halfRect);
            ctx->RSSetViewports(1, &eyeVp);
            draw();
        }
        ctx->RSSetState(gameRs);   // nullptr restaura el estado por defecto
        if (numRects > 0) ctx->RSSetScissorRects(1, &gameScissor);
        if (gameRs) gameRs->Release();
        ctx->RSSetViewports(1, &vp);
        g_inReplay.store(false, std::memory_order_relaxed);
        g_draws2D.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ---- draw 3D ----
    if (!EnsureEyeConstantBuffers() || !UpdateEyeConstantBuffers(ctx, gameMatrix)) { draw(); return; }

    g_last3DFrame.store(frame, std::memory_order_relaxed);
    bool depthOff = DepthTestDisabled(ctx);
    bool worldSprite = depthOff && LooksLikeWorldSprite(ctx, vertexCount);
    bool noEye = depthOff && !worldSprite;          // cielo: al infinito
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
    EmitDraw(self, IndexCount, [&]() { g_originalDrawIndexed(self, IndexCount, StartIndexLocation, BaseVertexLocation); });
}

static void STDMETHODCALLTYPE HookedDraw(ID3D11DeviceContext* self, UINT VertexCount, UINT StartVertexLocation) {
    EmitDraw(self, VertexCount, [&]() { g_originalDraw(self, VertexCount, StartVertexLocation); });
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
    if (g_uiOverlayRtv) { g_uiOverlayRtv->Release(); g_uiOverlayRtv = nullptr; }
    if (g_uiOverlayTex) { g_uiOverlayTex->Release(); g_uiOverlayTex = nullptr; }
}

void LogStereoSummary() {
    HookLogger::Instance().Line(std::string("STEREO_SUMMARY") +
                                " separacion=" + std::to_string(StereoGetEyeSeparationUnits()) +
                                " interfaz=" + std::to_string(StereoGetUiSizePreset()));
}

} // namespace BladeVR
