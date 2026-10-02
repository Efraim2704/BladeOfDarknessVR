#include "DioramaGpuLevel.h"
#include "HookLogger.h"
#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <utility>
#include <string>
#include <cmath>
#include <cstring>

namespace BladeVR {

// =====================================================================
// NIVEL DEL DIORAMA EN LA GPU
//
// El motor transforma e ilumina en la CPU cada cara del nivel en cada
// pasada: con el mapa entero es lo mas caro de la maqueta. Aqui la geometria
// estatica del nivel se sube UNA vez a la GPU (coordenadas del mundo) y se
// dibuja en cada ojo con la matriz de vista de la pasada; el motor sigue
// haciendo todo lo demas (personajes, objetos, agua, reflejos, sombras de
// objetos) y solo deja de dibujar las caras del nivel.
//
// Hilo del juego (DioramaBeginWorld / DioramaEndWorld):
//   * construccion = CAPTURA de lo que dibuja el propio motor: en una pasada
//     del diorama sin frustum (planos de la visita a 0), sin area, corte ni
//     cono y con todas las caras por los dos lados, cada poligono que el
//     motor manda a dibujar como cara (+0x9a8e0, ya recortado por el plano
//     del agua del sector, los huecos de portal y el arbol de las caras
//     complejas) se pasa de camara al mundo y se guarda con el material
//     activo (+0x750be0), el sector en curso ([+0xa51f20]) y la normal de su
//     superficie. Despues se sueldan (vertices a menos de 1 mm y uniones en
//     T) y se agrupan por textura. No se capturan (las sigue dibujando el
//     motor, con su estado de cada momento) las superficies que cambian
//     durante la partida: las de liquido que corre (vecino con plano de agua
//     y +0xe0 > 0; textura animada), las de las puertas que se deslizan y
//     los portales hacia ellas, y los portales hacia sectores cerrados (los
//     muros que se derriban). Ver SurfaceIsDynamicRaw. Se captura en la
//     primera pasada del diorama en cada nivel.
//   * cada pasada: una "ranura" con la fromWorld, el corte, el cono, los
//     sectores del area, las luces (ambiente y plana de cada sector y las
//     de sus conos de luz, +0x230) y la textura (handle de bgfx) de cada
//     grupo; su numero se escribe en la vista 250 de bgfx (sin uso), que
//     bgfx copia al fotograma (Context::frame, +0x656260).
// Hilo de render: RendererContextD3D11::submit (+0x67d310) lee ese numero
// del fotograma que va a dibujar; StereoHook llama a GpuLevelDraw en el
// primer draw 3D con profundidad de la maqueta y aqui se dibuja el nivel en
// las dos mitades con la ranura de ESE fotograma (misma camara que los
// personajes). Texturas: las de bgfx (s_renderD3D11 +0x843f8, 0x50 por
// handle). Luz como el raster (+0x750000): base (auto-iluminacion + luz
// ambiente + luz plana del sector) mas las puntuales I*1e6*cos/r^2, en
// 0..255, /255 y con el tope que conserva el tono; color = luz * textura.
// Cada luz solo suma dentro de alguno de sus conos de luz (como +0x9a950:
// planos del mundo de cada arista, B_LConeEdge +0). Los translucidos van
// con un sesgo de profundidad hacia la camara (como las calcomanias del
// motor).
// Cielo: con el fondo del juego (sin passthrough), antes del nivel se pinta
// el cielo del mapa (texturas DomeUp..DomeRight del raster, +0xd40 cada
// 0x370) como en el juego: cubo del mundo a 1000 unidades,
// u = (p.a * 255/2000 - 128)/256 (+0x750d30 / +0x744650), sin paralaje y
// girando con la maqueta.
// Lotes del motor que hay que recortar por pixel (la lamina de agua, los
// objetos cerca de un corte, las caras de sombra y de liquido): van
// marcados y aqui se dibujan con un pixel shader igual al del juego (mesh /
// mesh_fog) que ademas descarta lo que quitan el cono y los cortes
// (posicion en camara reconstruida desde la profundidad).
// =====================================================================

namespace {

// --- nivel ---------------------------------------------------------------
constexpr int kLevelSectorArray = 0x2118;
constexpr int kLevelSectorCount = 0x2120;
constexpr uintptr_t kLevelVertexData = 0xa013f0;
constexpr uintptr_t kLevelVertexCount = 0xa013f8;
// sector (B_MapSector)
constexpr int kSectorSurfaceCount = 0x10;
constexpr int kSectorActive = 0x6c;          // int; 0 = cerrado (sus portales se dibujan como caras)
constexpr int kSectorBoxMin = 0x70;
constexpr int kSectorBoxMax = 0x88;
constexpr int kSectorWaterPlane = 0xd8;      // puntero
constexpr int kSectorWaterFlow = 0xe0;       // double: desplazamiento de la textura del liquido
constexpr int kSectorAmbient = 0x100;        // B_AmbientLight incrustada
constexpr int kSectorPlaneLight = 0x178;     // B_PlaneLight incrustada
constexpr int kSectorConeList = 0x230;       // lista de B_LightCone (B_ObjLink)
constexpr int kLinkNext = 0x08;
constexpr int kLinkObject = 0x18;
constexpr int kConeLight = 0xd8;
constexpr int kMaxConesPerSector = 64;
// B_LightCone: aristas (DiArray de B_LConeEdge, 0x28 cada una: plano del
// mundo nx ny nz d en +0 y puntero a su copia en camara en +0x20); lo de
// dentro es n.p + d > 0 (+0x736da0 con +0x736950). En +0x78 el enlace al
// siguiente cono de la misma luz (+0x9a950 prueba los trozos que quedan
// fuera de un cono con el siguiente).
constexpr int kConeEdgeData = 0x08;
constexpr int kConeEdgeCount = 0x10;
constexpr int kConeEdgeStride = 0x28;
constexpr int kConeEdgeCamPlane = 0x20;
constexpr int kConeNextLink = 0x78;
constexpr int kMaxChainCones = 8;
constexpr int kMaxConePlanes = 32;
// raster: caras del cielo (B_WorldDomeMS -> raster+0x248 -> +0x744650)
constexpr int kRasterSkyFaces = 0xd40;       // Up, Down, Front, Back, Left, Right
constexpr int kSkyFaceStride = 0x370;
constexpr int kSkyFaceTexture = 0x00;        // id de textura (u32)
// (+0x368 = la cara se ve en el frustum de la camara de ese momento: +0x750d30
// la recorta contra el; no dice si tiene textura. No se usa: con ella, al
// mirar hacia donde la camara del motor no veia cielo, no habia cielo.)
constexpr int kRasterSkyLight = 0xd28;       // float: color del cielo = esto * 220 (0..255)
// luces
constexpr int kLightColor = 0x1c;            // bytes r, g, b
constexpr int kLightIntensity = 0x20;        // float (con el parpadeo de este fotograma)
constexpr int kOmniWorldPos = 0x40;          // 3 doubles (en +0x58 la copia en camara)
constexpr int kPlaneWorldDir = 0x78;         // 3 doubles (en +0x90 la copia en camara)
constexpr uintptr_t kVtOmniLight = 0x7a3158;
constexpr uintptr_t kVtAmbientLight = 0x7a2c30;
constexpr uintptr_t kVtPlaneLight = 0x7a2c70;
constexpr uintptr_t kVtGradientLight = 0x7a2bf0;
// Luces que se mueven: las de B_SpotEntity (las antorchas que lleva alguien:
// un objeto con luces en su descripcion crea una B_SpotEntity por luz,
// +0xf7a30) y B_MagicMissileEntity. Llevan dentro una B_SpotLS (fuente) y
// una B_OmniLight (la luz de los conos). Al moverse (+0xfd950, y +0x114360
// el proyectil) +0x89a40 les QUITA los conos de los sectores (+0x896d0) y
// las apunta en una lista (puntero +0x9c49c8, recuento +0x9c49d0);
// B_Map::Render (+0x80f20) las rehace al empezar (+0x8af10, llamada en
// +0x80f6f: vtable[2] de cada una = B_SpotLS +0x88e70, cono raiz en su
// sector y propagacion por los portales). La pasada de la GPU lee los conos
// de los sectores ANTES de llamar a B_Map::Render (desde su hook): sin mas,
// una luz que se movia en ese fotograma no tendria conos (ni luz ni sombra
// visible: el motor pinta el trozo en sombra como la cara sin esa luz). Se
// rehacen aqui, justo antes de leer los conos: es lo primero que haria
// B_Map::Render, en el mismo hilo y sin nada del juego entre medias (luego
// encuentra la lista vacia y no hace nada).
constexpr uintptr_t kFlushMovedLights = 0x8af10;
constexpr unsigned char kFlushMovedLightsPrologue[6] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };
constexpr uintptr_t kMovedLightCount = 0x9c49d0;  // u32
// superficies
constexpr int kSurfacePlane = 0x08;          // nx ny nz d, normal hacia dentro del sector
constexpr int kSurfaceFlags = 0x38;          // u32: 1 sin auto-iluminacion, 0x20 alfa propia, 0x100 luz fija
constexpr int kSurfaceSelfIllum = 0x3c;      // float 0..1
constexpr int kSurfaceTexture = 0x40;        // id (int)
constexpr int kSurfaceTexU = 0x48;
constexpr int kSurfaceTexV = 0x60;
constexpr int kSurfaceShiftU = 0x78;
constexpr int kSurfaceShiftV = 0x7c;
constexpr int kSurfaceAlpha = 0x9c;          // float 0..255 (con la bandera 0x20)
constexpr int kPolyPortalNeighbor = 0xd8;    // B_PolygonPortalMS: indice (u32) del sector vecino
constexpr int kPortalNeighbor = 0xb8;        // B_PortalMS: indice (u32) del sector vecino
// B_ComplexMS: sus huecos de portal (+0x974f0 / +0x981b0): recuento (u32) en
// +0xc0 y en +0xc8 los portales (0xa8 cada uno, indice del sector vecino en
// +0x18). El hueco solo se quita de la cara si el vecino esta abierto.
constexpr int kComplexPortalCount = 0xc0;
constexpr int kComplexPortals = 0xc8;
constexpr int kComplexPortalStride = 0xa8;
constexpr int kComplexPortalSector = 0x18;
constexpr unsigned kMaxComplexPortals = 256;
// Puertas (Doors.CreateDoor -> B_SlidingAreaEntity, sector en su +0x370):
// el sector de la puerta lleva en +0xd8 su superficie deslizante (una
// superficie del mapa) y en +0xe0 el desplazamiento (+0x261be2). En cada
// dibujo +0x9c190 mueve esa superficie y recorta las demas caras del sector
// con su plano, y los portales de los vecinos hacia el sector se dibujan
// recortados por el (la hoja de la puerta). En los sectores con agua, +0xd8
// es el plano del agua (B_WaterSurface / B_MirrorSurface), estatico.
constexpr uintptr_t kVtMapSurfaces[] = {
    0x7a3558,   // B_MapSurface
    0x7a35b0,   // B_WorldDomeMS
    0x7a3608,   // B_PolygonMS
    0x7a3660,   // B_PortalMS
    0x7a36b8,   // B_PolygonPortalMS
    0x7a3830,   // B_ComplexMS
};
constexpr int kMaterialInSurface = 0x38;     // el material (+0x750be0 lo recibe) va en superficie+0x38
// captura
constexpr uintptr_t kFaceEmit = 0x9a8e0;          // (cara, poligono en camara): envio de una cara
constexpr unsigned char kFaceEmitPrologue[6] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };
constexpr uintptr_t kSetMaterial = 0x750be0;      // raster vtable+0x1e0 (raster, material)
constexpr unsigned char kSetMaterialPrologue[5] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
constexpr uintptr_t kCurrentSectorPtr = 0xa51f20; // sector que se esta dibujando
constexpr int kPolyListCount = 0x08;              // lista de vertices: recuento (u32)
constexpr int kPolyListVerts = 0x10;              // 3 doubles cada 0x18
constexpr int kPolyListStride = 0x18;
constexpr unsigned kFlagNoSelfIllum = 0x1;
constexpr unsigned kFlagOwnAlpha = 0x20;
constexpr unsigned kFlagFixedLight = 0x100;
// raster (B_BgfxRasterDevice)
constexpr uintptr_t kRasterPtr = 0xd93a30;
constexpr int kRasterTexTable = 0x1ec730;    // punteros por id de textura
constexpr int kRasterTexCount = 0x1ec738;
constexpr int kTexEntryBgfx = 0x20;          // -> objeto con el TextureHandle (u16) en +8
constexpr int kBgfxTexHandle = 0x08;
constexpr int kRasterLightScale = 0xd2c;     // float: escala de las luces puntuales
constexpr int kRasterFixedLight = 0x1ec574;  // float: base de las caras con luz fija (0..255)
// bgfx
constexpr uintptr_t kBgfxContextPtr = 0xf6b658;   // bgfx::s_ctx
constexpr uintptr_t kBgfxCtxViews = 0x3325900;    // Context::m_view[256], 0xc0 cada una
constexpr int kBgfxViewSize = 0xc0;
constexpr int kBgfxViewMatrix = 0x20;             // View::m_view (16 floats)
constexpr int kBgfxFrameViews = 0x300;            // Frame::m_view
constexpr int kBgfxMarkerView = 250;
constexpr uintptr_t kRendererD3D11Ptr = 0xfdf018; // s_renderD3D11
constexpr uintptr_t kRendererTextures = 0x843f8;  // TextureD3D11 m_textures[], m_ptr en +0
constexpr int kRendererTextureStride = 0x50;
constexpr int kRendererSrvCandidates[3] = { 0x18, 0x20, 0x28 };
constexpr unsigned kMaxTextureHandles = 4096;
constexpr uintptr_t kRendererPrograms = 0x693f8;  // ProgramD3D11 m_program[], 0xd8; m_fsh en +8
constexpr int kRendererProgramStride = 0xd8;
constexpr int kProgramFsh = 0x08;                 // -> ShaderD3D11, ID3D11PixelShader* en +0
constexpr uintptr_t kProgMesh = 0x104aab4;        // ProgramHandle (u16) de mesh (con MRT)
constexpr uintptr_t kProgMeshFog = 0x104aab8;     // ProgramHandle (u16) de mesh_fog
constexpr uintptr_t kRendererSubmit = 0x67d310;   // RendererContextD3D11::submit(Frame*, ...)
constexpr unsigned char kSubmitPrologue[8] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41 };
constexpr uint32_t kMarkerMagic = 0x314C5642;     // "BVL1"

constexpr int kSlots = 4;
constexpr uint32_t kMaxLights = 1024;
constexpr uint32_t kMaxLightRefs = 65536;
constexpr uint32_t kMaxLightsPerSector = 32;
constexpr uint32_t kMaxCones = 32768;
constexpr uint32_t kMaxPlanes = 131072;
constexpr int kMaxPolyVerts = 128;
constexpr double kMinPolyArea = 1.0;              // mm^2
constexpr double kWeldTolMm = 1.0;                // soldadura de la captura: tolerancia
constexpr double kWeldCellMm = 4.0;               // rejilla para unir vertices
constexpr double kEdgeCellMm = 512.0;             // rejilla para los puntos en T
constexpr int64_t kMaxEdgeCells = 20000;          // aristas muy largas en diagonal: sin puntos en T
constexpr int kSkyFaces = 6;

// --- datos ------------------------------------------------------------------
struct GpuVertex {
    float p[3];
    float uv[2];
    float n[3];
    uint32_t sector;
    float mat[3];       // auto-iluminacion (0..1), alfa (0..1), luz fija (0/1)
};
static_assert(sizeof(GpuVertex) == 48, "GpuVertex");

struct GpuGroup {
    int tex;
    bool translucent;
    uint32_t first;
    uint32_t count;
};

struct SectorGpu {
    float amb[3];
    uint32_t lightStart;
    float pcol[3];
    uint32_t lightCount;
    float pdir[3];
    uint32_t visible;
};
static_assert(sizeof(SectorGpu) == 48, "SectorGpu");

struct LightGpu {
    float pos[3];       // puntual: posicion; plana: direccion
    uint32_t type;      // 0 puntual, 1 ambiente, 2 plana
    float col[3];       // ya escalado (ver LightIndex)
    float pad;
};
static_assert(sizeof(LightGpu) == 32, "LightGpu");

// Luz de un sector: indice de la luz y sus conos en ese sector (0 conos = en
// todo el sector).
struct LightRefGpu {
    uint32_t light;
    uint32_t coneStart;
    uint32_t coneCount;
    uint32_t pad;
};
static_assert(sizeof(LightRefGpu) == 16, "LightRefGpu");

struct ConeGpu {
    uint32_t planeStart;
    uint32_t planeCount;
};
static_assert(sizeof(ConeGpu) == 8, "ConeGpu");

struct PlaneGpu {
    float v[4];         // n.p + d > 0 dentro (mundo)
};
static_assert(sizeof(PlaneGpu) == 16, "PlaneGpu");

struct PerEyeCB {
    float viewProj[16];
    float cam[4];       // xyz camara (mundo), w sin uso
    float cut[4];       // x corte si/no, y Y del corte, z sin uso, w cono si/no
    float misc[4];      // x base de la luz fija (0..1), y corte vertical si/no
    float cone[kGpuConePlaneCount][4];
    float vcut[4];      // corte vertical (mundo): se conserva n.p + d > 0
};
static_assert(sizeof(PerEyeCB) % 16 == 0, "PerEyeCB");

struct FrameSlot {
    uint32_t seq = 0;
    bool drawGpu = false;
    double fw[16] = {};
    bool cutOn = false;
    float cutY = 0.0f;
    bool coneOn = false;
    float cone[kGpuConePlaneCount][4] = {};
    float coneCam[kGpuConePlaneCount][4] = {};
    bool vcutOn = false;
    float vcut[4] = {};
    float vcutCam[4] = {};
    bool reflOn = false;                // plano del agua reflejada (camara), para los lotes marcados
    float reflCam[4] = {};
    float cutCam[4] = {};               // corte de altura en camara (lotes marcados): se conserva n.v + d > 0
    bool poseOn = false;                // pose del visor de esta camara (se envia con su imagen)
    float pose[12] = {};
    float cam[3] = {};
    float fixedLight = 1.0f;
    std::vector<SectorGpu> sectors;
    std::vector<LightRefGpu> lightRefs;
    uint32_t lightRefCount = 0;
    std::vector<LightGpu> lights;
    uint32_t lightCount = 0;
    std::vector<ConeGpu> cones;
    uint32_t coneCount = 0;
    std::vector<PlaneGpu> planes;
    uint32_t planeCount = 0;
    std::vector<uint16_t> handles;      // por grupo
    // cielo (fondo del juego, sin passthrough)
    bool skyOn = false;
    uint16_t skyHandles[kSkyFaces] = {};
    float skyColor = 0.0f;
};

struct CapPoly {
    uint32_t first;     // primer vertice en capPos (3 doubles cada uno)
    uint32_t count;
    int group;
    uint32_t sector;
    float nrm[3];
    float mat[3];
    double U[3], V[3], du, dv;
};

struct LevelData {
    uint32_t gen = 0;
    long long level = 0;
    char** sectorArray = nullptr;
    unsigned sectorCount = 0;
    const char* vertTable = nullptr;
    unsigned vertCount = 0;
    std::vector<GpuVertex> verts;
    std::vector<uint32_t> indices;
    std::vector<GpuGroup> groups;
    std::vector<std::vector<uint32_t>> groupTris;   // solo durante la construccion
    std::vector<double> capPos;                     // captura: vertices (mundo) hasta soldar
    std::vector<CapPoly> capPolys;
    // Superficies que dibuja la GPU (el motor se las salta) y sector -> indice
    // (direccionamiento abierto; claves nulas = libres).
    std::vector<const char*> surfKeys;
    uint32_t surfMask = 0;
    std::vector<const char*> secKeys;
    std::vector<uint32_t> secVals;
    uint32_t secMask = 0;
    FrameSlot slots[kSlots];
    // Orientacion de los planos del mundo de los conos de luz respecto a su
    // copia en camara (la que usa el motor): +1 igual, -1 al reves.
    // 0 = sin conos (no se pudo comprobar): cada luz en todo su sector.
    float coneSign = 1.0f;
    bool coneSignChecked = false;
    // Caja de la geometria capturada (mundo): mas ajustada que las cajas de
    // los sectores (las de exterior llegan hasta el cielo). Rango de los cortes.
    bool geoValid = false;
    double geoMn[3] = {};
    double geoMx[3] = {};
    uint16_t skyHandleCache[kSkyFaces] = { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff };
    unsigned coneVoteSame = 0, coneVoteOpposite = 0, coneVotePasses = 0;
    // hilo de render
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* sectorBuf = nullptr;
    ID3D11Buffer* refBuf = nullptr;
    ID3D11Buffer* lightBuf = nullptr;
    ID3D11Buffer* coneBuf = nullptr;
    ID3D11Buffer* planeBuf = nullptr;
    ID3D11ShaderResourceView* sectorSrv = nullptr;
    ID3D11ShaderResourceView* refSrv = nullptr;
    ID3D11ShaderResourceView* lightSrv = nullptr;
    ID3D11ShaderResourceView* coneSrv = nullptr;
    ID3D11ShaderResourceView* planeSrv = nullptr;
    bool resTried = false;
    bool resOk = false;
};

struct Poly {
    int n;
    double p[kMaxPolyVerts][3];
};

struct FaceMat {
    int tex;
    double U[3], V[3];
    double du, dv;
    double n[3];
    float self, alpha, fixed;
    bool translucent;
};

// --- estado ---------------------------------------------------------------
// La primera pasada del diorama en un nivel sin capturar lo captura.
std::atomic<LevelData*> g_current{nullptr};
std::atomic<uint32_t> g_renderReadyGen{0};
std::atomic<uint32_t> g_renderLevelGen{0};
// hilo del juego
uintptr_t g_base = 0;
uint32_t g_nextGen = 1;
uint32_t g_seq = 0;
uint32_t g_pendingSeq = 0;
bool g_passActive = false;
bool g_skipPass = false;
LevelData* g_passLevel = nullptr;
bool g_markerWritten = false;
std::vector<LevelData*> g_retired;
long long g_failedLevel = 0;
unsigned g_failedCount = 0;
std::atomic<bool> g_retryCapture{false};    // al activar el diorama: se puede volver a intentar
bool g_submitTried = false;
bool g_submitHooked = false;
// captura (hilo del juego)
using PFN_FaceEmit = void(__fastcall*)(char*, char*);
using PFN_SetMaterial = void(__fastcall*)(char*, char*);
PFN_FaceEmit g_origFaceEmit = nullptr;
PFN_SetMaterial g_origSetMaterial = nullptr;
bool g_captureHooksTried = false;
bool g_captureHooksOk = false;
LevelData* g_captureLd = nullptr;           // en construccion durante la pasada de captura
bool g_capturing = false;
const char* g_captureSurface = nullptr;     // superficie que se esta dibujando (se captura)
const char* g_captureMat = nullptr;         // ultimo material que ha puesto el raster
double g_captureInv[9] = {};                // inversa de la 3x3 de la fromWorld de la captura
double g_captureT[3] = {};
const void* g_lightKeys[2048] = {};
int g_lightVals[2048] = {};
uint32_t g_lightStamps[2048] = {};
uint32_t g_lightStamp = 0;
// hilo de render
using PFN_Submit = void(__fastcall*)(void*, void*, void*, void*);
PFN_Submit g_origSubmit = nullptr;
uint32_t g_frameSeq = 0;
bool g_frameSeqValid = false;
LevelData* g_renderLevel = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
ID3D11RasterizerState* g_rsTranslucent = nullptr;   // con sesgo de profundidad hacia la camara
ID3D11DepthStencilState* g_dssOpaque = nullptr;
ID3D11DepthStencilState* g_dssTranslucent = nullptr;
ID3D11BlendState* g_blendTranslucent = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11Buffer* g_cb[2] = {};
ID3D11Texture2D* g_whiteTex = nullptr;
ID3D11ShaderResourceView* g_whiteSrv = nullptr;
void* g_srvVtable = nullptr;
void* g_tex2dVtable = nullptr;
bool g_sharedTried = false;
bool g_sharedOk = false;
// agua con el cono
ID3D11PixelShader* g_waterPsFog = nullptr;
ID3D11PixelShader* g_waterPsMrt = nullptr;
ID3D11Buffer* g_waterCb[2] = {};
void* g_psVtable = nullptr;
bool g_waterTried = false;
bool g_waterOk = false;
ID3D11PixelShader* g_waterSavedPs = nullptr;
ID3D11Buffer* g_waterSavedCb = nullptr;
const float (*g_waterCone)[4] = nullptr;    // planos del cono en camara de la ranura del fotograma
bool g_waterConeOn = false;
bool g_waterVcutOn = false;
float g_waterVcut[4] = {};
bool g_waterReflOn = false;
float g_waterRefl[4] = {};
bool g_waterHcutOn = false;
float g_waterHcut[4] = {};
// cielo
ID3D11VertexShader* g_skyVs = nullptr;
ID3D11PixelShader* g_skyPs = nullptr;
ID3D11DepthStencilState* g_skyDss = nullptr;
ID3D11SamplerState* g_skySampler = nullptr;
ID3D11Buffer* g_skyCb[2] = {};
bool g_skyTried = false;
bool g_skyOk = false;
struct OwnSrv { void* res; ID3D11ShaderResourceView* srv; };
OwnSrv g_ownSrv[kMaxTextureHandles] = {};
int g_flushMovedLightsState = 0;                   // 0 sin mirar, 1 bien, -1 no se usa

template <typename T>
T Rd(const char* p, int off) { return *reinterpret_cast<const T*>(p + off); }

// ---------------------------------------------------------------------------
// Captura de las caras que dibuja el motor (hilo del juego). Las funciones
// llamadas dentro de un __try no tienen objetos con destructor.
// ---------------------------------------------------------------------------
double PolyArea(const Poly& poly) {
    double a[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i < poly.n; ++i) {
        const double* p = poly.p[i];
        const double* q = poly.p[(i + 1) % poly.n];
        a[0] += (p[1] - q[1]) * (p[2] + q[2]);
        a[1] += (p[2] - q[2]) * (p[0] + q[0]);
        a[2] += (p[0] - q[0]) * (p[1] + q[1]);
    }
    return 0.5 * std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

int GroupFor(LevelData* ld, int tex, bool translucent) {
    for (size_t i = 0; i < ld->groups.size(); ++i) {
        if (ld->groups[i].tex == tex && ld->groups[i].translucent == translucent) return static_cast<int>(i);
    }
    GpuGroup g = { tex, translucent, 0, 0 };
    ld->groups.push_back(g);
    ld->groupTris.emplace_back();
    return static_cast<int>(ld->groups.size() - 1);
}

// Poligono capturado (mundo), guardado hasta el final de la captura para
// soldarlo con sus vecinos (WeldAndEmit).
void EmitPoly(LevelData* ld, uint32_t sector, const FaceMat& m, const Poly& poly) {
    if (poly.n < 3 || PolyArea(poly) < kMinPolyArea) return;
    CapPoly c;
    c.first = static_cast<uint32_t>(ld->capPos.size() / 3);
    c.count = static_cast<uint32_t>(poly.n);
    c.group = GroupFor(ld, m.tex, m.translucent);
    c.sector = sector;
    for (int k = 0; k < 3; ++k) {
        c.nrm[k] = static_cast<float>(m.n[k]);
        c.U[k] = m.U[k];
        c.V[k] = m.V[k];
    }
    c.du = m.du;
    c.dv = m.dv;
    c.mat[0] = m.self;
    c.mat[1] = m.alpha;
    c.mat[2] = m.fixed;
    for (int i = 0; i < poly.n; ++i) {
        for (int k = 0; k < 3; ++k) ld->capPos.push_back(poly.p[i][k]);
    }
    ld->capPolys.push_back(c);
}

// Soldadura de la captura:
//   1. vertices a menos de 1 mm se unen en uno (rejilla de 4 mm);
//   2. uniones en T: cada vertice que cae sobre una arista de otro poligono
//      (a menos de 1 mm, entre sus extremos) se mete en esa arista.
// Asi los poligonos vecinos comparten exactamente sus vertices y al
// rasterizar no quedan grietas por las que se vea el fondo (el cielo) como
// lineas claras en ciertas posiciones.
inline uint64_t CellKey(int64_t x, int64_t y, int64_t z) {
    return (static_cast<uint64_t>(x & 0x1FFFFF) << 42) | (static_cast<uint64_t>(y & 0x1FFFFF) << 21) |
           static_cast<uint64_t>(z & 0x1FFFFF);
}

void WeldAndEmit(LevelData* ld) {
    const size_t nv = ld->capPos.size() / 3;
    const double tol2 = kWeldTolMm * kWeldTolMm;
    std::vector<uint32_t> vrep(nv, 0u);
    std::vector<double> rep;
    rep.reserve(ld->capPos.size());
    {
        std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
        grid.reserve(nv * 2 + 16);
        for (size_t v = 0; v < nv; ++v) {
            const double* p = &ld->capPos[v * 3];
            int64_t c[3];
            for (int k = 0; k < 3; ++k) c[k] = static_cast<int64_t>(std::floor(p[k] / kWeldCellMm));
            int64_t found = -1;
            for (int dx = -1; dx <= 1 && found < 0; ++dx) {
                for (int dy = -1; dy <= 1 && found < 0; ++dy) {
                    for (int dz = -1; dz <= 1 && found < 0; ++dz) {
                        auto it = grid.find(CellKey(c[0] + dx, c[1] + dy, c[2] + dz));
                        if (it == grid.end()) continue;
                        for (uint32_t r : it->second) {
                            const double* q = &rep[static_cast<size_t>(r) * 3];
                            double d0 = p[0] - q[0], d1 = p[1] - q[1], d2 = p[2] - q[2];
                            if (d0 * d0 + d1 * d1 + d2 * d2 <= tol2) { found = r; break; }
                        }
                    }
                }
            }
            if (found < 0) {
                found = static_cast<int64_t>(rep.size() / 3);
                rep.push_back(p[0]);
                rep.push_back(p[1]);
                rep.push_back(p[2]);
                grid[CellKey(c[0], c[1], c[2])].push_back(static_cast<uint32_t>(found));
            }
            vrep[v] = static_cast<uint32_t>(found);
        }
    }
    const size_t nr = rep.size() / 3;
    std::unordered_map<uint64_t, std::vector<uint32_t>> egrid;
    egrid.reserve(nr + 16);
    for (size_t r = 0; r < nr; ++r) {
        const double* q = &rep[r * 3];
        egrid[CellKey(static_cast<int64_t>(std::floor(q[0] / kEdgeCellMm)), static_cast<int64_t>(std::floor(q[1] / kEdgeCellMm)),
                      static_cast<int64_t>(std::floor(q[2] / kEdgeCellMm)))].push_back(static_cast<uint32_t>(r));
    }
    std::vector<uint32_t> ring;
    std::vector<std::pair<double, uint32_t>> ins;
    for (const CapPoly& c : ld->capPolys) {
        ring.clear();
        for (uint32_t i = 0; i < c.count; ++i) {
            uint32_t ra = vrep[c.first + i];
            uint32_t rb = vrep[c.first + (i + 1) % c.count];
            if (ra == rb) continue;
            ring.push_back(ra);
            const double* a = &rep[static_cast<size_t>(ra) * 3];
            const double* b = &rep[static_cast<size_t>(rb) * 3];
            double d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            double l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (l2 <= 4.0 * tol2) continue;
            int64_t lo[3], hi[3];
            int64_t cells = 1;
            for (int k = 0; k < 3; ++k) {
                double mn = a[k] < b[k] ? a[k] : b[k];
                double mx = a[k] < b[k] ? b[k] : a[k];
                lo[k] = static_cast<int64_t>(std::floor((mn - kWeldTolMm) / kEdgeCellMm));
                hi[k] = static_cast<int64_t>(std::floor((mx + kWeldTolMm) / kEdgeCellMm));
                cells *= (hi[k] - lo[k] + 1);
            }
            if (cells > kMaxEdgeCells) continue;
            ins.clear();
            for (int64_t x = lo[0]; x <= hi[0]; ++x) {
                for (int64_t y = lo[1]; y <= hi[1]; ++y) {
                    for (int64_t z = lo[2]; z <= hi[2]; ++z) {
                        auto it = egrid.find(CellKey(x, y, z));
                        if (it == egrid.end()) continue;
                        for (uint32_t r : it->second) {
                            if (r == ra || r == rb) continue;
                            const double* p = &rep[static_cast<size_t>(r) * 3];
                            double w[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
                            double t = (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / l2;
                            if (!(t > 0.0 && t < 1.0)) continue;
                            double e0 = w[0] - t * d[0], e1 = w[1] - t * d[1], e2 = w[2] - t * d[2];
                            if (e0 * e0 + e1 * e1 + e2 * e2 > tol2) continue;
                            double sa = t * t * l2, sb = (1.0 - t) * (1.0 - t) * l2;
                            if (sa <= tol2 || sb <= tol2) continue;
                            ins.emplace_back(t, r);
                        }
                    }
                }
            }
            if (ins.empty()) continue;
            std::sort(ins.begin(), ins.end());
            uint32_t last = ra;
            for (const auto& e : ins) {
                if (e.second == last) continue;
                ring.push_back(e.second);
                last = e.second;
            }
        }
        // Vertices repetidos seguidos (tras unir) y el cierre.
        size_t w = 0;
        for (size_t i = 0; i < ring.size(); ++i) {
            if (w > 0 && ring[w - 1] == ring[i]) continue;
            ring[w++] = ring[i];
        }
        ring.resize(w);
        while (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
        if (ring.size() < 3) continue;
        uint32_t base = static_cast<uint32_t>(ld->verts.size());
        for (uint32_t r : ring) {
            const double* p = &rep[static_cast<size_t>(r) * 3];
            GpuVertex v;
            for (int k = 0; k < 3; ++k) v.p[k] = static_cast<float>(p[k]);
            v.uv[0] = static_cast<float>(c.U[0] * p[0] + c.U[1] * p[1] + c.U[2] * p[2] - c.du);
            v.uv[1] = static_cast<float>(c.V[0] * p[0] + c.V[1] * p[1] + c.V[2] * p[2] - c.dv);
            for (int k = 0; k < 3; ++k) v.n[k] = c.nrm[k];
            v.sector = c.sector;
            for (int k = 0; k < 3; ++k) v.mat[k] = c.mat[k];
            ld->verts.push_back(v);
        }
        std::vector<uint32_t>& tris = ld->groupTris[static_cast<size_t>(c.group)];
        const double* p0 = &rep[static_cast<size_t>(ring[0]) * 3];
        for (size_t i = 1; i + 1 < ring.size(); ++i) {
            // Sin triangulos de area nula (puntos en T de las aristas del abanico).
            const double* p1 = &rep[static_cast<size_t>(ring[i]) * 3];
            const double* p2 = &rep[static_cast<size_t>(ring[i + 1]) * 3];
            double u[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
            double q[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
            double x = u[1] * q[2] - u[2] * q[1], y = u[2] * q[0] - u[0] * q[2], z = u[0] * q[1] - u[1] * q[0];
            if (x * x + y * y + z * z < 1e-6) continue;
            tris.push_back(base);
            tris.push_back(base + static_cast<uint32_t>(i));
            tris.push_back(base + static_cast<uint32_t>(i + 1));
        }
    }
    ld->capPos.clear();
    ld->capPos.shrink_to_fit();
    ld->capPolys.clear();
    ld->capPolys.shrink_to_fit();
}

// Material con el formato de superficie+0x38 (el que recibe +0x750be0):
// banderas, auto-iluminacion, textura, ejes y desplazamientos, alfa.
bool ReadMaterial(const char* mat, FaceMat* m) {
    const char* surf = mat - kMaterialInSurface;
    m->tex = Rd<int>(surf, kSurfaceTexture);
    if (m->tex < 0) return false;
    const double* U = reinterpret_cast<const double*>(surf + kSurfaceTexU);
    const double* V = reinterpret_cast<const double*>(surf + kSurfaceTexV);
    for (int k = 0; k < 3; ++k) {
        m->U[k] = U[k];
        m->V[k] = V[k];
    }
    m->du = Rd<float>(surf, kSurfaceShiftU);
    m->dv = Rd<float>(surf, kSurfaceShiftV);
    unsigned flags = Rd<unsigned>(surf, kSurfaceFlags);
    float self = (flags & kFlagNoSelfIllum) ? 0.0f : Rd<float>(surf, kSurfaceSelfIllum);
    if (!(self == self) || self < 0.0f) self = 0.0f;
    if (self > 4.0f) self = 4.0f;
    float alpha = (flags & kFlagOwnAlpha) ? Rd<float>(surf, kSurfaceAlpha) / 255.0f : 1.0f;
    if (!(alpha == alpha)) alpha = 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    m->self = self;
    m->alpha = alpha;
    m->fixed = (flags & kFlagFixedLight) ? 1.0f : 0.0f;
    m->translucent = alpha < 0.999f;
    return true;
}

const char* Neighbor(const LevelData* ld, const char* surf, int off) {
    unsigned idx = Rd<unsigned>(surf, off);
    if (idx >= ld->sectorCount) return nullptr;
    return ld->sectorArray[idx];
}

bool SectorLiquidFlow(const char* s) {
    return s && Rd<int>(s, kSectorActive) != 0 && Rd<const void*>(s, kSectorWaterPlane) != nullptr &&
           Rd<double>(s, kSectorWaterFlow) > 1e-6;
}

// Sector de una puerta que se desliza (ver kVtMapSurfaces).
bool SectorSliding(const char* s) {
    if (!s) return false;
    const char* p = Rd<const char*>(s, kSectorWaterPlane);
    if (!p) return false;
    uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
    for (uintptr_t v : kVtMapSurfaces) {
        if (vt == g_base + v) return true;
    }
    return false;
}

// Lo que el motor dibuja de un portal hacia este sector cambia durante la
// partida: cerrado (Active = 0: el portal se dibuja como cara; los muros
// que se derriban), puerta que se desliza o liquido que corre.
bool NeighborDynamic(const char* s) {
    return s && (Rd<int>(s, kSectorActive) == 0 || SectorSliding(s) || SectorLiquidFlow(s));
}

uint32_t HashPtr(const void* p) {
    uint64_t v = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)) >> 3;
    v ^= v >> 29;
    v *= 0x9E3779B97F4A7C15ull;
    return static_cast<uint32_t>(v >> 32);
}

uint32_t PowerOfTwoAtLeast(uint32_t n) {
    uint32_t p = 1024;
    while (p < n && p < (1u << 26)) p <<= 1;
    return p;
}

bool SurfaceSetInsert(LevelData* ld, const char* p) {
    for (uint32_t i = HashPtr(p) & ld->surfMask, k = 0; k <= ld->surfMask; ++k, i = (i + 1) & ld->surfMask) {
        if (ld->surfKeys[i] == p) return false;
        if (!ld->surfKeys[i]) { ld->surfKeys[i] = p; return true; }
    }
    return false;
}

bool SurfaceSetContains(const LevelData* ld, const char* p) {
    if (!ld->surfMask) return false;
    for (uint32_t i = HashPtr(p) & ld->surfMask, k = 0; k <= ld->surfMask; ++k, i = (i + 1) & ld->surfMask) {
        if (ld->surfKeys[i] == p) return true;
        if (!ld->surfKeys[i]) return false;
    }
    return false;
}

void SectorMapInsert(LevelData* ld, const char* sec, uint32_t idx) {
    for (uint32_t i = HashPtr(sec) & ld->secMask, k = 0; k <= ld->secMask; ++k, i = (i + 1) & ld->secMask) {
        if (!ld->secKeys[i] || ld->secKeys[i] == sec) {
            ld->secKeys[i] = sec;
            ld->secVals[i] = idx;
            return;
        }
    }
}

bool SectorMapFind(const LevelData* ld, const char* sec, uint32_t* idx) {
    if (!ld->secMask || !sec) return false;
    for (uint32_t i = HashPtr(sec) & ld->secMask, k = 0; k <= ld->secMask; ++k, i = (i + 1) & ld->secMask) {
        if (ld->secKeys[i] == sec) { *idx = ld->secVals[i]; return true; }
        if (!ld->secKeys[i]) return false;
    }
    return false;
}

bool ReadLevelIdentityRaw(uintptr_t base, long long level, char*** sectors, unsigned* count, const char** verts,
                          unsigned* nverts) {
    __try {
        *count = *reinterpret_cast<unsigned*>(level + kLevelSectorCount);
        *sectors = *reinterpret_cast<char***>(level + kLevelSectorArray);
        *verts = *reinterpret_cast<const char* const*>(base + kLevelVertexData);
        *nverts = *reinterpret_cast<const unsigned*>(base + kLevelVertexCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *sectors && *verts && *count > 0 && *count < 200000 && *nverts > 0;
}

bool SameLevel(const LevelData* ld, long long level, char** sectors, unsigned count, const char* verts, unsigned nverts) {
    return ld && ld->level == level && ld->sectorArray == sectors && ld->sectorCount == count && ld->vertTable == verts &&
           ld->vertCount == nverts;
}

// Superficies del nivel (para dimensionar el conjunto).
bool CountSurfacesRaw(const LevelData* ld, uint32_t* total) {
    __try {
        uint64_t n = 0;
        for (unsigned s = 0; s < ld->sectorCount; ++s) {
            const char* sec = ld->sectorArray[s];
            if (!sec) continue;
            unsigned k = Rd<unsigned>(sec, kSectorSurfaceCount);
            if (k < 100000) n += k;
        }
        *total = static_cast<uint32_t>(n > 4000000 ? 4000000 : n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void FinishBuild(LevelData* ld) {
    WeldAndEmit(ld);
    ld->geoValid = !ld->verts.empty();
    for (int k = 0; k < 3; ++k) {
        ld->geoMn[k] = 1e300;
        ld->geoMx[k] = -1e300;
    }
    for (const GpuVertex& v : ld->verts) {
        for (int k = 0; k < 3; ++k) {
            if (v.p[k] < ld->geoMn[k]) ld->geoMn[k] = v.p[k];
            if (v.p[k] > ld->geoMx[k]) ld->geoMx[k] = v.p[k];
        }
    }
    size_t total = 0;
    for (const std::vector<uint32_t>& t : ld->groupTris) total += t.size();
    ld->indices.reserve(total);
    // Opacos primero, translucidos despues (se dibujan en ese orden).
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t g = 0; g < ld->groups.size(); ++g) {
            if (ld->groups[g].translucent != (pass == 1)) continue;
            ld->groups[g].first = static_cast<uint32_t>(ld->indices.size());
            ld->groups[g].count = static_cast<uint32_t>(ld->groupTris[g].size());
            ld->indices.insert(ld->indices.end(), ld->groupTris[g].begin(), ld->groupTris[g].end());
        }
    }
    ld->groupTris.clear();
    ld->groupTris.shrink_to_fit();
    for (int i = 0; i < kSlots; ++i) {
        ld->slots[i].sectors.assign(ld->sectorCount, SectorGpu{});
        ld->slots[i].lightRefs.assign(kMaxLightRefs, LightRefGpu{});
        ld->slots[i].lights.assign(kMaxLights, LightGpu{});
        ld->slots[i].cones.assign(kMaxCones, ConeGpu{});
        ld->slots[i].planes.assign(kMaxPlanes, PlaneGpu{});
        ld->slots[i].handles.assign(ld->groups.size(), static_cast<uint16_t>(0xffff));
    }
}

void DeleteRetired() {
    uint32_t renderGen = g_renderLevelGen.load(std::memory_order_acquire);
    for (size_t i = 0; i < g_retired.size();) {
        if (g_retired[i]->gen < renderGen) {
            delete g_retired[i];
            g_retired.erase(g_retired.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

// Un poligono que el motor manda a dibujar (camara) -> mundo, con el
// material activo y el sector en curso.
void CaptureRaw(const char* face, const char* poly) {
    LevelData* ld = g_captureLd;
    __try {
        unsigned n = Rd<unsigned>(poly, kPolyListCount);
        if (n < 3 || n > static_cast<unsigned>(kMaxPolyVerts)) return;
        Poly w;
        w.n = static_cast<int>(n);
        for (unsigned j = 0; j < n; ++j) {
            const double* v = reinterpret_cast<const double*>(poly + kPolyListVerts + static_cast<size_t>(j) * kPolyListStride);
            double d[3] = { v[0] - g_captureT[0], v[1] - g_captureT[1], v[2] - g_captureT[2] };
            for (int c = 0; c < 3; ++c) {
                w.p[j][c] = d[0] * g_captureInv[0 * 3 + c] + d[1] * g_captureInv[1 * 3 + c] + d[2] * g_captureInv[2 * 3 + c];
            }
        }
        const char* mat = g_captureMat ? g_captureMat : face + kMaterialInSurface;
        FaceMat m;
        if (!ReadMaterial(mat, &m)) return;
        const double* pl = reinterpret_cast<const double*>(face + kSurfacePlane);
        for (int k = 0; k < 3; ++k) m.n[k] = pl[k];
        const char* sec = *reinterpret_cast<const char* const*>(g_base + kCurrentSectorPtr);
        uint32_t si = 0;
        if (!SectorMapFind(ld, sec, &si)) return;
        EmitPoly(ld, si, m, w);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall HookedFaceEmit(char* face, char* poly) {
    if (g_capturing && g_captureSurface && face == g_captureSurface && poly) CaptureRaw(face, poly);
    g_origFaceEmit(face, poly);
}

void __fastcall HookedSetMaterial(char* raster, char* mat) {
    if (g_capturing) g_captureMat = mat;
    g_origSetMaterial(raster, mat);
}

bool BytesMatch(uintptr_t addr, const unsigned char* bytes, size_t n) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(addr), bytes, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool EnsureCaptureHooks(uintptr_t base) {
    if (g_captureHooksTried) return g_captureHooksOk;
    g_captureHooksTried = true;
    uintptr_t emit = base + kFaceEmit, setMat = base + kSetMaterial;
    if (!BytesMatch(emit, kFaceEmitPrologue, sizeof(kFaceEmitPrologue)) ||
        !BytesMatch(setMat, kSetMaterialPrologue, sizeof(kSetMaterialPrologue))) {
        HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: +0x9a8e0 o +0x750be0 no tienen el prologo esperado: "
                                    "sin captura del nivel.");
        return false;
    }
    void* e = reinterpret_cast<void*>(emit);
    void* m = reinterpret_cast<void*>(setMat);
    bool ok = MH_CreateHook(e, reinterpret_cast<void*>(&HookedFaceEmit), reinterpret_cast<void**>(&g_origFaceEmit)) == MH_OK &&
              MH_CreateHook(m, reinterpret_cast<void*>(&HookedSetMaterial), reinterpret_cast<void**>(&g_origSetMaterial)) == MH_OK &&
              MH_EnableHook(e) == MH_OK && MH_EnableHook(m) == MH_OK;
    g_captureHooksOk = ok;
    if (!ok) HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: no se pudieron enganchar +0x9a8e0 / +0x750be0.");
    return ok;
}

bool Invert3(const double* fw, double inv[9]) {
    double a = fw[0], b = fw[1], c = fw[2];
    double d = fw[4], e = fw[5], f = fw[6];
    double g = fw[8], h = fw[9], i = fw[10];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-12)) return false;
    double k = 1.0 / det;
    inv[0] = A * k;
    inv[1] = -(b * i - c * h) * k;
    inv[2] = (b * f - c * e) * k;
    inv[3] = B * k;
    inv[4] = (a * i - c * g) * k;
    inv[5] = -(a * f - c * d) * k;
    inv[6] = C * k;
    inv[7] = -(a * h - b * g) * k;
    inv[8] = (a * e - b * d) * k;
    return true;
}

// Empieza una captura para este nivel: devuelve false si no se puede.
bool StartCapture(uintptr_t base, long long level, char** sectors, unsigned count, const char* verts, unsigned nverts,
                  const double* fw) {
    if (!EnsureCaptureHooks(base)) return false;
    if (!Invert3(fw, g_captureInv)) return false;
    for (int k = 0; k < 3; ++k) g_captureT[k] = fw[12 + k];
    LevelData* ld = new LevelData();
    ld->gen = g_nextGen++;
    ld->level = level;
    ld->sectorArray = sectors;
    ld->sectorCount = count;
    ld->vertTable = verts;
    ld->vertCount = nverts;
    uint32_t surfaces = 0;
    if (!CountSurfacesRaw(ld, &surfaces)) {
        delete ld;
        return false;
    }
    uint32_t ssize = PowerOfTwoAtLeast(surfaces * 2 + 16);
    ld->surfKeys.assign(ssize, nullptr);
    ld->surfMask = ssize - 1;
    uint32_t csize = PowerOfTwoAtLeast(count * 2 + 16);
    ld->secKeys.assign(csize, nullptr);
    ld->secVals.assign(csize, 0u);
    ld->secMask = csize - 1;
    for (unsigned s = 0; s < count; ++s) {
        if (sectors[s]) SectorMapInsert(ld, sectors[s], s);
    }
    g_captureLd = ld;
    g_capturing = true;
    g_captureSurface = nullptr;
    g_captureMat = nullptr;
    return true;
}

// Fin de la pasada de captura: se publica la geometria.
bool FinishCapture() {
    LevelData* ld = g_captureLd;
    g_captureLd = nullptr;
    g_capturing = false;
    g_captureSurface = nullptr;
    g_captureMat = nullptr;
    if (!ld) return false;
    FinishBuild(ld);
    if (ld->indices.empty()) {
        HookLogger::Instance().Line("[DIORAMA][GPU] AVISO: no se pudo capturar el nivel; lo dibuja el motor.");
        delete ld;
        return false;
    }
    LevelData* cur = g_current.load(std::memory_order_relaxed);
    if (cur) g_retired.push_back(cur);
    g_current.store(ld, std::memory_order_release);
    DeleteRetired();
    return true;
}

// --- ranura de cada pasada (hilo del juego) -------------------------------------
int LightIndex(uintptr_t base, const char* light, FrameSlot* sl, float lightScale) {
    uintptr_t h = (reinterpret_cast<uintptr_t>(light) >> 4) * 2654435761u;
    for (int probe = 0; probe < 2048; ++probe) {
        int i = static_cast<int>((h + static_cast<uintptr_t>(probe)) & 2047u);
        if (g_lightStamps[i] != g_lightStamp) {
            // nueva
            if (sl->lightCount >= kMaxLights) return -1;
            uintptr_t vt = Rd<uintptr_t>(light, 0) - base;
            LightGpu& L = sl->lights[sl->lightCount];
            const unsigned char* c = reinterpret_cast<const unsigned char*>(light + kLightColor);
            float in = Rd<float>(light, kLightIntensity);
            if (!(in == in)) in = 0.0f;
            int result = -1;
            if (vt == kVtOmniLight) {
                const double* p = reinterpret_cast<const double*>(light + kOmniWorldPos);
                float k = in * 1e6f * lightScale / 255.0f;
                L.type = 0;
                for (int j = 0; j < 3; ++j) { L.pos[j] = static_cast<float>(p[j]); L.col[j] = c[j] * k; }
                result = static_cast<int>(sl->lightCount++);
            } else if (vt == kVtAmbientLight || vt == kVtGradientLight) {
                L.type = 1;
                for (int j = 0; j < 3; ++j) { L.pos[j] = 0.0f; L.col[j] = c[j] * in / 255.0f; }
                result = static_cast<int>(sl->lightCount++);
            } else if (vt == kVtPlaneLight) {
                const double* d = reinterpret_cast<const double*>(light + kPlaneWorldDir);
                L.type = 2;
                for (int j = 0; j < 3; ++j) { L.pos[j] = static_cast<float>(d[j]); L.col[j] = c[j] * in / 255.0f; }
                result = static_cast<int>(sl->lightCount++);
            }
            L.pad = 0.0f;
            g_lightStamps[i] = g_lightStamp;
            g_lightKeys[i] = light;
            g_lightVals[i] = result;
            return result;
        }
        if (g_lightKeys[i] == light) return g_lightVals[i];
    }
    return -1;
}

uint16_t TextureHandleRaw(const char* const* tbl, unsigned tcount, int tex) {
    if (!tbl || tex < 0 || static_cast<unsigned>(tex) >= tcount || tcount >= 1000000) return 0xffff;
    const char* e = tbl[tex];
    const char* obj = e ? Rd<const char*>(e, kTexEntryBgfx) : nullptr;
    return obj ? Rd<uint16_t>(obj, kBgfxTexHandle) : static_cast<uint16_t>(0xffff);
}

const char* NextConeOfLight(const char* cone) {
    const char* link = Rd<const char*>(cone, kConeNextLink);
    return link ? Rd<const char*>(link, kLinkObject) : nullptr;
}

// Orientacion de los planos del mundo de las aristas de un cono frente a su
// copia en camara (la que usa +0x9a950): un punto del plano del mundo y otro
// 10 cm por su lado positivo, llevados a camara con la fromWorld de la
// pasada, tienen que quedar sobre el plano de camara y por su lado positivo.
struct ConeSignVotes {
    unsigned same, opposite, planes;
};

void VoteConeSign(const char* cone, const double* fw, ConeSignVotes* v) {
    unsigned n = Rd<unsigned>(cone, kConeEdgeCount);
    const char* edges = Rd<const char*>(cone, kConeEdgeData);
    if (!edges || n == 0 || n > 256) return;
    for (unsigned i = 0; i < n && i < 8; ++i) {
        const char* e = edges + static_cast<size_t>(i) * kConeEdgeStride;
        const double* w = reinterpret_cast<const double*>(e);
        const double* c = Rd<const double*>(e, kConeEdgeCamPlane);
        if (!c) continue;
        double nn = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
        if (!(nn > 1e-12) || !(nn < 1e12)) continue;
        double inv = 1.0 / std::sqrt(nn);
        double p0[3], p1[3];
        for (int k = 0; k < 3; ++k) {
            p0[k] = -w[3] * w[k] / nn;
            p1[k] = p0[k] + w[k] * inv * 100.0;
        }
        double e0 = c[3], e1 = c[3];
        for (int r = 0; r < 3; ++r) {
            double c0 = p0[0] * fw[0 * 4 + r] + p0[1] * fw[1 * 4 + r] + p0[2] * fw[2 * 4 + r] + fw[12 + r];
            double c1 = p1[0] * fw[0 * 4 + r] + p1[1] * fw[1 * 4 + r] + p1[2] * fw[2 * 4 + r] + fw[12 + r];
            e0 += c[r] * c0;
            e1 += c[r] * c1;
        }
        if (!(e0 == e0) || !(e1 == e1) || std::fabs(e1 - e0) < 1e-3) continue;
        if (e1 > e0) ++v->same;
        else ++v->opposite;
        ++v->planes;
    }
}

// Luces de un sector, agrupadas por luz, con sus conos (los de la lista del
// sector y los siguientes de la misma luz). Sin __try propio: va dentro del
// de FillSlotRaw.
struct SectorLight {
    const char* light;
    int index;
    int coneCount;
    const char* cones[16];
};

void FillSectorLights(uintptr_t base, LevelData* ld, FrameSlot* sl, const char* sec, float lightScale, bool useCones,
                      ConeSignVotes* votes) {
    SectorLight loc[kMaxLightsPerSector];
    int nloc = 0;
    const char* node = Rd<const char*>(sec, kSectorConeList);
    for (int k = 0; k < kMaxConesPerSector && node; ++k, node = Rd<const char*>(node, kLinkNext)) {
        const char* cone = Rd<const char*>(node, kLinkObject);
        const char* light = cone ? Rd<const char*>(cone, kConeLight) : nullptr;
        if (!light) continue;
        int j = 0;
        while (j < nloc && loc[j].light != light) ++j;
        if (j == nloc) {
            if (nloc >= static_cast<int>(kMaxLightsPerSector)) continue;
            loc[j].light = light;
            loc[j].index = LightIndex(base, light, sl, lightScale);
            loc[j].coneCount = 0;
            ++nloc;
        }
        if (loc[j].index < 0) continue;
        const char* c = cone;
        for (int ch = 0; ch < kMaxChainCones && c; ++ch, c = NextConeOfLight(c)) {
            bool dup = false;
            for (int q = 0; q < loc[j].coneCount; ++q) {
                if (loc[j].cones[q] == c) { dup = true; break; }
            }
            if (dup || loc[j].coneCount >= 16) break;
            loc[j].cones[loc[j].coneCount++] = c;
            if (votes && votes->planes < 64) VoteConeSign(c, sl->fw, votes);
        }
    }
    for (int j = 0; j < nloc; ++j) {
        if (loc[j].index < 0 || sl->lightRefCount >= kMaxLightRefs) continue;
        LightRefGpu& r = sl->lightRefs[sl->lightRefCount++];
        r.light = static_cast<uint32_t>(loc[j].index);
        r.coneStart = sl->coneCount;
        r.coneCount = 0;
        r.pad = 0;
        if (!useCones) continue;
        uint32_t coneStart = sl->coneCount, planeStart = sl->planeCount;
        bool full = false;
        for (int q = 0; q < loc[j].coneCount; ++q) {
            const char* cone = loc[j].cones[q];
            unsigned n = Rd<unsigned>(cone, kConeEdgeCount);
            const char* edges = Rd<const char*>(cone, kConeEdgeData);
            if (!edges || n > 256) n = 0;
            if (n > static_cast<unsigned>(kMaxConePlanes)) n = kMaxConePlanes;
            if (sl->coneCount >= kMaxCones || sl->planeCount + n > kMaxPlanes) { full = true; break; }
            ConeGpu& cg = sl->cones[sl->coneCount++];
            cg.planeStart = sl->planeCount;
            cg.planeCount = n;
            for (unsigned i = 0; i < n; ++i) {
                const double* w = reinterpret_cast<const double*>(edges + static_cast<size_t>(i) * kConeEdgeStride);
                PlaneGpu& p = sl->planes[sl->planeCount++];
                for (int k = 0; k < 4; ++k) p.v[k] = static_cast<float>(w[k] * ld->coneSign);
            }
        }
        if (full) {
            // Sin sitio: esta luz en todo el sector (como antes de los conos).
            sl->coneCount = coneStart;
            sl->planeCount = planeStart;
        } else {
            r.coneCount = sl->coneCount - coneStart;
        }
    }
}

// Rehace los conos de las luces que se han movido (ver kFlushMovedLights).
// Devuelve cuantas habia; -1 si no se ha podido.
int FlushMovedLightsRaw(uintptr_t base) {
    __try {
        if (g_flushMovedLightsState == 0) {
            g_flushMovedLightsState =
                std::memcmp(reinterpret_cast<const void*>(base + kFlushMovedLights), kFlushMovedLightsPrologue,
                            sizeof(kFlushMovedLightsPrologue)) == 0 ? 1 : -1;
        }
        if (g_flushMovedLightsState < 0) return -1;
        unsigned n = *reinterpret_cast<const unsigned*>(base + kMovedLightCount);
        if (n == 0) return 0;
        if (n > 100000u) return -1;
        reinterpret_cast<void(__fastcall*)()>(base + kFlushMovedLights)();
        return static_cast<int>(n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_flushMovedLightsState = -1;
        return -1;
    }
}

bool FillSlotRaw(uintptr_t base, LevelData* ld, FrameSlot* sl, const GpuLevelPass& pass, ConeSignVotes* votes) {
    __try {
        const char* raster = *reinterpret_cast<const char* const*>(base + kRasterPtr);
        float lightScale = raster ? Rd<float>(raster, kRasterLightScale) : 1.0f;
        if (!(lightScale == lightScale) || lightScale < 0.0f || lightScale > 100.0f) lightScale = 1.0f;
        float fixedLight = raster ? Rd<float>(raster, kRasterFixedLight) : 255.0f;
        if (!(fixedLight == fixedLight) || fixedLight < 0.0f || fixedLight > 4096.0f) fixedLight = 255.0f;
        sl->fixedLight = fixedLight / 255.0f;
        // Textura de cada grupo (el handle puede cambiar: texturas animadas).
        const char* const* tbl = raster ? Rd<const char* const*>(raster, kRasterTexTable) : nullptr;
        unsigned tcount = raster ? Rd<unsigned>(raster, kRasterTexCount) : 0u;
        for (size_t g = 0; g < ld->groups.size(); ++g) sl->handles[g] = TextureHandleRaw(tbl, tcount, ld->groups[g].tex);
        // Cielo: las seis caras del raster (las que tengan textura).
        sl->skyOn = false;
        if (pass.skyOn && raster) {
            float k = Rd<float>(raster, kRasterSkyLight);
            if (!(k == k) || k < 0.0f || k > 4.0f) k = 1.0f;
            sl->skyColor = k * 220.0f / 255.0f;
            int faces = 0;
            for (int f = 0; f < kSkyFaces; ++f) {
                const char* face = raster + kRasterSkyFaces + f * kSkyFaceStride;
                uint16_t h = TextureHandleRaw(tbl, tcount, Rd<int>(face, kSkyFaceTexture));
                if (h == 0xffff) h = ld->skyHandleCache[f];      // la ultima buena del nivel
                else ld->skyHandleCache[f] = h;
                sl->skyHandles[f] = h;
                if (h != 0xffff) ++faces;
            }
            sl->skyOn = faces > 0;
        }
        // Sectores: area, luz ambiente y plana, y luces de sus conos.
        ++g_lightStamp;
        if (g_lightStamp == 0) ++g_lightStamp;
        sl->lightCount = 0;
        sl->lightRefCount = 0;
        sl->coneCount = 0;
        sl->planeCount = 0;
        bool useCones = ld->coneSign != 0.0f;
        for (unsigned s = 0; s < ld->sectorCount; ++s) {
            SectorGpu& o = sl->sectors[s];
            std::memset(&o, 0, sizeof(o));
            const char* sec = ld->sectorArray[s];
            if (!sec) continue;
            const double* mn = reinterpret_cast<const double*>(sec + kSectorBoxMin);
            const double* mx = reinterpret_cast<const double*>(sec + kSectorBoxMax);
            o.visible = (!pass.sectorInArea || pass.sectorInArea(mn, mx)) ? 1u : 0u;
            const char* amb = sec + kSectorAmbient;
            const unsigned char* ac = reinterpret_cast<const unsigned char*>(amb + kLightColor);
            float ai = Rd<float>(amb, kLightIntensity);
            if (!(ai == ai)) ai = 0.0f;
            const char* pln = sec + kSectorPlaneLight;
            const unsigned char* pc = reinterpret_cast<const unsigned char*>(pln + kLightColor);
            float pi = Rd<float>(pln, kLightIntensity);
            if (!(pi == pi)) pi = 0.0f;
            const double* pd = reinterpret_cast<const double*>(pln + kPlaneWorldDir);
            for (int j = 0; j < 3; ++j) {
                o.amb[j] = ac[j] * ai / 255.0f;
                o.pcol[j] = pc[j] * pi / 255.0f;
                o.pdir[j] = static_cast<float>(pd[j]);
            }
            uint32_t start = sl->lightRefCount;
            FillSectorLights(base, ld, sl, sec, lightScale, useCones, votes);
            o.lightStart = start;
            o.lightCount = sl->lightRefCount - start;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void FillSlotView(FrameSlot* sl, const GpuLevelPass& pass) {
    const double* fw = pass.fromWorld;
    for (int i = 0; i < 16; ++i) sl->fw[i] = fw[i];
    // Camara en el mundo: c[r] = -(t . fila r).
    for (int r = 0; r < 3; ++r) {
        sl->cam[r] = static_cast<float>(-(fw[12] * fw[r * 4 + 0] + fw[13] * fw[r * 4 + 1] + fw[14] * fw[r * 4 + 2]));
    }
    sl->cutOn = pass.cutOn;
    sl->cutY = static_cast<float>(pass.cutY);
    // Corte de altura en camara: n = fila 1 de la 3x3, d' = -cutY - n.t.
    sl->cutCam[0] = static_cast<float>(fw[4]);
    sl->cutCam[1] = static_cast<float>(fw[5]);
    sl->cutCam[2] = static_cast<float>(fw[6]);
    sl->cutCam[3] = static_cast<float>(-pass.cutY - (fw[4] * fw[12] + fw[5] * fw[13] + fw[6] * fw[14]));
    sl->poseOn = pass.hmdPose != nullptr;
    for (int k = 0; k < 12; ++k) sl->pose[k] = sl->poseOn ? pass.hmdPose[k] : 0.0f;
    sl->coneOn = pass.coneOn && pass.conePlanes;
    sl->reflOn = false;
    sl->vcutOn = pass.vcutOn && pass.vcutPlane && pass.vcutPlaneCam;
    for (int k = 0; k < 4; ++k) {
        sl->vcut[k] = sl->vcutOn ? static_cast<float>(pass.vcutPlane[k]) : 0.0f;
        sl->vcutCam[k] = sl->vcutOn ? static_cast<float>(pass.vcutPlaneCam[k]) : 0.0f;
    }
    // Planos del cono de camara al mundo: m.(p M + t) + d = p.(M m) + (m.t + d).
    for (int i = 0; i < kGpuConePlaneCount; ++i) {
        if (!sl->coneOn) {
            for (int k = 0; k < 4; ++k) sl->cone[i][k] = sl->coneCam[i][k] = 0.0f;
            continue;
        }
        const double* m = pass.conePlanes[i];
        for (int k = 0; k < 4; ++k) sl->coneCam[i][k] = static_cast<float>(m[k]);
        if (pass.coneRound && i == 2) {
            // Vertice (punto) de camara al mundo: q = (p - t) M^T.
            for (int r = 0; r < 3; ++r) {
                double q = (m[0] - fw[12]) * fw[r * 4 + 0] + (m[1] - fw[13]) * fw[r * 4 + 1] + (m[2] - fw[14]) * fw[r * 4 + 2];
                sl->cone[i][r] = static_cast<float>(q);
            }
            sl->cone[i][3] = 0.0f;
            continue;
        }
        if (pass.coneRound && i == 3) {
            for (int k = 0; k < 4; ++k) sl->cone[i][k] = static_cast<float>(m[k]);
            continue;
        }
        double n[3];
        for (int r = 0; r < 3; ++r) n[r] = fw[r * 4 + 0] * m[0] + fw[r * 4 + 1] * m[1] + fw[r * 4 + 2] * m[2];
        double d = m[0] * fw[12] + m[1] * fw[13] + m[2] * fw[14] + m[3];
        sl->cone[i][0] = static_cast<float>(n[0]);
        sl->cone[i][1] = static_cast<float>(n[1]);
        sl->cone[i][2] = static_cast<float>(n[2]);
        sl->cone[i][3] = static_cast<float>(d);
    }
}

// --- marca del fotograma en bgfx ----------------------------------------------
void WriteMarkerRaw(uintptr_t base, uint32_t seq, bool valid) {
    __try {
        char* ctx = *reinterpret_cast<char**>(base + kBgfxContextPtr);
        if (!ctx) return;
        uint32_t* m = reinterpret_cast<uint32_t*>(ctx + kBgfxCtxViews + static_cast<uintptr_t>(kBgfxMarkerView) * kBgfxViewSize +
                                                  kBgfxViewMatrix);
        m[0] = valid ? kMarkerMagic : 0u;
        m[1] = seq;
        m[2] = ~seq;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ReadMarkerRaw(const char* frame) {
    __try {
        const uint32_t* m = reinterpret_cast<const uint32_t*>(frame + kBgfxFrameViews +
                                                              static_cast<uintptr_t>(kBgfxMarkerView) * kBgfxViewSize +
                                                              kBgfxViewMatrix);
        if (m[0] == kMarkerMagic && m[2] == ~m[1]) {
            g_frameSeq = m[1];
            g_frameSeqValid = true;
        } else {
            g_frameSeqValid = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_frameSeqValid = false;
    }
}

void __fastcall HookedSubmit(void* self, void* frame, void* a3, void* a4) {
    if (frame) ReadMarkerRaw(static_cast<const char*>(frame));
    else g_frameSeqValid = false;
    g_origSubmit(self, frame, a3, a4);
}

bool PrologueMatches(uintptr_t addr) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(addr), kSubmitPrologue, sizeof(kSubmitPrologue)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void EnsureSubmitHook(uintptr_t base) {
    if (g_submitTried) return;
    g_submitTried = true;
    uintptr_t target = base + kRendererSubmit;
    if (!PrologueMatches(target)) {
        HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: RendererContextD3D11::submit (+0x67d310) no tiene el prologo "
                                    "esperado: el nivel en la GPU queda desactivado.");
        return;
    }
    void* tgt = reinterpret_cast<void*>(target);
    bool ok = MH_CreateHook(tgt, reinterpret_cast<void*>(&HookedSubmit), reinterpret_cast<void**>(&g_origSubmit)) == MH_OK &&
              MH_EnableHook(tgt) == MH_OK;
    g_submitHooked = ok;
    if (!ok) HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: no se pudo enganchar RendererContextD3D11::submit.");
}

// ---------------------------------------------------------------------------
// Hilo de render
// ---------------------------------------------------------------------------
const char kShaderSource[] =
    "cbuffer PerEye : register(b0)\n"
    "{\n"
    "    row_major float4x4 g_viewProj;\n"
    "    float4 g_cam;\n"
    "    float4 g_cut;\n"
    "    float4 g_misc;\n"
    "    float4 g_cone[8];\n"
    "    float4 g_vcut;\n"
    "};\n"
    // Cono de vision: lo mas que p queda fuera (<= 0,5: dentro, se quita).
    // Con la normal de g_cone[4] nula es el tronco de cono redondo (ver
    // GpuLevelPass::coneRound); si no, los 8 planos.
    "float ConeMax(float3 p)\n"
    "{\n"
    "    if (dot(g_cone[4].xyz, g_cone[4].xyz) < 0.25) {\n"
    "        float t = dot(g_cone[1].xyz, p) + g_cone[1].w;\n"
    "        float3 r = (p - g_cone[2].xyz) - t * g_cone[1].xyz;\n"
    "        float side = length(r) * g_cone[3].x - t * g_cone[3].y - g_cone[3].z;\n"
    "        return max(max(dot(g_cone[0].xyz, p) + g_cone[0].w, side), dot(g_cone[7].xyz, p) + g_cone[7].w);\n"
    "    }\n"
    "    float m = -1e30;\n"
    "    [unroll] for (int k = 0; k < 8; ++k) m = max(m, dot(g_cone[k].xyz, p) + g_cone[k].w);\n"
    "    return m;\n"
    "}\n"
    "struct SectorInfo { float3 amb; uint lightStart; float3 pcol; uint lightCount; float3 pdir; uint visible; };\n"
    "struct LightInfo { float3 pos; uint type; float3 col; float pad; };\n"
    "Texture2D g_tex : register(t0);\n"
    "StructuredBuffer<SectorInfo> g_sectors : register(t1);\n"
    "StructuredBuffer<uint4> g_lightRefs : register(t2);\n"
    "StructuredBuffer<LightInfo> g_lights : register(t3);\n"
    "StructuredBuffer<uint2> g_cones : register(t4);\n"
    "StructuredBuffer<float4> g_planes : register(t5);\n"
    "SamplerState g_samp : register(s0);\n"
    "struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float3 nrm : NORMAL; uint sector : TEXCOORD1; float3 mat : TEXCOORD2; };\n"
    "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 wpos : TEXCOORD1; float3 nrm : TEXCOORD2;\n"
    "              nointerpolation uint sector : TEXCOORD3; nointerpolation float3 mat : TEXCOORD4; };\n"
    "PSIn VSMain(VSIn i)\n"
    "{\n"
    "    PSIn o;\n"
    "    o.pos = mul(float4(i.pos, 1.0), g_viewProj);\n"
    "    o.uv = i.uv;\n"
    "    o.wpos = i.pos;\n"
    "    o.nrm = i.nrm;\n"
    "    o.sector = i.sector;\n"
    "    o.mat = i.mat;\n"
    "    return o;\n"
    "}\n"
    "struct PSOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };\n"
    "PSOut PSMain(PSIn i)\n"
    "{\n"
    "    SectorInfo s = g_sectors[i.sector];\n"
    "    if (s.visible == 0) discard;\n"
    "    if (g_cut.x > 0.5 && i.wpos.y < g_cut.y) discard;\n"
    "    if (g_cut.w > 0.5 && ConeMax(i.wpos) <= 0.5) discard;\n"
    "    if (g_misc.y > 0.5 && dot(g_vcut.xyz, i.wpos) + g_vcut.w <= 0.0) discard;\n"
    "    float3 c;\n"
    "    if (i.mat.z > 0.5) c = g_misc.xxx;\n"
    "    else c = i.mat.xxx + s.amb + s.pcol * max(0.0, -dot(s.pdir, i.nrm));\n"
    "    for (uint li = 0; li < s.lightCount; ++li) {\n"
    "        uint4 R = g_lightRefs[s.lightStart + li];\n"
    "        bool lit = R.z == 0;\n"
    "        for (uint ci = 0; ci < R.z && !lit; ++ci) {\n"
    "            uint2 C = g_cones[R.y + ci];\n"
    "            bool inside = true;\n"
    "            for (uint pi = 0; pi < C.y && inside; ++pi) {\n"
    "                float4 P = g_planes[C.x + pi];\n"
    "                inside = dot(P.xyz, i.wpos) + P.w > -0.5;\n"
    "            }\n"
    "            lit = inside;\n"
    "        }\n"
    "        if (!lit) continue;\n"
    "        LightInfo L = g_lights[R.x];\n"
    "        if (L.type == 0) {\n"
    "            float3 d = L.pos - i.wpos;\n"
    "            float r2 = max(dot(d, d), 1.0);\n"
    "            c += L.col * (max(0.0, dot(i.nrm, d)) / (r2 * sqrt(r2)));\n"
    "        } else if (L.type == 1) {\n"
    "            c += L.col;\n"
    "        } else {\n"
    "            c += L.col * max(0.0, -dot(L.pos, i.nrm));\n"
    "        }\n"
    "    }\n"
    "    float mx = max(c.r, max(c.g, c.b));\n"
    "    if (mx > 1.0) c /= mx;\n"
    "    float4 t = g_tex.Sample(g_samp, i.uv);\n"
    "    PSOut o;\n"
    "    o.c0 = saturate(float4(c, i.mat.y) * t);\n"
    "    o.c1 = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    return o;\n"
    "}\n";
using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);

PFN_D3DCompile g_compile = nullptr;

bool CompileShader(PFN_D3DCompile compile, const char* source, size_t sourceLen, const char* entry, const char* target,
                   ID3DBlob** out) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(source, sourceLen, "BladeVR_GpuLevel", nullptr, nullptr, entry, target,
                         (1u << 15) /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, out, &errors);
    if (FAILED(hr) || !*out) {
        std::string msg = "[DIORAMA][GPU] ERROR al compilar el shader ";
        msg += entry;
        if (errors && errors->GetBufferPointer()) {
            msg += ": ";
            msg.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        HookLogger::Instance().Line(msg);
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    return true;
}

bool EnsureShared(ID3D11Device* dev) {
    if (g_sharedTried) return g_sharedOk;
    g_sharedTried = true;
    if (dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: el device no es de nivel 11_0: sin nivel en la GPU.");
        return false;
    }
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_D3DCompile compile = dll ? reinterpret_cast<PFN_D3DCompile>(reinterpret_cast<void*>(GetProcAddress(dll, "D3DCompile")))
                                 : nullptr;
    if (!compile) {
        HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: no se encuentra D3DCompile (d3dcompiler_47.dll).");
        return false;
    }
    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    g_compile = compile;
    bool ok = CompileShader(compile, kShaderSource, sizeof(kShaderSource) - 1, "VSMain", "vs_5_0", &vsb) &&
              CompileShader(compile, kShaderSource, sizeof(kShaderSource) - 1, "PSMain", "ps_5_0", &psb);
    if (ok) {
        ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)) &&
             SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
        if (ok) g_psVtable = *reinterpret_cast<void**>(g_ps);
    }
    if (ok) {
        D3D11_INPUT_ELEMENT_DESC el[5] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ok = SUCCEEDED(dev->CreateInputLayout(el, 5, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout));
    }
    if (vsb) vsb->Release();
    if (psb) psb->Release();
    if (ok) {
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_NONE;
        r.DepthClipEnable = TRUE;
        ok = SUCCEEDED(dev->CreateRasterizerState(&r, &g_rs));
        // Translucidos (rejas, cristales, calcomanias con alfa): casi siempre
        // pegados a otra cara; como las calcomanias del motor en StereoHook.
        r.DepthBias = -16;
        r.SlopeScaledDepthBias = -1.0f;
        r.DepthBiasClamp = 0.0f;
        ok = ok && SUCCEEDED(dev->CreateRasterizerState(&r, &g_rsTranslucent));
    }
    if (ok) {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ok = SUCCEEDED(dev->CreateDepthStencilState(&d, &g_dssOpaque));
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&d, &g_dssTranslucent));
    }
    if (ok) {
        D3D11_BLEND_DESC b{};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = SUCCEEDED(dev->CreateBlendState(&b, &g_blendTranslucent));
    }
    if (ok) {
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_ANISOTROPIC;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        s.MaxAnisotropy = 8;
        s.ComparisonFunc = D3D11_COMPARISON_NEVER;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&s, &g_sampler));
    }
    if (ok) {
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(PerEyeCB);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &g_cb[0])) && SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &g_cb[1]));
    }
    if (ok) {
        uint32_t white = 0xffffffffu;
        D3D11_TEXTURE2D_DESC t{};
        t.Width = t.Height = 1;
        t.MipLevels = t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_IMMUTABLE;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ &white, 4, 4 };
        ok = SUCCEEDED(dev->CreateTexture2D(&t, &init, &g_whiteTex)) &&
             SUCCEEDED(dev->CreateShaderResourceView(g_whiteTex, nullptr, &g_whiteSrv));
        if (ok) {
            g_srvVtable = *reinterpret_cast<void**>(g_whiteSrv);
            g_tex2dVtable = *reinterpret_cast<void**>(g_whiteTex);
        }
    }
    g_sharedOk = ok;
    if (!ok) HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: no se pudieron crear los shaders o estados del nivel.");
    return ok;
}

void ReleaseLevelRes(LevelData* ld) {
    if (!ld) return;
    ID3D11DeviceChild* all[12] = { ld->vb, ld->ib, ld->sectorBuf, ld->refBuf, ld->lightBuf, ld->coneBuf, ld->planeBuf,
                                   ld->sectorSrv, ld->refSrv, ld->lightSrv, ld->coneSrv, ld->planeSrv };
    for (ID3D11DeviceChild* p : all) if (p) p->Release();
    ld->vb = ld->ib = ld->sectorBuf = ld->refBuf = ld->lightBuf = ld->coneBuf = ld->planeBuf = nullptr;
    ld->sectorSrv = ld->refSrv = ld->lightSrv = ld->coneSrv = ld->planeSrv = nullptr;
    ld->resTried = false;
    ld->resOk = false;
}

bool CreateStructured(ID3D11Device* dev, UINT stride, UINT count, ID3D11Buffer** buf, ID3D11ShaderResourceView** srv) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = stride * (count ? count : 1);
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = stride;
    if (FAILED(dev->CreateBuffer(&d, nullptr, buf)) || !*buf) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_UNKNOWN;
    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    v.Buffer.FirstElement = 0;
    v.Buffer.NumElements = count ? count : 1;
    return SUCCEEDED(dev->CreateShaderResourceView(*buf, &v, srv)) && *srv;
}

bool EnsureLevelRes(ID3D11Device* dev, LevelData* ld) {
    if (ld->resTried) return ld->resOk;
    ld->resTried = true;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = static_cast<UINT>(ld->verts.size() * sizeof(GpuVertex));
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{ ld->verts.data(), 0, 0 };
    bool ok = SUCCEEDED(dev->CreateBuffer(&d, &init, &ld->vb));
    if (ok) {
        d.ByteWidth = static_cast<UINT>(ld->indices.size() * sizeof(uint32_t));
        d.BindFlags = D3D11_BIND_INDEX_BUFFER;
        init.pSysMem = ld->indices.data();
        ok = SUCCEEDED(dev->CreateBuffer(&d, &init, &ld->ib));
    }
    ok = ok && CreateStructured(dev, sizeof(SectorGpu), ld->sectorCount, &ld->sectorBuf, &ld->sectorSrv) &&
         CreateStructured(dev, sizeof(LightRefGpu), kMaxLightRefs, &ld->refBuf, &ld->refSrv) &&
         CreateStructured(dev, sizeof(LightGpu), kMaxLights, &ld->lightBuf, &ld->lightSrv) &&
         CreateStructured(dev, sizeof(ConeGpu), kMaxCones, &ld->coneBuf, &ld->coneSrv) &&
         CreateStructured(dev, sizeof(PlaneGpu), kMaxPlanes, &ld->planeBuf, &ld->planeSrv);
    ld->resOk = ok;
    if (!ok) {
        HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: no se pudieron crear los buffers del nivel.");
        ReleaseLevelRes(ld);
        ld->resTried = true;
    }
    return ok;
}

// TextureD3D11 de bgfx para un handle: el recurso (m_ptr) y, si se reconoce
// (misma vtable que un SRV nuestro), su shader resource view.
bool ReadBgfxTextureRaw(uintptr_t base, uint16_t h, void** res, void** srv) {
    __try {
        *res = nullptr;
        *srv = nullptr;
        const char* r = *reinterpret_cast<const char* const*>(base + kRendererD3D11Ptr);
        if (!r || h >= kMaxTextureHandles) return false;
        const char* e = r + kRendererTextures + static_cast<size_t>(h) * kRendererTextureStride;
        void* p = *reinterpret_cast<void* const*>(e);
        if (p && g_tex2dVtable && *reinterpret_cast<void* const*>(p) == g_tex2dVtable) *res = p;
        for (int off : kRendererSrvCandidates) {
            void* c = *reinterpret_cast<void* const*>(e + off);
            if (c && g_srvVtable && *reinterpret_cast<void* const*>(c) == g_srvVtable) {
                *srv = c;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// SRV de una textura de bgfx: el suyo si se reconoce; si no, uno propio del
// recurso; si no hay, la blanca.
ID3D11ShaderResourceView* ResolveTexture(ID3D11Device* dev, uint16_t h) {
    void* res = nullptr;
    void* srv = nullptr;
    if (h == 0xffff || !ReadBgfxTextureRaw(g_base, h, &res, &srv)) return g_whiteSrv;
    if (srv) return static_cast<ID3D11ShaderResourceView*>(srv);
    if (!res) return g_whiteSrv;
    OwnSrv& own = g_ownSrv[h];
    if (own.res != res) {
        if (own.srv) own.srv->Release();
        own.srv = nullptr;
        own.res = res;
        if (FAILED(dev->CreateShaderResourceView(static_cast<ID3D11Resource*>(res), nullptr, &own.srv))) own.srv = nullptr;
    }
    return own.srv ? own.srv : g_whiteSrv;
}

void ComputeViewProj(const double fw[16], const float P[16], float out[16]) {
    double F[16];
    for (int i = 0; i < 16; ++i) F[i] = fw[i];
    F[3] = F[7] = F[11] = 0.0;
    F[15] = 1.0;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += F[i * 4 + k] * static_cast<double>(P[k * 4 + j]);
            out[i * 4 + j] = static_cast<float>(s);
        }
    }
}

template <typename T>
bool UploadDynamic(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, const T* data, size_t count) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return false;
    if (count) std::memcpy(m.pData, data, count * sizeof(T));
    ctx->Unmap(buf, 0);
    return true;
}

struct SavedState {
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    UINT vbStride = 0, vbOffset = 0;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* vsCb = nullptr;
    ID3D11Buffer* psCb = nullptr;
    ID3D11ShaderResourceView* psSrv[6] = {};
    ID3D11SamplerState* psSamp = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0xffffffff;
    ID3D11DepthStencilState* dss = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* rs = nullptr;
    UINT numVp = 0;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
};

void SaveState(ID3D11DeviceContext* ctx, SavedState& s) {
    ctx->IAGetInputLayout(&s.layout);
    ctx->IAGetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOffset);
    ctx->IAGetIndexBuffer(&s.ib, &s.ibFormat, &s.ibOffset);
    ctx->IAGetPrimitiveTopology(&s.topo);
    ctx->VSGetShader(&s.vs, nullptr, nullptr);
    ctx->PSGetShader(&s.ps, nullptr, nullptr);
    ctx->VSGetConstantBuffers(0, 1, &s.vsCb);
    ctx->PSGetConstantBuffers(0, 1, &s.psCb);
    ctx->PSGetShaderResources(0, 6, s.psSrv);
    ctx->PSGetSamplers(0, 1, &s.psSamp);
    ctx->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
    ctx->OMGetDepthStencilState(&s.dss, &s.stencilRef);
    ctx->RSGetState(&s.rs);
    s.numVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&s.numVp, s.vp);
}

void RestoreState(ID3D11DeviceContext* ctx, SavedState& s) {
    ctx->IASetInputLayout(s.layout);
    ctx->IASetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOffset);
    ctx->IASetIndexBuffer(s.ib, s.ibFormat, s.ibOffset);
    ctx->IASetPrimitiveTopology(s.topo);
    ctx->VSSetShader(s.vs, nullptr, 0);
    ctx->PSSetShader(s.ps, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &s.vsCb);
    ctx->PSSetConstantBuffers(0, 1, &s.psCb);
    ctx->PSSetShaderResources(0, 6, s.psSrv);
    ctx->PSSetSamplers(0, 1, &s.psSamp);
    ctx->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
    ctx->OMSetDepthStencilState(s.dss, s.stencilRef);
    ctx->RSSetState(s.rs);
    if (s.numVp) ctx->RSSetViewports(s.numVp, s.vp);
    IUnknown* refs[] = { s.layout, s.vb, s.ib, s.vs, s.ps, s.vsCb, s.psCb, s.psSrv[0], s.psSrv[1], s.psSrv[2], s.psSrv[3],
                         s.psSrv[4], s.psSrv[5], s.psSamp, s.blend, s.dss, s.rs };
    for (IUnknown* p : refs) if (p) p->Release();
}

// --- lotes recortados por pixel (agua, objetos, caras del motor) ------------------
// Mismo calculo que mesh_fog.fsbin / mesh.fsbin del juego (color * textura,
// niebla) y la misma firma de entrada (la salida de mesh.vsbin), mas el
// descarte de lo que quitan el cono y los cortes: la posicion en camara sale
// de la profundidad y del pixel con la proyeccion del ojo (vector fila,
// w = z).
const char kWaterShaderSource[] =
    "Texture2D s_texColorTexture : register(t0);\n"
    "SamplerState s_texColorSampler : register(s0);\n"
    "cbuffer FogCB : register(b0) { float4 fog; };\n"
    "cbuffer WaterClip : register(b3) { float4 w_vp; float4 w_proj; float4 w_projZ; float4 w_cone[8]; float4 w_flags; float4 w_vcut; float4 w_refl; float4 w_hcut; };\n"
    "struct WIn { float4 pos : SV_POSITION; float4 color : COLOR0; float depth : COLOR1; float4 tc : TEXCOORD0; };\n"
    "float WConeMax(float3 v)\n"
    "{\n"
    "    if (dot(w_cone[4].xyz, w_cone[4].xyz) < 0.25) {\n"
    "        float t = dot(w_cone[1].xyz, v) + w_cone[1].w;\n"
    "        float3 r = (v - w_cone[2].xyz) - t * w_cone[1].xyz;\n"
    "        float side = length(r) * w_cone[3].x - t * w_cone[3].y - w_cone[3].z;\n"
    "        return max(max(dot(w_cone[0].xyz, v) + w_cone[0].w, side), dot(w_cone[7].xyz, v) + w_cone[7].w);\n"
    "    }\n"
    "    float m = -1e30;\n"
    "    [unroll] for (int k = 0; k < 8; ++k) m = max(m, dot(w_cone[k].xyz, v) + w_cone[k].w);\n"
    "    return m;\n"
    "}\n"
    "void ConeClip(float4 pos)\n"
    "{\n"
    "    float vz = w_projZ.y / (pos.z - w_projZ.x);\n"
    "    float xn = (pos.x - w_vp.x) / w_vp.z * 2.0 - 1.0;\n"
    "    float yn = 1.0 - (pos.y - w_vp.y) / w_vp.w * 2.0;\n"
    "    float3 v = float3((xn * vz - vz * w_proj.z - w_projZ.z) / w_proj.x, (yn * vz - vz * w_proj.w) / w_proj.y, vz);\n"
    // Reflejo: se prueba el punto donde el rayo de vista del ojo (en
    // x = -P30/P00 de la camara) cruza el agua (lo que esta mas alla del
    // plano del agua es el recinto reflejado).
    "    if (w_flags.z > 0.5) {\n"
    "        float3 e = float3(-w_projZ.z / w_proj.x, 0.0, 0.0);\n"
    "        float nv = dot(w_refl.xyz, v - e);\n"
    "        if (abs(nv) > 1e-6) {\n"
    "            float t = -(dot(w_refl.xyz, e) + w_refl.w) / nv;\n"
    "            if (t > 0.0 && t < 0.999) v = e + (v - e) * t;\n"
    "        }\n"
    "    }\n"
    "    if (w_flags.x > 0.5 && WConeMax(v) <= 0.5) discard;\n"
    "    if (w_flags.y > 0.5 && dot(w_vcut.xyz, v) + w_vcut.w <= 0.0) discard;\n"
    "    if (w_flags.w > 0.5 && dot(w_hcut.xyz, v) + w_hcut.w < 0.0) discard;\n"
    "}\n"
    "float FogAmount(WIn i) { return 1.0 - saturate(exp(-(i.depth / i.tc.w) * fog.w)); }\n"
    "float4 PSWaterFog(WIn i) : SV_Target0\n"
    "{\n"
    "    ConeClip(i.pos);\n"
    "    float4 c = saturate(i.color * s_texColorTexture.Sample(s_texColorSampler, i.tc.xy));\n"
    "    return lerp(c, float4(fog.xyz, 1.0), FogAmount(i));\n"
    "}\n"
    "struct WOut2 { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };\n"
    "WOut2 PSWaterMrt(WIn i)\n"
    "{\n"
    "    ConeClip(i.pos);\n"
    "    WOut2 o;\n"
    "    o.c0 = saturate(i.color * s_texColorTexture.Sample(s_texColorSampler, i.tc.xy));\n"
    "    o.c1 = float4(fog.xyz, FogAmount(i));\n"
    "    return o;\n"
    "}\n";

struct WaterCB {
    float vp[4];        // x, y, ancho, alto del viewport del ojo
    float proj[4];      // P00, P11, P20, P21
    float projZ[4];     // P22, P32, P30, -
    float cone[kGpuConePlaneCount][4];
    float flags[4];     // x cono si/no, y corte vertical si/no, z reflejo (plano del agua) si/no, w corte de altura si/no
    float vcut[4];      // corte vertical en camara
    float refl[4];      // plano del agua reflejada en camara
    float hcut[4];      // corte de altura en camara
};
static_assert(sizeof(WaterCB) % 16 == 0, "WaterCB");

bool EnsureWater(ID3D11Device* dev) {
    if (g_waterTried) return g_waterOk;
    g_waterTried = true;
    if (!g_compile || !g_psVtable) return false;
    ID3DBlob* a = nullptr;
    ID3DBlob* b = nullptr;
    bool ok = CompileShader(g_compile, kWaterShaderSource, sizeof(kWaterShaderSource) - 1, "PSWaterFog", "ps_5_0", &a) &&
              CompileShader(g_compile, kWaterShaderSource, sizeof(kWaterShaderSource) - 1, "PSWaterMrt", "ps_5_0", &b);
    if (ok) {
        ok = SUCCEEDED(dev->CreatePixelShader(a->GetBufferPointer(), a->GetBufferSize(), nullptr, &g_waterPsFog)) &&
             SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_waterPsMrt));
    }
    if (a) a->Release();
    if (b) b->Release();
    if (ok) {
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(WaterCB);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &g_waterCb[0])) && SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &g_waterCb[1]));
    }
    g_waterOk = ok;
    if (!ok) HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: no se pudieron crear los shaders del agua.");
    return ok;
}

// Pixel shader de un programa de bgfx del raster (mesh / mesh_fog), si se
// reconoce (misma vtable que un pixel shader nuestro).
ID3D11PixelShader* EnginePixelShaderRaw(uintptr_t progOffset) {
    __try {
        uint16_t h = *reinterpret_cast<const uint16_t*>(g_base + progOffset);
        if (h == 0xffff || h >= 4096) return nullptr;
        const char* r = *reinterpret_cast<const char* const*>(g_base + kRendererD3D11Ptr);
        if (!r) return nullptr;
        const char* prog = r + kRendererPrograms + static_cast<size_t>(h) * kRendererProgramStride;
        const char* fsh = *reinterpret_cast<const char* const*>(prog + kProgramFsh);
        if (!fsh) return nullptr;
        void* ps = *reinterpret_cast<void* const*>(fsh);
        if (ps && g_psVtable && *reinterpret_cast<void* const*>(ps) == g_psVtable) return static_cast<ID3D11PixelShader*>(ps);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// --- cielo ---------------------------------------------------------------------
// Fondo con el cielo del mapa, como lo pinta el juego (+0x744650): un cubo
// alineado con el mundo, cada cara a 1000 unidades en la direccion -n (Y del
// juego hacia abajo), con p = d * (-1000 / d.n) y
// u = (p.a * 255/2000 - 128) / 256, v = (p.b * 255/2000 - 128) / 256
// (+0x750d30; +0x751480 le pasa los ejes del mundo en camara):
//   Up    n = Y,  a = X,  b = Z      Down  n = -Y, a = X,  b = -Z
//   Front n = -Z, a = X,  b = Y      Back  n = Z,  a = -X, b = Y
//   Left  n = X,  a = Z,  b = Y      Right n = -X, a = -Z, b = Y
// Color = raster+0xd28 * 220 (0..255). Un triangulo a pantalla completa en
// cada ojo, al fondo (z = 1, sin escribir profundidad): lo que se dibuje
// despues lo tapa. La direccion del pixel sale de la proyeccion del ojo
// (sin su desplazamiento: al infinito) y de la inversa de la fromWorld.
const char kSkyShaderSource[] =
    "cbuffer Sky : register(b0) { float4 s_rot0; float4 s_rot1; float4 s_rot2; float4 s_proj; float4 s_vp; float4 s_color; };\n"
    "Texture2D s_up : register(t0);\n"
    "Texture2D s_down : register(t1);\n"
    "Texture2D s_front : register(t2);\n"
    "Texture2D s_back : register(t3);\n"
    "Texture2D s_left : register(t4);\n"
    "Texture2D s_right : register(t5);\n"
    "SamplerState s_samp : register(s0);\n"
    "float4 VSSky(uint id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    float2 t = float2((id << 1) & 2, id & 2);\n"
    "    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 1.0, 1.0);\n"
    "}\n"
    "float4 PSSky(float4 pos : SV_Position) : SV_Target0\n"
    "{\n"
    "    float xn = (pos.x - s_vp.x) / s_vp.z * 2.0 - 1.0;\n"
    "    float yn = 1.0 - (pos.y - s_vp.y) / s_vp.w * 2.0;\n"
    "    float3 c = float3((xn - s_proj.z) / s_proj.x, (yn - s_proj.w) / s_proj.y, 1.0);\n"
    "    float3 d = c.x * s_rot0.xyz + c.y * s_rot1.xyz + c.z * s_rot2.xyz;\n"
    "    float3 a = abs(d);\n"
    "    float2 uv;\n"
    "    uint f;\n"
    "    if (a.y >= a.x && a.y >= a.z) {\n"
    "        if (d.y < 0.0) { float k = -1000.0 / d.y; uv = float2(d.x * k, d.z * k); f = 0; }\n"
    "        else { float k = 1000.0 / d.y; uv = float2(d.x * k, -d.z * k); f = 1; }\n"
    "    } else if (a.z >= a.x) {\n"
    "        if (d.z > 0.0) { float k = 1000.0 / d.z; uv = float2(d.x * k, d.y * k); f = 2; }\n"
    "        else { float k = -1000.0 / d.z; uv = float2(-d.x * k, d.y * k); f = 3; }\n"
    "    } else {\n"
    "        if (d.x < 0.0) { float k = -1000.0 / d.x; uv = float2(d.z * k, d.y * k); f = 4; }\n"
    "        else { float k = 1000.0 / d.x; uv = float2(-d.z * k, d.y * k); f = 5; }\n"
    "    }\n"
    "    uv = frac((uv * (255.0 / 2000.0) - 128.0) / 256.0);\n"
    "    float4 t;\n"
    "    [branch] switch (f) {\n"
    "        case 0: t = s_up.SampleLevel(s_samp, uv, 0.0); break;\n"
    "        case 1: t = s_down.SampleLevel(s_samp, uv, 0.0); break;\n"
    "        case 2: t = s_front.SampleLevel(s_samp, uv, 0.0); break;\n"
    "        case 3: t = s_back.SampleLevel(s_samp, uv, 0.0); break;\n"
    "        case 4: t = s_left.SampleLevel(s_samp, uv, 0.0); break;\n"
    "        default: t = s_right.SampleLevel(s_samp, uv, 0.0); break;\n"
    "    }\n"
    "    return float4(saturate(t.rgb * s_color.rgb), 1.0);\n"
    "}\n";

struct SkyCB {
    float rot[3][4];    // filas de la inversa de la 3x3 de la fromWorld (camara -> mundo)
    float proj[4];      // P00, P11, P20, P21 del ojo
    float vp[4];        // x, y, ancho, alto del viewport del ojo
    float color[4];
};
static_assert(sizeof(SkyCB) % 16 == 0, "SkyCB");

bool EnsureSky(ID3D11Device* dev) {
    if (g_skyTried) return g_skyOk;
    g_skyTried = true;
    if (!g_compile) return false;
    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    bool ok = CompileShader(g_compile, kSkyShaderSource, sizeof(kSkyShaderSource) - 1, "VSSky", "vs_5_0", &vsb) &&
              CompileShader(g_compile, kSkyShaderSource, sizeof(kSkyShaderSource) - 1, "PSSky", "ps_5_0", &psb);
    if (ok) {
        ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_skyVs)) &&
             SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_skyPs));
    }
    if (vsb) vsb->Release();
    if (psb) psb->Release();
    if (ok) {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ok = SUCCEEDED(dev->CreateDepthStencilState(&d, &g_skyDss));
    }
    if (ok) {
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.ComparisonFunc = D3D11_COMPARISON_NEVER;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&s, &g_skySampler));
    }
    if (ok) {
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(SkyCB);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &g_skyCb[0])) && SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &g_skyCb[1]));
    }
    g_skyOk = ok;
    if (!ok) HookLogger::Instance().Line("[DIORAMA][GPU] ERROR: no se pudieron crear los shaders del cielo.");
    return ok;
}

// Dibuja el cielo en los dos ojos (estado ya guardado por el llamador).
bool DrawSky(ID3D11DeviceContext* ctx, ID3D11Device* dev, const FrameSlot& sl, const D3D11_VIEWPORT& fullVp,
             const float eyeProj[2][16]) {
    if (!sl.skyOn || !EnsureSky(dev)) return false;
    double inv[9];
    if (!Invert3(sl.fw, inv)) return false;
    ID3D11ShaderResourceView* faces[kSkyFaces];
    uint32_t okFaces = 0;
    for (int f = 0; f < kSkyFaces; ++f) {
        faces[f] = ResolveTexture(dev, sl.skyHandles[f]);
        if (faces[f] != g_whiteSrv) ++okFaces;
    }
    if (!okFaces) return false;
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_skyVs, nullptr, 0);
    ctx->PSSetShader(g_skyPs, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &g_skySampler);
    ctx->PSSetShaderResources(0, kSkyFaces, faces);
    ctx->RSSetState(g_rs);
    const FLOAT noFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ctx->OMSetBlendState(nullptr, noFactor, 0xffffffff);
    ctx->OMSetDepthStencilState(g_skyDss, 0);
    for (int eye = 0; eye < 2; ++eye) {
        D3D11_VIEWPORT vp = fullVp;
        vp.Width = fullVp.Width * 0.5f;
        vp.TopLeftX = fullVp.TopLeftX + (eye == 1 ? vp.Width : 0.0f);
        SkyCB cb{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) cb.rot[r][c] = static_cast<float>(inv[r * 3 + c]);
            cb.rot[r][3] = 0.0f;
        }
        cb.proj[0] = eyeProj[eye][0];
        cb.proj[1] = eyeProj[eye][5];
        cb.proj[2] = eyeProj[eye][8];
        cb.proj[3] = eyeProj[eye][9];
        cb.vp[0] = vp.TopLeftX;
        cb.vp[1] = vp.TopLeftY;
        cb.vp[2] = vp.Width;
        cb.vp[3] = vp.Height;
        cb.color[0] = cb.color[1] = cb.color[2] = sl.skyColor;
        cb.color[3] = 1.0f;
        if (!UploadDynamic(ctx, g_skyCb[eye], &cb, 1)) return false;
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetConstantBuffers(0, 1, &g_skyCb[eye]);
        ctx->PSSetConstantBuffers(0, 1, &g_skyCb[eye]);
        ctx->Draw(3, 0);
    }
    return true;
}

// Decide con los votos de las primeras pasadas si los planos del mundo de
// los conos van como su copia en camara (+1), al reves (-1) o si no se puede
// saber (0: sin recorte por conos, cada luz en todo su sector).
void DecideConeSign(LevelData* ld, const ConeSignVotes& v) {
    ld->coneVoteSame += v.same;
    ld->coneVoteOpposite += v.opposite;
    ++ld->coneVotePasses;
    unsigned total = ld->coneVoteSame + ld->coneVoteOpposite;
    if (total < 64 && ld->coneVotePasses < 600) return;
    ld->coneSignChecked = true;
    if (total < 8) ld->coneSign = 0.0f;                                         // sin datos
    else if (ld->coneVoteSame >= 4 * ld->coneVoteOpposite) ld->coneSign = 1.0f;  // misma orientacion
    else if (ld->coneVoteOpposite >= 4 * ld->coneVoteSame) ld->coneSign = -1.0f; // al reves
    else ld->coneSign = 0.0f;                                                   // no concuerdan
}

} // namespace

// =====================================================================
// API
// =====================================================================
int GpuLevelBeginPass(uintptr_t base, const GpuLevelPass& pass) {
    g_skipPass = false;
    g_passActive = false;
    g_passLevel = nullptr;
    if (!base || !pass.level || !pass.fromWorld) return kGpuPassEngine;
    if (g_retryCapture.exchange(false, std::memory_order_relaxed)) g_failedLevel = 0;
    g_base = base;
    char** sectors = nullptr;
    unsigned count = 0, nverts = 0;
    const char* verts = nullptr;
    if (!ReadLevelIdentityRaw(base, pass.level, &sectors, &count, &verts, &nverts)) return kGpuPassEngine;
    LevelData* cur = g_current.load(std::memory_order_relaxed);
    if (!SameLevel(cur, pass.level, sectors, count, verts, nverts)) {
        bool failedBefore = g_failedLevel == pass.level && g_failedCount == count;
        if (!failedBefore) {
            if (StartCapture(base, pass.level, sectors, count, verts, nverts, pass.fromWorld)) {
                // En esta pasada el nivel lo dibuja el motor entero; la GPU no.
                WriteMarkerRaw(base, 0, false);
                return kGpuPassCapture;
            }
            g_failedLevel = pass.level;
            g_failedCount = count;
        }
        return kGpuPassEngine;
    }
    if (!cur) return kGpuPassEngine;
    EnsureSubmitHook(base);
    if (!g_submitHooked) return kGpuPassEngine;
    LevelData* ld = cur;
    DeleteRetired();
    g_passActive = true;
    uint32_t seq = ++g_seq;
    if (seq == 0) seq = ++g_seq;
    FrameSlot& sl = ld->slots[seq % kSlots];
    sl.seq = 0;
    FillSlotView(&sl, pass);
    ConeSignVotes votes = {};
    bool wantVotes = !ld->coneSignChecked;
    // Luces que se mueven: sus conos, antes de leerlos (ver kFlushMovedLights).
    bool flushFailedBefore = g_flushMovedLightsState < 0;
    FlushMovedLightsRaw(base);
    if (!flushFailedBefore && g_flushMovedLightsState < 0) {
        HookLogger::Instance().Line("[DIORAMA][GPU] AVISO: +0x8af10 no es la esperada o ha fallado; las antorchas que se mueven no iluminan la maqueta.");
    }
    bool filled = FillSlotRaw(base, ld, &sl, pass, wantVotes ? &votes : nullptr);
    if (wantVotes) DecideConeSign(ld, votes);
    sl.drawGpu = filled && g_renderReadyGen.load(std::memory_order_acquire) == ld->gen;
    sl.seq = seq;
    g_pendingSeq = seq;
    g_skipPass = sl.drawGpu;
    g_passLevel = ld;
    return g_skipPass ? kGpuPassGpu : kGpuPassEngine;
}

namespace {
// Superficie que no se captura porque lo que dibuja el motor de ella cambia
// durante la partida: las del sector de una puerta, y los portales (de las
// caras con portal, de los portales y de los huecos de las caras complejas)
// hacia un sector cerrado, una puerta o un liquido que corre. Kind: 0
// compleja, 1 poligono, 2 poligono con portal, 3 portal.
bool SurfaceIsDynamicRaw(const LevelData* ld, const char* surface, int kind) {
    __try {
        const char* cur = *reinterpret_cast<const char* const*>(g_base + kCurrentSectorPtr);
        if (SectorSliding(cur)) return true;
        if (kind == 2 || kind == 3) {
            return NeighborDynamic(Neighbor(ld, surface, kind == 2 ? kPolyPortalNeighbor : kPortalNeighbor));
        }
        if (kind == 0) {
            unsigned n = Rd<unsigned>(surface, kComplexPortalCount);
            const char* portals = Rd<const char*>(surface, kComplexPortals);
            if (n == 0 || !portals) return false;
            if (n > kMaxComplexPortals) return true;
            for (unsigned i = 0; i < n; ++i) {
                unsigned idx = Rd<unsigned>(portals + static_cast<size_t>(i) * kComplexPortalStride, kComplexPortalSector);
                if (idx < ld->sectorCount && NeighborDynamic(ld->sectorArray[idx])) return true;
            }
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
} // namespace

void GpuLevelCaptureSurface(const char* surface, int kind) {
    g_captureSurface = nullptr;
    g_captureMat = nullptr;
    LevelData* ld = g_captureLd;
    if (!g_capturing || !ld || !surface) return;
    if (SurfaceIsDynamicRaw(ld, surface, kind)) return;
    SurfaceSetInsert(ld, surface);
    g_captureSurface = surface;
}

void GpuLevelCaptureSurfaceDone() {
    g_captureSurface = nullptr;
}

bool GpuLevelSkipsSurface(const char* surface) {
    if (!g_skipPass || !g_passLevel) return false;
    return SurfaceSetContains(g_passLevel, surface);
}

bool GpuLevelEndPass() {
    bool captured = false;
    if (g_capturing) {
        long long level = g_captureLd ? g_captureLd->level : 0;
        unsigned count = g_captureLd ? g_captureLd->sectorCount : 0;
        captured = FinishCapture();
        if (!captured) {
            g_failedLevel = level;
            g_failedCount = count;
        } else {
            g_failedLevel = 0;
        }
        g_markerWritten = false;
    } else if (g_passActive) {
        WriteMarkerRaw(g_base, g_pendingSeq, true);
        g_markerWritten = true;
    } else if (g_markerWritten) {
        // Pasada sin el nivel en la GPU: el hilo de render no dibuja una
        // ranura vieja.
        WriteMarkerRaw(g_base, 0, false);
        g_markerWritten = false;
    }
    g_skipPass = false;
    g_passActive = false;
    g_passLevel = nullptr;
    return captured;
}

bool GpuLevelWaterClipBegin(ID3D11DeviceContext* ctx, bool reflection) {
    if (!ctx) return false;
    LevelData* lvl = g_renderLevel;
    if (!lvl || !g_frameSeqValid) return false;
    FrameSlot& sl = lvl->slots[g_frameSeq % kSlots];
    if (sl.seq != g_frameSeq || !sl.drawGpu || (!sl.coneOn && !sl.vcutOn && !sl.cutOn)) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    bool ready = EnsureShared(dev) && EnsureWater(dev);
    dev->Release();
    if (!ready) return false;
    ID3D11PixelShader* bound = nullptr;
    ctx->PSGetShader(&bound, nullptr, nullptr);
    ID3D11PixelShader* use = nullptr;
    if (bound && bound == EnginePixelShaderRaw(kProgMeshFog)) use = g_waterPsFog;
    else if (bound && bound == EnginePixelShaderRaw(kProgMesh)) use = g_waterPsMrt;
    if (!use) {
        if (bound) bound->Release();
        return false;
    }
    g_waterSavedPs = bound;
    g_waterSavedCb = nullptr;
    ctx->PSGetConstantBuffers(3, 1, &g_waterSavedCb);
    g_waterCone = sl.coneCam;
    g_waterConeOn = sl.coneOn;
    g_waterVcutOn = sl.vcutOn;
    std::memcpy(g_waterVcut, sl.vcutCam, sizeof(g_waterVcut));
    g_waterReflOn = reflection && sl.reflOn;
    std::memcpy(g_waterRefl, sl.reflCam, sizeof(g_waterRefl));
    g_waterHcutOn = sl.cutOn;
    std::memcpy(g_waterHcut, sl.cutCam, sizeof(g_waterHcut));
    ctx->PSSetShader(use, nullptr, 0);
    return true;
}

void GpuLevelWaterClipEye(ID3D11DeviceContext* ctx, int eye, const D3D11_VIEWPORT& eyeVp, const float proj[16]) {
    if (!ctx || !g_waterCone || eye < 0 || eye > 1) return;
    WaterCB cb{};
    cb.vp[0] = eyeVp.TopLeftX;
    cb.vp[1] = eyeVp.TopLeftY;
    cb.vp[2] = eyeVp.Width;
    cb.vp[3] = eyeVp.Height;
    cb.proj[0] = proj[0];
    cb.proj[1] = proj[5];
    cb.proj[2] = proj[8];
    cb.proj[3] = proj[9];
    cb.projZ[0] = proj[10];
    cb.projZ[1] = proj[14];
    cb.projZ[2] = proj[12];
    std::memcpy(cb.cone, g_waterCone, sizeof(cb.cone));
    cb.flags[0] = g_waterConeOn ? 1.0f : 0.0f;
    cb.flags[1] = g_waterVcutOn ? 1.0f : 0.0f;
    cb.flags[2] = g_waterReflOn ? 1.0f : 0.0f;
    cb.flags[3] = g_waterHcutOn ? 1.0f : 0.0f;
    std::memcpy(cb.vcut, g_waterVcut, sizeof(cb.vcut));
    std::memcpy(cb.refl, g_waterRefl, sizeof(cb.refl));
    std::memcpy(cb.hcut, g_waterHcut, sizeof(cb.hcut));
    // Muchos lotes marcados por fotograma (objetos, sombras): solo se sube si
    // cambia (el contenido es el mismo para todos los de un ojo y fotograma).
    static WaterCB s_last[2];
    static bool s_lastValid[2] = { false, false };
    if (s_lastValid[eye] && std::memcmp(&s_last[eye], &cb, sizeof(cb)) == 0) {
        ctx->PSSetConstantBuffers(3, 1, &g_waterCb[eye]);
        return;
    }
    if (UploadDynamic(ctx, g_waterCb[eye], &cb, 1)) {
        ctx->PSSetConstantBuffers(3, 1, &g_waterCb[eye]);
        s_last[eye] = cb;
        s_lastValid[eye] = true;
    } else {
        s_lastValid[eye] = false;
    }
}

void GpuLevelRetryCapture() {
    g_retryCapture.store(true, std::memory_order_relaxed);
}

bool GpuLevelBounds(long long level, double mn[3], double mx[3]) {
    LevelData* ld = g_current.load(std::memory_order_acquire);
    if (!ld || !level || ld->level != level || !ld->geoValid) return false;
    for (int k = 0; k < 3; ++k) {
        mn[k] = ld->geoMn[k];
        mx[k] = ld->geoMx[k];
    }
    return true;
}

bool GpuLevelFramePose(float out12[12]) {
    if (!out12) return false;
    LevelData* lvl = g_current.load(std::memory_order_acquire);
    if (!lvl || !g_frameSeqValid) return false;
    const FrameSlot& sl = lvl->slots[g_frameSeq % kSlots];
    if (sl.seq != g_frameSeq || !sl.poseOn) return false;
    for (int k = 0; k < 12; ++k) out12[k] = sl.pose[k];
    return true;
}

void GpuLevelSetReflectionPlane(double waterY) {
    if (!g_passActive || !g_passLevel) return;
    FrameSlot& sl = g_passLevel->slots[g_pendingSeq % kSlots];
    if (sl.seq != g_pendingSeq) return;
    // Plano y = waterY llevado a camara: n = fila 1 de la 3x3, d' = -y - n.t.
    const double* fw = sl.fw;
    double n[3] = { fw[4], fw[5], fw[6] };
    sl.reflCam[0] = static_cast<float>(n[0]);
    sl.reflCam[1] = static_cast<float>(n[1]);
    sl.reflCam[2] = static_cast<float>(n[2]);
    sl.reflCam[3] = static_cast<float>(-waterY - (n[0] * fw[12] + n[1] * fw[13] + n[2] * fw[14]));
    sl.reflOn = true;
}

void GpuLevelWaterClipEnd(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    ctx->PSSetShader(g_waterSavedPs, nullptr, 0);
    ctx->PSSetConstantBuffers(3, 1, &g_waterSavedCb);
    if (g_waterSavedPs) g_waterSavedPs->Release();
    if (g_waterSavedCb) g_waterSavedCb->Release();
    g_waterSavedPs = nullptr;
    g_waterSavedCb = nullptr;
    g_waterCone = nullptr;
}

bool GpuLevelDraw(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& fullVp, const float eyeProj[2][16]) {
    if (!ctx) return true;
    LevelData* lvl = g_current.load(std::memory_order_acquire);
    if (!lvl) return true;
    {
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        bool haveDsv = dsv != nullptr;
        if (rtv) rtv->Release();
        if (dsv) dsv->Release();
        if (!haveDsv) return false;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return true;
    if (lvl != g_renderLevel) {
        ReleaseLevelRes(g_renderLevel);
        g_renderLevel = lvl;
        g_renderLevelGen.store(lvl->gen, std::memory_order_release);
    }
    bool ok = EnsureShared(dev) && EnsureLevelRes(dev, lvl);
    if (!ok) { dev->Release(); return true; }
    g_renderReadyGen.store(lvl->gen, std::memory_order_release);
    if (!g_frameSeqValid) { dev->Release(); return true; }
    FrameSlot& sl = lvl->slots[g_frameSeq % kSlots];
    if (sl.seq != g_frameSeq || !sl.drawGpu) { dev->Release(); return true; }

    // Datos de este fotograma.
    bool up = UploadDynamic(ctx, lvl->sectorBuf, sl.sectors.data(), sl.sectors.size()) &&
              UploadDynamic(ctx, lvl->refBuf, sl.lightRefs.data(), sl.lightRefCount) &&
              UploadDynamic(ctx, lvl->lightBuf, sl.lights.data(), sl.lightCount) &&
              UploadDynamic(ctx, lvl->coneBuf, sl.cones.data(), sl.coneCount) &&
              UploadDynamic(ctx, lvl->planeBuf, sl.planes.data(), sl.planeCount);
    for (int eye = 0; eye < 2 && up; ++eye) {
        PerEyeCB cb{};
        ComputeViewProj(sl.fw, eyeProj[eye], cb.viewProj);
        cb.cam[0] = sl.cam[0];
        cb.cam[1] = sl.cam[1];
        cb.cam[2] = sl.cam[2];
        cb.cut[0] = sl.cutOn ? 1.0f : 0.0f;
        cb.cut[1] = sl.cutY;
        cb.cut[3] = sl.coneOn ? 1.0f : 0.0f;
        cb.misc[0] = sl.fixedLight;
        cb.misc[1] = sl.vcutOn ? 1.0f : 0.0f;
        std::memcpy(cb.cone, sl.cone, sizeof(cb.cone));
        std::memcpy(cb.vcut, sl.vcut, sizeof(cb.vcut));
        up = UploadDynamic(ctx, g_cb[eye], &cb, 1);
    }
    if (!up) { dev->Release(); return true; }

    // Texturas de los grupos.
    std::vector<ID3D11ShaderResourceView*> srvs(lvl->groups.size(), g_whiteSrv);
    for (size_t g = 0; g < lvl->groups.size(); ++g) srvs[g] = ResolveTexture(dev, sl.handles[g]);

    SavedState saved;
    SaveState(ctx, saved);
    // Cielo de fondo (fondo del juego, sin passthrough), antes que el nivel.
    DrawSky(ctx, dev, sl, fullVp, eyeProj);
    UINT stride = sizeof(GpuVertex), offset = 0;
    ctx->IASetInputLayout(g_layout);
    ctx->IASetVertexBuffers(0, 1, &lvl->vb, &stride, &offset);
    ctx->IASetIndexBuffer(lvl->ib, DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &g_sampler);
    ctx->RSSetState(g_rs);
    ID3D11ShaderResourceView* frameSrvs[5] = { lvl->sectorSrv, lvl->refSrv, lvl->lightSrv, lvl->coneSrv, lvl->planeSrv };
    ctx->PSSetShaderResources(1, 5, frameSrvs);
    const FLOAT noFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int eye = 0; eye < 2; ++eye) {
        D3D11_VIEWPORT vp = fullVp;
        vp.Width = fullVp.Width * 0.5f;
        vp.TopLeftX = fullVp.TopLeftX + (eye == 1 ? vp.Width : 0.0f);
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetConstantBuffers(0, 1, &g_cb[eye]);
        ctx->PSSetConstantBuffers(0, 1, &g_cb[eye]);
        for (int pass = 0; pass < 2; ++pass) {
            ctx->OMSetBlendState(pass == 0 ? nullptr : g_blendTranslucent, noFactor, 0xffffffff);
            ctx->OMSetDepthStencilState(pass == 0 ? g_dssOpaque : g_dssTranslucent, 0);
            ctx->RSSetState(pass == 0 ? g_rs : g_rsTranslucent);
            for (size_t g = 0; g < lvl->groups.size(); ++g) {
                const GpuGroup& gr = lvl->groups[g];
                if (gr.translucent != (pass == 1) || gr.count == 0) continue;
                ctx->PSSetShaderResources(0, 1, &srvs[g]);
                ctx->DrawIndexed(gr.count, gr.first, 0);
            }
        }
    }
    RestoreState(ctx, saved);
    dev->Release();
    return true;
}

} // namespace BladeVR
