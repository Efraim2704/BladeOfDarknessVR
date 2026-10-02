#pragma once

#include <cstdint>
#include <cstddef>

struct ID3D11DeviceContext;
struct D3D11_VIEWPORT;

namespace BladeVR {

// ---------------------------------------------------------------------
// Nivel del diorama dibujado en la GPU (ver DioramaGpuLevel.cpp). Solo
// actua con el diorama puesto: fuera de el no se llama nada de esto y el
// modo VR principal no cambia. Si el nivel no se puede capturar, lo dibuja
// el motor como siempre.
// ---------------------------------------------------------------------

// Datos de la pasada del mundo del diorama (hilo del juego).
constexpr int kGpuConePlaneCount = 8;   // 7 del cono + el del suelo, en camara
struct GpuLevelPass {
    long long level;                  // singleton del nivel (+0x7ceb0)
    const double* fromWorld;          // 16 doubles de la pasada (vectores fila)
    // Area visible: nullptr = todos los sectores. Si no, el sector se dibuja
    // cuando su caja (min, max; doubles del juego) la toca (mismo criterio
    // que la lista de sectores del motor).
    bool (*sectorInArea)(const double* boxMin, const double* boxMax);
    bool cutOn;
    double cutY;                      // se conserva Y > cutY (Y del juego hacia abajo)
    bool coneOn;
    const double (*conePlanes)[4];    // kGpuConePlaneCount planos en camara; se quita lo que
                                      // queda dentro de todos (n.v + d <= 0,5)
    // Cono redondo: conePlanes lleva entonces [0] el plano trasero, [1] el
    // eje como plano por el vertice (t = n.p + d), [2] el vertice (punto; en
    // camara el origen), [3] (cos a, sin a, r_ojo cos a, 1), [4..6] a cero y
    // [7] el suelo. Se quita lo que tiene
    // max(trasero, |p - v - t n| cos a - t sin a - r_ojo cos a, suelo) <= 0,5.
    // Los shaders lo reconocen por la normal nula de [4].
    bool coneRound;
    bool skyOn;                       // cielo del mapa de fondo (fondo del juego, sin passthrough)
    // Corte vertical: se conserva n.p + d > 0.
    bool vcutOn;
    const double* vcutPlane;          // mundo (nx, 0, nz, d)
    const double* vcutPlaneCam;       // el mismo en camara
    // Pose del visor (3x4 de OpenVR) con la que se calculo la camara de esta
    // pasada, o nullptr: se envia con la imagen de este fotograma.
    const float* hmdPose;
};

// Hilo del juego, en cada pasada del diorama. Devuelve que hacer con las
// caras del nivel en esta pasada:
constexpr int kGpuPassEngine = 0;    // las dibuja el motor, como siempre
constexpr int kGpuPassGpu = 1;       // las dibuja la GPU: el motor se salta las capturadas
constexpr int kGpuPassCapture = 2;   // pasada de captura: sin frustum, area, corte ni cono,
                                     // todas las caras por los dos lados; el motor las dibuja
                                     // y se guardan (GpuLevelCaptureSurface alrededor de cada una)
int GpuLevelBeginPass(uintptr_t base, const GpuLevelPass& pass);
// Pasada de captura: alrededor del dibujo de cada superficie del nivel
// (kind 0 compleja, 1 poligono, 2 poligono con portal, 3 portal).
void GpuLevelCaptureSurface(const char* surface, int kind);
void GpuLevelCaptureSurfaceDone();
// Pasada en la GPU: esta superficie la dibuja la GPU y el motor no.
bool GpuLevelSkipsSurface(const char* surface);
// Fin de la pasada. true: se acaba de capturar el nivel (con exito).
bool GpuLevelEndPass();
// Cualquier hilo (al activar el diorama): si la captura de este nivel fallo,
// se vuelve a intentar en la siguiente pasada.
void GpuLevelRetryCapture();

// Hilo de render (StereoHook), en el primer draw 3D con profundidad de la
// pasada del diorama de cada fotograma: dibuja el cielo (si toca) y el
// nivel en las dos mitades.
// eyeProj: matrices de proyeccion de cada ojo (las de los draws del juego).
// Devuelve false si hay que volver a intentarlo en el siguiente draw (no
// hay profundidad enlazada todavia).
bool GpuLevelDraw(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& fullVp, const float eyeProj[2][16]);

// Hilo de render (StereoHook): lotes marcados (la lamina de agua en el cono
// o cruzando un corte, los objetos de malla cerca de un corte y las caras de
// sombra y de liquido de los sectores que cruza), con el nivel en la GPU.
// Begin cambia el pixel shader del juego por uno igual que ademas descarta
// lo que queda dentro del cono, delante del corte vertical y por encima del
// de altura (false si no se puede); Eye antes de cada ojo; End lo deja como
// estaba. reflection: lote que puede ser del reflejo del agua (el recorte se
// prueba donde el rayo de vista cruza el agua); false en los de objetos y
// caras del motor.
bool GpuLevelWaterClipBegin(ID3D11DeviceContext* ctx, bool reflection);
void GpuLevelWaterClipEye(ID3D11DeviceContext* ctx, int eye, const D3D11_VIEWPORT& eyeVp, const float proj[16]);
void GpuLevelWaterClipEnd(ID3D11DeviceContext* ctx);
// Hilo del juego, durante la pasada en la GPU: el reflejo de la lamina de
// agua a esta altura (Y del juego) va marcado y se recorta por donde el rayo
// de vista cruza el agua (si el agua esta cortada ahi, no hay reflejo).
void GpuLevelSetReflectionPlane(double waterY);

// Hilo del juego: caja (mundo) de la geometria capturada de 'level', si es
// el nivel capturado. Para el rango de los cortes del diorama.
bool GpuLevelBounds(long long level, double mn[3], double mx[3]);

// Hilo de render, al enviar la imagen al visor (Dx11Hook, solo con el
// diorama): pose del visor con la que se calculo la maqueta del fotograma
// que se presenta (el de la ultima marca leida). false: no la hay (se usa la
// de siempre).
bool GpuLevelFramePose(float out12[12]);

} // namespace BladeVR
