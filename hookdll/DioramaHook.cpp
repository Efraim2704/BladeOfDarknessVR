#include "DioramaHook.h"
#include "DioramaGpuLevel.h"
#include "HeadTrackHook.h"
#include "HookLogger.h"
#include "OpenVRHook.h"
#include "StereoHook.h"
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <string>

namespace BladeVR {

// =====================================================================
// MODO DIORAMA (F5; R3 del mando con la capa de la maqueta)
//
// La partida se ve como una maqueta: el mapa entero a escala (1:10 al
// entrar) sobre una mesa virtual delante del jugador, un poco por debajo de
// los ojos, que se rodea moviendo la cabeza. La camara del juego pasa a ser
// la cabeza del jugador llevada al mundo del juego:
//   camara = ancla + giro * (cabeza - punto de la mesa) * 1000 * escala
// y la separacion entre ojos se multiplica por la escala. El ancla es el
// personaje (objetivo de la entidad Camera): en el modo fija se queda donde
// se coloco; en los acoplados es el personaje de cada fotograma (altura
// suavizada) y la maqueta se desplaza con el. Controles en DioramaHook.h.
//
// Que se cambia en el dibujo del motor (detalles en cada seccion):
//   * B_Map::Render (+0x80f20) busca los sectores que contienen la camara
//     (+0x7aa90) y sigue los portales (+0x7fc90). En la maqueta la camara
//     esta fuera del nivel: +0x7aa90 devuelve a B_Map::Render los sectores
//     del area visible (500 en su lista, el resto como visitas en la cola
//     del motor, todas con el frustum completo) y no se entra por portales
//     (salvo dentro del reflejo del agua): cada sector se dibuja una vez.
//   * Caras por los dos lados: las 5 funciones de dibujo de superficies
//     descartan la cara vista por detras comparando con un double compartido
//     (+0x79fd88, 0.5); se repuntan a uno propio.
//   * Cortes (altura y vertical) y cono de vision: con el nivel del motor,
//     con el plano extra del recortador de caras ([+0xa51f28]); con el nivel
//     en la GPU, por pixel. Los objetos se ocultan o se recortan por pixel
//     (entradas de sus vtables); los personajes se ven siempre, salvo que se
//     pida cortarlos tambien.
//   * Sin el cielo del motor, sin niebla y sin portales de atmosfera.
//   * Nivel en la GPU (DioramaGpuLevel.cpp): se captura una vez lo que
//     dibuja el motor y despues lo dibuja la GPU con las luces del juego; el
//     motor sigue con las entidades, el agua y las sombras de objetos.
//   * Sombras de objetos: sus trozos se dibujan sin escritura de
//     profundidad (StereoHook los trata como calcomanias).
//   * Reflejo del agua: se dibuja una vez desde cada ojo.
//   * Cinematicas: la vista es la camara de la cinematica (solo zoom, cono y
//     cortes).
//   * El mundo del diorama se dibuja UNA vez por fotograma, desde el centro
//     de la cabeza, y StereoHook emite cada draw dos veces con el
//     desplazamiento de cada ojo, con profundidad LESS_EQUAL.
//
// Realidad aumentada (F7, Y): el fondo de la maqueta pasa a un color plano
// (magenta o verde) que el passthrough de Virtual Desktop sustituye por la
// imagen de las camaras. Fuera del diorama no actua nada de esto.
// =====================================================================

// --- offsets (Blade.exe, md5 8ed91d061b75b8889dd26f30061af874) --------------
static constexpr uintptr_t kSectorsAtPointOffset = 0x7aa90;    // bool (nivel, punto, lista)
static constexpr uintptr_t kRenderQueryReturn = 0x8101c;       // su retorno dentro de B_Map::Render
static constexpr uintptr_t kEnterNeighborOffset = 0x7fc90;     // entrada al vecino por un portal
static constexpr uintptr_t kDrawComplexOffset = 0x984a0;       // B_ComplexMS::vtable[5]
static constexpr uintptr_t kDrawPolygonOffset = 0x98870;       // B_PolygonMS::vtable[5]
static constexpr uintptr_t kDrawPolygonPortalOffset = 0x98980; // B_PolygonPortalMS::vtable[5]
static constexpr uintptr_t kDrawPortalOffset = 0x98fd0;        // B_PortalMS::vtable[5]
static constexpr uintptr_t kDrawSkyOffset = 0x99350;           // B_WorldDomeMS::vtable[5] (cielo)
static constexpr int kBackfaceSiteCount = 5;
static constexpr uintptr_t kBackfaceSites[kBackfaceSiteCount] = {
    0x984c7, 0x9889c, 0x989a6, 0x98ffd, 0x9937c };             // uno en cada funcion de dibujo
static constexpr uintptr_t kHalfConstant = 0x79fd88;           // double 0.5 compartido
static constexpr uintptr_t kFaceClipPlanePtr = 0xa51f28;       // plano extra del recortador de caras
static constexpr uintptr_t kCurrentSectorPtr = 0xa51f20;       // sector que dibuja +0x81480 (lo pone al empezar)
static constexpr uintptr_t kVisitPoolOffset = 0xa51b40;        // deposito de visitas
static constexpr uintptr_t kVisitPoolGetOffset = 0x82460;      // visita* (deposito)
static constexpr uintptr_t kDefaultZonePlaneOffset = 0xa51fa0; // plano de zona de las visitas iniciales
static constexpr uintptr_t kPlaneSetCopyOffset = 0x1ad1b0;     // copia de un juego de planos (dst, src)
static constexpr uintptr_t kVisitQueueOffset = 0x9c4948;       // cola de visitas pendientes
static constexpr uintptr_t kVisitQueuePushOffset = 0x85890;    // insertar (cola, visita)
static constexpr uintptr_t kRasterPtrOffset = 0xd93a30;        // B_BgfxRasterDevice*
static constexpr uintptr_t kRasterVtableOffset = 0x8fa790;
static constexpr int kSetAtmosphereSlot = 0x1c0 / 8;           // (raster, plano, zona)
static constexpr uintptr_t kSetAtmosphereThunk = 0x14493;      // -> +0x7479b0
static constexpr int kRasterFogCoef = 0x1ec510;                // double: densidad^2 * k
static constexpr uintptr_t kFogDensityGlobal = 0x109ab40;      // float: densidad de la niebla del sombreador
static constexpr int kAppDrawObjectShadows = 0x768;            // int: sombras de objetos (SetDrawObjectShadows)
static constexpr uintptr_t kShadowRedrawOffset = 0x9b4b0;      // redibujo de la parte en sombra de una cara
// Cara iluminada: raster vtable+0x208 (+0x7451c0 -> +0x750000, primaria;
// (raster, poligono)). La llaman +0x9a8e0 y la hoja de +0x9a950 antes de
// las sombras de cada luz (+0x8fb00 recoge los objetos de la lista del
// sector del cono, +0x250, y de sus conos hijos; +0x9b4b0 las dibuja).
static constexpr uintptr_t kLitFaceOffset = 0x750000;
static constexpr uintptr_t kConePlanesOffset = 0x1ad580;       // planos por el ojo de un poligono (planos, poligono)
static constexpr int kSectorLightCones = 0x230;               // lista de B_LightCone (nodo: sig. +8, cono +0x18)
static constexpr int kSectorEntities = 0x250;                 // lista de entidades del sector
static constexpr int kConeSector = 0xd0;
static constexpr int kConeLight = 0xd8;
static constexpr int kConeChildData = 0xb8;                   // DiArray de conos hijos
static constexpr int kConeChildCount = 0xc0;
static constexpr int kLightCastsShadows = 0x10;               // int: la luz proyecta sombras
// Con el nivel en la GPU, sombras solo en los sectores a menos de esto del
// personaje (en horizontal, metros del juego): cada cara con sombras la
// vuelve a procesar el motor. Con la maqueta muy pequena (escala por encima
// de 1:50) el radio baja (2500 / escala, minimo 15 m): alli las sombras
// lejanas miden milimetros y costarian varios ms por pasada.
static constexpr double kGpuShadowRadiusM = 50.0;
static constexpr double kGpuShadowRadiusMinM = 15.0;
static constexpr double kGpuShadowRadiusScaleM = 2500.0;
static constexpr uintptr_t kBatchOpenOffset = 0x74c030;        // abrir lote de primitivas del raster (ecx modo, edx n)
static constexpr uintptr_t kRasterBgfxState = 0x104aaa8;       // uint64: estado de bgfx actual del raster
static constexpr uint64_t kBgfxWriteZ = 0x0000004000000000ull; // BGFX_STATE_WRITE_Z
static constexpr uint64_t kBgfxDepthTestMask = 0xF0;
static constexpr uint64_t kBgfxDepthTestAlways = 0x80;
// Marca de los lotes que se recortan por pixel (lamina de agua, objetos y
// caras del motor cerca de un corte): BGFX_STATE_LINEAA. En D3D11 es
// AntialiasedLineEnable del rasterizado (sin efecto en triangulos) y bgfx
// rehace el rasterizado cuando cambia (submit +0x67e4ca: mascara
// 0x0700003000000000), asi que StereoHook la ve en cada lote.
static constexpr uint64_t kBgfxWaterTag = 0x0200000000000000ull;
// BGFX_STATE_MSAA (MultisampleEnable; con triangulos no cambia nada, el
// raster del juego nunca la pone): junto con la marca, lote de objeto o de
// cara del motor (no del reflejo del agua): su recorte no usa el plano del
// agua reflejada.
static constexpr uint64_t kBgfxNoReflTag = 0x0100000000000000ull;
// Lotes del reflejo del agua: el juego nunca pone el descarte de caras y
// StereoHook lo quita en la maqueta, asi que sirve de marca del ojo desde el
// que se dibujo cada uno: BGFX_STATE_CULL_CW (en bgfx D3D11_CULL_FRONT,
// tabla +0x8e2f60 = {NONE, FRONT, BACK}) el izquierdo y
// BGFX_STATE_CULL_CCW (D3D11_CULL_BACK) el derecho. StereoHook dibuja cada
// lote solo en su mitad y un poco hacia el fondo (lo real que queda en su
// mismo plano, bajo el agua junto a la orilla, gana como en el juego).
static constexpr uint64_t kBgfxMirrorLeftTag = 0x0000001000000000ull;
static constexpr uint64_t kBgfxMirrorRightTag = 0x0000002000000000ull;
static constexpr int kRasterWaterMode = 0x64;                   // int: raster en modo agua/espejo (vtable 0x110/0x118)
static constexpr uintptr_t kDrawAtmPortalsOffset = 0x81630;    // portales de atmosfera (lista +0x9c4968)
static constexpr uintptr_t kMirrorRenderOffset = 0xf0fd0;      // reflejo del agua (B_MirrorSurface vtable[1])
// Superficie del agua: B_WaterSurface vtable[3] (+0xf0be0, this = superficie,
// r9d = fotograma) la dibuja con el raster en modo agua. Su plano en el mundo
// es n = (0, -1, 0), d = superficie+0x08 (+0xf1cb0 -> +0x7317d0): el agua
// esta a la altura Y = superficie+0x08 (Y del juego hacia abajo).
static constexpr uintptr_t kWaterDrawOffset = 0xf0be0;
static constexpr int kWaterSurfaceHeight = 0x08;
// bgfx: s_ctx; vistas en s_ctx+0x3325900 (0xC0 cada una). El borrado va al
// principio de cada vista (bgfx::Clear): color r, g, b, a en +0x00 (bytes),
// profundidad +0x08, stencil +0x0C, flags (u16) en +0x0E. Comprobado en
// bgfx::setViewClear (+0x653ee0: byte 0 = rgba>>24 ... byte 3 = rgba) y en su
// version con paleta (+0x653f60, que pone 0x8001).
static constexpr uintptr_t kBgfxCtxOffset = 0xF6B658;
static constexpr uintptr_t kBgfxViewsOffset = 0x3325900;
static constexpr int kViewClearFlags = 0x0E;
static constexpr uint16_t kBgfxClearColor = 0x0001;            // BGFX_CLEAR_COLOR
static constexpr uint16_t kBgfxClearUsePalette = 0x8000;       // BGFX_CLEAR_COLOR_USE_PALETTE
static constexpr uintptr_t kAppPointerOffset = 0xF4A6A8;       // objeto de aplicacion
static constexpr int kAppCameraEntity = 0xE8;                  // -> entidad Camera
// B_Entity+0x168: entidad a la que va enlazada ("Parent" en Python, getter
// +0xd7f20 caso 0xe). El inventario (+0x1509e0) la compara con su dueno: un
// arma en la mano, un escudo o una antorcha van enlazados a su personaje.
static constexpr int kEntityParent = 0x168;
static constexpr uintptr_t kPersonVtable = 0x7a4a70;           // B_PersonEntity (RTTI)
static constexpr uintptr_t kBipedVtable = 0x7a3b18;            // B_BipedEntity (RTTI)
static constexpr int kCameraTargetPos = 0x568;                 // TPos (3 doubles)
// Vista de persona (primera persona): el dibujo de una persona (+0xb2d80 /
// +0xb3060, vtable[29]/[30]) no dibuja la cabeza (modelo+0x108 |= 2) si la
// camara la sigue (camara+0x4c0 != 0, +0x4c4 == 3 y su nombre en +0x488),
// app+0x48 lo permite y camara+0x5ac (int) es 0. Solo esas funciones (y la
// de B_ClientEntity) leen +0x5ac.
static constexpr int kCameraShowHead = 0x5ac;

// nivel (singleton +0x7ceb0; B_Map en nivel+0x2110)
static constexpr int kLevelSectorArray = 0x2118;
static constexpr int kLevelSectorCount = 0x2120;
static constexpr uintptr_t kGetLevelOffset = 0x7ceb0;          // FUN_14007ceb0() -> nivel
// La geometria del nivel la captura DioramaGpuLevel.cpp.
// sector (B_MapSector)
static constexpr int kSectorBoxMin = 0x70;       // 3 doubles
static constexpr int kSectorBoxMax = 0x88;       // 3 doubles
static constexpr int kSectorVisitList = 0xa8;    // cabeza de la lista de visitas (sig. en +8)
static constexpr int kSectorZone = 0xc8;
// visita (0x368 bytes)
static constexpr int kVisitSectorNode = 0x20;
static constexpr int kVisitFlag = 0x40;
static constexpr int kVisitZone = 0x44;
static constexpr int kVisitZonePlane = 0x48;
static constexpr int kVisitPlanes = 0x50;
static constexpr int kVisitSector = 0x358;
static constexpr int kVisitSortKey = 0x360;
// El cono no quita nada por debajo de un plano horizontal 50 cm por debajo
// del objetivo de la camara (el suelo por el que va el personaje, con
// cualquier inclinacion, y lo que haya mas abajo).
static constexpr double kConeGroundBelowTargetM = 0.5;
static constexpr int kMirrorToWater = 0x28;          // B_MirrorSurface esta en B_WaterSurface+0x28
// B_Map::Render: fromWorld en el bloque de pose (+0x98) y frustum en el
// tercer argumento (+0x18)
static constexpr int kPoseFromWorld = 0x98;
static constexpr int kRenderArgPlanes = 0x18;

// --- parametros -----------------------------------------------------------------
static constexpr double kDefaultScale = 10.0;       // 1:10
static constexpr double kMinScale = 1.0;
static constexpr double kMaxScale = 500.0;
static constexpr double kScaleStep = 1.25;
static constexpr double kUnitsPerMeter = 1000.0;
static constexpr double kTableForwardM = 0.45;      // el ancla, 45 cm por delante de los ojos...
static constexpr double kTableDropM = 0.35;         // ...y 35 cm por debajo
static constexpr int kMaxStartSectors = 500;        // la lista de B_Map::Render admite 512
static constexpr double kKeepFaceThreshold = 0.5;   // el original
static constexpr double kAlwaysDrawThreshold = -1e30;
static constexpr int kAreaPresetCount = 4;
static const double kAreaPresetM[kAreaPresetCount] = { 0.0, 60.0, 30.0, 15.0 };   // 0 = mapa entero
// Area con los gatillos (metros del juego, horizontal alrededor del personaje).
static constexpr double kAreaMinM = 5.0;
static constexpr double kAreaMaxM = 400.0;          // mas alla: mapa entero
static constexpr double kAreaStartFromWholeM = 120.0;
// Corte vertical (el mismo gatillo, moviendo la mano en horizontal hacia la
// maqueta o hacia uno): plano vertical a vcutM metros del juego por delante
// del personaje; se quita lo que queda entre ese lado y el plano. En los
// modos fija y acoplada es paralelo a los ejes X o Z del mapa (recto como
// sus paredes) y se queda en el lado de la maqueta en que se hace aunque se
// gire o se rodee. En el modo por la espalda se pone de cara a la mirada y
// gira con la maqueta: siempre se ve cortada de frente; o, con el corte
// recto (A mantenido), va por el lado del mapa que queda de cara y cambia
// de lado al girar la maqueta (StraightVcut).
// (Sin la caja del nivel: empieza a 3 m, de -30 a 60 m.)
static constexpr double kVcutStartM = 3.0;
static constexpr double kVcutMinM = -30.0;
static constexpr double kVcutMaxM = 60.0;          // mas alla: sin corte vertical
// Mismo corte que el que ya hay (seguir moviendolo y no empezar otro): la
// direccion nueva y la suya forman menos de unos 3 grados (por ejes del mapa)
// o de 45 (modo por la espalda).
static constexpr double kVcutSameAxisCos = 0.999;
static constexpr double kVcutSameViewCos = 0.707;
// Con el gatillo, el primer eje de la mano que pasa de 5 cm (altura o hacia
// la maqueta) decide el corte de ese gesto; el otro no cambia hasta soltar.
static constexpr double kCutHandDeadzoneM = 0.05;
// Corte de altura (metros del juego por encima del objetivo de la camara).
// (Sin la caja del nivel: empieza a 2,5 m, de -30 a 60 m.)
static constexpr double kCutStartM = 2.5;
static constexpr double kCutMinM = -30.0;
static constexpr double kCutMaxM = 60.0;            // mas alla: sin corte
// Los dos cortes van con la caja del nivel (la union de las cajas de sus
// sectores; con area, la de los sectores del area). El
// que no estaba puesto empieza en el borde del mapa (arriba del todo, o el
// borde de ese lado), asi que al empezar no quita nada de golpe; se quita al
// volver a pasar ese borde (tampoco cambia nada a la vista) y llega hasta el
// borde contrario (todo cortado). Lo que se mueve la mano, por la escala.
static constexpr double kCutEdgeMarginM = 0.05;
// Objetos de malla con el nivel en la GPU: a menos de esta distancia (metros
// del juego) de un corte, por cualquier lado, se dibujan y se recortan pixel
// a pixel; mas adentro de lo que se quita, no se dibujan.
static constexpr double kEntityClipBandM = 25.0;
// Area visible (siempre cuadrada, alineada con la maqueta; medio lado en
// metros del juego). Clic del stick derecho: mapa entero -> 50 m -> 20 m.
static constexpr double kDefaultAreaM = 20.0;
static constexpr double kAreaBigM = 50.0;
static constexpr double kAreaSmallM = 20.0;
// Modos de la maqueta (clic del stick izquierdo o L3):
//   0 fija: se queda donde se deja y el personaje se mueve por ella;
//   1 acoplada: el punto de la mesa es el personaje (la maqueta se desplaza
//     con el, sin moverse ni girar la mesa). En horizontal, directo; la
//     altura, suavizada (un salto no sube y baja toda la maqueta);
//   2 acoplada con vista por la espalda: ademas la maqueta gira despacio
//     para que se vea desde donde mira la camara del juego (detras del
//     personaje).
// El cono de vision (se quita todo lo que queda dentro de un cono entre el
// ojo y el personaje, hasta un poco por delante de el: paredes, techos,
// suelos y objetos; los personajes se ven siempre) va aparte de los modos:
// se enciende y se apaga en cualquier modo y empieza apagado.
static constexpr int kModelModeCount = 3;
static constexpr int kModeBehind = 2;               // acoplada, por la espalda
static constexpr double kFollowYTauS = 0.5;
static constexpr double kBackYawTauS = 0.7;
static constexpr double kViewCutAheadM = 0.8;      // el cono acaba 80 cm por delante del personaje...
static constexpr double kViewCutAheadMaxFrac = 0.4; // ...o antes si el ojo esta muy cerca (40 % de la distancia)
// Radio del cono a la altura del ojo, como fraccion del radio junto al
// personaje: el vertice queda detras del ojo, asi que una pared pegada a la
// cara tambien se abre.
static constexpr double kViewConeEyeFrac = 0.75;
// Radio del cono a la altura del personaje: centimetros sobre la mesa (en el
// juego crece con la escala: al alejar/encoger la maqueta el agujero se ve
// igual), con un minimo en metros del juego. Tres anchos: pequeno 5 cm /
// 1,25 m, mediano 10 cm / 2,5 m (por defecto), grande 16 cm / 4 m.
static constexpr int kConeWidthCount = 3;
static const double kConeRadiusRealM[kConeWidthCount] = { 0.05, 0.10, 0.16 };
static const double kConeMinRadiusM[kConeWidthCount] = { 1.25, 2.5, 4.0 };
static const double kConeMaxHalfAngle[kConeWidthCount] = { 0.45, 0.6, 0.75 };   // radianes
static constexpr int kConeSides = 6;               // piramide de 6 caras
static constexpr int kConePlaneCount = kConeSides + 1;
// Gestos
static constexpr double kMinHandSpanM = 0.05;       // manos mas juntas: no se escala
static constexpr double kMinYawSpanM = 0.10;        // separacion horizontal minima para girar
static constexpr double kPi = 3.14159265358979323846;
// Fondo de la maqueta: el del juego (0) o un color clave para el
// passthrough (magenta, verde).
struct Background {
    unsigned char r, g, b;
};
static constexpr int kBackgroundCount = 3;
static const Background kBackgrounds[kBackgroundCount] = {
    { 0, 0, 0 },
    { 255, 0, 255 },
    { 0, 255, 0 },
};

// --- modelo de la maqueta ------------------------------------------------------------
// Lo lee el hilo del juego (camara, sectores, corte) y lo cambian el hilo del
// juego (colocar) y el de Present (teclas y mandos).
struct Model {
    bool placed = false;
    double anchor[3] = {0.0, 0.0, 0.0};   // punto del juego que va al punto de la mesa
    double table[3] = {0.0, 0.0, 0.0};    // punto de la mesa (espacio de seguimiento, ejes del juego, m)
    double yaw = 0.0;                     // giro seguimiento -> juego
    double scale = kDefaultScale;         // metros del juego por metro real
    double areaM = kDefaultAreaM;         // radio (redonda) o medio lado (cuadrada) del area visible
    int areaShape = 0;                    // 0 sin recorte (mapa entero), 1 redonda, 2 cuadrada
    bool cutOn = false;
    double cutAboveM = kCutStartM;        // corte: metros por encima del personaje
    bool vcutOn = false;
    double vcutM = kVcutStartM;           // corte vertical: metros por delante del personaje (hacia ese lado)
    // Normal del corte vertical (x, z del juego, unitaria; se quita el lado
    // -normal) con el giro de la maqueta (yaw) en que se fijo. En el modo
    // por la espalda gira con la maqueta (VcutDirection).
    double vcutDir[2] = {1.0, 0.0};
    double vcutYawRef = 0.0;
    bool vcutStraight = false;            // corte recto (A mantenido): siempre por un eje del mapa
    int mode = 1;                         // modo de la maqueta (kModelModeCount); por defecto acoplada
};
static std::mutex g_modelMutex;
static Model g_model;
static std::atomic<double> g_scaleMirror{kDefaultScale};   // lectura sin cerrojo (hilo de render)

// --- estado -------------------------------------------------------------------------
static uintptr_t g_base = 0;
static bool g_installed = false;
static std::atomic<bool> g_enabled{false};
static std::atomic<int> g_areaPreset{0};
static std::atomic<int> g_anchorRequest{0};
// Recolocar con el boton (X del mando de Xbox y de Quest, Inicio) pone
// ademas la escala kLocateScale: el personaje se ve como
// un muneco (1,8 m -> 12 cm sobre la mesa) delante, para localizarlo rapido.
// Al cambiar de nivel o activar el diorama se recoloca sin tocar la escala.
static constexpr double kLocateScale = 15.0;
static std::atomic<int> g_anchorLocateRequest{0};
static std::atomic<bool> g_drawActive{false};
static std::atomic<bool> g_f5WasDown{false};
static std::atomic<bool> g_f6WasDown{false};
static std::atomic<bool> g_f7WasDown{false};
static std::atomic<int> g_background{0};
// Ancho del cono (F4, clic largo del stick izquierdo, cruceta arriba/abajo).
static std::atomic<int> g_coneWidth{1};
static std::atomic<bool> g_coneOn{false};           // cono de vision (cruceta izq./dcha. del mando)
static std::atomic<bool> g_f4WasDown{false};
static std::atomic<bool> g_pgUpWasDown{false};
static std::atomic<bool> g_pgDnWasDown{false};

// solo hilo del juego
static bool g_worldActive = false;
static const char* g_renderPlanes = nullptr;
static double g_charPos[3] = {0.0, 0.0, 0.0};      // objetivo de la camara en este fotograma
static bool g_haveCharPos = false;
// Punto que mira la camara de una cinematica (sombras; ver kCineShadowAheadM).
static constexpr double kCineShadowAheadM = 8.0;
static double g_cineFocus[3] = {0.0, 0.0, 0.0};   // hilo del juego
static bool g_haveCineFocus = false;
static double g_followY = 0.0;                    // altura suavizada del ancla al seguir
static bool g_followWasOn = false;                // modo acoplado en la ultima camara calculada
static LARGE_INTEGER g_followLastQpc = {};
static double* g_threshold = nullptr;             // double propio, a menos de 2 GB del modulo
static int32_t g_backfaceOriginalDisp[kBackfaceSiteCount] = {};
static bool g_backfacePatched = false;
static bool g_fogHookTried = false;
static bool g_bgApplied = false;                  // el borrado de la vista 0 es el nuestro
static unsigned char g_bgSavedColor[4] = {};
static uint16_t g_bgSavedFlags = 0;
// Filtro de sectores y plano de corte de la pasada en curso.
struct SectorFilter {
    bool useRadius;
    double radiusSq;          // unidades^2, horizontal
    bool useSquare;           // cuadrado alineado con la maqueta (ejes u, v en X/Z)
    double half;              // medio lado, unidades
    double u[2], v[2];        // (x, z)
    double center[3];
    bool useCut;
    double cutY;              // Y del juego (hacia abajo): se conserva y > cutY
};
static SectorFilter g_filter = {};
// Caja del nivel (la del area si la hay) y personaje de la ultima pasada,
// para el rango de los cortes con el gatillo. La escribe el hilo del juego
// en cada pasada; los mandos la leen al empezar el gesto. Con g_modelMutex.
struct CutRange {
    bool valid;
    double mn[3];
    double mx[3];
    double center[3];
};
static CutRange g_cutRange = {};
static double g_cutPlaneCam[4] = {0.0, 0.0, 0.0, 0.0};   // nx ny nz d en espacio de camara
static bool g_cutActive = false;
// Corte vertical de la pasada: plano del mundo (nx, 0, nz, d) y en camara; se
// conserva n.p + d > 0. Clase del sector en curso: 0 detras entero, 1 lo
// cruza, 2 delante entero (no se dibuja).
static bool g_vcutActive = false;
static double g_vcutPlaneW[4] = {0.0, 0.0, 0.0, 0.0};
static double g_vcutPlaneCam[4] = {0.0, 0.0, 0.0, 0.0};
static long long g_vcutLastSector = 0;
static int g_vcutClass = 0;
// Corte de la vista (modo 2): el plano solo se aplica a las caras (no suelos)
// de los sectores que toca el tunel entre la camara y el personaje.
static bool g_viewCut = false;
// Cono de la vista (camara en el origen, espacio de camara): lo que se quita
// es lo que esta dentro de la piramide y por delante del plano trasero. Cada
// cara afectada se dibuja una vez por plano, recortada a su lado de fuera; la
// union de esas pasadas es la cara sin el cono (el motor solo recorta contra
// un plano extra por dibujo).
static double g_conePlanes[kConePlaneCount][4] = {};
// Cono redondo (con el nivel en la GPU, que recorta por pixel): eje
// (camara), cos y sin de la apertura y radio en el ojo por el cos.
// g_conePlanes lleva entonces la piramide circunscrita (para "fuera entero"
// y el motor) y g_conePlanesIn la inscrita (para "dentro entero").
static constexpr double kConeRoundScale = 1.05;     // mismo area que la piramide de 6 caras
static bool g_coneRound = false;
static double g_coneAxis[3] = {};
static double g_coneCosA = 1.0, g_coneSinA = 0.0, g_coneEyeRCos = 0.0;
static double g_conePlanesIn[6][4] = {};
static double g_coneGround[4] = {};                 // se conserva lo que queda por debajo (espacio de camara)
static bool g_passFwValid = false;
static int g_tunnelClass = 0;                       // 0 fuera del cono, 1 lo cruza, 2 dentro entero
static int g_mirrorDepth = 0;                       // dentro de +0xf0fd0 (reflejo del agua)
// Reflejo del agua: se dibuja una vez desde cada ojo (como el mundo en el VR
// normal), cada ojo con sus conos exactos (ver HookedMirrorRender). Medio
// ojo de la pasada en unidades del juego (lo que StereoHook desplaza cada
// ojo).
static int g_mirrorEye = -1;                  // ojo del reflejo en curso (0 izquierdo, 1 derecho)
static double g_mirrorEyeShift = 0.0;         // camara del centro -> ojo: x + shift
static double g_passEyeHalf = 0.0;
static double g_passFw[16] = {};                  // fromWorld de la pasada (para personajes y objetos)
static long long g_cutLastSector = 0;             // sector en curso respecto al corte de altura
static int g_cutLastClass = 0;                    // 0 por debajo entero, 1 lo cruza, 2 por encima entero
static double g_tunnelA[3] = {0.0, 0.0, 0.0};     // camara (mundo)
static double g_tunnelB[3] = {0.0, 0.0, 0.0};     // personaje (mundo)
static double g_tunnelRadius = 0.0;               // unidades
static long long g_tunnelLastSector = 0;
static bool g_shadowsSaved = false;               // sombras de objetos apagadas por nosotros
static int g_savedShadowFlag = 0;
static int g_shadowRedrawDepth = 0;               // dentro de +0x9b4b0 (recursiva)
// Nivel en la GPU con sombras: la cara la procesa el motor pero sin dibujar
// la cara iluminada (la dibuja la GPU); solo sus trozos en sombra.
static bool g_shadowOnly = false;
static long long g_shadowLastSector = 0;
static bool g_shadowLastResult = false;
static double g_gpuShadowRadiusM = kGpuShadowRadiusM;   // de esta pasada
// Personajes (jugador y enemigos) en los cortes.
// false (por defecto): siempre a la vista; true: se cortan
// como el resto (pixel a pixel cerca del corte, ocultos mas adentro), con lo
// que llevan enlazado (armas, escudos). Pausa, clic largo del stick derecho
// o R3 mantenido del mando.
static std::atomic<bool> g_cutPersons{false};
static std::atomic<bool> g_pauseWasDown{false};

// Nivel en la GPU (DioramaGpuLevel.cpp): en esta pasada el motor no dibuja
// las caras del nivel (hilo del juego).
static bool g_gpuSkipFaces = false;
// Pasada de captura del nivel para la GPU: sin frustum (recuento de planos
// de la pasada a 0), sin area, corte ni cono, y cada cara del motor se
// guarda.
static bool g_gpuCapturePass = false;
static unsigned int* g_capturePlaneCountPtr = nullptr;
static unsigned int g_capturePlaneCountSaved = 0;
// Lamina de agua en el cono con el nivel en la GPU: sus lotes se marcan y
// StereoHook la dibuja recortada por pixel.
static bool g_waterClipTag = false;
// Lotes del motor (objetos de malla cerca de un corte, caras de sombra y de
// liquido de los sectores que cruza un corte) marcados igual que la lamina
// de agua, con el nivel en la GPU: StereoHook cambia su pixel shader por el
// que descarta lo que quitan los cortes y el cono.
static bool g_clipTag = false;
// Pose del visor con la que se calculo la ultima camara de la maqueta (hilo
// del juego) y la matriz que salio: si la pasada usa esa matriz, la pose va
// con su fotograma y se envia con su imagen (DioramaGpuLevel.cpp).
static float g_camPose[12] = {};
static double g_camPoseFw[16] = {};
static bool g_camPoseValid = false;
// Laminas de agua quitadas por el cono en la pasada anterior y en esta (su
// reflejo, que se dibuja antes, usa la lista de la anterior).
static constexpr int kHiddenWaterMax = 16;
static const void* g_hiddenWaterPrev[kHiddenWaterMax] = {};
static const void* g_hiddenWaterCur[kHiddenWaterMax] = {};
static int g_hiddenWaterPrevCount = 0;
static int g_hiddenWaterCurCount = 0;
// Con el nivel en la GPU, laminas recortadas por pixel (cono o corte
// vertical): su reflejo se dibuja marcado y se recorta por donde el rayo de
// vista cruza el agua (DioramaGpuLevel.cpp). Uno por pasada (un solo plano).
static const void* g_clipWaterPrev[kHiddenWaterMax] = {};
static const void* g_clipWaterCur[kHiddenWaterMax] = {};
static int g_clipWaterPrevCount = 0;
static int g_clipWaterCurCount = 0;
static const void* g_reflClipWater = nullptr;       // el de esta pasada
// Caja de cada masa de agua: union de las cajas de los sectores que la
// dibujan (la de la pasada anterior, completa; ver WaterBodyBox).
struct WaterBox {
    const void* water;
    double mn[3];
    double mx[3];
};
static constexpr int kWaterBoxMax = 32;
static WaterBox g_waterBoxCur[kWaterBoxMax] = {};
static WaterBox g_waterBoxPrev[kWaterBoxMax] = {};
static int g_waterBoxCurCount = 0, g_waterBoxPrevCount = 0;

// --- funciones del juego --------------------------------------------------------
using PFN_SectorsAtPoint = int(__fastcall*)(long long, const double*, int*);
using PFN_EnterNeighbor = void(__fastcall*)(long long, long long, void*, void*);
using PFN_SurfaceDraw = void(__fastcall*)(char*, void*, void*, int);
using PFN_SetAtmosphere = void(__fastcall*)(char*, const double*, unsigned int);
using PFN_VisitPoolGet = char*(__fastcall*)(void*);
using PFN_PlaneSetCopy = void*(__fastcall*)(void*, const void*);
using PFN_VisitQueuePush = void(__fastcall*)(void*, void*);
using PFN_ShadowRedraw = unsigned long long(__fastcall*)(long long, long long, long long);
using PFN_BatchOpen = void(__fastcall*)(int, int);
using PFN_DrawAtmPortals = void(__fastcall*)(long long, long long, long long);
using PFN_MirrorRender = void(__fastcall*)(long long, long long);
using PFN_WaterDraw = void(__fastcall*)(char*, void*, void*, int);
using PFN_LitFace = unsigned long long(__fastcall*)(char*, char*);

static PFN_SectorsAtPoint g_origSectorsAtPoint = nullptr;
static PFN_EnterNeighbor g_origEnterNeighbor = nullptr;
static constexpr int kFaceDrawCount = 4;
static PFN_SurfaceDraw g_origFaceDraw[kFaceDrawCount] = {};
static PFN_SurfaceDraw g_origSkyDraw = nullptr;
static PFN_SetAtmosphere g_origSetAtmosphere = nullptr;
static void** g_atmosphereSlot = nullptr;
static PFN_ShadowRedraw g_origShadowRedraw = nullptr;
static PFN_BatchOpen g_origBatchOpen = nullptr;
static PFN_DrawAtmPortals g_origDrawAtmPortals = nullptr;
static PFN_MirrorRender g_origMirrorRender = nullptr;
static PFN_WaterDraw g_origWaterDraw = nullptr;
static PFN_LitFace g_origLitFace = nullptr;

static constexpr int kHookCount = 14;
static bool g_hooked[kHookCount] = {};
static const uintptr_t kHookOffsets[kHookCount] = {
    kSectorsAtPointOffset, kEnterNeighborOffset, kDrawComplexOffset, kDrawPolygonOffset,
    kDrawPolygonPortalOffset, kDrawPortalOffset, kDrawSkyOffset,
    kShadowRedrawOffset, kBatchOpenOffset, kDrawAtmPortalsOffset, kMirrorRenderOffset, kWaterDrawOffset,
    kLitFaceOffset, kConePlanesOffset };

static Model SnapshotModel() {
    std::lock_guard<std::mutex> lock(g_modelMutex);
    return g_model;
}

// En una cinematica el zoom va entre dos limites para que la vista no se
// salga del margen en que se ve bien en estereo:
// como mucho kCineMaxScale (1:20; recentrada en la camara, lo que esta a
// 4 m queda a 20 cm de los ojos) y como poco kCineZoomInFactor de la escala
// de referencia (al acercar, la vista avanza hacia el pivote y lo que esta
// entre la camara y el pivote se acerca a los ojos). 0 = sin cinematica.
static constexpr double kCineMaxScale = 20.0;
static constexpr double kCineZoomInFactor = 0.6;
static std::atomic<double> g_cineZoomLo{0.0};
static std::atomic<double> g_cineZoomHi{0.0};

static double ClampScale(double k) {
    if (k < kMinScale) k = kMinScale;
    if (k > kMaxScale) k = kMaxScale;
    double hi = g_cineZoomHi.load(std::memory_order_relaxed);
    if (hi > 0.0) {
        double lo = g_cineZoomLo.load(std::memory_order_relaxed);
        if (k > hi) k = hi;
        if (k < lo) k = lo;
    }
    return k;
}

static double WrapPi(double a) {
    while (a > kPi) a -= 2.0 * kPi;
    while (a < -kPi) a += 2.0 * kPi;
    return a;
}

static void RotateY(double v[3], double c, double s) {
    double x = v[0] * c + v[2] * s;
    double z = -v[0] * s + v[2] * c;
    v[0] = x;
    v[2] = z;
}

// Normal del corte vertical en el juego (x, z). Por la espalda gira lo que
// haya girado la maqueta desde que se fijo: queda igual respecto a la vista.
static void VcutDirection(const Model& m, double out[2]) {
    double v[3] = { m.vcutDir[0], 0.0, m.vcutDir[1] };
    if (m.mode == kModeBehind) {
        double d = m.yaw - m.vcutYawRef;
        RotateY(v, std::cos(d), std::sin(d));
    }
    double l = std::sqrt(v[0] * v[0] + v[2] * v[2]);
    if (!(l > 1e-9)) { out[0] = 1.0; out[1] = 0.0; return; }
    out[0] = v[0] / l;
    out[1] = v[2] / l;
}

// Eje del mapa (X o Z) mas cercano a una direccion horizontal.
static void SnapToMapAxis(double d[2]) {
    if (std::abs(d[0]) >= std::abs(d[1])) { d[0] = d[0] >= 0.0 ? 1.0 : -1.0; d[1] = 0.0; }
    else { d[1] = d[1] >= 0.0 ? 1.0 : -1.0; d[0] = 0.0; }
}

// Ejes del mapa como normal del corte vertical (x, z): 0 +X, 1 -X, 2 +Z, 3 -Z.
static const double kMapAxisDir[4][2] = { { 1.0, 0.0 }, { -1.0, 0.0 }, { 0.0, 1.0 }, { 0.0, -1.0 } };
static int NearestMapAxis(const double d[2]) {
    int best = 0;
    double bestDot = -2.0;
    for (int a = 0; a < 4; ++a) {
        double v = kMapAxisDir[a][0] * d[0] + kMapAxisDir[a][1] * d[1];
        if (v > bestDot) { bestDot = v; best = a; }
    }
    return best;
}

// Recorrido del corte vertical con normal (nx, nz) en una caja del nivel, en
// metros del juego por delante del personaje: el plano esta en
// n.p = n.c - v; sin quitar nada con el minimo de n.p en la caja (nearM),
// todo con el maximo (farM).
static void VcutEdges(const CutRange& r, double nx, double nz, double* nearM, double* farM) {
    double c = nx * r.center[0] + nz * r.center[2];
    double x0 = nx * r.mn[0], x1 = nx * r.mx[0], z0 = nz * r.mn[2], z1 = nz * r.mx[2];
    double lo = (x0 < x1 ? x0 : x1) + (z0 < z1 ? z0 : z1);
    double hi = (x0 > x1 ? x0 : x1) + (z0 > z1 ? z0 : z1);
    *nearM = (c - lo) / kUnitsPerMeter;
    *farM = (c - hi) / kUnitsPerMeter;
}

// Corte recto por la espalda (hilo del juego): va por el eje del mapa mas
// cercano a la direccion que gira con la maqueta. Cuando esa direccion se
// aparta mas de 53 grados de su eje (kVcutStraightSwitchCos: a mitad del
// giro, con margen para que no vaya y venga), el corte se retira hasta el
// borde del mapa (deja de cortar) en kVcutStraightMoveS, pasa al eje nuevo
// y entra hasta su sitio en otro tanto; si la maqueta vuelve antes, entra
// otra vez por el mismo lado. En los otros modos, el eje mas cercano.
static constexpr double kVcutStraightSwitchCos = 0.6018;
static constexpr double kVcutStraightMoveS = 0.25;
static int g_vsAxis = -1;              // eje del corte recto (kMapAxisDir); -1 sin fijar
static double g_vsOut = 0.0;           // 0 en su sitio .. 1 retirado del todo
static uint64_t g_vsTick = 0;
static double g_vsKey[3] = {0.0, 0.0, 0.0};   // corte al que corresponde (vcutDir, vcutYawRef)

// ---------------------------------------------------------------------
// Sectores: todos (o los del area y bajo el corte) como visitas con el
// frustum completo
// ---------------------------------------------------------------------

// Distancia horizontal (x, z) al cuadrado de un punto a la caja de un sector.
static double BoxDistanceSqXZ(const double* mn, const double* mx, const double p[3]) {
    double d2 = 0.0;
    const int axes[2] = { 0, 2 };
    for (int k = 0; k < 2; ++k) {
        int i = axes[k];
        double v = p[i];
        if (v < mn[i]) d2 += (mn[i] - v) * (mn[i] - v);
        else if (v > mx[i]) d2 += (v - mx[i]) * (v - mx[i]);
    }
    return d2;
}

// La caja de un sector (en X/Z) toca el cuadrado del area (alineado con la
// maqueta): separacion por ejes (X, Z, u, v).
static bool BoxTouchesSquareXZ(const double* mn, const double* mx, const SectorFilter* f) {
    double bc[2] = { 0.5 * (mn[0] + mx[0]), 0.5 * (mn[2] + mx[2]) };
    double bh[2] = { 0.5 * (mx[0] - mn[0]), 0.5 * (mx[2] - mn[2]) };
    double c[2] = { f->center[0], f->center[2] };
    // Ejes del mundo.
    for (int k = 0; k < 2; ++k) {
        double e = f->half * (std::abs(f->u[k]) + std::abs(f->v[k]));
        if (c[k] - e > bc[k] + bh[k] || c[k] + e < bc[k] - bh[k]) return false;
    }
    // Ejes del cuadrado.
    const double* axes[2] = { f->u, f->v };
    for (int k = 0; k < 2; ++k) {
        const double* a = axes[k];
        double bp = bc[0] * a[0] + bc[1] * a[1];
        double be = bh[0] * std::abs(a[0]) + bh[1] * std::abs(a[1]);
        double cp = c[0] * a[0] + c[1] * a[1];
        if (cp - f->half > bp + be || cp + f->half < bp - be) return false;
    }
    return true;
}

// El segmento a-b pasa a menos de r de la caja (caja agrandada r, prueba de
// losas). Para el tunel del corte de la vista.
static bool SegmentNearBox(const double* mn, const double* mx, const double a[3], const double b[3], double r) {
    double t0 = 0.0, t1 = 1.0;
    for (int k = 0; k < 3; ++k) {
        double lo = mn[k] - r, hi = mx[k] + r;
        double d = b[k] - a[k];
        if (std::abs(d) < 1e-9) {
            if (a[k] < lo || a[k] > hi) return false;
            continue;
        }
        double ta = (lo - a[k]) / d, tb = (hi - a[k]) / d;
        if (ta > tb) { double t = ta; ta = tb; tb = t; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return false;
    }
    return true;
}

// Esquinas de una caja del mundo en espacio de camara (c = p * M + t).
static void BoxCornersCamera(const double* mn, const double* mx, double c[8][3]) {
    for (int k = 0; k < 8; ++k) {
        double p[3] = { (k & 1) ? mx[0] : mn[0], (k & 2) ? mx[1] : mn[1], (k & 4) ? mx[2] : mn[2] };
        for (int j = 0; j < 3; ++j) {
            c[k][j] = p[0] * g_passFw[0 * 4 + j] + p[1] * g_passFw[1 * 4 + j] + p[2] * g_passFw[2 * 4 + j] + g_passFw[12 + j];
        }
    }
}

// Donde queda un sector respecto a lo que quita el cono (dentro de los 7
// planos del cono y por encima del plano del suelo): 0 nada (se dibuja
// entero, una vez), 1 en parte (una pasada por plano), 2 todo (no se dibuja).
static int BoxConeClass(const double* mn, const double* mx) {
    if (!SegmentNearBox(mn, mx, g_tunnelA, g_tunnelB, g_tunnelRadius)) return 0;
    double c[8][3];
    BoxCornersCamera(mn, mx, c);
    bool allInside = true;
    for (int i = 0; i <= kConePlaneCount; ++i) {
        const double* m = i < kConePlaneCount ? g_conePlanes[i] : g_coneGround;
        int out = 0;
        for (int k = 0; k < 8; ++k) {
            if (m[0] * c[k][0] + m[1] * c[k][1] + m[2] * c[k][2] + m[3] > 0.0) ++out;
        }
        if (out == 8) return 0;
        if (out > 0) allInside = false;
    }
    // Redondo: entero dentro solo si esta dentro de la piramide inscrita.
    if (g_coneRound) {
        for (int i = 0; i < 6 && allInside; ++i) {
            const double* m = g_conePlanesIn[i];
            for (int k = 0; k < 8; ++k) {
                if (m[0] * c[k][0] + m[1] * c[k][1] + m[2] * c[k][2] + m[3] > 0.0) { allInside = false; break; }
            }
        }
    }
    return allInside ? 2 : 1;
}

static int SectorConeClass(long long sector) {
    __try {
        if (!sector) return 0;
        const double* mn = reinterpret_cast<const double*>(sector + kSectorBoxMin);
        const double* mx = reinterpret_cast<const double*>(sector + kSectorBoxMax);
        return BoxConeClass(mn, mx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// Lamina de agua dentro del cono: su caja en X/Z (la de toda el agua si se
// conoce, si no la del sector que se esta dibujando), a la altura del agua.
// Por debajo del plano del suelo del cono nunca (el agua por la que va el
// personaje se queda).
static bool WaterInViewCone(const char* water, const double* bodyMn, const double* bodyMx) {
    if (!g_viewCut || !g_passFwValid) return false;
    __try {
        double h = *reinterpret_cast<const double*>(water + kWaterSurfaceHeight);
        if (!(h == h) || h > 1e8 || h < -1e8) return false;
        const double* mn = bodyMn;
        const double* mx = bodyMx;
        if (!mn || !mx) {
            long long sector = *reinterpret_cast<long long*>(g_base + kCurrentSectorPtr);
            if (!sector) return false;
            mn = reinterpret_cast<const double*>(sector + kSectorBoxMin);
            mx = reinterpret_cast<const double*>(sector + kSectorBoxMax);
        }
        double bmn[3] = { mn[0], h - 1.0, mn[2] };
        double bmx[3] = { mx[0], h + 1.0, mx[2] };
        return BoxConeClass(bmn, bmx) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Sector entero por debajo del corte de altura (no le hace falta el plano).
static bool SectorBelowCut(long long sector) {
    __try {
        if (!sector) return false;
        const double* mn = reinterpret_cast<const double*>(sector + kSectorBoxMin);
        return mn[1] > g_filter.cutY + 1.0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Corte de altura: 0 = sector entero por debajo (se conserva), 1 = lo cruza,
// 2 = entero por encima (se quita).
static int SectorCutClass(long long sector) {
    __try {
        if (!sector) return 1;
        const double* mn = reinterpret_cast<const double*>(sector + kSectorBoxMin);
        const double* mx = reinterpret_cast<const double*>(sector + kSectorBoxMax);
        if (mx[1] < g_filter.cutY) return 2;
        if (mn[1] > g_filter.cutY + 1.0) return 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// Corte vertical: 0 = caja entera detras del plano (se conserva), 1 = lo
// cruza, 2 = entera delante (se quita). El plano es vertical: basta X/Z.
static int BoxVcutClass(const double* mn, const double* mx) {
    double lo = 1e300, hi = -1e300;
    for (int i = 0; i < 4; ++i) {
        double x = (i & 1) ? mx[0] : mn[0];
        double z = (i & 2) ? mx[2] : mn[2];
        double e = g_vcutPlaneW[0] * x + g_vcutPlaneW[2] * z + g_vcutPlaneW[3];
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    if (lo > 0.0) return 0;
    if (hi <= 0.0) return 2;
    return 1;
}

static int SectorVcutClass(long long sector) {
    __try {
        if (!sector) return 1;
        const double* mn = reinterpret_cast<const double*>(sector + kSectorBoxMin);
        const double* mx = reinterpret_cast<const double*>(sector + kSectorBoxMax);
        return BoxVcutClass(mn, mx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// Una visita nueva para 'sector' en la cola del motor, como las que crea el
// recorrido de portales (+0x7fc90) pero con el frustum completo y enlazada a
// su sector como las iniciales de B_Map::Render. El motor la saca de la cola
// despues de las iniciales y la procesa igual (+0x802c0 -> +0x86bb0). Sin
// destructores: se llama dentro de __try.
static bool QueueSectorVisit(char* sector) {
    PFN_VisitPoolGet poolGet = reinterpret_cast<PFN_VisitPoolGet>(g_base + kVisitPoolGetOffset);
    PFN_PlaneSetCopy planeCopy = reinterpret_cast<PFN_PlaneSetCopy>(g_base + kPlaneSetCopyOffset);
    PFN_VisitQueuePush queuePush = reinterpret_cast<PFN_VisitQueuePush>(g_base + kVisitQueuePushOffset);
    char* v = poolGet(reinterpret_cast<void*>(g_base + kVisitPoolOffset));
    if (!v) return false;
    *reinterpret_cast<double*>(v + kVisitSortKey) = 0.0;
    *reinterpret_cast<int*>(v + kVisitFlag) = 0;
    *reinterpret_cast<int*>(v + kVisitZone) = *reinterpret_cast<int*>(sector + kSectorZone);
    *reinterpret_cast<uintptr_t*>(v + kVisitZonePlane) = g_base + kDefaultZonePlaneOffset;
    *reinterpret_cast<char**>(v + kVisitSector) = sector;
    planeCopy(v + kVisitPlanes, g_renderPlanes);
    // Nodo de la visita en la lista del sector: detras de la cabeza.
    char* node = v + kVisitSectorNode;
    char* head = sector + kSectorVisitList;
    if (node != head) {
        char* next = *reinterpret_cast<char**>(head + 8);
        *reinterpret_cast<char**>(node + 8) = next;
        *reinterpret_cast<char**>(node + 0x10) = head;
        *reinterpret_cast<char**>(head + 8) = node;
        if (next) *reinterpret_cast<char**>(next + 0x10) = node;
    }
    queuePush(reinterpret_cast<void*>(g_base + kVisitQueueOffset), v);
    return true;
}

// Rellena la lista de salida de +0x7aa90 (recuento en el primer int,
// punteros desde +8) con los primeros kMaxStartSectors sectores elegidos y
// encola el resto. Sin destructores: __try.
static bool FillDioramaSectorsRaw(long long level, int* out, const SectorFilter* f) {
    __try {
        if (!level || !out || !g_renderPlanes) return false;
        unsigned int count = *reinterpret_cast<unsigned int*>(level + kLevelSectorCount);
        char** sectors = *reinterpret_cast<char***>(level + kLevelSectorArray);
        if (!sectors || count == 0 || count > 200000) return false;
        char** entries = reinterpret_cast<char**>(reinterpret_cast<char*>(out) + 8);
        int n = 0;
        out[0] = 0;
        for (unsigned int i = 0; i < count; ++i) {
            char* s = sectors[i];
            if (!s) continue;
            const double* mn = reinterpret_cast<const double*>(s + kSectorBoxMin);
            const double* mx = reinterpret_cast<const double*>(s + kSectorBoxMax);
            if (f->useRadius && BoxDistanceSqXZ(mn, mx, f->center) > f->radiusSq) continue;
            if (f->useSquare && !BoxTouchesSquareXZ(mn, mx, f)) continue;
            // (El corte de altura no quita sectores enteros: sus caras se
            // recortan y asi personajes y objetos siguen a la vista.)
            if (n < kMaxStartSectors) {
                entries[n] = s;
                ++n;
                out[0] = n;
            } else {
                QueueSectorVisit(s);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static int __fastcall HookedSectorsAtPoint(long long level, const double* point, int* out) {
    if (g_worldActive && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_base + kRenderQueryReturn) {
        FillDioramaSectorsRaw(level, out, &g_filter);
        if (out && out[0] > 0) return 1;
        // Nada que dibujar (area sin ningun sector): sigue el juego, que
        // buscara el sector de la camara (fuera del nivel: no dibuja).
    }
    return g_origSectorsAtPoint(level, point, out);
}

// Sin recorrido de portales en diorama: cada sector ya tiene su visita (los
// de agua incluidos). Dentro del reflejo del agua el recorrido es el del
// juego. El motor no mira si un sector ya se visito (acaba porque cada
// portal estrecha el cono); por seguridad hay un tope de entradas por
// sector y por reflejo.
static constexpr int kMirrorEntrySlots = 4096;                  // potencia de 2
static constexpr uint32_t kMirrorSectorEntriesMax = 16;
static constexpr uint32_t kMirrorEntriesMax = 2048;
struct MirrorEntry {
    long long sector;
    uint32_t gen;
    uint32_t count;
};
static MirrorEntry g_mirrorEntries[kMirrorEntrySlots];
static uint32_t g_mirrorGen = 0;
static uint32_t g_mirrorEntriesTotal = 0;

static void BeginMirrorEntries() {
    ++g_mirrorGen;
    if (g_mirrorGen == 0) {
        std::memset(g_mirrorEntries, 0, sizeof(g_mirrorEntries));
        g_mirrorGen = 1;
    }
    g_mirrorEntriesTotal = 0;
}

static bool MirrorEntryAllowed(long long sector) {
    if (g_mirrorEntriesTotal >= kMirrorEntriesMax) return false;
    uint64_t h = static_cast<uint64_t>(sector);
    h ^= h >> 17;
    h *= 0x9E3779B97F4A7C15ull;
    uint32_t i = static_cast<uint32_t>(h >> 40);
    for (int probe = 0; probe < 64; ++probe) {
        MirrorEntry& e = g_mirrorEntries[(i + static_cast<uint32_t>(probe)) & (kMirrorEntrySlots - 1)];
        if (e.gen != g_mirrorGen) {
            e.gen = g_mirrorGen;
            e.sector = sector;
            e.count = 1;
            ++g_mirrorEntriesTotal;
            return true;
        }
        if (e.sector == sector) {
            if (e.count >= kMirrorSectorEntriesMax) return false;
            ++e.count;
            ++g_mirrorEntriesTotal;
            return true;
        }
    }
    ++g_mirrorEntriesTotal;
    return true;
}

static void __fastcall HookedEnterNeighbor(long long sector, long long polygon, void* fromWorld, void* plane) {
    // Dentro del reflejo del agua el recorrido de portales es el del juego:
    // sin el, el reflejo salia a trozos ("plataformas" bajo el agua).
    if (g_worldActive && g_mirrorDepth == 0) return;
    if (g_worldActive && g_mirrorDepth > 0 && !MirrorEntryAllowed(sector)) return;
    g_origEnterNeighbor(sector, polygon, fromWorld, plane);
}

// ---------------------------------------------------------------------
// Caras por los dos lados, cortes, cono y sin cielo
// ---------------------------------------------------------------------

// Sombras con el nivel en la GPU: un sector las puede recibir si alguna luz
// de sus conos proyecta sombras (luz+0x10) y hay entidades en el sector de
// ese cono o en el de alguno de sus descendientes: +0x8fb00, de donde el
// motor saca los objetos que hacen sombra, se llama a si mismo con cada hijo
// (+0xb8 / +0xc0) y junta sus listas, asi que es todo el arbol de conos.
static constexpr int kShadowConeBudget = 512;   // conos mirados por sector como mucho
static bool SectorReceivesShadowsRaw(long long sector) {
    __try {
        const char* stack[kShadowConeBudget];
        const char* node = *reinterpret_cast<const char* const*>(sector + kSectorLightCones);
        for (int k = 0; k < 64 && node; ++k, node = *reinterpret_cast<const char* const*>(node + 8)) {
            const char* cone = *reinterpret_cast<const char* const*>(node + 0x18);
            if (!cone) continue;
            const char* light = *reinterpret_cast<const char* const*>(cone + kConeLight);
            if (!light || *reinterpret_cast<const int*>(light + kLightCastsShadows) == 0) continue;
            int top = 0, visited = 0;
            stack[top++] = cone;
            while (top > 0 && visited < kShadowConeBudget) {
                const char* c = stack[--top];
                ++visited;
                const char* sec = *reinterpret_cast<const char* const*>(c + kConeSector);
                if (sec && *reinterpret_cast<const void* const*>(sec + kSectorEntities)) return true;
                const char* const* kids = *reinterpret_cast<const char* const* const*>(c + kConeChildData);
                unsigned nk = *reinterpret_cast<const unsigned*>(c + kConeChildCount);
                for (unsigned j = 0; kids && j < nk && top < kShadowConeBudget; ++j) {
                    if (kids[j]) stack[top++] = kids[j];
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

static bool SectorNearCharacterRaw(long long sector) {
    if (!g_haveCharPos) return true;
    __try {
        const double* mn = reinterpret_cast<const double*>(sector + kSectorBoxMin);
        const double* mx = reinterpret_cast<const double*>(sector + kSectorBoxMax);
        double r = g_gpuShadowRadiusM * kUnitsPerMeter;
        if (BoxDistanceSqXZ(mn, mx, g_charPos) <= r * r) return true;
        // Cinematica: tambien alrededor de lo que mira su camara (ver g_cineFocus).
        return g_haveCineFocus && BoxDistanceSqXZ(mn, mx, g_cineFocus) <= r * r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool GpuShadowsForCurrentSector() {
    long long sector = *reinterpret_cast<long long*>(g_base + kCurrentSectorPtr);
    if (!sector) return false;
    if (sector != g_shadowLastSector) {
        g_shadowLastSector = sector;
        g_shadowLastResult = SectorNearCharacterRaw(sector) && SectorReceivesShadowsRaw(sector);
    }
    return g_shadowLastResult;
}

struct ShadowOnlyScope {
    bool prev;
    explicit ShadowOnlyScope(bool on) : prev(g_shadowOnly) { g_shadowOnly = on; }
    ~ShadowOnlyScope() { g_shadowOnly = prev; }
};

// La cara iluminada (raster+0x208): con el nivel en la GPU y sombras, el
// motor procesa la cara (conos de luz, sombras de cada luz) pero la cara
// entera no se dibuja; los trozos en sombra (dentro de +0x9b4b0) si.
static unsigned long long __fastcall HookedLitFace(char* raster, char* polygon) {
    if (g_shadowOnly && g_shadowRedrawDepth == 0) return 0;
    return g_origLitFace(raster, polygon);
}

template <int I>
static void __fastcall HookedFaceDraw(char* surface, void* fromWorld, void* planes, int frame) {
    if (!g_worldActive) {
        g_origFaceDraw[I](surface, fromWorld, planes, frame);
        return;
    }
    // Reflejo del agua: como en el juego (su camara es la reflejada; ni
    // corte, ni cono, ni caras por los dos lados).
    if (g_mirrorDepth > 0) {
        if (g_threshold) *g_threshold = kKeepFaceThreshold;
        g_origFaceDraw[I](surface, fromWorld, planes, frame);
        return;
    }
    // Pasada de captura (se guarda lo que dibuja el motor, por los dos lados
    // y sin recortes propios) o el nivel lo dibuja la GPU (menos las caras
    // que cambian durante la partida, que no se capturan: liquido que corre,
    // puertas, portales hacia sectores cerrados).
    if (g_gpuCapturePass) {
        if (g_threshold) *g_threshold = kAlwaysDrawThreshold;
        GpuLevelCaptureSurface(surface, I);
        g_origFaceDraw[I](surface, fromWorld, planes, frame);
        GpuLevelCaptureSurfaceDone();
        if (g_threshold) *g_threshold = kKeepFaceThreshold;
        return;
    }
    // Nivel en la GPU: el motor no dibuja estas caras, salvo para poner las
    // sombras de los objetos en los sectores que las reciben (sin la cara
    // iluminada: HookedLitFace), con el mismo corte y cono que siempre.
    bool shadowOnly = false;
    if (g_gpuSkipFaces && GpuLevelSkipsSurface(surface)) {
        if (!GpuShadowsForCurrentSector()) return;
        shadowOnly = true;
    }
    ShadowOnlyScope shadowScope(shadowOnly);
    // Nivel en la GPU (aqui solo llegan caras de sombra y las no
    // capturadas): en los sectores que cruza algun corte sus lotes van
    // marcados y se recortan pixel a pixel con todos (altura, vertical y
    // cono; con el cono, en una sola pasada); el plano del motor se sigue
    // poniendo si esta libre.
    bool pixelClip = false;
    // Corte vertical: los sectores enteros delante del plano no se dibujan;
    // los que lo cruzan llevan el plano (si el unico plano extra esta libre).
    if (g_vcutActive) {
        long long vs = *reinterpret_cast<long long*>(g_base + kCurrentSectorPtr);
        if (vs != g_vcutLastSector) {
            g_vcutLastSector = vs;
            g_vcutClass = SectorVcutClass(vs);
        }
        if (g_vcutClass == 2) return;
        if (g_vcutClass == 1 && g_gpuSkipFaces) pixelClip = true;
    }
    // Todas las caras por los dos lados (techos incluidos).
    if (g_threshold) *g_threshold = kAlwaysDrawThreshold;
    // Un solo plano extra por dibujo (lo que admite el motor). Los sectores
    // con agua traen su plano (el de la superficie): el corte no se les puede
    // aplicar y, enteros por encima del corte, no se dibujan. El resto se
    // recorta cara a cara. El cono de vision (si esta encendido) va en los
    // sectores que cruza (tambien los de agua: en esas pasadas el plano del
    // agua se sustituye), con una pasada por plano: los 7 del cono y el del
    // suelo del personaje. Los que quedan dentro enteros no se dibujan. El
    // resto lleva el corte.
    void** clipSlot = reinterpret_cast<void**>(g_base + kFaceClipPlanePtr);
    void* engineClip = *clipSlot;
    long long sector = *reinterpret_cast<long long*>(g_base + kCurrentSectorPtr);
    if (g_cutActive && (engineClip != nullptr || g_gpuSkipFaces)) {
        if (sector != g_cutLastSector) {
            g_cutLastSector = sector;
            g_cutLastClass = SectorCutClass(sector);
        }
        if (g_cutLastClass == 2) {
            if (g_threshold) *g_threshold = kKeepFaceThreshold;
            return;
        }
        if (g_cutLastClass == 1 && g_gpuSkipFaces) pixelClip = true;
    }
    if (g_viewCut) {
        if (sector != g_tunnelLastSector) {
            g_tunnelLastSector = sector;
            g_tunnelClass = SectorConeClass(sector);
        }
        if (g_tunnelClass == 2) {
            if (g_threshold) *g_threshold = kKeepFaceThreshold;
            return;
        }
        if (g_tunnelClass == 1 && g_gpuSkipFaces) {
            pixelClip = true;
        } else if (g_tunnelClass == 1) {
            for (int p = 0; p <= kConePlaneCount; ++p) {
                *clipSlot = p < kConePlaneCount ? g_conePlanes[p] : g_coneGround;
                g_origFaceDraw[I](surface, fromWorld, planes, frame);
            }
            *clipSlot = engineClip;
            if (g_threshold) *g_threshold = kKeepFaceThreshold;
            return;
        }
    }
    double* plane = nullptr;
    if (engineClip == nullptr) {
        // Un solo plano: el del corte de altura o el vertical (el que haga
        // falta; si hacen falta los dos, el de altura).
        bool needCut = g_cutActive;
        bool needV = g_vcutActive && g_vcutClass == 1;
        if (needCut && needV && SectorBelowCut(sector)) needCut = false;
        if (needCut) plane = g_cutPlaneCam;
        else if (needV) plane = g_vcutPlaneCam;
    }
    bool prevTag = g_clipTag;
    if (pixelClip) g_clipTag = true;
    if (plane) *clipSlot = plane;
    g_origFaceDraw[I](surface, fromWorld, planes, frame);
    if (plane) *clipSlot = nullptr;
    g_clipTag = prevTag;
    if (g_threshold) *g_threshold = kKeepFaceThreshold;
}

static void __fastcall HookedSkyDraw(char* surface, void* fromWorld, void* planes, int frame) {
    if (g_worldActive) return;
    g_origSkyDraw(surface, fromWorld, planes, frame);
}

// ---------------------------------------------------------------------
// Sombras en la maqueta
//
// +0x9a950 parte cada cara por los volumenes de sombra y la dibuja entera
// con todas las luces; despues +0x9b4b0 vuelve a dibujar ENCIMA, sin la luz
// que la tapa, los trozos en sombra (+0x93ef0 / +0x942d0 -> raster+0x220).
// Son poligonos nuevos (recortados) en el plano de la cara, con el mismo
// estado que el nivel (DepthFunc ALWAYS con escritura). Con ALWAYS ganan por
// orden; con el LESS_EQUAL de la maqueta su profundidad sale casi igual que
// la de la cara y pierden a ratos: las lineas que parpadeaban.
// Mientras se dibujan esos trozos, cada lote que abre el raster (+0x74c030,
// por donde pasan todas sus primitivas) sale sin escritura de profundidad:
// eso fuerza un lote aparte (el estado forma parte de la comparacion para
// juntar lotes) y StereoHook lo trata como calcomania (LESS_EQUAL con un
// pequeno sesgo hacia la camara), asi que queda siempre encima de su cara.
// ---------------------------------------------------------------------
static unsigned long long __fastcall HookedShadowRedraw(long long face, long long polygon, long long node) {
    ++g_shadowRedrawDepth;
    unsigned long long r = g_origShadowRedraw(face, polygon, node);
    --g_shadowRedrawDepth;
    return r;
}

static bool RasterInWaterMode() {
    uintptr_t raster = *reinterpret_cast<uintptr_t*>(g_base + kRasterPtrOffset);
    return raster && *reinterpret_cast<const int*>(raster + kRasterWaterMode) != 0;
}

static void __fastcall HookedBatchOpen(int mode, int count) {
    if (g_worldActive) {
        uint64_t* state = reinterpret_cast<uint64_t*>(g_base + kRasterBgfxState);
        uint64_t s = *state;
        uint64_t t = s;
        if (g_shadowRedrawDepth > 0 && (s & kBgfxWriteZ) != 0 && (s & kBgfxDepthTestMask) == kBgfxDepthTestAlways) {
            t &= ~kBgfxWriteZ;
        }
        // Lamina de agua recortada por pixel (+0xf0be0 dibuja con el raster en
        // modo agua) y lotes del motor cerca de un corte: marcados para que
        // StereoHook los recorte.
        bool waterTag = g_waterClipTag && RasterInWaterMode();
        if (waterTag || g_clipTag) {
            t |= kBgfxWaterTag;
            if (!waterTag) t |= kBgfxNoReflTag;
        }
        if (g_mirrorDepth > 0 && g_mirrorEye >= 0) t |= g_mirrorEye == 0 ? kBgfxMirrorLeftTag : kBgfxMirrorRightTag;
        if (t != s) {
            *state = t;
            g_origBatchOpen(mode, count);
            *state = s;
            return;
        }
    }
    g_origBatchOpen(mode, count);
}

// Reflejos del agua: +0x82080 (desde B_Map::Render, antes de dibujar las
// visitas y con el raster en "modo espejo", raster+0x64) llama a +0xf0fd0
// por cada superficie espejo apuntada en el fotograma, y dibuja el recinto
// reflejado directamente en la imagen, bajo el plano del agua, con el
// recorrido de portales del juego dentro (HookedEnterNeighbor) y las caras
// como en el juego (HookedFaceDraw). Sin el si el corte deja el agua por
// encima o el cono / corte vertical quitan su lamina.
static bool MirrorWaterAboveCut(long long mirror) {
    __try {
        double h = *reinterpret_cast<const double*>(mirror - kMirrorToWater + kWaterSurfaceHeight);
        return h == h && h < g_filter.cutY;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void AddHiddenWater(const void* water) {
    for (int i = 0; i < g_hiddenWaterCurCount; ++i) {
        if (g_hiddenWaterCur[i] == water) return;
    }
    if (g_hiddenWaterCurCount < kHiddenWaterMax) g_hiddenWaterCur[g_hiddenWaterCurCount++] = water;
}

static void AddClipWater(const void* water) {
    for (int i = 0; i < g_clipWaterCurCount; ++i) {
        if (g_clipWaterCur[i] == water) return;
    }
    if (g_clipWaterCurCount < kHiddenWaterMax) g_clipWaterCur[g_clipWaterCurCount++] = water;
}

static bool MirrorWaterClipped(long long mirror) {
    const void* water = reinterpret_cast<const void*>(mirror - kMirrorToWater);
    for (int i = 0; i < g_clipWaterPrevCount; ++i) {
        if (g_clipWaterPrev[i] == water) return true;
    }
    return false;
}

static bool MirrorWaterHeight(long long mirror, double* y) {
    __try {
        double h = *reinterpret_cast<const double*>(mirror - kMirrorToWater + kWaterSurfaceHeight);
        if (!(h == h) || h > 1e8 || h < -1e8) return false;
        *y = h;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Caja del sector en curso (el de la lamina que se esta dibujando). Sin
// destructores: __try.
static bool CurrentSectorBoxRaw(double mn[3], double mx[3]) {
    __try {
        long long sector = *reinterpret_cast<long long*>(g_base + kCurrentSectorPtr);
        if (!sector) return false;
        const double* a = reinterpret_cast<const double*>(sector + kSectorBoxMin);
        const double* b = reinterpret_cast<const double*>(sector + kSectorBoxMax);
        for (int k = 0; k < 3; ++k) {
            if (!(a[k] == a[k]) || !(b[k] == b[k]) || a[k] > b[k] || a[k] < -1e8 || b[k] > 1e8) return false;
            mn[k] = a[k];
            mx[k] = b[k];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static void RecordWaterBox(const void* water) {
    double mn[3], mx[3];
    if (!CurrentSectorBoxRaw(mn, mx)) return;
    for (int i = 0; i < g_waterBoxCurCount; ++i) {
        WaterBox& w = g_waterBoxCur[i];
        if (w.water != water) continue;
        for (int k = 0; k < 3; ++k) {
            if (mn[k] < w.mn[k]) w.mn[k] = mn[k];
            if (mx[k] > w.mx[k]) w.mx[k] = mx[k];
        }
        return;
    }
    if (g_waterBoxCurCount >= kWaterBoxMax) return;
    WaterBox& w = g_waterBoxCur[g_waterBoxCurCount++];
    w.water = water;
    for (int k = 0; k < 3; ++k) {
        w.mn[k] = mn[k];
        w.mx[k] = mx[k];
    }
}

// Caja de toda la masa de agua: la union de las cajas de los sectores que
// la dibujan (la de la pasada anterior, completa; si no, lo que lleva esta).
// El motor dibuja la lamina UNA vez por fotograma, con los poligonos de
// todos sus sectores, desde el primero que la pide.
static bool WaterBodyBox(const void* water, double mn[3], double mx[3]) {
    for (int pass = 0; pass < 2; ++pass) {
        const WaterBox* list = pass == 0 ? g_waterBoxPrev : g_waterBoxCur;
        int n = pass == 0 ? g_waterBoxPrevCount : g_waterBoxCurCount;
        for (int i = 0; i < n; ++i) {
            if (list[i].water != water) continue;
            for (int k = 0; k < 3; ++k) {
                mn[k] = list[i].mn[k];
                mx[k] = list[i].mx[k];
            }
            return true;
        }
    }
    return false;
}

static bool MirrorWaterHiddenByCone(long long mirror) {
    const void* water = reinterpret_cast<const void*>(mirror - kMirrorToWater);
    for (int i = 0; i < g_hiddenWaterPrevCount; ++i) {
        if (g_hiddenWaterPrev[i] == water) return true;
    }
    return false;
}

// Matriz de 16 doubles (sin destructores: __try).
static bool ReadMatrix16Raw(long long src, double out[16]) {
    __try {
        if (!src) return false;
        const double* m = reinterpret_cast<const double*>(src);
        for (int i = 0; i < 16; ++i) {
            if (!(m[i] == m[i])) return false;
            out[i] = m[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static void __fastcall HookedMirrorRender(long long mirror, long long view) {
    if (g_worldActive) {
        if ((g_cutActive && MirrorWaterAboveCut(mirror)) ||
            ((g_viewCut || g_vcutActive) && MirrorWaterHiddenByCone(mirror))) {
            return;
        }
        // Nivel en la GPU y su lamina recortada por pixel (cono o corte
        // vertical, pasada anterior): el reflejo va marcado y se recorta por
        // donde el rayo de vista cruza el agua. Solo uno por pasada; otro
        // reflejo de agua recortada no se dibuja.
        bool clip = false;
        if (g_gpuSkipFaces && (g_viewCut || g_vcutActive) && MirrorWaterClipped(mirror)) {
            const void* water = reinterpret_cast<const void*>(mirror - kMirrorToWater);
            double y = 0.0;
            if ((g_reflClipWater && g_reflClipWater != water) || !MirrorWaterHeight(mirror, &y)) return;
            if (!g_reflClipWater) {
                g_reflClipWater = water;
                GpuLevelSetReflectionPlane(y);
            }
            clip = true;
        }
        if (g_mirrorDepth == 0) BeginMirrorEntries();
        bool prevTag = g_waterClipTag;
        if (clip) g_waterClipTag = true;
        // Un reflejo por ojo: 'view' es la fromWorld de la pasada
        // (B_Map::Render: pose+0x98). Cada ojo la recibe movida medio ojo por
        // su eje x (como las pasadas por ojo del VR normal) y el motor calcula
        // sus conos desde ese ojo: sin grietas a ninguna distancia y sin nada
        // fuera del agua (desde el centro de la cabeza, cada ojo veia un poco
        // mas alla de los conos). Sus lotes van marcados con el ojo.
        double eyeFw[2][16];
        bool perEye = g_mirrorDepth == 0 && g_passEyeHalf > 0.0 && ReadMatrix16Raw(view, eyeFw[0]);
        if (perEye) {
            for (int k = 0; k < 16; ++k) eyeFw[1][k] = eyeFw[0][k];
            for (int eye = 0; eye < 2; ++eye) {
                // Ojo izquierdo en x = -medio ojo de la camara: p_ojo = p + medio ojo.
                double shift = eye == 0 ? g_passEyeHalf : -g_passEyeHalf;
                eyeFw[eye][12] += shift;
                g_mirrorEye = eye;
                g_mirrorEyeShift = shift;
                BeginMirrorEntries();
                ++g_mirrorDepth;
                g_origMirrorRender(mirror, reinterpret_cast<long long>(eyeFw[eye]));
                --g_mirrorDepth;
            }
            g_mirrorEye = -1;
            g_mirrorEyeShift = 0.0;
        } else {
            ++g_mirrorDepth;
            g_origMirrorRender(mirror, view);
            --g_mirrorDepth;
        }
        g_waterClipTag = prevTag;
        return;
    }
    g_origMirrorRender(mirror, view);
}

// Superficie del agua por encima del corte de altura: no se dibuja (la
// lamina va aparte de las caras).
static bool WaterAboveCut(const char* water, double* y) {
    __try {
        double h = *reinterpret_cast<const double*>(water + kWaterSurfaceHeight);
        if (!(h == h) || h > 1e8 || h < -1e8) return false;
        *y = h;
        return h < g_filter.cutY;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void __fastcall HookedWaterDraw(char* water, void* a2, void* a3, int frame) {
    if (!g_worldActive) {
        g_origWaterDraw(water, a2, a3, frame);
        return;
    }
    // Cono de vision: la lamina va directa al raster (sin el recortador de
    // caras). Con el nivel dibujado por el motor se quita entera si esta
    // dentro, y su reflejo tambien (lista de la pasada anterior); con el
    // nivel en la GPU sus lotes van marcados y StereoHook la dibuja con un
    // pixel shader que descarta lo que cae dentro del cono, y su reflejo se
    // recorta igual (HookedMirrorRender).
    bool clipTag = false;
    // La lamina es de toda el agua (el motor la dibuja una vez, con los
    // poligonos de todos sus sectores, desde el primero que la pide): se
    // decide con la caja de toda el agua, no con la del sector en curso.
    double bodyMn[3], bodyMx[3];
    bool haveBody = false;
    if (g_mirrorDepth == 0 && water) {
        RecordWaterBox(water);
        haveBody = WaterBodyBox(water, bodyMn, bodyMx);
    }
    // Corte vertical: la lamina entera delante del plano fuera (con su
    // reflejo); si lo cruza, como con el cono.
    if (g_mirrorDepth == 0 && water && g_vcutActive) {
        long long ws = *reinterpret_cast<long long*>(g_base + kCurrentSectorPtr);
        int vc = haveBody ? BoxVcutClass(bodyMn, bodyMx) : SectorVcutClass(ws);
        if (vc == 2) {
            AddHiddenWater(water);
            return;
        }
        if (vc == 1) {
            if (g_gpuSkipFaces) {
                clipTag = true;
                AddClipWater(water);
            } else {
                AddHiddenWater(water);
            }
        }
    }
    if (g_mirrorDepth == 0 && water &&
        WaterInViewCone(water, haveBody ? bodyMn : nullptr, haveBody ? bodyMx : nullptr)) {
        if (!g_gpuSkipFaces) {
            AddHiddenWater(water);
            return;
        }
        clipTag = true;
        AddClipWater(water);
    }
    if (g_cutActive && water) {
        double y = 0.0;
        if (WaterAboveCut(water, &y)) return;
    }
    bool prevTag = g_waterClipTag;
    g_waterClipTag = clipTag || prevTag;
    g_origWaterDraw(water, a2, a3, frame);
    g_waterClipTag = prevTag;
}

// ---------------------------------------------------------------------
// Personajes y objetos con el corte y el cono
//
// Las entidades no son caras del nivel y el recortador no las toca. Se
// dibujan en +0x817a0: por cada sector visitado recorre su lista
// (sector+0x250, nodo+0x18 = entidad) y pregunta vtable[27] (+0xd8, tiene
// parte opaca) y vtable[28] (+0xe0, tiene parte translucida); las que dicen
// que si van a la lista de dibujo (posicion: vtable[8] devuelve su matriz de
// mundo, traslacion en +0x60) y se dibujan con vtable[29]/[30]. En diorama,
// con el corte de altura o el cono puestos, esas dos preguntas contestan "no"
// para los objetos (no personas) que quedan por encima del corte o dentro
// del cono: solo quedan a la vista los personajes (B_PersonEntity) y lo que
// va enlazado a ellos (armas en la mano, escudos, antorchas; B_Entity+0x168).
// Se cambian las entradas de las vtables de esas clases (no las funciones,
// que comparten varias). Son todas las clases con dibujo propio salvo las de
// red: objetos, armas sueltas (sillas, copas, lamparas...), actores,
// fisicos, calcomanias, fuegos, charcos, flechas, particulas, focos, auras,
// chispas, estelas, rayos, proyectiles magicos y, si se pide cortarlos
// tambien, los personajes.
//
// Con el nivel en la GPU, los objetos de malla (objetos, armas, actores,
// fisicos, calcomanias, charcos, flechas y personajes) no desaparecen
// enteros al pasar el corte por su origen: cerca de un corte
// (kEntityClipBandM, por los dos lados) se dibujan con sus lotes marcados y
// se recortan pixel a pixel como el nivel; solo se quitan enteros los que
// quedan mas adentro de lo que se quita. Para saber cuando se dibuja
// cada uno se cambian tambien las entradas de dibujo (vtable[29] opaco,
// vtable[30] translucido; +0x817a0 las llama con la entidad en rcx). Los
// efectos (fuego, particulas, chispas...) se ocultan por su punto.
// ---------------------------------------------------------------------
struct EntitySlotPatch {
    uintptr_t vtable;       // RVA de la vtable (RTTI)
    int slot;
    uintptr_t expected;     // RVA de la funcion original (tras el salto del thunk)
    const char* cls;
    int kind;               // 0 efecto (por su punto), 1 malla (por pixel con el nivel en la GPU),
                            // 2 personaje (como la malla, solo con g_cutPersons)
    bool noMirror;          // no se dibuja dentro del reflejo del agua
};
static const EntitySlotPatch kEntitySlotPatches[] = {
    { 0x7aa090, 27, 0xe34a0, "B_WeaponEntity", 1 },     { 0x7aa090, 28, 0xe34d0, "B_WeaponEntity", 1 },
    { 0x7aaf90, 28, 0x10ba60, "B_AuraEntity", 0 },
    { 0x7abb80, 28, 0x118110, "B_SparkEntity", 0 },
    { 0x7ac020, 28, 0x11c220, "B_TrailEntity", 0 },
    { 0x7ab3a8, 28, 0x10f2b0, "B_ElectricBoltEntity", 0 },
    { 0x7ab8e0, 28, 0x115790, "B_MagicMissileEntity", 0 },
    { 0x7a9da0, 27, 0xe34a0, "B_ObjectEntity", 1 },     { 0x7a9da0, 28, 0xe34d0, "B_ObjectEntity", 1 },
    { 0x7aad48, 27, 0x10a050, "B_ActorEntity", 1 },     { 0x7aad48, 28, 0x10a080, "B_ActorEntity", 1 },
    { 0x7a8dd8, 27, 0xe34a0, "B_PhysicSIEntity", 1 },   { 0x7a8dd8, 28, 0xe34d0, "B_PhysicSIEntity", 1 },
    { 0x7ab680, 27, 0x112f10, "B_DecalEntity", 1 },     { 0x7ab680, 28, 0x112f40, "B_DecalEntity", 1 },
    { 0x7a5070, 28, 0xc0f90, "B_FireEntity", 0 },
    { 0x7a5758, 28, 0xc0f90, "B_DinamicFireEntity", 0 },
    { 0x7aa520, 27, 0x1054f0, "B_PoolEntity", 1 },
    { 0x7abd88, 27, 0xe34a0, "B_ArrowEntity", 1 },      { 0x7abd88, 28, 0xe34d0, "B_ArrowEntity", 1 },
    { 0x7aa800, 28, 0x106fa0, "B_PrtlSysEntity", 0, true },
    { 0x7aa300, 28, 0xfdf50, "B_SpotEntity", 0 },
    { 0x7a4a70, 27, 0xbd990, "B_PersonEntity", 2 },  { 0x7a4a70, 28, 0xbd9c0, "B_PersonEntity", 2 },
    { 0x7a3b18, 27, 0xaedd0, "B_BipedEntity", 2 },
};
// Dibujo de los objetos de malla (vtable[29] y [30]; las vacias no).
static const EntitySlotPatch kEntityDrawPatches[] = {
    { 0x7aa090, 29, 0xf7270, "B_WeaponEntity", 1 },     { 0x7aa090, 30, 0xf73e0, "B_WeaponEntity", 1 },
    { 0x7a9da0, 29, 0xf7270, "B_ObjectEntity", 1 },     { 0x7a9da0, 30, 0xf73e0, "B_ObjectEntity", 1 },
    { 0x7aad48, 29, 0x108530, "B_ActorEntity", 1 },     { 0x7aad48, 30, 0x1086f0, "B_ActorEntity", 1 },
    { 0x7a8dd8, 29, 0xf7270, "B_PhysicSIEntity", 1 },   { 0x7a8dd8, 30, 0xf73e0, "B_PhysicSIEntity", 1 },
    { 0x7ab680, 29, 0x112000, "B_DecalEntity", 1 },     { 0x7ab680, 30, 0x112080, "B_DecalEntity", 1 },
    { 0x7aa520, 29, 0xff110, "B_PoolEntity", 1 },
    { 0x7abd88, 29, 0xf7270, "B_ArrowEntity", 1 },      { 0x7abd88, 30, 0xf73e0, "B_ArrowEntity", 1 },
    { 0x7a4a70, 29, 0xb2d80, "B_PersonEntity", 2 },     { 0x7a4a70, 30, 0xb3060, "B_PersonEntity", 2 },
    { 0x7a3b18, 29, 0xa3350, "B_BipedEntity", 2 },
};
static constexpr int kEntityDrawPatchCount = sizeof(kEntityDrawPatches) / sizeof(kEntityDrawPatches[0]);
static constexpr int kEntitySlotPatchCount = sizeof(kEntitySlotPatches) / sizeof(kEntitySlotPatches[0]);
static constexpr int kEntityTransformSlot = 8;
static constexpr int kEntityTransformPos = 0x60;
using PFN_EntityQuery = int(__fastcall*)(void*);
using PFN_EntityTransform = const char*(__fastcall*)(void*);
static constexpr int kEntityWrapCapacity = 28;
static_assert(kEntitySlotPatchCount <= kEntityWrapCapacity, "faltan envoltorios");
static PFN_EntityQuery g_entityOrig[kEntityWrapCapacity] = {};
static int g_entityKind[kEntityWrapCapacity] = {};
static bool g_entityNoMirror[kEntityWrapCapacity] = {};
static bool g_entityPatched[kEntitySlotPatchCount] = {};
using PFN_EntityDraw = void*(__fastcall*)(void*, void*, void*, uintptr_t);
static constexpr int kEntityDrawWrapCapacity = 20;
static int g_entityDrawKind[kEntityDrawWrapCapacity] = {};
static_assert(kEntityDrawPatchCount <= kEntityDrawWrapCapacity, "faltan envoltorios de dibujo");
static PFN_EntityDraw g_entityDrawOrig[kEntityDrawWrapCapacity] = {};
static bool g_entityDrawPatched[kEntityDrawPatchCount] = {};

// Enlazada (directamente o a traves de otra entidad: la estela de un arma) a
// un personaje. Sin destructores: __try.
static bool LinkedToPerson(void* entity) {
    __try {
        const char* e = static_cast<const char*>(entity);
        for (int depth = 0; depth < 4 && e; ++depth) {
            const char* p = *reinterpret_cast<const char* const*>(e + kEntityParent);
            if (!p || p == e) return false;
            uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
            if (vt == g_base + kPersonVtable || vt == g_base + kBipedVtable) return true;
            e = p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

static bool EntityWorldPosition(void* entity, double out[3]) {
    __try {
        void** vt = *reinterpret_cast<void***>(entity);
        auto getXf = reinterpret_cast<PFN_EntityTransform>(vt[kEntityTransformSlot]);
        const char* m = getXf(entity);
        if (!m) return false;
        const double* t = reinterpret_cast<const double*>(m + kEntityTransformPos);
        for (int i = 0; i < 3; ++i) {
            if (!(t[i] == t[i]) || t[i] > 1e8 || t[i] < -1e8) return false;
            out[i] = t[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Lo mas que un punto queda fuera de los planos del cono (<= 0: dentro).
static double ConeOutside(const double p[3]) {
    double v[3];
    for (int j = 0; j < 3; ++j) {
        v[j] = p[0] * g_passFw[0 * 4 + j] + p[1] * g_passFw[1 * 4 + j] + p[2] * g_passFw[2 * 4 + j] + g_passFw[12 + j];
    }
    double out = -1e300;
    if (g_coneRound) {
        // Tronco de cono redondo: distancia al eje por el cos menos la
        // distancia por el eje por el sin y el radio en el ojo por el cos.
        const double* a = g_coneAxis;
        double t = a[0] * v[0] + a[1] * v[1] + a[2] * v[2];
        double r[3] = { v[0] - t * a[0], v[1] - t * a[1], v[2] - t * a[2] };
        double side = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]) * g_coneCosA - t * g_coneSinA - g_coneEyeRCos;
        const double* b = g_conePlanes[0];
        double back = b[0] * v[0] + b[1] * v[1] + b[2] * v[2] + b[3];
        const double* g = g_coneGround;
        double ground = g[0] * v[0] + g[1] * v[1] + g[2] * v[2] + g[3];
        out = side;
        if (back > out) out = back;
        if (ground > out) out = ground;
        return out;
    }
    for (int i = 0; i <= kConePlaneCount; ++i) {
        const double* m = i < kConePlaneCount ? g_conePlanes[i] : g_coneGround;
        double e = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
        if (e > out) out = e;
    }
    return out;
}

// Que hacer con un objeto por su posicion: 0 dibujarlo entero, 1 dibujarlo
// recortado pixel a pixel (lotes marcados), 2 no dibujarlo. Con el recorte
// por pixel (objeto de malla o personaje, nivel en la GPU, fuera del
// reflejo): mas de kEntityClipBandM dentro de lo que quita algun corte -> 2;
// a menos de esa distancia de un corte -> 1. Sin el: dentro -> 2.
// Personajes (kind 2) y lo que llevan enlazado: siempre 0 salvo con
// g_cutPersons.
static int EntityCutClass(void* entity, int kind) {
    if (!entity || !(g_cutActive || g_viewCut || g_vcutActive)) return 0;
    bool cutPersons = g_cutPersons.load(std::memory_order_relaxed);
    if (kind == 2 && !cutPersons) return 0;
    double p[3];
    if (!EntityWorldPosition(entity, p)) return 0;
    bool usePixel = kind != 0 && g_gpuSkipFaces && g_mirrorDepth == 0;
    double band = usePixel ? kEntityClipBandM * kUnitsPerMeter : 0.0;
    double depth[3];      // cuanto queda dentro de lo que quita cada corte (unidades)
    int n = 0;
    if (g_cutActive) depth[n++] = g_filter.cutY - p[1];                  // por encima (Y hacia abajo)
    if (g_vcutActive) depth[n++] = -(g_vcutPlaneW[0] * p[0] + g_vcutPlaneW[2] * p[2] + g_vcutPlaneW[3]);
    if (g_viewCut) depth[n++] = -ConeOutside(p);
    int cls = 0;
    for (int i = 0; i < n; ++i) {
        if (depth[i] > band) { cls = 2; break; }
        if (usePixel && depth[i] > -band) cls = 1;
    }
    if (cls != 0 && !cutPersons && LinkedToPerson(entity)) return 0;
    return cls;
}

// Reflejo del agua: en la maqueta el reflejo es un volumen bajo la lamina
// translucida, y el de los sistemas de particulas (una cascada) se veia
// subir donde el agua cae. No se dibujan dentro del reflejo.
template <int K>
static int __fastcall EntityQueryWrap(void* entity) {
    if (g_worldActive && g_mirrorDepth > 0 && g_entityNoMirror[K]) return 0;
    if (g_worldActive && EntityCutClass(entity, g_entityKind[K]) == 2) return 0;
    return g_entityOrig[K](entity);
}

// Indicador de "cabeza a la vista" de la camara (kCameraShowHead), o nullptr.
// Sin destructores: __try.
static int* CameraShowHeadRaw() {
    __try {
        uintptr_t app = *reinterpret_cast<uintptr_t*>(g_base + kAppPointerOffset);
        if (!app) return nullptr;
        uintptr_t cam = *reinterpret_cast<uintptr_t*>(app + kAppCameraEntity);
        if (!cam) return nullptr;
        int* p = reinterpret_cast<int*>(cam + kCameraShowHead);
        volatile int probe = *p;
        (void)probe;
        return p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Dibujo de un objeto de malla: cerca de un corte, con los lotes marcados.
// Personajes: en la maqueta se ven desde fuera, asi que con la vista de
// persona del juego (primera persona) se dibujan con la cabeza.
template <int K>
static void* __fastcall EntityDrawWrap(void* entity, void* a2, void* a3, uintptr_t a4) {
    PFN_EntityDraw orig = g_entityDrawOrig[K];
    if (!g_worldActive) return orig(entity, a2, a3, a4);
    int* showHead = g_entityDrawKind[K] == 2 ? CameraShowHeadRaw() : nullptr;
    int savedShowHead = 0;
    if (showHead) {
        savedShowHead = *showHead;
        *showHead = 1;
    }
    void* r = nullptr;
    if (!g_clipTag && EntityCutClass(entity, g_entityDrawKind[K]) == 1) {
        g_clipTag = true;
        r = orig(entity, a2, a3, a4);
        g_clipTag = false;
    } else {
        r = orig(entity, a2, a3, a4);
    }
    if (showHead) *showHead = savedShowHead;
    return r;
}

static PFN_EntityDraw EntityDrawWrapFor(int k) {
    switch (k) {
        case 0: return &EntityDrawWrap<0>;   case 1: return &EntityDrawWrap<1>;
        case 2: return &EntityDrawWrap<2>;   case 3: return &EntityDrawWrap<3>;
        case 4: return &EntityDrawWrap<4>;   case 5: return &EntityDrawWrap<5>;
        case 6: return &EntityDrawWrap<6>;   case 7: return &EntityDrawWrap<7>;
        case 8: return &EntityDrawWrap<8>;   case 9: return &EntityDrawWrap<9>;
        case 10: return &EntityDrawWrap<10>; case 11: return &EntityDrawWrap<11>;
        case 12: return &EntityDrawWrap<12>; case 13: return &EntityDrawWrap<13>;
        case 14: return &EntityDrawWrap<14>; case 15: return &EntityDrawWrap<15>;
        case 16: return &EntityDrawWrap<16>; case 17: return &EntityDrawWrap<17>;
        case 18: return &EntityDrawWrap<18>; case 19: return &EntityDrawWrap<19>;
        default: return nullptr;
    }
}

static PFN_EntityQuery EntityWrapFor(int k) {
    switch (k) {
        case 0: return &EntityQueryWrap<0>;   case 1: return &EntityQueryWrap<1>;
        case 2: return &EntityQueryWrap<2>;   case 3: return &EntityQueryWrap<3>;
        case 4: return &EntityQueryWrap<4>;   case 5: return &EntityQueryWrap<5>;
        case 6: return &EntityQueryWrap<6>;   case 7: return &EntityQueryWrap<7>;
        case 8: return &EntityQueryWrap<8>;   case 9: return &EntityQueryWrap<9>;
        case 10: return &EntityQueryWrap<10>; case 11: return &EntityQueryWrap<11>;
        case 12: return &EntityQueryWrap<12>; case 13: return &EntityQueryWrap<13>;
        case 14: return &EntityQueryWrap<14>; case 15: return &EntityQueryWrap<15>;
        case 16: return &EntityQueryWrap<16>; case 17: return &EntityQueryWrap<17>;
        case 18: return &EntityQueryWrap<18>; case 19: return &EntityQueryWrap<19>;
        case 20: return &EntityQueryWrap<20>; case 21: return &EntityQueryWrap<21>;
        case 22: return &EntityQueryWrap<22>; case 23: return &EntityQueryWrap<23>;
        case 24: return &EntityQueryWrap<24>; case 25: return &EntityQueryWrap<25>;
        case 26: return &EntityQueryWrap<26>; case 27: return &EntityQueryWrap<27>;
        default: return nullptr;
    }
}

static uintptr_t FollowThunk(uintptr_t addr) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    if (p[0] == 0xE9) return addr + 5 + *reinterpret_cast<const int32_t*>(p + 1);
    return addr;
}

static bool WriteVtableSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return true;
}

static void InstallEntityHiding() {
    for (int k = 0; k < kEntitySlotPatchCount; ++k) {
        const EntitySlotPatch& e = kEntitySlotPatches[k];
        void** slot = reinterpret_cast<void**>(g_base + e.vtable) + e.slot;
        uintptr_t cur = reinterpret_cast<uintptr_t>(*slot);
        if (cur < g_base || FollowThunk(cur) != g_base + e.expected || !EntityWrapFor(k)) {
            std::ostringstream o;
            o << "[DIORAMA] AVISO: vtable de " << e.cls << " no esperada (ranura " << e.slot
              << "); esos objetos no se ocultan con el corte.";
            HookLogger::Instance().Line(o.str());
            continue;
        }
        g_entityOrig[k] = reinterpret_cast<PFN_EntityQuery>(cur);
        g_entityKind[k] = e.kind;
        g_entityNoMirror[k] = e.noMirror;
        if (WriteVtableSlot(slot, reinterpret_cast<void*>(EntityWrapFor(k)))) g_entityPatched[k] = true;
    }
    for (int k = 0; k < kEntityDrawPatchCount; ++k) {
        const EntitySlotPatch& e = kEntityDrawPatches[k];
        void** slot = reinterpret_cast<void**>(g_base + e.vtable) + e.slot;
        uintptr_t cur = reinterpret_cast<uintptr_t>(*slot);
        if (cur < g_base || FollowThunk(cur) != g_base + e.expected || !EntityDrawWrapFor(k)) {
            std::ostringstream w;
            w << "[DIORAMA] AVISO: vtable de " << e.cls << " no esperada (ranura " << e.slot
              << "); esos objetos no se recortan por pixel.";
            HookLogger::Instance().Line(w.str());
            continue;
        }
        g_entityDrawOrig[k] = reinterpret_cast<PFN_EntityDraw>(cur);
        g_entityDrawKind[k] = e.kind;
        if (WriteVtableSlot(slot, reinterpret_cast<void*>(EntityDrawWrapFor(k)))) g_entityDrawPatched[k] = true;
    }
}

static void UninstallEntityHiding() {
    for (int k = 0; k < kEntitySlotPatchCount; ++k) {
        if (!g_entityPatched[k]) continue;
        void** slot = reinterpret_cast<void**>(g_base + kEntitySlotPatches[k].vtable) + kEntitySlotPatches[k].slot;
        WriteVtableSlot(slot, reinterpret_cast<void*>(g_entityOrig[k]));
        g_entityPatched[k] = false;
    }
    for (int k = 0; k < kEntityDrawPatchCount; ++k) {
        if (!g_entityDrawPatched[k]) continue;
        void** slot = reinterpret_cast<void**>(g_base + kEntityDrawPatches[k].vtable) + kEntityDrawPatches[k].slot;
        WriteVtableSlot(slot, reinterpret_cast<void*>(g_entityDrawOrig[k]));
        g_entityDrawPatched[k] = false;
    }
}

// Portales de atmosfera: al pasar de una zona a otra (del aire al agua) el
// motor apunta el poligono del portal (+0x7fc90 -> +0x7f960) y +0x81630 lo
// dibuja con la niebla de la zona de detras. En la maqueta no hay niebla y
// saldrian como placas oscuras. No se dibujan; B_Map::Render vacia la lista
// justo despues (+0x87d30).
static void __fastcall HookedDrawAtmPortals(long long map, long long view, long long list) {
    if (g_worldActive) return;
    g_origDrawAtmPortals(map, view, list);
}

static void* AllocNearModule(uintptr_t base, size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (uintptr_t delta = gran; delta < 0x60000000; delta += gran) {
        if (base > delta) {
            uintptr_t below = (base - delta) & ~(gran - 1);
            void* p = VirtualAlloc(reinterpret_cast<void*>(below), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

// "movsd xmm0, [rip+disp]" (F2 0F 10 05 disp32): se repunta disp al double propio.
static bool PatchBackfaceSite(uintptr_t site, const void* target, int32_t* original) {
    unsigned char* p = reinterpret_cast<unsigned char*>(site);
    if (p[0] != 0xF2 || p[1] != 0x0F || p[2] != 0x10 || p[3] != 0x05) return false;
    int32_t disp = *reinterpret_cast<int32_t*>(p + 4);
    if (original) *original = disp;
    long long delta = static_cast<long long>(reinterpret_cast<uintptr_t>(target)) - static_cast<long long>(site + 8);
    if (delta > 0x7FFFFFFFLL || delta < -0x7FFFFFFFLL) return false;
    DWORD old = 0;
    if (!VirtualProtect(p + 4, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<int32_t*>(p + 4) = static_cast<int32_t>(delta);
    DWORD tmp = 0;
    VirtualProtect(p + 4, 4, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    return true;
}

static bool InstallBackfaceThreshold() {
    // Comprobar TODOS los sitios antes de tocar ninguno.
    for (int i = 0; i < kBackfaceSiteCount; ++i) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(g_base + kBackfaceSites[i]);
        if (p[0] != 0xF2 || p[1] != 0x0F || p[2] != 0x10 || p[3] != 0x05) return false;
        int32_t disp = *reinterpret_cast<const int32_t*>(p + 4);
        if (g_base + kBackfaceSites[i] + 8 + static_cast<intptr_t>(disp) != g_base + kHalfConstant) return false;
    }
    g_threshold = static_cast<double*>(AllocNearModule(g_base, 4096));
    if (!g_threshold) return false;
    *g_threshold = kKeepFaceThreshold;
    int done = 0;
    for (int i = 0; i < kBackfaceSiteCount; ++i) {
        if (PatchBackfaceSite(g_base + kBackfaceSites[i], g_threshold, &g_backfaceOriginalDisp[i])) ++done;
    }
    g_backfacePatched = (done == kBackfaceSiteCount);
    return g_backfacePatched;
}

static void UninstallBackfaceThreshold() {
    if (!g_backfacePatched) return;
    for (int i = 0; i < kBackfaceSiteCount; ++i) {
        unsigned char* p = reinterpret_cast<unsigned char*>(g_base + kBackfaceSites[i]);
        DWORD old = 0;
        if (VirtualProtect(p + 4, 4, PAGE_EXECUTE_READWRITE, &old)) {
            *reinterpret_cast<int32_t*>(p + 4) = g_backfaceOriginalDisp[i];
            DWORD tmp = 0;
            VirtualProtect(p + 4, 4, old, &tmp);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_backfacePatched = false;
}

// ---------------------------------------------------------------------
// Sin niebla en diorama
// ---------------------------------------------------------------------
// Las dos nieblas que escribe el cambio de atmosfera: la de la CPU por
// vertice (coeficiente en el raster) y la del sombreador (densidad global que
// copia cada lote).
static void __fastcall HookedSetAtmosphere(char* raster, const double* plane, unsigned int zone) {
    g_origSetAtmosphere(raster, plane, zone);
    if (g_worldActive && raster) {
        *reinterpret_cast<double*>(raster + kRasterFogCoef) = 0.0;
        *reinterpret_cast<float*>(g_base + kFogDensityGlobal) = 0.0f;
    }
}

// Sombras de objetos del juego (app+0x768). Sin destructores: __try.
static bool ReadShadowFlagRaw(int* out) {
    __try {
        uintptr_t app = *reinterpret_cast<uintptr_t*>(g_base + kAppPointerOffset);
        if (!app) return false;
        *out = *reinterpret_cast<int*>(app + kAppDrawObjectShadows);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool WriteShadowFlagRaw(int value) {
    __try {
        uintptr_t app = *reinterpret_cast<uintptr_t*>(g_base + kAppPointerOffset);
        if (!app) return false;
        *reinterpret_cast<int*>(app + kAppDrawObjectShadows) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// 1 = enganchado, 0 = aun no hay raster, -1 = no es lo esperado. Sin
// destructores: __try.
static int PatchAtmosphereSlotRaw() {
    __try {
        char* raster = *reinterpret_cast<char**>(g_base + kRasterPtrOffset);
        if (!raster) return 0;
        void** vtable = *reinterpret_cast<void***>(raster);
        if (reinterpret_cast<uintptr_t>(vtable) != g_base + kRasterVtableOffset) return -1;
        void** slot = vtable + kSetAtmosphereSlot;
        if (reinterpret_cast<uintptr_t>(*slot) != g_base + kSetAtmosphereThunk) return -1;
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return -1;
        g_origSetAtmosphere = reinterpret_cast<PFN_SetAtmosphere>(*slot);
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HookedSetAtmosphere));
        DWORD tmp = 0;
        VirtualProtect(slot, sizeof(void*), old, &tmp);
        g_atmosphereSlot = slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return 1;
}

// Reflejo del agua sin el recorte por bandas.
// +0xf0fd0 (reflejo) pone el raster en modo espejo con su vtable+0x88
// (+0x7532f0: raster+0xdc = 1 y raster+0x1e25a8 = 1). Con +0x1e25a8 cada
// poligono del reflejo (caras, luz, sombras, objetos) se trocea en bandas de
// pantalla (+0x751eb0 / +0x752710, hasta 200 filas con la proyeccion de la
// CPU) y se recorta con la silueta de las laminas de agua apuntada por filas
// (raster+0x230 -> +0x750650 -> +0x74e660: como mucho 16 bordes por fila y
// los casi iguales se juntan). Con la maqueta lejos, muchas laminas caen en
// las mismas filas: bordes perdidos o juntados y huecos por los que se veia
// el cielo. En la maqueta se quita ese modo al empezar el reflejo: el
// reflejo ya va recortado exactamente por el cono de cada lamina (visitas
// con los planos de su poligono, +0x1ad580).
//
// Conos del reflejo por ojo: +0x1ad580(planos, poligono) hace los planos por
// el ojo de un poligono en camara (+0x08 recuento, +0x10 vertices de 3
// doubles). Desde +0xf0fd0 (retorno +0xf142c) recibe los poligonos de la
// lamina, que apunto la pasada del centro de la cabeza en su camara: en el
// reflejo de cada ojo se mueven a ese ojo (x + medio ojo). Los de los
// portales ya los calcula el motor con la matriz del ojo.
static constexpr uintptr_t kConePlanesMirrorRet = 0xf142c;     // retorno en +0xf0fd0 (lamina del reflejo)
static constexpr int kPolyCountOffset = 0x08;
static constexpr int kPolyVertsOffset = 0x10;
static constexpr int kPolyMaxVerts = 32;                       // 0x300 / 24 planos
using PFN_ConePlanes = void(__fastcall*)(char*, const char*);
static PFN_ConePlanes g_origConePlanes = nullptr;

// Lee el poligono (sin destructores: __try). Devuelve el recuento o 0.
static int ReadConePolygonRaw(const char* poly, double* v, unsigned char* header) {
    __try {
        int n = *reinterpret_cast<const int*>(poly + kPolyCountOffset);
        if (n < 3 || n > kPolyMaxVerts) return 0;
        std::memcpy(header, poly, kPolyVertsOffset);
        std::memcpy(v, poly + kPolyVertsOffset, static_cast<size_t>(n) * 24);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void __fastcall HookedConePlanes(char* planes, const char* poly) {
    if (g_worldActive && g_mirrorDepth > 0 && g_mirrorEye >= 0 && poly &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_base + kConePlanesMirrorRet) {
        double buf[kPolyVertsOffset / 8 + kPolyMaxVerts * 3];
        double vin[kPolyMaxVerts * 3];
        int nin = ReadConePolygonRaw(poly, vin, reinterpret_cast<unsigned char*>(buf));
        if (nin >= 3) {
            double* out = buf + kPolyVertsOffset / 8;
            for (int i = 0; i < nin; ++i) {
                out[3 * i + 0] = vin[3 * i + 0] + g_mirrorEyeShift;
                out[3 * i + 1] = vin[3 * i + 1];
                out[3 * i + 2] = vin[3 * i + 2];
            }
            g_origConePlanes(planes, reinterpret_cast<const char*>(buf));
            return;
        }
    }
    g_origConePlanes(planes, poly);
}

// Modo espejo del raster sin bandas (ver arriba): raster vtable+0x88.
static constexpr int kRasterBeginMirrorSlot = 0x88 / 8;
static constexpr uintptr_t kRasterBeginMirrorFn = 0x7532f0;
static constexpr int kRasterScanlineMode = 0x1e25a8;
using PFN_RasterVoid = void(__fastcall*)(char*);
static PFN_RasterVoid g_origRasterBeginMirror = nullptr;
static void** g_beginMirrorSlot = nullptr;
static bool g_beginMirrorTried = false;

static void __fastcall HookedRasterBeginMirror(char* raster) {
    g_origRasterBeginMirror(raster);
    if (g_worldActive && raster) *reinterpret_cast<int*>(raster + kRasterScanlineMode) = 0;
}

// 1 = enganchado, 0 = aun no hay raster, -1 = no es lo esperado. Sin
// destructores: __try.
static int PatchBeginMirrorSlotRaw() {
    __try {
        char* raster = *reinterpret_cast<char**>(g_base + kRasterPtrOffset);
        if (!raster) return 0;
        void** vtable = *reinterpret_cast<void***>(raster);
        if (reinterpret_cast<uintptr_t>(vtable) != g_base + kRasterVtableOffset) return -1;
        void** slot = vtable + kRasterBeginMirrorSlot;
        uintptr_t cur = reinterpret_cast<uintptr_t>(*slot);
        if (cur < g_base || FollowThunk(cur) != g_base + kRasterBeginMirrorFn) return -1;
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return -1;
        g_origRasterBeginMirror = reinterpret_cast<PFN_RasterVoid>(cur);
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HookedRasterBeginMirror));
        DWORD tmp = 0;
        VirtualProtect(slot, sizeof(void*), old, &tmp);
        g_beginMirrorSlot = slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return 1;
}

static void EnsureMirrorBandsHook() {
    if (g_beginMirrorTried) return;
    int r = PatchBeginMirrorSlotRaw();
    if (r == 0) return;   // se reintenta en el siguiente fotograma
    g_beginMirrorTried = true;
    if (r != 1) {
        HookLogger::Instance().Line("[DIORAMA] AVISO: la tabla del raster no es la esperada; los reflejos siguen con el recorte por bandas.");
    }
}

static void UninstallMirrorBandsHook() {
    if (!g_beginMirrorSlot || !g_origRasterBeginMirror) return;
    DWORD old = 0;
    if (VirtualProtect(g_beginMirrorSlot, sizeof(void*), PAGE_READWRITE, &old)) {
        InterlockedExchangePointer(g_beginMirrorSlot, reinterpret_cast<void*>(g_origRasterBeginMirror));
        DWORD tmp = 0;
        VirtualProtect(g_beginMirrorSlot, sizeof(void*), old, &tmp);
    }
    g_beginMirrorSlot = nullptr;
}

static void EnsureFogHook() {
    if (g_fogHookTried) return;
    int r = PatchAtmosphereSlotRaw();
    if (r == 0) return;   // se reintenta en el siguiente fotograma
    g_fogHookTried = true;
    if (r != 1) HookLogger::Instance().Line("[DIORAMA] AVISO: la tabla del raster no es la esperada; la maqueta tendra niebla.");
}

static void UninstallFogHook() {
    if (!g_atmosphereSlot || !g_origSetAtmosphere) return;
    DWORD old = 0;
    if (VirtualProtect(g_atmosphereSlot, sizeof(void*), PAGE_READWRITE, &old)) {
        InterlockedExchangePointer(g_atmosphereSlot, reinterpret_cast<void*>(g_origSetAtmosphere));
        DWORD tmp = 0;
        VirtualProtect(g_atmosphereSlot, sizeof(void*), old, &tmp);
    }
    g_atmosphereSlot = nullptr;
}

// ---------------------------------------------------------------------
// Camara de la maqueta
// ---------------------------------------------------------------------

// Punto al que mira la camara del juego (TPos de la entidad Camera): el
// personaje. Sin destructores: __try.
static bool ReadCameraTarget(double out[3]) {
    __try {
        uintptr_t app = *reinterpret_cast<uintptr_t*>(g_base + kAppPointerOffset);
        if (!app) return false;
        uintptr_t cam = *reinterpret_cast<uintptr_t*>(app + kAppCameraEntity);
        if (!cam) return false;
        const double* t = reinterpret_cast<const double*>(cam + kCameraTargetPos);
        bool allZero = true;
        for (int i = 0; i < 3; ++i) {
            double v = t[i];
            if (!(v == v) || v > 1e8 || v < -1e8) return false;
            if (v != 0.0) allZero = false;
            out[i] = v;
        }
        return !allZero;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Cinematicas. Mientras dura una, la vista ES la camara de la cinematica: su
// posicion y su guinada (cabeceo y alabeo del visor, como en el VR normal),
// mas lo que se mueva la cabeza desde que empezo, por la escala. La maqueta
// (mesa, ancla, giro) no se toca; los mandos no la mueven ni la giran
// mientras tanto (solo zoom, cono y cortes). Una llamada sin camara de
// cinematica dentro de kCineHoldMs sigue en ese modo (evita saltos entre
// camaras del mismo fotograma).
//
// Zoom de maqueta: el mundo se escala alrededor de un pivote fijo
// kCinePivotM (45 cm, lo mismo que la mesa) por delante de la vista. Con la
// escala k y la de referencia k0 la vista se coloca en
// pivote + (camara - pivote) * k / k0, es decir, camara + adelante * 0,45 m *
// (k0 - k): con k = k0 se ve exactamente lo del juego; al alejar (k mayor)
// la vista retrocede por su direccion de mirada y el mundo encoge como una
// maqueta que sigue el mismo recorrido; al acercar avanza.
// Cada cinematica empieza a kCineRefScale (1:10, la de la mesa). Recentrar
// (Inicio, X del mando de Xbox y de Quest) vuelve a la camara de la
// cinematica conservando el tamano: la escala de ese momento pasa a ser la
// de referencia. Al acabar, la maqueta se queda con esa escala (sin salto de
// tamano).
// Sombras: el motor solo las procesa cerca del personaje; la camara de una
// cinematica puede estar mirando a otro sitio, asi que tambien valen los
// sectores cerca de un punto kCineShadowAheadM por delante de ella.
static constexpr uint64_t kCineHoldMs = 600;
static constexpr unsigned kCineToastMs = 2500;
static constexpr double kCinePivotM = kTableForwardM;
static constexpr double kCineRefScale = kDefaultScale;
static bool g_cineCallPending = false;          // hilo del juego
static bool g_cineActive = false;               // hilo del juego
static std::atomic<uint64_t> g_cineLastTick{0};
static double g_cineHeadRef[3] = {};
static double g_cineHeadYawRef = 0.0;
static double g_cineScaleRef = 0.0;
static std::atomic<int> g_cineRecenterRequest{0};

static void SetModelScaleLocked(double k) {
    std::lock_guard<std::mutex> lock(g_modelMutex);
    g_model.scale = k;
    g_scaleMirror.store(k, std::memory_order_relaxed);
}
void DioramaMarkCinematicCall() { g_cineCallPending = true; }
bool DioramaCinematicActive() {
    uint64_t t = g_cineLastTick.load(std::memory_order_relaxed);
    return g_enabled.load(std::memory_order_relaxed) && t != 0 && GetTickCount64() - t < kCineHoldMs;
}

bool DioramaComputeCamera(const float m[12], const double game[16], bool recenter, double out[16]) {
    bool cineCall = g_cineCallPending;
    g_cineCallPending = false;
    if (!m || !game || !out) return false;
    // Camara del juego: c[r] = -(t . fila_r); adelante = columna 2.
    double c[3];
    for (int rr = 0; rr < 3; ++rr) {
        c[rr] = -(game[12] * game[rr * 4 + 0] + game[13] * game[rr * 4 + 1] + game[14] * game[rr * 4 + 2]);
    }
    double baseYaw = std::atan2(game[2], game[10]);
    // Ejes y posicion del visor en ejes del juego (cambio de base (x, -y, -z)).
    double r0[3] = { m[0], -m[4], -m[8] };
    double u0[3] = { -m[1], m[5], m[9] };
    double f0[3] = { -m[2], m[6], m[10] };
    double hp[3] = { m[3], -m[7], -m[11] };

    // Personaje en este fotograma (centro del area y referencia del corte).
    double target[3];
    bool fromTarget = ReadCameraTarget(target);
    if (!fromTarget) { target[0] = c[0]; target[1] = c[1]; target[2] = c[2]; }
    for (int i = 0; i < 3; ++i) g_charPos[i] = target[i];
    g_haveCharPos = true;

    // Cinematica: la vista es la camara del juego (ver kCineHoldMs).
    uint64_t nowTick = GetTickCount64();
    uint64_t lastCine = g_cineLastTick.load(std::memory_order_relaxed);
    bool cine = cineCall || (g_cineActive && lastCine != 0 && nowTick - lastCine < kCineHoldMs);
    if (cine) {
        double hmdYaw = std::atan2(f0[0], f0[2]);
        bool cineRecenter = recenter || g_cineRecenterRequest.exchange(0, std::memory_order_relaxed) != 0;
        if (!g_cineActive) SetModelScaleLocked(kCineRefScale);
        double k = g_scaleMirror.load(std::memory_order_relaxed);
        if (!(k > 0.0)) k = 1.0;
        if (!g_cineActive || cineRecenter) {
            for (int i = 0; i < 3; ++i) g_cineHeadRef[i] = hp[i];
            g_cineHeadYawRef = hmdYaw;
            g_cineScaleRef = k;
            double lo = k * kCineZoomInFactor;
            if (lo < kMinScale) lo = kMinScale;
            g_cineZoomLo.store(lo, std::memory_order_relaxed);
            g_cineZoomHi.store(kCineMaxScale > k ? kCineMaxScale : k, std::memory_order_relaxed);
        }
        if (!g_cineActive) {
            g_cineActive = true;
        } else if (cineRecenter) {
            OpenVRShowToast(L"Cinem\u00e1tica", L"Recentrada en la c\u00e1mara", kCineToastMs);
        }
        if (cineCall) g_cineLastTick.store(nowTick, std::memory_order_relaxed);
        {
            double fg[3] = { game[2], game[6], game[10] };
            double fl = std::sqrt(fg[0] * fg[0] + fg[1] * fg[1] + fg[2] * fg[2]);
            if (fl > 1e-9) {
                double a = kCineShadowAheadM * kUnitsPerMeter / fl;
                for (int i = 0; i < 3; ++i) g_cineFocus[i] = c[i] + fg[i] * a;
                g_haveCineFocus = true;
            }
        }
        double delta = baseYaw - g_cineHeadYawRef;
        double cD = std::cos(delta), sD = std::sin(delta);
        double r[3] = { r0[0], r0[1], r0[2] };
        double u[3] = { u0[0], u0[1], u0[2] };
        double f[3] = { f0[0], f0[1], f0[2] };
        RotateY(r, cD, sD);
        RotateY(u, cD, sD);
        RotateY(f, cD, sD);
        double off[3] = { hp[0] - g_cineHeadRef[0], hp[1] - g_cineHeadRef[1], hp[2] - g_cineHeadRef[2] };
        RotateY(off, cD, sD);
        double s = kUnitsPerMeter * k;
        double cam[3] = { c[0] + off[0] * s, c[1] + off[1] * s, c[2] + off[2] * s };
        if (g_cineScaleRef > 0.0) {
            // Adelante de la camara del juego = columna 2 de su matriz.
            double fg[3] = { game[2], game[6], game[10] };
            double fl = std::sqrt(fg[0] * fg[0] + fg[1] * fg[1] + fg[2] * fg[2]);
            if (fl > 1e-9) {
                double fwd = kCinePivotM * kUnitsPerMeter * (g_cineScaleRef - k) / fl;   // < 0: atras
                for (int i = 0; i < 3; ++i) cam[i] += fg[i] * fwd;
            }
        }
        for (int i = 0; i < 16; ++i) out[i] = game[i];
        out[0] = r[0]; out[4] = r[1]; out[8] = r[2];
        out[1] = u[0]; out[5] = u[1]; out[9] = u[2];
        out[2] = f[0]; out[6] = f[1]; out[10] = f[2];
        out[12] = -(cam[0] * r[0] + cam[1] * r[1] + cam[2] * r[2]);
        out[13] = -(cam[0] * u[0] + cam[1] * u[1] + cam[2] * u[2]);
        out[14] = -(cam[0] * f[0] + cam[1] * f[1] + cam[2] * f[2]);
        for (int i = 0; i < 12; ++i) g_camPose[i] = m[i];
        for (int i = 0; i < 16; ++i) g_camPoseFw[i] = out[i];
        g_camPoseValid = true;
        return true;
    }
    g_haveCineFocus = false;
    if (g_cineActive) {
        // Fin de la cinematica: la maqueta se queda con la escala de la vista.
        g_cineActive = false;
        g_cineZoomHi.store(0.0, std::memory_order_relaxed);
        g_cineZoomLo.store(0.0, std::memory_order_relaxed);
        g_cineRecenterRequest.store(0, std::memory_order_relaxed);
    }

    Model md;
    bool requested = g_anchorRequest.exchange(0, std::memory_order_relaxed) != 0;
    bool locate = g_anchorLocateRequest.exchange(0, std::memory_order_relaxed) != 0 || recenter;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        bool follow = g_model.mode >= 1;
        if (recenter || requested || !g_model.placed) {
            if (locate) {
                // Boton de recolocar (no nivel nuevo ni activar): tambien la
                // escala para localizar al personaje.
                g_model.scale = ClampScale(kLocateScale);
                g_scaleMirror.store(g_model.scale, std::memory_order_relaxed);
            }
            for (int i = 0; i < 3; ++i) g_model.anchor[i] = target[i];
            double hmdYaw = std::atan2(f0[0], f0[2]);
            g_model.yaw = baseYaw - hmdYaw;   // al frente del visor = al frente de la camara del juego
            double fh[3] = { f0[0], 0.0, f0[2] };
            double len = std::sqrt(fh[0] * fh[0] + fh[2] * fh[2]);
            if (len < 1e-6) { fh[0] = 0.0; fh[2] = 1.0; } else { fh[0] /= len; fh[2] /= len; }
            g_model.table[0] = hp[0] + fh[0] * kTableForwardM;
            g_model.table[1] = hp[1] + kTableDropM;             // Y hacia abajo
            g_model.table[2] = hp[2] + fh[2] * kTableForwardM;
            g_model.placed = true;
            g_followY = target[1];
            g_followWasOn = follow;
            QueryPerformanceCounter(&g_followLastQpc);
        } else if (follow && fromTarget) {
            // El ancla (lo que va al punto de la mesa) es el personaje de
            // este fotograma: la maqueta se desplaza cuando avanza.
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            double dt = freq.QuadPart > 0 ? static_cast<double>(now.QuadPart - g_followLastQpc.QuadPart) / freq.QuadPart : 0.0;
            g_followLastQpc = now;
            if (dt < 0.0) dt = 0.0;
            if (dt > 0.25) dt = 0.25;
            if (!g_followWasOn) {
                // Se acaba de acoplar: el punto de la mesa pasa a donde se ve
                // ahora el personaje, asi la maqueta no salta.
                double rel[3] = { target[0] - g_model.anchor[0], target[1] - g_model.anchor[1],
                                  target[2] - g_model.anchor[2] };
                RotateY(rel, std::cos(-g_model.yaw), std::sin(-g_model.yaw));
                double inv = 1.0 / (kUnitsPerMeter * g_model.scale);
                for (int i = 0; i < 3; ++i) g_model.table[i] += rel[i] * inv;
                g_followY = target[1];
            }
            g_followY += (target[1] - g_followY) * (1.0 - std::exp(-dt / kFollowYTauS));
            g_model.anchor[0] = target[0];
            g_model.anchor[1] = g_followY;
            g_model.anchor[2] = target[2];
            if (g_model.mode == kModeBehind) {
                // Vista por la espalda: la direccion cabeza -> mesa (en
                // horizontal) llevada al juego tiende a la de la camara del
                // juego, que va detras del personaje. Gira alrededor del
                // punto de la mesa (el personaje), asi que basta la guinada.
                double dx = g_model.table[0] - hp[0], dz = g_model.table[2] - hp[2];
                if (dx * dx + dz * dz > 0.0025) {
                    double want = WrapPi(baseYaw - std::atan2(dx, dz));
                    double a = 1.0 - std::exp(-dt / kBackYawTauS);
                    g_model.yaw = WrapPi(g_model.yaw + WrapPi(want - g_model.yaw) * a);
                }
            }
            g_followWasOn = true;
        }
        if (!follow) g_followWasOn = false;
        md = g_model;
    }

    double cD = std::cos(md.yaw), sD = std::sin(md.yaw);
    double r[3] = { r0[0], r0[1], r0[2] };
    double u[3] = { u0[0], u0[1], u0[2] };
    double f[3] = { f0[0], f0[1], f0[2] };
    RotateY(r, cD, sD);
    RotateY(u, cD, sD);
    RotateY(f, cD, sD);
    double off[3] = { hp[0] - md.table[0], hp[1] - md.table[1], hp[2] - md.table[2] };
    RotateY(off, cD, sD);
    double s = kUnitsPerMeter * md.scale;
    double cam[3] = { md.anchor[0] + off[0] * s, md.anchor[1] + off[1] * s, md.anchor[2] + off[2] * s };

    for (int i = 0; i < 16; ++i) out[i] = game[i];
    out[0] = r[0]; out[4] = r[1]; out[8] = r[2];
    out[1] = u[0]; out[5] = u[1]; out[9] = u[2];
    out[2] = f[0]; out[6] = f[1]; out[10] = f[2];
    out[12] = -(cam[0] * r[0] + cam[1] * r[1] + cam[2] * r[2]);
    out[13] = -(cam[0] * u[0] + cam[1] * u[1] + cam[2] * u[2]);
    out[14] = -(cam[0] * f[0] + cam[1] * f[1] + cam[2] * f[2]);
    for (int i = 0; i < 12; ++i) g_camPose[i] = m[i];
    for (int i = 0; i < 16; ++i) g_camPoseFw[i] = out[i];
    g_camPoseValid = true;
    return true;
}

// ---------------------------------------------------------------------
// Fondo de la maqueta (clave de color para el passthrough)
// ---------------------------------------------------------------------

// Borrado de la vista 0 de bgfx ("World Pass"). Se escribe en el hilo del
// juego antes de que bgfx cierre el fotograma, igual que las vistas por ojo
// de SceneCullingRootHook. Sin destructores: __try.
static bool ReadViewClearRaw(unsigned char color[4], uint16_t* flags) {
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(g_base + kBgfxCtxOffset);
        if (!ctx) return false;
        const unsigned char* view0 = reinterpret_cast<const unsigned char*>(ctx + kBgfxViewsOffset);
        for (int i = 0; i < 4; ++i) color[i] = view0[i];
        *flags = *reinterpret_cast<const uint16_t*>(view0 + kViewClearFlags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool WriteViewClearRaw(const unsigned char color[4], uint16_t flags) {
    __try {
        uintptr_t ctx = *reinterpret_cast<uintptr_t*>(g_base + kBgfxCtxOffset);
        if (!ctx) return false;
        unsigned char* view0 = reinterpret_cast<unsigned char*>(ctx + kBgfxViewsOffset);
        for (int i = 0; i < 4; ++i) view0[i] = color[i];
        *reinterpret_cast<uint16_t*>(view0 + kViewClearFlags) = flags;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// keyed = la maqueta se esta dibujando con un fondo de color (F7). Se guarda
// el borrado del juego la primera vez y se devuelve al dejar de usarlo; cada
// fotograma se vuelve a escribir por si el juego lo pone de nuevo.
static void ApplyBackground(bool keyed) {
    if (keyed) {
        if (!g_bgApplied) {
            if (!ReadViewClearRaw(g_bgSavedColor, &g_bgSavedFlags)) return;
            g_bgApplied = true;
        }
        const Background& bg = kBackgrounds[g_background.load(std::memory_order_relaxed) % kBackgroundCount];
        unsigned char c[4] = { bg.r, bg.g, bg.b, 255 };
        uint16_t f = static_cast<uint16_t>((g_bgSavedFlags | kBgfxClearColor) & ~kBgfxClearUsePalette);
        WriteViewClearRaw(c, f);
    } else if (g_bgApplied) {
        WriteViewClearRaw(g_bgSavedColor, g_bgSavedFlags);
        g_bgApplied = false;
    }
}

// ---------------------------------------------------------------------
// Pasada del mundo
// ---------------------------------------------------------------------

// fromWorld del bloque de pose de B_Map::Render. Sin destructores: __try.
static bool ReadPoseFromWorld(long long pose, double out[16]) {
    __try {
        const double* fw = reinterpret_cast<const double*>(pose + kPoseFromWorld);
        for (int i = 0; i < 16; ++i) out[i] = fw[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Filtro de sectores y plano de corte en espacio de camara (como +0x71d70:
// n' = n * M(3x3), d' = d - n'.t). Se conserva lo que cumple n.p + d > 0.5,
// con n = (0, 1, 0) y d = -cutY: lo que esta por debajo (Y hacia abajo).
// Corte recto (ver kVcutStraightSwitchCos): cambia dir por el eje del corte
// y, mientras cambia de lado, lleva *offM hacia el borde del mapa.
static void StraightVcut(const Model& md, double dir[2], double* offM) {
    uint64_t now = GetTickCount64();
    double dt = g_vsTick != 0 ? static_cast<double>(now - g_vsTick) / 1000.0 : 0.0;
    g_vsTick = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.25) dt = 0.25;
    bool newCut = g_vsAxis < 0 || md.vcutDir[0] != g_vsKey[0] || md.vcutDir[1] != g_vsKey[1] ||
                  md.vcutYawRef != g_vsKey[2];
    g_vsKey[0] = md.vcutDir[0];
    g_vsKey[1] = md.vcutDir[1];
    g_vsKey[2] = md.vcutYawRef;
    if (newCut || md.mode != kModeBehind) {
        g_vsAxis = NearestMapAxis(dir);
        g_vsOut = 0.0;
    } else {
        const double* a = kMapAxisDir[g_vsAxis];
        double step = dt / kVcutStraightMoveS;
        if (a[0] * dir[0] + a[1] * dir[1] < kVcutStraightSwitchCos) {
            g_vsOut += step;
            if (g_vsOut >= 1.0) {
                g_vsOut = 1.0;
                g_vsAxis = NearestMapAxis(dir);
            }
        } else {
            g_vsOut -= step;
            if (g_vsOut < 0.0) g_vsOut = 0.0;
        }
    }
    dir[0] = kMapAxisDir[g_vsAxis][0];
    dir[1] = kMapAxisDir[g_vsAxis][1];
    if (g_vsOut > 0.0) {
        CutRange r;
        {
            std::lock_guard<std::mutex> lock(g_modelMutex);
            r = g_cutRange;
        }
        double nearM = kVcutMaxM, farM = kVcutMinM;
        if (r.valid) VcutEdges(r, dir[0], dir[1], &nearM, &farM);
        double e = g_vsOut * g_vsOut * (3.0 - 2.0 * g_vsOut);
        if (nearM > *offM) *offM += (nearM - *offM) * e;
    }
}

static void PrepareFilterAndCut(long long pose) {
    Model md = SnapshotModel();
    // Medio ojo en unidades del juego (lo que StereoHook desplaza cada ojo):
    // para el reflejo del agua visto con los dos ojos.
    g_passEyeHalf = 0.5 * static_cast<double>(StereoGetEyeSeparationUnits()) * md.scale;
    if (!(g_passEyeHalf > 0.0) || g_passEyeHalf > 1e6) g_passEyeHalf = 0.0;
    bool coneOn = g_coneOn.load(std::memory_order_relaxed);
    g_coneRound = false;
    const double* center = g_haveCharPos ? g_charPos : md.anchor;
    double size = md.areaM * kUnitsPerMeter;
    g_filter.useRadius = md.areaShape == 1 && md.areaM > 0.0;
    g_filter.radiusSq = size * size;
    g_filter.useSquare = md.areaShape == 2 && md.areaM > 0.0;
    g_filter.half = size;
    // Ejes del cuadrado: los de la mesa llevados al juego (giro de la maqueta).
    double cy = std::cos(md.yaw), sy = std::sin(md.yaw);
    g_filter.u[0] = cy;  g_filter.u[1] = -sy;
    g_filter.v[0] = sy;  g_filter.v[1] = cy;
    for (int i = 0; i < 3; ++i) g_filter.center[i] = center[i];
    g_filter.useCut = md.cutOn;
    g_filter.cutY = center[1] - md.cutAboveM * kUnitsPerMeter;
    g_cutActive = false;
    g_viewCut = false;
    g_vcutActive = false;
    g_vcutLastSector = 0;
    g_vcutClass = 0;
    g_tunnelLastSector = 0;
    g_cutLastSector = 0;
    g_cutLastClass = 0;
    g_tunnelClass = 0;
    g_mirrorDepth = 0;
    for (int i = 0; i < g_hiddenWaterCurCount; ++i) g_hiddenWaterPrev[i] = g_hiddenWaterCur[i];
    g_hiddenWaterPrevCount = g_hiddenWaterCurCount;
    g_hiddenWaterCurCount = 0;
    for (int i = 0; i < g_clipWaterCurCount; ++i) g_clipWaterPrev[i] = g_clipWaterCur[i];
    g_clipWaterPrevCount = g_clipWaterCurCount;
    g_clipWaterCurCount = 0;
    for (int i = 0; i < g_waterBoxCurCount; ++i) g_waterBoxPrev[i] = g_waterBoxCur[i];
    g_waterBoxPrevCount = g_waterBoxCurCount;
    g_waterBoxCurCount = 0;
    g_reflClipWater = nullptr;
    double fw[16];
    g_passFwValid = ReadPoseFromWorld(pose, fw);
    if (!g_passFwValid) return;
    for (int i = 0; i < 16; ++i) g_passFw[i] = fw[i];
    g_gpuShadowRadiusM = kGpuShadowRadiusM;
    if (md.scale > 0.0 && kGpuShadowRadiusScaleM / md.scale < g_gpuShadowRadiusM) {
        g_gpuShadowRadiusM = kGpuShadowRadiusScaleM / md.scale;
        if (g_gpuShadowRadiusM < kGpuShadowRadiusMinM) g_gpuShadowRadiusM = kGpuShadowRadiusMinM;
    }
    if (md.vcutOn) {
        // Plano vertical a vcutM por delante del personaje, con la normal
        // del modelo (ejes del mapa, o de cara a la vista y girando con la
        // maqueta por la espalda: VcutDirection; recto: StraightVcut). Se
        // conserva lo de detras.
        double dir[2];
        VcutDirection(md, dir);
        double offM = md.vcutM;
        if (md.vcutStraight) StraightVcut(md, dir, &offM);
        double nx = dir[0], nz = dir[1];
        double off = offM * kUnitsPerMeter;
        double px = center[0] - nx * off, pz = center[2] - nz * off;
        g_vcutPlaneW[0] = nx;
        g_vcutPlaneW[1] = 0.0;
        g_vcutPlaneW[2] = nz;
        g_vcutPlaneW[3] = -(nx * px + nz * pz);
        // En camara (como +0x71d70): n' = n M, d' = d - n'.t.
        double nc[3];
        for (int j = 0; j < 3; ++j) nc[j] = nx * fw[0 * 4 + j] + nz * fw[2 * 4 + j];
        g_vcutPlaneCam[0] = nc[0];
        g_vcutPlaneCam[1] = nc[1];
        g_vcutPlaneCam[2] = nc[2];
        g_vcutPlaneCam[3] = g_vcutPlaneW[3] - (nc[0] * fw[12] + nc[1] * fw[13] + nc[2] * fw[14]);
        g_vcutActive = true;
    }
    if (!(md.cutOn || coneOn)) return;
    if (md.cutOn) {
        // Plano horizontal y = cutY llevado a camara: n = fila 1 de M.
        double n[3] = { fw[4], fw[5], fw[6] };
        g_cutPlaneCam[0] = n[0];
        g_cutPlaneCam[1] = n[1];
        g_cutPlaneCam[2] = n[2];
        g_cutPlaneCam[3] = -g_filter.cutY - (n[0] * fw[12] + n[1] * fw[13] + n[2] * fw[14]);
        g_cutActive = true;
    }
    if (coneOn) {
        // Personaje en espacio de camara: c = p * M + t (vectores fila).
        double c[3];
        for (int j = 0; j < 3; ++j) {
            c[j] = center[0] * fw[0 * 4 + j] + center[1] * fw[1 * 4 + j] + center[2] * fw[2 * 4 + j] + fw[12 + j];
        }
        double len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        // Muy cerca (maqueta grande y el ojo junto al personaje) el final del
        // cono se acerca al ojo en proporcion.
        double ahead = kViewCutAheadM * kUnitsPerMeter;
        if (ahead > kViewCutAheadMaxFrac * len) ahead = kViewCutAheadMaxFrac * len;
        int cw = g_coneWidth.load(std::memory_order_relaxed);
        if (cw < 0 || cw >= kConeWidthCount) cw = 1;
        double radius = kConeRadiusRealM[cw] * md.scale;
        if (radius < kConeMinRadiusM[cw]) radius = kConeMinRadiusM[cw];
        radius *= kUnitsPerMeter;
        double eyeRadius = kViewConeEyeFrac * radius;
        if (len > 0.05 * kUnitsPerMeter) {
            // Suelo: se conserva y > groundY (Y hacia abajo), en camara.
            double groundY = center[1] + kConeGroundBelowTargetM * kUnitsPerMeter;
            g_coneGround[0] = fw[4];
            g_coneGround[1] = fw[5];
            g_coneGround[2] = fw[6];
            g_coneGround[3] = -groundY - (fw[4] * fw[12] + fw[5] * fw[13] + fw[6] * fw[14]);
            double n[3] = { c[0] / len, c[1] / len, c[2] / len };
            // Plano trasero: se conserva lo que queda mas lejos que
            // (len - ahead) en la direccion camara -> personaje.
            g_conePlanes[0][0] = n[0];
            g_conePlanes[0][1] = n[1];
            g_conePlanes[0][2] = n[2];
            g_conePlanes[0][3] = -(len - ahead);
            // Base perpendicular al eje (e1 desde el eje X de la camara).
            double e1[3] = { 1.0 - n[0] * n[0], -n[0] * n[1], -n[0] * n[2] };
            double l1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
            if (l1 < 1e-6) { e1[0] = -n[1] * n[0]; e1[1] = 1.0 - n[1] * n[1]; e1[2] = -n[1] * n[2];
                             l1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]); }
            for (int j = 0; j < 3; ++j) e1[j] /= l1;
            double e2[3] = { n[1] * e1[2] - n[2] * e1[1], n[2] * e1[0] - n[0] * e1[2], n[0] * e1[1] - n[1] * e1[0] };
            // Tronco de cono: radio eyeRadius en el ojo y radius junto al
            // personaje (apertura limitada).
            double half = std::atan2(radius - eyeRadius, len);
            if (half > kConeMaxHalfAngle[cw]) half = kConeMaxHalfAngle[cw];
            // Redondo (el nivel en la GPU lo recorta por pixel): mismo area
            // que la piramide (radio 1,05 veces su apotema); la piramide de
            // abajo lo circunscribe (la usa el motor).
            g_coneRound = true;
            if (g_coneRound) {
                eyeRadius *= kConeRoundScale;
                half = std::atan(std::tan(half) * kConeRoundScale);
            }
            double ch = std::cos(half), sh = std::sin(half);
            for (int j = 0; j < 3; ++j) g_coneAxis[j] = n[j];
            g_coneCosA = ch;
            g_coneSinA = sh;
            g_coneEyeRCos = eyeRadius * ch;
            if (g_coneRound) {
                // Piramide inscrita: apotema = radio por cos 30.
                const double c30 = 0.8660254037844386;
                double hin = std::atan(std::tan(half) * c30);
                double chi = std::cos(hin), shi = std::sin(hin);
                for (int i = 0; i < kConeSides; ++i) {
                    double phi = 2.0 * kPi * i / kConeSides;
                    double cp = std::cos(phi), sp = std::sin(phi);
                    for (int j = 0; j < 3; ++j) g_conePlanesIn[i][j] = chi * (cp * e1[j] + sp * e2[j]) - shi * n[j];
                    g_conePlanesIn[i][3] = -eyeRadius * c30 * chi;
                }
            }
            // Caras (normal hacia fuera m = cos(a) d - sin(a) n): a la altura
            // del ojo pasan a eyeRadius del eje; se conserva
            // m . v - eyeRadius cos(a) > 0.
            for (int i = 0; i < kConeSides; ++i) {
                double phi = 2.0 * kPi * i / kConeSides;
                double cp = std::cos(phi), sp = std::sin(phi);
                for (int j = 0; j < 3; ++j) g_conePlanes[1 + i][j] = ch * (cp * e1[j] + sp * e2[j]) - sh * n[j];
                g_conePlanes[1 + i][3] = -eyeRadius * ch;
            }
            // Para elegir los sectores: cilindro camara -> personaje (algo
            // mas ancho que el cono; lo que sobre solo se dibuja entero).
            for (int r = 0; r < 3; ++r) {
                g_tunnelA[r] = -(fw[12] * fw[r * 4 + 0] + fw[13] * fw[r * 4 + 1] + fw[14] * fw[r * 4 + 2]);
                g_tunnelB[r] = center[r];
            }
            g_tunnelRadius = g_coneRound ? radius * kConeRoundScale : radius;
            g_viewCut = true;
        }
    }
}


// Nivel (singleton +0x7ceb0), llamado desde el hilo del juego.
static long long GetLevelRaw() {
    __try {
        using PFN_GetLevelLocal = long long(__fastcall*)();
        return reinterpret_cast<PFN_GetLevelLocal>(g_base + kGetLevelOffset)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Area visible para el nivel en la GPU: el mismo criterio que la lista de
// sectores del motor (FillDioramaSectorsRaw).
static bool SectorInAreaForGpu(const double* mn, const double* mx) {
    if (g_filter.useRadius && BoxDistanceSqXZ(mn, mx, g_filter.center) > g_filter.radiusSq) return false;
    if (g_filter.useSquare && !BoxTouchesSquareXZ(mn, mx, &g_filter)) return false;
    return true;
}

static_assert(kConePlaneCount + 1 == kGpuConePlaneCount, "planos del cono");

// Caja del nivel: union de las cajas de los sectores (los del area visible
// si la hay). Sin destructores: __try.
static bool LevelBoxRaw(long long level, const SectorFilter* f, double mn[3], double mx[3]) {
    for (int k = 0; k < 3; ++k) {
        mn[k] = 1e300;
        mx[k] = -1e300;
    }
    bool any = false;
    __try {
        if (!level) return false;
        unsigned int count = *reinterpret_cast<unsigned int*>(level + kLevelSectorCount);
        char** sectors = *reinterpret_cast<char***>(level + kLevelSectorArray);
        if (!sectors || count == 0 || count > 200000) return false;
        for (unsigned int i = 0; i < count; ++i) {
            const char* s = sectors[i];
            if (!s) continue;
            const double* a = reinterpret_cast<const double*>(s + kSectorBoxMin);
            const double* b = reinterpret_cast<const double*>(s + kSectorBoxMax);
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                if (!(a[k] == a[k]) || !(b[k] == b[k]) || a[k] < -1e8 || b[k] > 1e8 || a[k] > b[k]) ok = false;
            }
            if (!ok) continue;
            if (f->useRadius && BoxDistanceSqXZ(a, b, f->center) > f->radiusSq) continue;
            if (f->useSquare && !BoxTouchesSquareXZ(a, b, f)) continue;
            for (int k = 0; k < 3; ++k) {
                if (a[k] < mn[k]) mn[k] = a[k];
                if (b[k] > mx[k]) mx[k] = b[k];
            }
            any = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return any;
}

// Tras PrepareFilterAndCut: la caja del nivel y el personaje, para los mandos.
// La de los sectores, ajustada a la geometria capturada (si la hay; con un
// margen para lo que sobresale: objetos, antorchas).
static constexpr double kCutRangeMarginM = 2.0;
static void UpdateCutRange() {
    double mn[3], mx[3];
    long long level = GetLevelRaw();
    bool ok = LevelBoxRaw(level, &g_filter, mn, mx);
    double gmn[3], gmx[3];
    if (ok && GpuLevelBounds(level, gmn, gmx)) {
        double m = kCutRangeMarginM * kUnitsPerMeter;
        for (int k = 0; k < 3; ++k) {
            double lo = gmn[k] - m, hi = gmx[k] + m;
            double a = mn[k] > lo ? mn[k] : lo;
            double b = mx[k] < hi ? mx[k] : hi;
            if (a < b) {
                mn[k] = a;
                mx[k] = b;
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_modelMutex);
    g_cutRange.valid = ok;
    for (int k = 0; k < 3; ++k) {
        if (ok) {
            g_cutRange.mn[k] = mn[k];
            g_cutRange.mx[k] = mx[k];
        }
        g_cutRange.center[k] = g_filter.center[k];
    }
}

// Recuento de planos del frustum de la pasada (renderArg3 + 0x18 + 0x300;
// las visitas los copian): a 0 en la pasada de captura, y de vuelta al final.
static constexpr int kPlaneSetCountOffset = 0x300;

static bool ZeroPassPlaneCountRaw(long long renderArg3) {
    __try {
        unsigned int* p = reinterpret_cast<unsigned int*>(renderArg3 + kRenderArgPlanes + kPlaneSetCountOffset);
        g_capturePlaneCountSaved = *p;
        *p = 0;
        g_capturePlaneCountPtr = p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_capturePlaneCountPtr = nullptr;
        return false;
    }
    return true;
}

static void RestorePassPlaneCountRaw() {
    if (!g_capturePlaneCountPtr) return;
    __try {
        *g_capturePlaneCountPtr = g_capturePlaneCountSaved;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_capturePlaneCountPtr = nullptr;
}

static void BeginGpuLevelPass(long long renderArg3) {
    g_gpuSkipFaces = false;
    g_gpuCapturePass = false;
    if (!g_passFwValid) return;
    double cone[kGpuConePlaneCount][4];
    if (g_coneRound) {
        // Ver GpuLevelPass::coneRound: trasero, eje (por el vertice), vertice
        // (el ojo = origen de la camara), (cos, sin, r_ojo cos, 1), suelo.
        for (int i = 0; i < kGpuConePlaneCount; ++i) for (int k = 0; k < 4; ++k) cone[i][k] = 0.0;
        for (int k = 0; k < 4; ++k) cone[0][k] = g_conePlanes[0][k];
        for (int k = 0; k < 3; ++k) cone[1][k] = g_coneAxis[k];
        cone[3][0] = g_coneCosA;
        cone[3][1] = g_coneSinA;
        cone[3][2] = g_coneEyeRCos;
        cone[3][3] = 1.0;
        for (int k = 0; k < 4; ++k) cone[kGpuConePlaneCount - 1][k] = g_coneGround[k];
    } else {
        for (int i = 0; i < kGpuConePlaneCount; ++i) {
            const double* m = i < kConePlaneCount ? g_conePlanes[i] : g_coneGround;
            for (int k = 0; k < 4; ++k) cone[i][k] = m[k];
        }
    }
    GpuLevelPass gp{};
    gp.level = GetLevelRaw();
    gp.fromWorld = g_passFw;
    gp.sectorInArea = (g_filter.useRadius || g_filter.useSquare) ? &SectorInAreaForGpu : nullptr;
    gp.cutOn = g_cutActive;
    gp.cutY = g_filter.cutY;
    gp.coneOn = g_viewCut;
    gp.conePlanes = cone;
    gp.coneRound = g_viewCut && g_coneRound;
    gp.skyOn = g_background.load(std::memory_order_relaxed) == 0;
    gp.vcutOn = g_vcutActive;
    gp.vcutPlane = g_vcutPlaneW;
    gp.vcutPlaneCam = g_vcutPlaneCam;
    // Pose del visor de esta camara (si la pasada usa la matriz que salio de
    // ella): se envia con la imagen de este fotograma.
    bool poseOk = g_camPoseValid;
    for (int i = 0; i < 16 && poseOk; ++i) poseOk = g_camPoseFw[i] == g_passFw[i];
    gp.hmdPose = poseOk ? g_camPose : nullptr;
    g_shadowLastSector = 0;
    int r = GpuLevelBeginPass(g_base, gp);
    if (r == kGpuPassGpu) {
        g_gpuSkipFaces = true;
    } else if (r == kGpuPassCapture) {
        // Todo el mapa, sin corte ni cono y sin frustum: el motor dibuja
        // todas sus caras (tambien las que quedan detras) y se capturan.
        g_gpuCapturePass = true;
        g_filter.useRadius = false;
        g_filter.useSquare = false;
        g_cutActive = false;
        g_viewCut = false;
        g_vcutActive = false;
        ZeroPassPlaneCountRaw(renderArg3);
    }
}

void DioramaBeginWorld(bool active, long long pose, long long renderArg3) {
    if (active && (!renderArg3 || !pose)) active = false;
    g_worldActive = active;
    g_shadowOnly = false;
    g_renderPlanes = active ? reinterpret_cast<const char*>(renderArg3 + kRenderArgPlanes) : nullptr;
    g_gpuSkipFaces = false;
    if (active) {
        EnsureFogHook();
        EnsureMirrorBandsHook();
        PrepareFilterAndCut(pose);
        UpdateCutRange();
        BeginGpuLevelPass(renderArg3);
        // Pasada de captura: sin sombras de objetos (se devuelve el valor del
        // juego en DioramaEndWorld).
        int flag = 0;
        if (g_gpuCapturePass && !g_shadowsSaved && ReadShadowFlagRaw(&flag)) {
            g_savedShadowFlag = flag;
            g_shadowsSaved = true;
            if (flag != 0) WriteShadowFlagRaw(0);
        }
    } else {
        g_cutActive = false;
        g_viewCut = false;
        g_vcutActive = false;
    }
    ApplyBackground(active && g_background.load(std::memory_order_relaxed) != 0);
    g_drawActive.store(active, std::memory_order_relaxed);
}

void DioramaEndWorld() {
    GpuLevelEndPass();
    RestorePassPlaneCountRaw();
    g_gpuCapturePass = false;
    g_gpuSkipFaces = false;
    g_waterClipTag = false;
    g_worldActive = false;
    g_shadowRedrawDepth = 0;
    g_shadowOnly = false;
    g_renderPlanes = nullptr;
    g_cutActive = false;
    g_viewCut = false;
    g_vcutActive = false;
    if (g_threshold) *g_threshold = kKeepFaceThreshold;
    if (g_shadowsSaved) {
        if (g_savedShadowFlag != 0) WriteShadowFlagRaw(g_savedShadowFlag);
        g_shadowsSaved = false;
    }
}

bool DioramaDrawActive() { return g_drawActive.load(std::memory_order_relaxed); }
bool DioramaViewSteeringAllowed() {
    std::lock_guard<std::mutex> lock(g_modelMutex);
    return g_model.mode != kModeBehind;
}
bool DioramaEnabled() { return g_installed && g_enabled.load(std::memory_order_relaxed); }
void DioramaRequestAnchor() { g_anchorRequest.store(1, std::memory_order_relaxed); }
double DioramaScale() { return g_scaleMirror.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------
// Mando del juego por Steam Input
//
// L3 cambia todo el mando entre el juego y la capa de la maqueta (siempre;
// el juego no recibe L3). Con la capa puesta el juego no recibe los botones
// ni los sticks ni los gatillos (salvo Start y Back) y son de la maqueta: R3
// diorama si/no (mantenido: personajes en los cortes), X recolocar la mesa,
// LB modos, RB area, cruceta izquierda/derecha cono si/no, cruceta
// arriba/abajo ancho del cono, Y passthrough, A corte vertical delante del
// personaje (mantenido: recto o girando con la maqueta por la espalda), B
// corte de altura sobre el personaje, stick izquierdo mover la mesa, stick
// derecho rodear y zoom, RT/LT zoom.
//
// El juego lee el mando con ISteamInput (SteamInput006): cada fotograma
// pide, por nombre de accion, GetDigitalActionData (vtable+0x88; el asa con
// GetDigitalActionHandle, +0x80) y GetAnalogActionData (+0xa8; asa con
// +0xa0), ver +0x271000.. (acciones del manifiesto
// game_actions_1710170.vdf). Se enganchan esas dos funciones del interfaz:
// se apunta lo que llega y, con la capa de la maqueta, el juego lo recibe a
// cero.
// ---------------------------------------------------------------------
struct SteamDigitalData { bool bState; bool bActive; };
struct SteamAnalogData { int eMode; float x; float y; bool bActive; };
using PFN_SteamGetHSteamUser = int(__cdecl*)();
using PFN_SteamFindOrCreateUserInterface = void*(__cdecl*)(int, const char*);
using PFN_SteamGetHandle = unsigned long long(__fastcall*)(void*, const char*);
using PFN_SteamGetDigital = SteamDigitalData*(__fastcall*)(void*, SteamDigitalData*, unsigned long long, unsigned long long);
using PFN_SteamGetAnalog = SteamAnalogData*(__fastcall*)(void*, SteamAnalogData*, unsigned long long, unsigned long long);
static constexpr int kSteamGetDigitalHandleSlot = 0x80 / 8;
static constexpr int kSteamGetDigitalDataSlot = 0x88 / 8;
static constexpr int kSteamGetAnalogHandleSlot = 0xa0 / 8;
static constexpr int kSteamGetAnalogDataSlot = 0xa8 / 8;

static PFN_SteamGetHandle g_steamGetDigitalHandle = nullptr;
static PFN_SteamGetHandle g_steamGetAnalogHandle = nullptr;
static PFN_SteamGetDigital g_origSteamGetDigital = nullptr;
static PFN_SteamGetAnalog g_origSteamGetAnalog = nullptr;
static void* g_steamDigitalTarget = nullptr;
static void* g_steamAnalogTarget = nullptr;
static bool g_steamHooksTried = false;
static std::atomic<bool> g_steamHooksOk{false};
// Acciones del manifiesto (game_actions_1710170.vdf): digitales y analogicas.
enum PadButton {
    kPadA, kPadB, kPadX, kPadY, kPadLB, kPadRB, kPadUp, kPadDown, kPadLeft, kPadRight, kPadL3, kPadR3,
    kPadButtonCount
};
static const char* const kPadButtonNames[kPadButtonCount] = {
    "ButtonSouth", "ButtonEast", "ButtonWest", "ButtonNorth", "ButtonLeftShoulder", "ButtonRightShoulder",
    "ButtonUp", "ButtonDown", "ButtonLeft", "ButtonRight", "ButtonLeftStick", "ButtonRightStick"
};
enum PadAxis { kPadLeftStick, kPadRightStick, kPadLT, kPadRT, kPadAxisCount };
static const char* const kPadAxisNames[kPadAxisCount] = {
    "LeftJoystick", "RightJoystick", "ButtonLeftTrigger", "ButtonRightTrigger"
};
// Asas (se piden en el hilo del juego, dentro del gancho, como hace el
// juego en cada fotograma). Sin mando encendido Steam devuelve 0 (aun no
// tiene la configuracion de acciones del juego): se vuelven a pedir cada
// kSteamResolveMs hasta tenerlas todas y despues cada kSteamRecheckMs, por
// si el mando se enciende o se cambia con el juego abierto.
static unsigned long long g_hPadButton[kPadButtonCount] = {};
static unsigned long long g_hPadAxis[kPadAxisCount] = {};
static constexpr uint64_t kSteamResolveMs = 250;
static constexpr uint64_t kSteamRecheckMs = 2000;
static uint64_t g_steamResolveTick = 0;
static bool g_steamHandlesAll = false;
static bool g_padDown[kPadButtonCount] = {};                 // hilo del juego
static std::atomic<bool> g_padHeld[kPadButtonCount];         // apretado ahora
static std::atomic<uint32_t> g_padPresses[kPadButtonCount];  // pulsaciones
static std::atomic<float> g_padAxisX[kPadAxisCount];
static std::atomic<float> g_padAxisY[kPadAxisCount];
static std::atomic<bool> g_padLayer{false};                  // el mando es de la maqueta

static void ResolveSteamHandles(void* self) {
    uint64_t now = GetTickCount64();
    uint64_t wait = g_steamHandlesAll ? kSteamRecheckMs : kSteamResolveMs;
    if (g_steamResolveTick != 0 && now - g_steamResolveTick < wait) return;
    g_steamResolveTick = now;
    bool all = true;
    for (int b = 0; b < kPadButtonCount; ++b) {
        unsigned long long h = g_steamGetDigitalHandle ? g_steamGetDigitalHandle(self, kPadButtonNames[b]) : 0;
        if (h) g_hPadButton[b] = h;
        if (!g_hPadButton[b]) all = false;
    }
    for (int a = 0; a < kPadAxisCount; ++a) {
        unsigned long long h = g_steamGetAnalogHandle ? g_steamGetAnalogHandle(self, kPadAxisNames[a]) : 0;
        if (h) g_hPadAxis[a] = h;
        if (!g_hPadAxis[a]) all = false;
    }
    g_steamHandlesAll = all;
}

static SteamDigitalData* __fastcall HookedSteamGetDigital(void* self, SteamDigitalData* out,
                                                         unsigned long long controller, unsigned long long handle) {
    SteamDigitalData* r = g_origSteamGetDigital(self, out, controller, handle);
    ResolveSteamHandles(self);
    if (out && handle != 0) {
        for (int b = 0; b < kPadButtonCount; ++b) {
            if (handle != g_hPadButton[b]) continue;
            bool down = out->bState;
            g_padHeld[b].store(down, std::memory_order_relaxed);
            if (down && !g_padDown[b]) g_padPresses[b].fetch_add(1, std::memory_order_relaxed);
            g_padDown[b] = down;
            // L3 siempre es de la maqueta (cambia la capa); con la capa, todos.
            if (b == kPadL3 || g_padLayer.load(std::memory_order_relaxed)) out->bState = false;
            break;
        }
    }
    return r;
}

static SteamAnalogData* __fastcall HookedSteamGetAnalog(void* self, SteamAnalogData* out,
                                                       unsigned long long controller, unsigned long long handle) {
    SteamAnalogData* r = g_origSteamGetAnalog(self, out, controller, handle);
    ResolveSteamHandles(self);
    if (out && handle != 0) {
        for (int a = 0; a < kPadAxisCount; ++a) {
            if (handle != g_hPadAxis[a]) continue;
            g_padAxisX[a].store(out->x, std::memory_order_relaxed);
            g_padAxisY[a].store(out->y, std::memory_order_relaxed);
            if (g_padLayer.load(std::memory_order_relaxed)) {
                out->x = 0.0f;
                out->y = 0.0f;
            }
            break;
        }
    }
    return r;
}

static bool ReadSteamInputVtable(void* iface, void** digitalHandle, void** digitalData, void** analogHandle,
                                 void** analogData) {
    __try {
        void** vt = *reinterpret_cast<void***>(iface);
        if (!vt) return false;
        *digitalHandle = vt[kSteamGetDigitalHandleSlot];
        *digitalData = vt[kSteamGetDigitalDataSlot];
        *analogHandle = vt[kSteamGetAnalogHandleSlot];
        *analogData = vt[kSteamGetAnalogDataSlot];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *digitalHandle && *digitalData && *analogHandle && *analogData;
}

// Hilo de Present, una vez (cuando el juego ya tiene Steam iniciado).
static void TryInstallSteamInputHooks() {
    if (g_steamHooksTried) return;
    HMODULE steam = GetModuleHandleA("steam_api64.dll");
    if (!steam) return;
    auto getUser = reinterpret_cast<PFN_SteamGetHSteamUser>(GetProcAddress(steam, "SteamAPI_GetHSteamUser"));
    auto findIface = reinterpret_cast<PFN_SteamFindOrCreateUserInterface>(
        GetProcAddress(steam, "SteamInternal_FindOrCreateUserInterface"));
    if (!getUser || !findIface) {
        g_steamHooksTried = true;
        HookLogger::Instance().Line("[DIORAMA] AVISO: steam_api64.dll sin las funciones esperadas; el mando no controla la maqueta.");
        return;
    }
    int user = getUser();
    if (user == 0) return;   // aun no: se reintenta
    g_steamHooksTried = true;
    void* iface = findIface(user, "SteamInput006");
    void *dh = nullptr, *dd = nullptr, *ah = nullptr, *ad = nullptr;
    if (!iface || !ReadSteamInputVtable(iface, &dh, &dd, &ah, &ad)) {
        HookLogger::Instance().Line("[DIORAMA] AVISO: no se encontro ISteamInput (SteamInput006); el mando no controla la maqueta.");
        return;
    }
    g_steamGetDigitalHandle = reinterpret_cast<PFN_SteamGetHandle>(dh);
    g_steamGetAnalogHandle = reinterpret_cast<PFN_SteamGetHandle>(ah);
    bool ok = MH_CreateHook(dd, reinterpret_cast<void*>(&HookedSteamGetDigital),
                            reinterpret_cast<void**>(&g_origSteamGetDigital)) == MH_OK;
    if (ok) {
        g_steamDigitalTarget = dd;
        ok = MH_EnableHook(dd) == MH_OK;
    }
    if (ok) {
        ok = MH_CreateHook(ad, reinterpret_cast<void*>(&HookedSteamGetAnalog),
                           reinterpret_cast<void**>(&g_origSteamGetAnalog)) == MH_OK;
        if (ok) {
            g_steamAnalogTarget = ad;
            ok = MH_EnableHook(ad) == MH_OK;
        }
    }
    g_steamHooksOk.store(ok, std::memory_order_relaxed);
    if (!ok) {
        HookLogger::Instance().Line("[DIORAMA] AVISO: no se pudieron enganchar las funciones de Steam Input; el mando no controla la maqueta.");
    }
}

static void UninstallSteamInputHooks() {
    if (g_steamDigitalTarget) { MH_DisableHook(g_steamDigitalTarget); g_steamDigitalTarget = nullptr; }
    if (g_steamAnalogTarget) { MH_DisableHook(g_steamAnalogTarget); g_steamAnalogTarget = nullptr; }
    g_steamHooksOk.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------
// Mandos (hilo de Present): mandos de Quest (entrada heredada de SteamVR,
// OpenVRHook) y mando del juego con la capa de la maqueta. Asignaciones en
// DioramaHook.h.
// ---------------------------------------------------------------------
enum GestureMode { kGestureNone = 0, kGestureMove, kGestureScale, kGestureArea, kGestureCut };

struct Gesture {
    int mode = kGestureNone;
    int hand = -1;
    Model start;
    double hand0[3] = {0.0, 0.0, 0.0};     // mano del gesto de una mano
    double lastOne[3] = {0.0, 0.0, 0.0};   // ... y su ultima posicion
    double mid0[3] = {0.0, 0.0, 0.0};      // punto medio entre las manos
    double span0 = 0.0;                    // distancia entre las manos
    double angle0 = 0.0;                   // angulo horizontal de la recta entre las manos
    bool angleValid = false;
    double grabbed[3] = {0.0, 0.0, 0.0};   // punto del juego bajo el punto medio
    uint64_t startTick = 0;
    double inDir[2] = {0.0, 1.0};          // corte: direccion horizontal cabeza -> maqueta (seguimiento)
    bool inValid = false;
    int cutAxis = 0;                       // corte: 0 sin decidir, 1 altura, 2 vertical
    // Rango de los cortes con la caja del nivel (al empezar el gesto): la
    // caja y, en metros del juego relativos al personaje, la altura sin
    // quitar nada (arriba del todo) y con todo quitado. Vertical: VcutEdges.
    bool rangeValid = false;
    CutRange range = {};
    double hTop = 0.0, hBottom = 0.0;
};
static Gesture g_gesture;

static constexpr double kStickDeadzone = 0.15;
static constexpr double kStickPanMps = 0.5;           // stick izquierdo: metros reales por segundo
static constexpr double kStickTurnRadPs = kPi / 2.0;  // stick derecho: 90 grados por segundo
static constexpr double kStickZoomPerS = 0.6931;      // zoom: el doble (o la mitad) por segundo
static constexpr double kButtonLiftMps = 0.3;         // A / B

static void HandPosition(const VrHandState& h, double out[3]) {
    out[0] = h.pose[3];
    out[1] = -h.pose[7];     // ejes del juego: (x, -y, -z)
    out[2] = -h.pose[11];
}

// Para el aviso en el visor (texto ancho; las tildes como \u para que el
// fuente siga siendo ASCII).
static const wchar_t* ModeNameW(int m) {
    switch (m) {
        case 1: return L"2 \u00b7 Acoplada al personaje";
        case 2: return L"3 \u00b7 Acoplada, por la espalda";
        default: return L"1 \u00b7 Fija";
    }
}

static std::wstring AreaTextW(const Model& md) {
    if (md.areaShape == 0 || md.areaM <= 0.0) return L"Sin recorte (mapa entero)";
    wchar_t buf[96];
    swprintf(buf, 96, L"%ls, %d m alrededor", md.areaShape == 2 ? L"Cuadrada" : L"Redonda",
             static_cast<int>(md.areaM + 0.5));
    return buf;
}

static constexpr unsigned kToastMs = 2500;
// Zona muerta de la mano con el gatillo (lo que pase de ella cuenta entero).
static double HandDeadzone(double d) {
    if (d > kCutHandDeadzoneM) return d - kCutHandDeadzoneM;
    if (d < -kCutHandDeadzoneM) return d + kCutHandDeadzoneM;
    return 0.0;
}

static void CycleModelMode() {
    int m;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        // El corte vertical se queda donde esta (sin salto al cambiar de modo).
        double dir[2];
        VcutDirection(g_model, dir);
        if (g_model.vcutStraight) {
            int ax = g_vsAxis;
            if (ax < 0 || ax > 3) ax = NearestMapAxis(dir);
            dir[0] = kMapAxisDir[ax][0];
            dir[1] = kMapAxisDir[ax][1];
        }
        g_model.vcutDir[0] = dir[0];
        g_model.vcutDir[1] = dir[1];
        g_model.vcutYawRef = g_model.yaw;
        g_model.mode = (g_model.mode + 1) % kModelModeCount;
        m = g_model.mode;
    }
    OpenVRShowToast(L"Modo de la maqueta", ModeNameW(m), kToastMs);
}

// Ancho del cono de vision, siguiente (cruceta arriba / abajo del mando; F4
// y el clic largo del stick izquierdo van por CycleConeOffAndWidth).
static void CycleConeWidth() {
    int next = (g_coneWidth.load(std::memory_order_relaxed) + 1) % kConeWidthCount;
    g_coneWidth.store(next, std::memory_order_relaxed);
    bool on = g_coneOn.load(std::memory_order_relaxed);
    static const wchar_t* const kNamesW[kConeWidthCount] = { L"Peque\u00f1o", L"Mediano", L"Grande" };
    static const wchar_t* const kOffW[kConeWidthCount] = { L"Peque\u00f1o (apagado)", L"Mediano (apagado)",
                                                           L"Grande (apagado)" };
    OpenVRShowToast(L"Cono de visi\u00f3n", on ? kNamesW[next] : kOffW[next], kToastMs);
}

// Cono de vision encendido / apagado (cruceta izquierda o derecha del mando
// con la capa de la maqueta), en cualquier modo; empieza apagado.
static void ToggleCone() {
    bool next = !g_coneOn.load(std::memory_order_relaxed);
    g_coneOn.store(next, std::memory_order_relaxed);
    int cw = g_coneWidth.load(std::memory_order_relaxed);
    if (cw < 0 || cw >= kConeWidthCount) cw = 1;
    static const wchar_t* const kOnW[kConeWidthCount] = { L"Encendido (peque\u00f1o)", L"Encendido (mediano)",
                                                          L"Encendido (grande)" };
    OpenVRShowToast(L"Cono de visi\u00f3n", next ? kOnW[cw] : L"Apagado", kToastMs);
}

// F4 y clic largo del stick izquierdo (mandos Quest), sin botones libres para
// un interruptor aparte: apagado -> pequeno -> mediano -> grande -> apagado.
static void CycleConeOffAndWidth() {
    if (!g_coneOn.load(std::memory_order_relaxed)) {
        g_coneWidth.store(0, std::memory_order_relaxed);
        ToggleCone();
    } else if (g_coneWidth.load(std::memory_order_relaxed) >= kConeWidthCount - 1) {
        ToggleCone();
    } else {
        CycleConeWidth();
    }
}

// Personajes en los cortes (Pausa, clic largo del stick derecho, R3
// mantenido del mando): siempre a la vista <-> se cortan como el resto.
static void ToggleCutPersons() {
    bool next = !g_cutPersons.load(std::memory_order_relaxed);
    g_cutPersons.store(next, std::memory_order_relaxed);
    OpenVRShowToast(L"Personajes en los cortes", next ? L"Se cortan" : L"Siempre a la vista", kToastMs);
}

// Cruceta arriba / abajo: un paso mas ancho / mas estrecho (sin dar la vuelta).
static void StepConeWidth(int delta) {
    int cur = g_coneWidth.load(std::memory_order_relaxed);
    int next = cur + delta;
    if (next < 0) next = 0;
    if (next >= kConeWidthCount) next = kConeWidthCount - 1;
    if (next == cur) {
        static const wchar_t* const kLimitW[2] = { L"Ya es el m\u00e1s peque\u00f1o", L"Ya es el m\u00e1s grande" };
        OpenVRShowToast(L"Cono de visi\u00f3n", kLimitW[delta > 0 ? 1 : 0], kToastMs);
        return;
    }
    g_coneWidth.store((next + kConeWidthCount - 1) % kConeWidthCount, std::memory_order_relaxed);
    CycleConeWidth();   // pasa al siguiente: el pedido
}

static void CycleAreaShape() {
    Model md;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        // Mapa entero -> cuadrada de 50 m -> cuadrada de 20 m -> mapa entero
        // (si el area se cambio con los gatillos, al siguiente paso por tamano).
        bool whole = g_model.areaShape == 0 || g_model.areaM <= 0.0;
        if (whole) {
            g_model.areaShape = 2;
            g_model.areaM = kAreaBigM;
        } else if (g_model.areaM > kAreaSmallM + 0.5) {
            g_model.areaShape = 2;
            g_model.areaM = kAreaSmallM;
        } else {
            g_model.areaShape = 0;
            g_model.cutOn = false;   // "sin recorte" = el mapa entero, tambien en altura
            g_model.vcutOn = false;  // y sin corte vertical
        }
        md = g_model;
    }
    OpenVRShowToast(L"\u00c1rea visible", AreaTextW(md).c_str(), kToastMs);
}

// Diorama si/no (F5, R3 con el mando en la capa de la maqueta).
static void ToggleDiorama() {
    bool next = !g_enabled.load(std::memory_order_relaxed);
    if (next) {
        g_anchorRequest.store(1, std::memory_order_relaxed);
        GpuLevelRetryCapture();   // si fallo la captura del nivel, se vuelve a intentar
    }
    g_enabled.store(next, std::memory_order_relaxed);
    if (!next) HeadTrackRequestRecenter();   // la vista vuelve a los ojos del personaje (como Inicio)
    Model md = SnapshotModel();
    HookLogger::Instance().Line(next ? "[DIORAMA] modo diorama activado." : "[DIORAMA] modo diorama desactivado.");
    OpenVRShowToast(next ? L"Diorama activado" : L"Diorama", next ? ModeNameW(md.mode) : L"Desactivado", kToastMs);
}

// Passthrough si/no con el ultimo color clave (Y de Quest, Y del mando).
static int g_lastChroma = 1;
static void TogglePassthrough() {
    int bg = g_background.load(std::memory_order_relaxed);
    int next;
    if (bg != 0) { g_lastChroma = bg; next = 0; } else { next = g_lastChroma; }
    g_background.store(next, std::memory_order_relaxed);
    OpenVRShowToast(L"Passthrough", next != 0 ? (next == 2 ? L"S\u00ed (verde)" : L"S\u00ed (magenta)") : L"No",
                    kToastMs);
}

// Direccion de la mirada hacia la maqueta en el juego (x, z, unitaria): la
// horizontal cabeza -> punto de la mesa, girada con la maqueta.
static bool ViewCutDirection(const Model& s, double out[2]) {
    float hm[12];
    if (!GetHmdPoseMatrix34(hm)) return false;
    double hx = hm[3], hz = -hm[11];
    double dx = s.table[0] - hx, dz = s.table[2] - hz;
    double dl = std::sqrt(dx * dx + dz * dz);
    if (dl < 0.05) {   // encima de la maqueta: el adelante de la mirada
        dx = -hm[2];
        dz = hm[10];
        dl = std::sqrt(dx * dx + dz * dz);
    }
    if (dl < 1e-6) return false;
    dx /= dl;
    dz /= dl;
    double cy = std::cos(s.yaw), sy = std::sin(s.yaw);
    out[0] = dx * cy + dz * sy;
    out[1] = -dx * sy + dz * cy;
    return true;
}

// A del mando (capa de la maqueta): corte vertical delante del personaje, en
// el lado que se mira (quita la pared que tapa; por la espalda, de cara a la
// mirada); otra vez, sin corte.
static constexpr double kPadVcutAheadM = 1.5;
static void TogglePadVcut() {
    Model s = SnapshotModel();
    double dir[2];
    bool haveDir = ViewCutDirection(s, dir);
    if (haveDir && s.mode != kModeBehind) SnapToMapAxis(dir);
    Model md;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        if (g_model.vcutOn) {
            g_model.vcutOn = false;
        } else {
            g_model.vcutOn = true;
            g_model.vcutM = kPadVcutAheadM;
            if (haveDir) {
                g_model.vcutDir[0] = dir[0];
                g_model.vcutDir[1] = dir[1];
                g_model.vcutYawRef = s.yaw;
            }
        }
        md = g_model;
    }
    OpenVRShowToast(L"Corte vertical", md.vcutOn ? L"Delante del personaje" : L"Sin corte", kToastMs);
}

// A mantenido (capa de la maqueta): corte vertical recto o de frente (solo
// cambia algo por la espalda: ver StraightVcut). Sin corte, lo pone.
static void ToggleVcutStraight() {
    Model s = SnapshotModel();
    double dir[2];
    bool haveDir = ViewCutDirection(s, dir);
    if (haveDir && s.mode != kModeBehind) SnapToMapAxis(dir);
    bool straight;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        g_model.vcutStraight = !g_model.vcutStraight;
        if (!g_model.vcutOn) {
            g_model.vcutOn = true;
            g_model.vcutM = kPadVcutAheadM;
            if (haveDir) {
                g_model.vcutDir[0] = dir[0];
                g_model.vcutDir[1] = dir[1];
                g_model.vcutYawRef = s.yaw;
            }
        }
        straight = g_model.vcutStraight;
    }
    OpenVRShowToast(L"Corte vertical", straight ? L"Recto: cambia de lado" : L"Gira con la maqueta", kToastMs);
}

// B del mando (capa de la maqueta): corte de altura un poco por encima del
// personaje (quita techos y pisos de arriba); otra vez, sin corte.
static constexpr double kPadHcutAboveM = 1.5;
static void TogglePadHcut() {
    Model md;
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        if (g_model.cutOn) {
            g_model.cutOn = false;
        } else {
            g_model.cutOn = true;
            g_model.cutAboveM = kPadHcutAboveM;
        }
        md = g_model;
    }
    OpenVRShowToast(L"Corte de altura", md.cutOn ? L"Sobre el personaje" : L"Sin corte", kToastMs);
}

// Estado de los botones y sticks del fotograma anterior (flancos).
static bool g_prevStickClick[2] = {false, false};
// Clic del stick izquierdo: corto (al soltar) = modo; mantenido = ancho del cono.
static constexpr uint64_t kLongPressMs = 500;
static uint64_t g_leftClickDownTick = 0;
static bool g_leftClickLong = false;
// Clic del stick derecho: corto (al soltar) = area; mantenido = personajes
// en los cortes. R3 del mando: corto = zoom con el stick derecho; mantenido
// = personajes en los cortes.
static uint64_t g_rightClickDownTick = 0;
static bool g_rightClickLong = false;
static bool g_padR3Pending = false;             // R3 del mando: corto o mantenido
static uint64_t g_padR3Tick = 0;
static bool g_padAPending = false;              // A del mando: corto (corte si/no) o mantenido (recto)
static uint64_t g_padATick = 0;
static bool g_prevButtonA[2] = {false, false};
static bool g_prevButtonB[2] = {false, false};
static uint32_t g_padSeen[kPadButtonCount] = {};
static LARGE_INTEGER g_lastControlsQpc = {};

// Botones del mando del juego (hilo de Present, siempre, tambien sin
// diorama: L3 cambia la capa y, con ella, R3 activa el diorama).
static void UpdatePadLayer() {
    bool fresh[kPadButtonCount];
    for (int b = 0; b < kPadButtonCount; ++b) {
        uint32_t n = g_padPresses[b].load(std::memory_order_relaxed);
        fresh[b] = n != g_padSeen[b];
        g_padSeen[b] = n;
    }
    if (!g_steamHooksOk.load(std::memory_order_relaxed)) return;
    if (fresh[kPadL3]) {
        bool next = !g_padLayer.load(std::memory_order_relaxed);
        g_padLayer.store(next, std::memory_order_relaxed);
        g_padR3Pending = false;
        g_padAPending = false;
        OpenVRShowToast(L"Mando", next ? L"Controla la maqueta (L3: juego)" : L"Controla el juego", kToastMs);
        return;
    }
    if (!g_padLayer.load(std::memory_order_relaxed)) {
        g_padR3Pending = false;
        g_padAPending = false;
        return;
    }
    uint64_t now = GetTickCount64();
    if (fresh[kPadR3]) {
        g_padR3Pending = true;
        g_padR3Tick = now;
    }
    if (g_padR3Pending) {
        bool held = g_padHeld[kPadR3].load(std::memory_order_relaxed);
        if (held && now - g_padR3Tick >= kLongPressMs) {
            g_padR3Pending = false;
            if (g_enabled.load(std::memory_order_relaxed)) ToggleCutPersons();
        } else if (!held) {
            g_padR3Pending = false;
            ToggleDiorama();
        }
    }
    if (!g_enabled.load(std::memory_order_relaxed)) {
        g_padAPending = false;
        return;
    }
    if (fresh[kPadA]) {
        g_padAPending = true;
        g_padATick = now;
    }
    if (g_padAPending) {
        bool held = g_padHeld[kPadA].load(std::memory_order_relaxed);
        if (held && now - g_padATick >= kLongPressMs) {
            g_padAPending = false;
            ToggleVcutStraight();
        } else if (!held) {
            g_padAPending = false;
            TogglePadVcut();
        }
    }
    if (fresh[kPadX]) {
        if (DioramaCinematicActive()) {
            g_cineRecenterRequest.store(1, std::memory_order_relaxed);   // cinematica: vista del juego
        } else {
            g_anchorLocateRequest.store(1, std::memory_order_relaxed);
            g_anchorRequest.store(1, std::memory_order_relaxed);
            OpenVRShowToast(L"Maqueta", L"Centrada en el personaje (1:15)", kToastMs);
        }
    }
    if (fresh[kPadLB]) CycleModelMode();
    if (fresh[kPadRB]) CycleAreaShape();
    if (fresh[kPadUp]) StepConeWidth(+1);
    if (fresh[kPadDown]) StepConeWidth(-1);
    if (fresh[kPadLeft] || fresh[kPadRight]) ToggleCone();
    if (fresh[kPadY]) TogglePassthrough();
    if (fresh[kPadB]) TogglePadHcut();
}

static void UpdateStickAndButtonControls(bool hl, const VrHandState& L, bool hr, const VrHandState& R,
                                         bool gestureActive) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double dt = (g_lastControlsQpc.QuadPart != 0 && freq.QuadPart > 0)
        ? static_cast<double>(now.QuadPart - g_lastControlsQpc.QuadPart) / freq.QuadPart : 0.0;
    g_lastControlsQpc = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;

    bool click[2] = { hl && L.stickClick, hr && R.stickClick };
    bool btnA[2] = { hl && L.buttonA, hr && R.buttonA };
    bool btnB[2] = { hl && L.buttonB, hr && R.buttonB };
    bool clickPressed[2], clickReleased[2], aPressed[2], bPressed[2];
    for (int h = 0; h < 2; ++h) {
        clickPressed[h] = click[h] && !g_prevStickClick[h];
        clickReleased[h] = !click[h] && g_prevStickClick[h];
        aPressed[h] = btnA[h] && !g_prevButtonA[h];
        bPressed[h] = btnB[h] && !g_prevButtonB[h];
        g_prevStickClick[h] = click[h];
        g_prevButtonA[h] = btnA[h];
        g_prevButtonB[h] = btnB[h];
    }
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    // --- clics y botones ---
    if (clickPressed[0]) {
        g_leftClickDownTick = GetTickCount64();
        g_leftClickLong = false;
    }
    if (click[0] && g_leftClickDownTick != 0 && !g_leftClickLong && GetTickCount64() - g_leftClickDownTick >= kLongPressMs) {
        g_leftClickLong = true;
        CycleConeOffAndWidth();
    }
    if (clickReleased[0]) {
        if (g_leftClickDownTick != 0 && !g_leftClickLong) CycleModelMode();
        g_leftClickDownTick = 0;
    }
    if (clickPressed[1]) {
        g_rightClickDownTick = GetTickCount64();
        g_rightClickLong = false;
    }
    if (click[1] && g_rightClickDownTick != 0 && !g_rightClickLong &&
        GetTickCount64() - g_rightClickDownTick >= kLongPressMs) {
        g_rightClickLong = true;
        ToggleCutPersons();
    }
    if (clickReleased[1]) {
        if (g_rightClickDownTick != 0 && !g_rightClickLong) CycleAreaShape();
        g_rightClickDownTick = 0;
    }
    bool cine = DioramaCinematicActive();
    if (aPressed[0] && cine) {    // X en una cinematica: vista del juego
        g_cineRecenterRequest.store(1, std::memory_order_relaxed);
    } else if (aPressed[0]) {     // X
        g_anchorLocateRequest.store(1, std::memory_order_relaxed);
        g_anchorRequest.store(1, std::memory_order_relaxed);
        OpenVRShowToast(L"Maqueta", L"Centrada en el personaje (1:15)", kToastMs);
    }
    if (bPressed[0]) TogglePassthrough();

    // --- movimientos continuos (no mientras se agarra la maqueta) ---
    if (dt <= 0.0 || gestureActive) return;
    double lx = hl ? L.stickX : 0.0, ly = hl ? L.stickY : 0.0;
    double rx = hr ? R.stickX : 0.0, ry = hr ? R.stickY : 0.0;
    if (std::abs(lx) < kStickDeadzone) lx = 0.0;
    if (std::abs(ly) < kStickDeadzone) ly = 0.0;
    if (std::abs(rx) < kStickDeadzone) rx = 0.0;
    if (std::abs(ry) < kStickDeadzone) ry = 0.0;
    // Mando del juego con la capa de la maqueta (L3): stick izquierdo = el
    // izquierdo de Quest, stick derecho = el derecho, RT/LT = zoom.
    if (g_padLayer.load(std::memory_order_relaxed)) {
        double plx = g_padAxisX[kPadLeftStick].load(std::memory_order_relaxed);
        double ply = g_padAxisY[kPadLeftStick].load(std::memory_order_relaxed);
        double prx = g_padAxisX[kPadRightStick].load(std::memory_order_relaxed);
        double pry = g_padAxisY[kPadRightStick].load(std::memory_order_relaxed);
        double trig = static_cast<double>(g_padAxisX[kPadRT].load(std::memory_order_relaxed)) -
                      static_cast<double>(g_padAxisX[kPadLT].load(std::memory_order_relaxed));
        if (std::abs(plx) >= kStickDeadzone) lx += plx;
        if (std::abs(ply) >= kStickDeadzone) ly += ply;
        if (std::abs(prx) >= kStickDeadzone) rx += prx;
        if (std::abs(pry) >= kStickDeadzone) ry += pry;
        if (std::abs(trig) >= 0.05) ry += trig;
    }
    double lift = 0.0;
    if (btnA[1]) lift += kButtonLiftMps * dt;   // A: bajar (Y del juego hacia abajo)
    if (btnB[1]) lift -= kButtonLiftMps * dt;   // B: subir
    if (cine) {
        // Cinematica: ni mover ni girar la maqueta; el zoom si.
        if (ry != 0.0) {
            std::lock_guard<std::mutex> lock(g_modelMutex);
            double k = ClampScale(g_model.scale * std::exp(-ry * kStickZoomPerS * dt));
            g_model.scale = k;
            g_scaleMirror.store(k, std::memory_order_relaxed);
        }
        return;
    }
    if (lx == 0.0 && ly == 0.0 && rx == 0.0 && ry == 0.0 && lift == 0.0) return;

    // Ejes horizontales de la mirada (seguimiento, ejes del juego).
    double fwd[2] = {0.0, 1.0}, right[2] = {1.0, 0.0};
    float m[12];
    if ((lx != 0.0 || ly != 0.0) && GetHmdPoseMatrix34(m)) {
        double fx = -m[2], fz = m[10];     // adelante del visor (x, -y, -z)
        double ux = -m[1], uz = m[9];      // "abajo" de la cabeza: sirve mirando hacia abajo
        double hx = fx - ux, hz = fz - uz;
        double len = std::sqrt(hx * hx + hz * hz);
        if (len > 1e-6) {
            fwd[0] = hx / len; fwd[1] = hz / len;
            right[0] = fwd[1]; right[1] = -fwd[0];
        }
    }
    std::lock_guard<std::mutex> lock(g_modelMutex);
    if (!g_model.placed) return;
    // Stick izquierdo: adelante aleja la mesa, a la izquierda la lleva a la
    // izquierda (respecto a la mirada).
    g_model.table[0] -= (right[0] * lx + fwd[0] * ly) * kStickPanMps * dt;
    g_model.table[2] -= (right[1] * lx + fwd[1] * ly) * kStickPanMps * dt;
    g_model.table[1] += lift;
    // Stick derecho: derecha = rodear la maqueta por la derecha (gira en el
    // sentido de las agujas del reloj vista desde arriba) alrededor del punto
    // de la mesa; en el modo "por la espalda" el giro es automatico.
    if (rx != 0.0 && g_model.mode != kModeBehind) g_model.yaw = WrapPi(g_model.yaw - rx * kStickTurnRadPs * dt);
    if (ry != 0.0) {
        double k = ClampScale(g_model.scale * std::exp(-ry * kStickZoomPerS * dt));
        g_model.scale = k;
        g_scaleMirror.store(k, std::memory_order_relaxed);
    }
}

static void UpdateHandGestures() {
    VrHandState L{}, R{};
    bool hl = GetVrHandState(0, &L) && L.valid;
    bool hr = GetVrHandState(1, &R) && R.valid;
    int mode = kGestureNone;
    int hand = -1;
    if (g_enabled.load(std::memory_order_relaxed)) {
        bool gl = hl && L.grip, gr = hr && R.grip, tl = hl && L.trigger, tr = hr && R.trigger;
        if (gl && gr) mode = kGestureScale;
        else if (gl || gr) { mode = kGestureMove; hand = gl ? 0 : 1; }
        else if (tl && tr) mode = kGestureArea;
        else if (tl || tr) { mode = kGestureCut; hand = tl ? 0 : 1; }
    }
    UpdateStickAndButtonControls(hl, L, hr, R, mode == kGestureMove || mode == kGestureScale);

    double pl[3] = {0.0, 0.0, 0.0}, pr[3] = {0.0, 0.0, 0.0};
    if (hl) HandPosition(L, pl);
    if (hr) HandPosition(R, pr);
    double mid[3] = { 0.5 * (pl[0] + pr[0]), 0.5 * (pl[1] + pr[1]), 0.5 * (pl[2] + pr[2]) };
    double v[3] = { pr[0] - pl[0], pr[1] - pl[1], pr[2] - pl[2] };
    double span = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    double hspan = std::sqrt(v[0] * v[0] + v[2] * v[2]);
    double angle = std::atan2(v[0], v[2]);
    const double* one = (hand == 0) ? pl : pr;

    // Empieza (o cambia) un gesto: foto del modelo y de las manos.
    if (mode != g_gesture.mode || hand != g_gesture.hand) {
        g_gesture.mode = mode;
        g_gesture.hand = hand;
        if (mode == kGestureNone) return;
        g_gesture.start = SnapshotModel();
        if (!g_gesture.start.placed) { g_gesture.mode = kGestureNone; return; }
        g_gesture.startTick = GetTickCount64();
        for (int i = 0; i < 3; ++i) {
            g_gesture.hand0[i] = one[i];
            g_gesture.lastOne[i] = one[i];
            g_gesture.mid0[i] = mid[i];
        }
        g_gesture.span0 = span;
        g_gesture.angle0 = angle;
        g_gesture.angleValid = hspan > kMinYawSpanM;
        // Corte: "hacia la maqueta" = de la cabeza al punto de la mesa, en
        // horizontal (ejes del juego en el espacio de seguimiento).
        g_gesture.inValid = false;
        g_gesture.cutAxis = 0;
        g_gesture.rangeValid = false;
        if (mode == kGestureCut) {
            std::lock_guard<std::mutex> lock(g_modelMutex);
            const CutRange& r = g_cutRange;
            if (r.valid) {
                // Y del juego hacia abajo: arriba del todo = mn[1].
                g_gesture.hTop = (r.center[1] - r.mn[1]) / kUnitsPerMeter;
                g_gesture.hBottom = (r.center[1] - r.mx[1]) / kUnitsPerMeter;
                g_gesture.range = r;
                g_gesture.rangeValid = true;
            }
        }
        float hm[12];
        if (mode == kGestureCut && GetHmdPoseMatrix34(hm)) {
            double hx = hm[3], hz = -hm[11];
            double dx = g_gesture.start.table[0] - hx, dz = g_gesture.start.table[2] - hz;
            double dl = std::sqrt(dx * dx + dz * dz);
            if (dl < 0.05) {   // encima de la maqueta: el adelante de la mirada
                dx = -hm[2];
                dz = hm[10];
                dl = std::sqrt(dx * dx + dz * dz);
            }
            if (dl > 1e-6) {
                g_gesture.inDir[0] = dx / dl;
                g_gesture.inDir[1] = dz / dl;
                g_gesture.inValid = true;
            }
        }
        // Punto del juego que esta ahora bajo el punto medio de las manos.
        const Model& s = g_gesture.start;
        double off[3] = { mid[0] - s.table[0], mid[1] - s.table[1], mid[2] - s.table[2] };
        RotateY(off, std::cos(s.yaw), std::sin(s.yaw));
        double k = kUnitsPerMeter * s.scale;
        for (int i = 0; i < 3; ++i) g_gesture.grabbed[i] = s.anchor[i] + off[i] * k;
        return;
    }
    if (mode == kGestureNone) return;
    for (int i = 0; i < 3; ++i) g_gesture.lastOne[i] = one[i];

    const Model& s = g_gesture.start;
    bool cineGesture = DioramaCinematicActive();
    std::lock_guard<std::mutex> lock(g_modelMutex);
    if (!g_model.placed) return;
    // Cinematica: agarrar no mueve la maqueta; con los dos, solo la escala.
    if (cineGesture && mode == kGestureMove) return;
    if (cineGesture && mode == kGestureScale) {
        if (g_gesture.span0 > kMinHandSpanM && span > kMinHandSpanM) {
            double k = ClampScale(s.scale * g_gesture.span0 / span);
            g_model.scale = k;
            g_scaleMirror.store(k, std::memory_order_relaxed);
        }
        return;
    }
    switch (mode) {
        case kGestureMove: {
            // La maqueta sigue a la mano.
            for (int i = 0; i < 3; ++i) g_model.table[i] = s.table[i] + (one[i] - g_gesture.hand0[i]);
            break;
        }
        case kGestureScale: {
            // Separar las manos = maqueta mas grande (menos metros del juego
            // por metro real); girar la recta entre las manos gira la maqueta.
            // El punto del juego que estaba entre las manos sigue entre ellas.
            double k = s.scale;
            if (g_gesture.span0 > kMinHandSpanM && span > kMinHandSpanM) k = ClampScale(s.scale * g_gesture.span0 / span);
            double yaw = s.yaw;
            if (g_gesture.angleValid && hspan > kMinYawSpanM) yaw = WrapPi(s.yaw - WrapPi(angle - g_gesture.angle0));
            double rel[3] = { g_gesture.grabbed[0] - s.anchor[0], g_gesture.grabbed[1] - s.anchor[1],
                              g_gesture.grabbed[2] - s.anchor[2] };
            RotateY(rel, std::cos(-yaw), std::sin(-yaw));
            double inv = 1.0 / (kUnitsPerMeter * k);
            for (int i = 0; i < 3; ++i) g_model.table[i] = mid[i] - rel[i] * inv;
            g_model.yaw = yaw;
            g_model.scale = k;
            g_scaleMirror.store(k, std::memory_order_relaxed);
            break;
        }
        case kGestureArea: {
            // Separar las manos = mas area; muy separadas = sin recorte. Si
            // no habia recorte, empieza cuadrada.
            bool had = s.areaShape != 0 && s.areaM > 0.0;
            double r0 = had ? s.areaM : kAreaStartFromWholeM;
            double r = r0;
            if (g_gesture.span0 > kMinHandSpanM && span > kMinHandSpanM) r = r0 * span / g_gesture.span0;
            if (r > kAreaMaxM) {
                g_model.areaShape = 0;
            } else {
                if (r < kAreaMinM) r = kAreaMinM;
                g_model.areaM = r;
                g_model.areaShape = 2;
            }
            break;
        }
        default: {
            // Subir la mano (Y del juego hacia abajo) sube el corte; lo que
            // se mueve la mano en metros reales, por la escala, en el juego.
            // El primer eje que pasa de la zona muerta es el de este gesto.
            double rawRise = g_gesture.hand0[1] - one[1];
            double rawPush = g_gesture.inValid ? (one[0] - g_gesture.hand0[0]) * g_gesture.inDir[0] +
                                                 (one[2] - g_gesture.hand0[2]) * g_gesture.inDir[1]
                                               : 0.0;
            if (g_gesture.cutAxis == 0 && (std::abs(rawRise) > kCutHandDeadzoneM || std::abs(rawPush) > kCutHandDeadzoneM)) {
                g_gesture.cutAxis = std::abs(rawRise) >= std::abs(rawPush) ? 1 : 2;
            }
            double rise = g_gesture.cutAxis == 1 ? HandDeadzone(rawRise) : 0.0;
            if (rise != 0.0) {
                // Sin corte empieza arriba del todo del mapa (no quita nada);
                // por encima de ahi, sin corte; como mucho, todo cortado.
                bool rv = g_gesture.rangeValid;
                double top = rv ? g_gesture.hTop : kCutMaxM;
                double bottom = rv ? g_gesture.hBottom - kCutEdgeMarginM : kCutMinM;
                double h0 = s.cutOn ? s.cutAboveM : (rv ? top : kCutStartM);
                double h = h0 + rise * s.scale;
                if (h > top) {
                    g_model.cutOn = false;               // por encima del mapa: sin corte
                } else {
                    if (h < bottom) h = bottom;
                    g_model.cutOn = true;
                    g_model.cutAboveM = h;
                }
            } else {
                g_model.cutOn = s.cutOn;
                g_model.cutAboveM = s.cutAboveM;
            }
            // Corte vertical: acercar la mano a la maqueta mete el plano en
            // ella (quita mas); traerla hacia uno lo saca (muy fuera: sin
            // corte vertical). El lado es el eje del mapa (X o Z) que queda
            // de cara al empezar el gesto (por la espalda, la mirada misma);
            // si es otro lado que el del corte que habia, empieza uno nuevo
            // en ese lado.
            if (g_gesture.inValid) {
                double push = g_gesture.cutAxis == 2 ? HandDeadzone(rawPush) : 0.0;
                if (push != 0.0) {
                    // Mirada hacia la maqueta en el juego (giro de la maqueta).
                    double cy = std::cos(s.yaw), sy = std::sin(s.yaw);
                    double dir[2] = { g_gesture.inDir[0] * cy + g_gesture.inDir[1] * sy,
                                      -g_gesture.inDir[0] * sy + g_gesture.inDir[1] * cy };
                    bool behind = s.mode == kModeBehind;
                    if (!behind) SnapToMapAxis(dir);
                    double cur[2];
                    VcutDirection(s, cur);
                    bool same = s.vcutOn &&
                                dir[0] * cur[0] + dir[1] * cur[1] > (behind ? kVcutSameViewCos : kVcutSameAxisCos);
                    if (same) { dir[0] = cur[0]; dir[1] = cur[1]; }
                    // Sin corte (o en otro lado) empieza en el borde del mapa
                    // de ese lado (no quita nada); fuera de el, sin corte;
                    // como mucho, hasta el borde contrario (todo cortado).
                    bool rv = g_gesture.rangeValid;
                    double nearEdge = kVcutMaxM, farEdge = kVcutMinM;
                    if (rv) {
                        double ed[2] = { dir[0], dir[1] };
                        if (s.vcutStraight) SnapToMapAxis(ed);
                        VcutEdges(g_gesture.range, ed[0], ed[1], &nearEdge, &farEdge);
                        farEdge -= kCutEdgeMarginM;
                    }
                    double v0 = same ? s.vcutM : (rv ? nearEdge : kVcutStartM);
                    double v = v0 - push * s.scale;
                    g_model.vcutDir[0] = s.vcutDir[0];
                    g_model.vcutDir[1] = s.vcutDir[1];
                    g_model.vcutYawRef = s.vcutYawRef;
                    if (v > nearEdge && !same && s.vcutOn) {
                        // Hacia uno desde otro lado: el corte que habia se
                        // queda (quitarlo seria un salto).
                        g_model.vcutOn = true;
                        g_model.vcutM = s.vcutM;
                    } else if (v > nearEdge) {
                        g_model.vcutOn = false;
                    } else {
                        if (v < farEdge) v = farEdge;
                        g_model.vcutOn = true;
                        g_model.vcutM = v;
                        if (!same) {
                            // Corte nuevo en este lado (por la espalda,
                            // fijado al giro con que empezo el gesto).
                            g_model.vcutDir[0] = dir[0];
                            g_model.vcutDir[1] = dir[1];
                            g_model.vcutYawRef = s.yaw;
                        }
                    }
                } else {
                    g_model.vcutOn = s.vcutOn;
                    g_model.vcutM = s.vcutM;
                    g_model.vcutDir[0] = s.vcutDir[0];
                    g_model.vcutDir[1] = s.vcutDir[1];
                    g_model.vcutYawRef = s.vcutYawRef;
                }
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------
// Teclas (hilo de Present)
// ---------------------------------------------------------------------
static bool KeyPressedOnce(int vk, std::atomic<bool>& wasDown) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool was = wasDown.exchange(down, std::memory_order_relaxed);
    return down && !was;
}

void DioramaPollKeys() {
    if (!g_installed) return;
    TryInstallSteamInputHooks();
    OpenVRUpdateToast();
    UpdatePadLayer();
    if (KeyPressedOnce(VK_F5, g_f5WasDown)) ToggleDiorama();
    if (KeyPressedOnce(VK_F6, g_f6WasDown)) {
        int next = (g_areaPreset.load(std::memory_order_relaxed) + 1) % kAreaPresetCount;
        g_areaPreset.store(next, std::memory_order_relaxed);
        Model md;
        {
            std::lock_guard<std::mutex> lock(g_modelMutex);
            if (kAreaPresetM[next] <= 0.0) {
                g_model.areaShape = 0;
            } else {
                g_model.areaM = kAreaPresetM[next];
                g_model.areaShape = 2;
            }
            md = g_model;
        }
        if (g_enabled.load(std::memory_order_relaxed)) OpenVRShowToast(L"\u00c1rea visible", AreaTextW(md).c_str(), kToastMs);
    }
    if (KeyPressedOnce(VK_F4, g_f4WasDown) && g_enabled.load(std::memory_order_relaxed)) CycleConeOffAndWidth();
    if (KeyPressedOnce(VK_PAUSE, g_pauseWasDown) && g_enabled.load(std::memory_order_relaxed)) ToggleCutPersons();
    if (KeyPressedOnce(VK_F7, g_f7WasDown)) {
        int next = (g_background.load(std::memory_order_relaxed) + 1) % kBackgroundCount;
        g_background.store(next, std::memory_order_relaxed);
        // En Virtual Desktop: Streaming > VR Passthrough > Environment >
        // Configure, con el mismo color.
        if (g_enabled.load(std::memory_order_relaxed)) {
            OpenVRShowToast(L"Fondo", next == 0 ? L"El del juego (negro)" : (next == 1 ? L"Magenta" : L"Verde"), kToastMs);
        }
    }
    // Re Pag / Av Pag solo cambian la escala en diorama (fuera, son la
    // separacion entre ojos: StereoPollKeys). Escalan alrededor del punto de
    // la mesa.
    bool up = KeyPressedOnce(VK_PRIOR, g_pgUpWasDown);
    bool dn = KeyPressedOnce(VK_NEXT, g_pgDnWasDown);
    if (g_enabled.load(std::memory_order_relaxed) && (up || dn)) {
        double k;
        {
            std::lock_guard<std::mutex> lock(g_modelMutex);
            k = g_model.scale;
            if (up) k /= kScaleStep;   // maqueta mas grande
            if (dn) k *= kScaleStep;   // maqueta mas pequena
            k = ClampScale(k);
            g_model.scale = k;
        }
        g_scaleMirror.store(k, std::memory_order_relaxed);
    }
    UpdateHandGestures();
}

// ---------------------------------------------------------------------
// Instalacion
// ---------------------------------------------------------------------

// Primeros bytes esperados de cada funcion (todas primarias en .pdata, sin
// CHAININFO; comprobado en el binario).
static const unsigned char kExpectedPrologue[kHookCount][5] = {
    {0x48, 0x89, 0x5C, 0x24, 0x10},   // +0x7aa90
    {0x48, 0x8B, 0xC4, 0x55, 0x48},   // +0x7fc90
    {0x44, 0x89, 0x4C, 0x24, 0x20},   // +0x984a0
    {0x48, 0x89, 0x5C, 0x24, 0x08},   // +0x98870
    {0x48, 0x89, 0x5C, 0x24, 0x18},   // +0x98980
    {0x48, 0x89, 0x5C, 0x24, 0x10},   // +0x98fd0
    {0x48, 0x89, 0x5C, 0x24, 0x08},   // +0x99350
    {0x48, 0x89, 0x5C, 0x24, 0x08},   // +0x9b4b0
    {0x40, 0x53, 0x48, 0x83, 0xEC},   // +0x74c030
    {0x4C, 0x8B, 0xDC, 0x55, 0x41},   // +0x81630
    {0x48, 0x8B, 0xC4, 0x48, 0x89},   // +0xf0fd0
    {0x40, 0x57, 0x41, 0x56, 0x41},   // +0xf0be0
    {0x48, 0x89, 0x5C, 0x24, 0x08},   // +0x750000
    {0x48, 0x89, 0x5C, 0x24, 0x08},   // +0x1ad580
};

static void* HookDetour(int i) {
    switch (i) {
        case 0: return reinterpret_cast<void*>(&HookedSectorsAtPoint);
        case 1: return reinterpret_cast<void*>(&HookedEnterNeighbor);
        case 2: return reinterpret_cast<void*>(&HookedFaceDraw<0>);
        case 3: return reinterpret_cast<void*>(&HookedFaceDraw<1>);
        case 4: return reinterpret_cast<void*>(&HookedFaceDraw<2>);
        case 5: return reinterpret_cast<void*>(&HookedFaceDraw<3>);
        case 6: return reinterpret_cast<void*>(&HookedSkyDraw);
        case 7: return reinterpret_cast<void*>(&HookedShadowRedraw);
        case 8: return reinterpret_cast<void*>(&HookedBatchOpen);
        case 9: return reinterpret_cast<void*>(&HookedDrawAtmPortals);
        case 10: return reinterpret_cast<void*>(&HookedMirrorRender);
        case 11: return reinterpret_cast<void*>(&HookedWaterDraw);
        case 12: return reinterpret_cast<void*>(&HookedLitFace);
        default: return reinterpret_cast<void*>(&HookedConePlanes);
    }
}

static void** HookOriginal(int i) {
    switch (i) {
        case 0: return reinterpret_cast<void**>(&g_origSectorsAtPoint);
        case 1: return reinterpret_cast<void**>(&g_origEnterNeighbor);
        case 2: return reinterpret_cast<void**>(&g_origFaceDraw[0]);
        case 3: return reinterpret_cast<void**>(&g_origFaceDraw[1]);
        case 4: return reinterpret_cast<void**>(&g_origFaceDraw[2]);
        case 5: return reinterpret_cast<void**>(&g_origFaceDraw[3]);
        case 6: return reinterpret_cast<void**>(&g_origSkyDraw);
        case 7: return reinterpret_cast<void**>(&g_origShadowRedraw);
        case 8: return reinterpret_cast<void**>(&g_origBatchOpen);
        case 9: return reinterpret_cast<void**>(&g_origDrawAtmPortals);
        case 10: return reinterpret_cast<void**>(&g_origMirrorRender);
        case 11: return reinterpret_cast<void**>(&g_origWaterDraw);
        case 12: return reinterpret_cast<void**>(&g_origLitFace);
        default: return reinterpret_cast<void**>(&g_origConePlanes);
    }
}

bool InstallDioramaHooks() {
    if (g_installed) return true;
    HMODULE hMain = GetModuleHandleA(nullptr);
    if (!hMain) return false;
    g_base = reinterpret_cast<uintptr_t>(hMain);

    // Comprobar todo antes de tocar nada.
    for (int i = 0; i < kHookCount; ++i) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(g_base + kHookOffsets[i]);
        if (std::memcmp(p, kExpectedPrologue[i], 5) != 0) {
            std::ostringstream o;
            o << "[DIORAMA] ERROR: bytes inesperados en Blade.exe+0x" << std::hex << kHookOffsets[i]
              << " (otra version de Blade.exe?); modo diorama no disponible.";
            HookLogger::Instance().Line(o.str());
            return false;
        }
    }
    bool ok = true;
    for (int i = 0; i < kHookCount && ok; ++i) {
        void* target = reinterpret_cast<void*>(g_base + kHookOffsets[i]);
        if (MH_CreateHook(target, HookDetour(i), HookOriginal(i)) != MH_OK) { ok = false; break; }
        g_hooked[i] = true;
        if (MH_EnableHook(target) != MH_OK) ok = false;
    }
    if (!ok) {
        for (int i = 0; i < kHookCount; ++i) {
            if (!g_hooked[i]) continue;
            void* target = reinterpret_cast<void*>(g_base + kHookOffsets[i]);
            MH_DisableHook(target);
            MH_RemoveHook(target);
            g_hooked[i] = false;
        }
        HookLogger::Instance().Line("[DIORAMA] ERROR: no se pudieron enganchar las funciones del render; modo diorama no disponible.");
        return false;
    }
    bool backface = InstallBackfaceThreshold();
    InstallEntityHiding();
    g_installed = true;
    HookLogger::Instance().Line(backface
        ? "[DIORAMA] Modo diorama disponible (F5)."
        : "[DIORAMA] Modo diorama disponible (F5), AVISO: sin caras por los dos lados (sitios del descarte no esperados).");
    return true;
}

void UninstallDioramaHooks() {
    if (!g_installed) return;
    UninstallSteamInputHooks();
    g_enabled.store(false, std::memory_order_relaxed);
    g_worldActive = false;
    g_cutActive = false;
    g_vcutActive = false;
    ApplyBackground(false);
    UninstallFogHook();
    UninstallMirrorBandsHook();
    UninstallBackfaceThreshold();
    UninstallEntityHiding();
    for (int i = 0; i < kHookCount; ++i) {
        if (!g_hooked[i]) continue;
        MH_DisableHook(reinterpret_cast<void*>(g_base + kHookOffsets[i]));
        g_hooked[i] = false;
    }
    g_installed = false;
}

} // namespace BladeVR
