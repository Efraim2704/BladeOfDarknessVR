#pragma once
#include <cmath>

// ---------------------------------------------------------------------
// Cinematica inversa de un brazo (combate con los mandos, version 2.0).
// Solo matematicas, sin nada de Windows ni del juego, para poder probarla
// aparte. Convencion del motor: matrices 4x4 de doubles con vectores fila,
// filas 0-2 = ejes del nodo en el mundo, traslacion en [12..14]; un vector
// local pasa al mundo como v * M. Las rotaciones 3x3 van igual (filas = ejes).
//
// Cadena (como en el motor): S = brazo (R_Arm, la articulacion del hombro),
// E = codo (R_Elbow), Fo = antebrazo (R_Forearm, giro), Wr = muneca
// (R_Wrist) y H = mano (R_Hand, sin hijos).
// ---------------------------------------------------------------------

namespace BladeVR {
namespace ik {

constexpr double kPi = 3.14159265358979323846;

inline double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

inline void Cross(const double a[3], const double b[3], double o[3]) {
    double x = a[1] * b[2] - a[2] * b[1];
    double y = a[2] * b[0] - a[0] * b[2];
    double z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}

inline double Len(const double a[3]) { return std::sqrt(Dot(a, a)); }

inline void Copy3(const double a[3], double o[3]) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }

inline void Sub(const double a[3], const double b[3], double o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }

inline void Add(const double a[3], const double b[3], double o[3]) { o[0] = a[0] + b[0]; o[1] = a[1] + b[1]; o[2] = a[2] + b[2]; }

// o += a * s
inline void Madd(double o[3], const double a[3], double s) { o[0] += a[0] * s; o[1] += a[1] * s; o[2] += a[2] * s; }

inline void Lerp3(const double a[3], const double b[3], double t, double o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = a[i] + (b[i] - a[i]) * t;
}

// Normaliza; false (y deja el vector igual) si es casi nulo.
inline bool Norm(double a[3], double minLen = 1e-9) {
    double l = Len(a);
    if (l < minLen) return false;
    a[0] /= l; a[1] /= l; a[2] /= l;
    return true;
}

// Componente de v perpendicular al unitario u.
inline void Perp(const double v[3], const double u[3], double o[3]) {
    double d = Dot(v, u);
    o[0] = v[0] - u[0] * d; o[1] = v[1] - u[1] * d; o[2] = v[2] - u[2] * d;
}

// --- 3x3 (filas = ejes) --------------------------------------------------
inline void M3Mul(const double a[9], const double b[9], double o[9]) {
    double t[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) t[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    }
    for (int i = 0; i < 9; ++i) o[i] = t[i];
}

inline void M3T(const double a[9], double o[9]) {
    double t[9] = { a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8] };
    for (int i = 0; i < 9; ++i) o[i] = t[i];
}

// o = v * m (vector fila: local -> mundo)
inline void VecM3(const double v[3], const double m[9], double o[3]) {
    double x = v[0] * m[0] + v[1] * m[3] + v[2] * m[6];
    double y = v[0] * m[1] + v[1] * m[4] + v[2] * m[7];
    double z = v[0] * m[2] + v[1] * m[5] + v[2] * m[8];
    o[0] = x; o[1] = y; o[2] = z;
}

// o = m * v (mundo -> local para una rotacion: componentes sobre cada eje)
inline void M3Vec(const double m[9], const double v[3], double o[3]) {
    double x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    double y = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    double z = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    o[0] = x; o[1] = y; o[2] = z;
}

// Gram-Schmidt sobre las filas x e y; z = x * y.
inline void M3Orthonormalize(double m[9]) {
    double x[3] = { m[0], m[1], m[2] }, y[3] = { m[3], m[4], m[5] }, z[3];
    if (!Norm(x)) { x[0] = 1; x[1] = 0; x[2] = 0; }
    Perp(y, x, y);
    if (!Norm(y)) {
        double t[3] = { 0, 1, 0 };
        if (std::fabs(x[1]) > 0.9) { t[0] = 1; t[1] = 0; }
        Perp(t, x, y);
        Norm(y);
    }
    Cross(x, y, z);
    for (int i = 0; i < 3; ++i) { m[i] = x[i]; m[3 + i] = y[i]; m[6 + i] = z[i]; }
}

// Rotacion de 4x4: filas normalizadas (rot) y su longitud (escala de cada eje).
inline void SplitM4(const double m[16], double rot[9], double scale[3], double pos[3]) {
    for (int r = 0; r < 3; ++r) {
        double row[3] = { m[r * 4], m[r * 4 + 1], m[r * 4 + 2] };
        double l = Len(row);
        scale[r] = l > 1e-12 ? l : 1.0;
        for (int c = 0; c < 3; ++c) rot[r * 3 + c] = row[c] / scale[r];
    }
    pos[0] = m[12]; pos[1] = m[13]; pos[2] = m[14];
}

inline void ComposeM4(const double rot[9], const double scale[3], const double pos[3], double m[16]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m[r * 4 + c] = rot[r * 3 + c] * scale[r];
        m[r * 4 + 3] = 0.0;
    }
    m[12] = pos[0]; m[13] = pos[1]; m[14] = pos[2]; m[15] = 1.0;
}

// Cuaternion de una rotacion (formula estandar; se usa siempre en pareja
// con M3FromQuat, asi que la convencion da igual).
inline void QuatFromM3(const double m[9], double q[4]) {
    double tr = m[0] + m[4] + m[8];
    if (tr > 0.0) {
        double s = std::sqrt(tr + 1.0) * 2.0;
        q[3] = 0.25 * s;
        q[0] = (m[7] - m[5]) / s;
        q[1] = (m[2] - m[6]) / s;
        q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2.0;
        q[3] = (m[7] - m[5]) / s;
        q[0] = 0.25 * s;
        q[1] = (m[1] + m[3]) / s;
        q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2.0;
        q[3] = (m[2] - m[6]) / s;
        q[0] = (m[1] + m[3]) / s;
        q[1] = 0.25 * s;
        q[2] = (m[5] + m[7]) / s;
    } else {
        double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2.0;
        q[3] = (m[3] - m[1]) / s;
        q[0] = (m[2] + m[6]) / s;
        q[1] = (m[5] + m[7]) / s;
        q[2] = 0.25 * s;
    }
}

inline void M3FromQuat(const double qin[4], double m[9]) {
    double q[4] = { qin[0], qin[1], qin[2], qin[3] };
    double l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-12) { q[0] = q[1] = q[2] = 0.0; q[3] = 1.0; l = 1.0; }
    for (int i = 0; i < 4; ++i) q[i] /= l;
    double x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

// Interpolacion esferica entre dos rotaciones (t = 0 -> a, t = 1 -> b).
inline void SlerpM3(const double a[9], const double b[9], double t, double o[9]) {
    if (t <= 0.0) { for (int i = 0; i < 9; ++i) o[i] = a[i]; return; }
    if (t >= 1.0) { for (int i = 0; i < 9; ++i) o[i] = b[i]; return; }
    double qa[4], qb[4], q[4];
    QuatFromM3(a, qa);
    QuatFromM3(b, qb);
    double d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0.0) { d = -d; for (int i = 0; i < 4; ++i) qb[i] = -qb[i]; }
    double ka, kb;
    if (d > 0.9995) {
        ka = 1.0 - t; kb = t;
    } else {
        double th = std::acos(d), s = std::sin(th);
        ka = std::sin((1.0 - t) * th) / s;
        kb = std::sin(t * th) / s;
    }
    for (int i = 0; i < 4; ++i) q[i] = qa[i] * ka + qb[i] * kb;
    M3FromQuat(q, o);
}

// Angulo (rad) entre dos rotaciones.
inline double M3AngleBetween(const double a[9], const double b[9]) {
    double at[9], r[9];
    M3T(a, at);
    M3Mul(at, b, r);
    double c = (r[0] + r[4] + r[8] - 1.0) * 0.5;
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return std::acos(c);
}

// Rotacion de 'ang' rad alrededor del unitario u, para vectores fila (v * R).
inline void AxisAngleM3(const double u[3], double ang, double o[9]) {
    double c = std::cos(ang), s = std::sin(ang), k = 1.0 - c;
    double x = u[0], y = u[1], z = u[2];
    // Rodrigues (columnas) traspuesta.
    o[0] = c + x * x * k;     o[1] = x * y * k + z * s; o[2] = x * z * k - y * s;
    o[3] = x * y * k - z * s; o[4] = c + y * y * k;     o[5] = y * z * k + x * s;
    o[6] = x * z * k + y * s; o[7] = y * z * k - x * s; o[8] = c + z * z * k;
}

// Angulo con signo (rad) de v1 a v2 alrededor del unitario u (regla de la mano derecha).
inline double SignedAngle(const double v1[3], const double v2[3], const double u[3]) {
    double c[3];
    Cross(v1, v2, c);
    return std::atan2(Dot(c, u), Dot(v1, v2));
}

// Normal del plano del brazo firmada hacia el eje anatomico ref (con el
// brazo casi recto, m es pequena y ref la completa).
inline bool SignedPlaneNormal(const double m[3], const double ref[3], double n[3]) {
    double lm = Len(m);
    double s = Dot(m, ref) >= 0.0 ? 1.0 : -1.0;
    double kk = 0.15 - lm;
    if (kk < 0.0) kk = 0.0;
    for (int i = 0; i < 3; ++i) n[i] = s * m[i] + kk * ref[i];
    return Norm(n, 1e-6);
}

// Base con filas: dir, n (perpendicular a dir), dir x n.
inline bool FrameFromDirNormal(const double dirIn[3], const double nIn[3], double f[9]) {
    double d[3], n[3], b[3];
    Copy3(dirIn, d);
    if (!Norm(d)) return false;
    Perp(nIn, d, n);
    if (!Norm(n, 1e-6)) return false;
    Cross(d, n, b);
    for (int i = 0; i < 3; ++i) { f[i] = d[i]; f[3 + i] = n[i]; f[6 + i] = b[i]; }
    return true;
}

// Rotacion del mundo que lleva la base fa a la base fb (fa * R = fb).
inline void AlignFrames(const double fa[9], const double fb[9], double r[9]) {
    double t[9];
    M3T(fa, t);
    M3Mul(t, fb, r);
}

// --- agarre: de los ejes del mando a los de la mano ---------------------------
// "Cuerpo" = base con filas derecha, abajo, adelante (como el mundo del juego,
// girada con la guinada del personaje o de la vista).
//
// Espacio de la entidad del personaje (debajo de la raiz del esqueleto; leido
// del caballero, 3DChars/Kgt_N.BOD): x = izquierda, y = atras, z = arriba.
inline void EntityToBody(const double ve[3], double vb[3]) {
    double r = -ve[0], d = -ve[2], f = -ve[1];
    vb[0] = r; vb[1] = d; vb[2] = f;
}

// Del brazo colgando al brazo estirado al frente (flexion de 90 grados del
// hombro alrededor de la derecha del cuerpo): abajo -> adelante, adelante ->
// arriba. Para vectores fila en la base del cuerpo (R, D, F): v * M.
inline void ArmsForwardBody(double m[9]) {
    double t[9] = { 1, 0, 0, 0, 0, 1, 0, -1, 0 };
    for (int i = 0; i < 9; ++i) m[i] = t[i];
}

// Agarre = mano en los ejes del mando: G = H * C^T (filas; H y C en la misma base).
inline void GripFrom(const double hand[9], const double ctrl[9], double grip[9]) {
    double ct[9];
    M3T(ctrl, ct);
    M3Mul(hand, ct, grip);
    M3Orthonormalize(grip);
}

// --- un brazo ----------------------------------------------------------------
enum ArmNode { kS = 0, kE = 1, kFo = 2, kWr = 3, kH = 4 };

struct ArmInput {
    double anim[5][16];       // W de la animacion (mundo): S, E, Fo, Wr, H
    double bindH[16];         // B de la mano (local respecto a la muneca)
    double handRot[9];        // rotacion pedida para la mano (filas)
    double handPos[3];        // posicion pedida para la mano (mm, mundo)
    double weight;            // 0 = animacion, 1 = mando
    double bodyRight[3];      // ejes del cuerpo (mundo, horizontales, unitarios)
    double bodyFwd[3];
    double down[3];           // abajo del mundo (0,1,0 en el juego)
    double outSign;           // +1 brazo derecho, -1 izquierdo
    double hingeLocal[3];     // eje del codo en los ejes de S (por si la animacion lo tiene recto)
    double poleHandWeight;    // 0..1: cuanto decide la orientacion de la mano hacia donde va el codo
    double maxStretch;        // >= 1: cuanto se puede alargar el brazo y el antebrazo para llegar a la mano
                              // (el alcance del jugador / el del personaje; 1 = nada)
    double twistShare;        // 0..1: parte del giro de la muneca que se lleva el antebrazo
    bool hasPrevTwist;        // el giro del fotograma anterior es valido (para no saltar de +180 a -180)
    double prevTwist;         // rad
    double restTwistW;        // ronda 27 (con restTwist): 0..1, cuanto se mide desde el reposo y cuanto
                              // desde la animacion (la izquierda al coger o soltar el arma a dos manos
                              // pasa de una a otra poco a poco; antes saltaba el antebrazo)
    bool restTwist;           // el giro del codo, antebrazo y muneca se mide desde la pose de reposo
                              // (relativa al plano del brazo), no desde la animacion: las animaciones
                              // del escudo giran el antebrazo y el giro del jugador se sumaba a ese
    double restRelE[9];       // reposo: rotacion de codo, antebrazo y muneca respecto a la base del
    double restRelFo[9];      // antebrazo (hueso + normal del plano del brazo), ver RestTwistRelations
    double restRelWr[9];
    bool lockWrist;           // escudo en el antebrazo: la muneca y la mano guardan con el antebrazo la
                              // relacion de la animacion (la mano no sale de la correa) y giran con el
                              // (twistShare del giro del jugador); la flexion de la muneca no se copia
    bool shieldPole;          // ronda 19 (con lockWrist): el codo va hacia donde lo pone la orientacion
                              // del mando (el antebrazo que llevaria la mano en la correa), sin rebajarlo
                              // cuando no coincide con el del cuerpo, y la normal del plano del brazo
                              // nunca cambia de lado (el escudo, en el antebrazo, daba media vuelta)
    double foreMaxDeg;        // tope del giro del antebrazo (grados; 0 = 90)
    double softReach;         // ronda 22: 0..0,1 del alcance: cerca de estirarse del todo el brazo se
                              // acerca a recto de forma suave (sin esto, 1 mm de ruido en el objetivo
                              // movia el codo 5-10 grados con el brazo casi recto)
    bool animPole;            // ronda 22: el codo hacia donde lo tiene la animacion (mano izquierda en
                              // el arma a dos manos), sin el que sugiere la mano, y la normal del plano
                              // del brazo siempre del lado del codo
    bool hasPrevPole;         // ronda 23: hacia donde iba el codo en la resolucion anterior (mundo)
    double prevPole[3];
    double poleAlpha;         // 0..1: cuanto se acerca el codo de la anterior a la nueva (1 = sin suavizar)
    double protractMaxMm;     // ronda 34: cuanto puede adelantarse el hombro hacia la muneca pedida
                              // (escapula; 0 = nada) cuando esta mas lejos que protractStart del alcance
    double protractStart;     // ronda 34: fraccion de brazo + antebrazo desde la que se adelanta
};

struct ArmOutput {
    double m[5][16];
    bool saturated;           // la mano pedida estaba fuera del alcance del brazo (ya alargado)
    bool tooClose;
    double reachReq;          // hombro -> muneca pedida (mm)
    double reachMax;          // brazo + antebrazo sin alargar (mm)
    double elbowDeg;          // angulo interior del codo (180 = recto)
    double twistDeg;          // giro extra de la muneca respecto al antebrazo (continuo)
    double twistRad;
    bool hingeMeasured;       // la animacion tenia el codo doblado: eje medido (solo informativo)
    double hingeLocal[3];
    double handErrMm;         // distancia entre la mano resuelta y la pedida
    double stretch;           // alargamiento usado (1 = ninguno)
    double pole[3];           // ronda 23: hacia donde va el codo (unitario, mundo)
    double protractMm;        // ronda 34: cuanto se adelanto el hombro (mm)
    double pullMm;            // ronda 35: cuanto se acerco la muneca al hombro con el escudo (mm)
};

// Alarga una base (filas = ejes en el mundo, con su escala) a lo largo del
// unitario u por el factor k: cada eje r pasa a r + (k - 1) (r . u) u.
inline void StretchAlong(double m16[16], const double u[3], double k) {
    if (k == 1.0) return;
    for (int r = 0; r < 3; ++r) {
        double row[3] = { m16[r * 4], m16[r * 4 + 1], m16[r * 4 + 2] };
        double d = Dot(row, u) * (k - 1.0);
        for (int c = 0; c < 3; ++c) m16[r * 4 + c] = row[c] + d * u[c];
    }
}

// Con la pose de reposo (W de hombro, codo, antebrazo y muneca, en cualquier
// espacio) y el eje del codo en los ejes del hombro: rotaciones de codo,
// antebrazo y muneca respecto a la base del antebrazo (hueso + normal del
// plano del brazo), como las usa SolveArm con restTwist.
inline bool RestTwistRelations(const double S[16], const double E[16], const double Fo[16], const double Wr[16],
                               const double hingeLocal[3], double relE[9], double relFo[9], double relWr[9]) {
    double RS[9], RE[9], RFo[9], RWr[9], sc[3], pS[3], pE[3], pFo[3], pWr[3];
    SplitM4(S, RS, sc, pS);
    SplitM4(E, RE, sc, pE);
    SplitM4(Fo, RFo, sc, pFo);
    SplitM4(Wr, RWr, sc, pWr);
    double a[3], c[3], h[3], m[3], n[3], f[9], fT[9];
    Sub(pE, pS, a);
    Sub(pWr, pE, c);
    if (!Norm(a) || !Norm(c)) return false;
    VecM3(hingeLocal, RS, h);
    if (!Norm(h)) return false;
    Cross(a, c, m);
    if (!SignedPlaneNormal(m, h, n) || !FrameFromDirNormal(c, n, f)) return false;
    M3T(f, fT);
    M3Mul(RE, fT, relE);
    M3Mul(RFo, fT, relFo);
    M3Mul(RWr, fT, relWr);
    return true;
}

inline bool SolveArm(const ArmInput& in, ArmOutput* out) {
    double R[5][9], sc[5][3], P[5][3];
    for (int i = 0; i < 5; ++i) SplitM4(in.anim[i], R[i], sc[i], P[i]);
    out->saturated = false;
    out->tooClose = false;
    out->hingeMeasured = false;
    out->twistDeg = 0.0;
    out->twistRad = 0.0;
    out->elbowDeg = 180.0;
    out->handErrMm = 0.0;
    out->stretch = 1.0;
    out->protractMm = 0.0;
    out->pullMm = 0.0;
    out->pole[0] = out->pole[1] = out->pole[2] = 0.0;
    for (int i = 0; i < 3; ++i) out->hingeLocal[i] = in.hingeLocal[i];

    double w = in.weight;
    if (w < 0.0) w = 0.0;
    if (w > 1.0) w = 1.0;
    const double* pS = P[kS];
    double vSE[3], vEW[3];
    Sub(P[kE], pS, vSE);
    Sub(P[kWr], P[kE], vEW);
    double a = Len(vSE), b = Len(vEW);
    out->reachMax = a + b;
    if (a < 1.0 || b < 1.0) return false;

    // 1. Mano: rotacion y posicion mezcladas entre la animacion y el mando.
    double bhRot[9], bhScale[3], bhT[3];
    SplitM4(in.bindH, bhRot, bhScale, bhT);
    double HR[9], pH[3];
    SlerpM3(R[kH], in.handRot, w, HR);
    Lerp3(P[kH], in.handPos, w, pH);

    // 2. Muneca: la mano no se dobla respecto a ella (R de la mano = identidad).
    double bhRotT[9], wrT[9], WR[9];
    M3T(bhRot, bhRotT);
    M3Mul(bhRotT, in.handRot, wrT);
    SlerpM3(R[kWr], wrT, w, WR);
    // Desde la muneca hasta la mano: B(mano).t con los ejes (y la escala) de la muneca.
    double off[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; ++k) {
        double row[3] = { WR[k * 3], WR[k * 3 + 1], WR[k * 3 + 2] };
        Madd(off, row, bhT[k] * sc[kWr][k]);
    }
    double pWr[3];
    Sub(pH, off, pWr);

    // Ronda 35: con el escudo (muneca fija al antebrazo) la orientacion del
    // mando dice hacia donde va el antebrazo (fw, de codo a muneca) y con ello
    // donde pide el codo: muneca - fw * b.
    bool shieldPole = in.shieldPole && in.lockWrist;
    double fwS[3] = { 0, 0, 0 };
    if (shieldPole) {
        // Ronda 19: la base del antebrazo (fila 0 = hueso) que lleva la muneca
        // pedida con la relacion del reposo (o la de la animacion sin restTwist).
        if (in.restTwist) {
            double rwT[9], ftImp[9];
            M3T(in.restRelWr, rwT);
            M3Mul(rwT, WR, ftImp);
            fwS[0] = ftImp[0]; fwS[1] = ftImp[1]; fwS[2] = ftImp[2];
        } else {
            double wrTa[9], rel[9], v[3];
            M3T(R[kWr], wrTa);
            M3Mul(wrTa, WR, rel);
            Sub(P[kWr], P[kFo], v);
            VecM3(v, rel, fwS);
        }
    }

    // Ronda 34: el hombro se adelanta (escapula) hacia la muneca pedida cuando
    // esta lejos. Con el escudo delante de la cara el hombro del personaje
    // queda atras (guardia de lado, ojos 10 cm por delante desde la ronda 26):
    // el brazo salia recto y la mano y el escudo, a 8-11 cm del mando.
    // Ronda 35: con el escudo, el hombro va hacia el codo que pide el mando
    // (hasta dejarlo a la longitud del brazo, a) en vez de por la recta hacia
    // la muneca: por la recta el antebrazo no coincidia con el del mando (mano
    // a 13-72 mm) y con la mano muy lejos el codo seguia recto (175 grados,
    // 118 mm) y temblaba. Lo que el hombro no alcanza (tope protractMaxMm) lo
    // ponen el alargamiento de brazo y antebrazo (hasta maxStretch, como
    // siempre) y, si aun falta, la muneca, que se acerca: el antebrazo, y el
    // escudo, siempre con la orientacion del mando y el codo doblado como el
    // del jugador. Entra poco a poco desde protractStart hasta 0,10 mas del
    // alcance (sin saltos).
    double kShield = 0.0;   // alargamiento ya decidido aqui (0 = el del paso 3)
    // Ronda 36: con el escudo en el antebrazo (lockWrist) el antebrazo no se
    // alarga nunca: su matriz escalada a lo largo del hueso deformaba el escudo
    // (de redondo a ovalado segun la inclinacion de la muneca). Lo que haga
    // falta lo pone el brazo (hasta maxStretch).
    const bool rigidFore = in.lockWrist;
    if (in.protractMaxMm > 0.0 && w > 0.0) {
        double dv0[3];
        Sub(pWr, P[kS], dv0);
        double d0 = Len(dv0);
        double start = (a + b) * in.protractStart;
        double fwn[3] = { fwS[0], fwS[1], fwS[2] };
        if (shieldPole && d0 > start && d0 > 1.0 && Norm(fwn)) {
            double gw = (d0 - start) / ((a + b) * 0.10);
            if (gw > 1.0) gw = 1.0;
            gw *= w;
            double pe[3] = { pWr[0], pWr[1], pWr[2] };
            Madd(pe, fwn, -b);
            double toE[3];
            Sub(pe, P[kS], toE);
            double e = Len(toE);
            if (e > a && e > 1.0) {
                // Solucion completa (peso 1); luego se mezcla con gw.
                double u[3] = { toE[0] / e, toE[1] / e, toE[2] / e };
                // Solo hacia delante (hacia la muneca): de lado o hacia atras no.
                double fwd = Dot(u, dv0) / d0 / 0.3;
                if (fwd < 0.0) fwd = 0.0;
                if (fwd > 1.0) fwd = 1.0;
                double mvF = (e - a) * fwd;
                if (mvF > in.protractMaxMm) mvF = in.protractMaxMm;
                double sF[3] = { P[kS][0], P[kS][1], P[kS][2] };
                Madd(sF, u, mvF);
                // Alargamiento kF (1..maxStretch): |muneca - fw * b * k - hombro| = a * k.
                // Ronda 36: con el escudo solo el brazo (el antebrazo, que lleva
                // el escudo, no se alarga: |muneca - fw * b - hombro| = a * k).
                double q[3];
                Sub(pWr, sF, q);
                double kMaxS = in.maxStretch > 1.0 ? in.maxStretch : 1.0;
                auto gap = [&](double kk) {
                    double v[3] = { q[0], q[1], q[2] };
                    Madd(v, fwn, -b * (rigidFore ? 1.0 : kk));
                    return Len(v) - a * kk;
                };
                double kF = 1.0;
                if (gap(1.0) > 0.0 && kMaxS > 1.0) {
                    kF = kMaxS;
                    const int kSteps = 16;
                    double lo = 1.0;
                    for (int st = 1; st <= kSteps; ++st) {
                        double hi = 1.0 + (kMaxS - 1.0) * st / kSteps;
                        if (gap(hi) <= 0.0) {
                            for (int it = 0; it < 30; ++it) {
                                double mid = 0.5 * (lo + hi);
                                if (gap(mid) > 0.0) lo = mid; else hi = mid;
                            }
                            kF = hi;
                            break;
                        }
                        lo = hi;
                    }
                }
                // Lo que aun falta: la muneca (y la mano) hacia el hombro.
                double peF[3] = { pWr[0], pWr[1], pWr[2] };
                Madd(peF, fwn, -b * (rigidFore ? 1.0 : kF));
                double back[3];
                Sub(peF, sF, back);
                double eF = Len(back);
                double rF = eF - a * kF;
                if (rF < 0.0 || !Norm(back)) rF = 0.0;
                double mv = mvF * gw;
                for (int kk = 0; kk < 3; ++kk) P[kS][kk] += u[kk] * mv;
                out->protractMm = mv;
                kShield = 1.0 + (kF - 1.0) * gw;
                double r = rF * gw;
                if (r > 0.0) {
                    Madd(pWr, back, -r);
                    Madd(pH, back, -r);
                    out->pullMm = r;
                }
            }
        } else if (!shieldPole && d0 > start && d0 > 1.0) {
            double mv = (d0 - start) * w;
            if (mv > in.protractMaxMm) mv = in.protractMaxMm;
            for (int kk = 0; kk < 3; ++kk) P[kS][kk] += dv0[kk] / d0 * mv;
            out->protractMm = mv;
        }
    }

    // 3. Dos huesos: hombro fijo, muneca a la distancia que se pueda. Si no
    // llega, brazo y antebrazo se alargan (como mucho maxStretch, el alcance
    // real del jugador) para que la mano siga en el mando.
    double dv[3];
    Sub(pWr, pS, dv);
    double d = Len(dv);
    out->reachReq = d;
    if (d < 1.0) return false;
    double dh[3] = { dv[0] / d, dv[1] / d, dv[2] / d };
    double kMax = in.maxStretch > 1.0 ? in.maxStretch : 1.0;
    double k = 1.0;
    if (d > (a + b) * 0.999) {
        k = d / ((a + b) * 0.999);   // con margen: el triangulo cierra (dmax > d)
        if (!rigidFore && k > kMax) k = kMax;
    }
    double kA = k, kB = k;           // brazo / antebrazo
    if (rigidFore) {
        // Ronda 36: el mismo alcance solo con el brazo.
        kA = (k * (a + b) - b) / a;
        if (kA < 1.0) kA = 1.0;
        if (kA > kMax) kA = kMax;
        kB = 1.0;
    }
    kA = 1.0 + (kA - 1.0) * w;
    kB = 1.0 + (kB - 1.0) * w;
    if (kShield > kA) kA = kShield;  // ronda 35: el del escudo (codo doblado)
    out->stretch = kA;
    double a2 = a * kA, b2 = b * kB;
    double dmax = (a2 + b2) * 0.9995;
    double dmin = std::fabs(a2 - b2) + 1.0;
    double dd = d;
    if (in.softReach > 0.0) {
        // Ronda 22: alcance suave. Desde (1 - softReach) del alcance la
        // distancia se acerca al maximo de forma exponencial: el codo no llega
        // a recto del todo y su angulo ya no se dispara con el ruido.
        double sw = in.softReach * (a2 + b2);
        if (sw > dmax * 0.5) sw = dmax * 0.5;
        double start = dmax - sw;
        if (dd > start) dd = start + sw * (1.0 - std::exp(-(dd - start) / sw));
        if (d > dmax) out->saturated = true;
    }
    if (dd > dmax) { dd = dmax; out->saturated = true; }
    if (dd < dmin) { dd = dmin; out->tooClose = true; }
    if (dd != d) {
        double shift[3] = { dh[0] * (dd - d), dh[1] * (dd - d), dh[2] * (dd - d) };
        Add(pWr, shift, pWr);
        Add(pH, shift, pH);
    }

    // Hacia donde va el codo (perpendicular a hombro -> muneca). El que
    // sugiere la mano solo cuenta si no contradice al del cuerpo: con el
    // escudo arriba, la mano daba un codo hacia arriba.
    double pAnim[3];
    Perp(vSE, dh, pAnim);
    double animPerpMm = Len(pAnim);
    bool haveAnimPole = Norm(pAnim, 1e-3);
    double pb[3] = { in.down[0], in.down[1], in.down[2] };
    Madd(pb, in.bodyRight, 0.5 * in.outSign);
    Madd(pb, in.bodyFwd, -0.3);
    double pbPerp[3];
    Perp(pb, dh, pbPerp);
    bool haveBody = Norm(pbPerp, 1e-3);
    double bone[3] = { off[0], off[1], off[2] };   // muneca -> mano: el antebrazo sigue por detras
    if (shieldPole) Copy3(fwS, bone);   // ronda 19 (fw calculado arriba, ronda 35)
    double phPerp[3] = { 0, 0, 0 };
    double handPerpMm = 0.0;
    bool haveHand = false;
    if (Norm(bone)) {
        double pe[3] = { pWr[0], pWr[1], pWr[2] };
        Madd(pe, bone, -b2);
        double v[3];
        Sub(pe, pS, v);
        Perp(v, dh, phPerp);
        handPerpMm = Len(phPerp);
        haveHand = Norm(phPerp, 1e-3);
    }
    double pole[3] = { 0, 0, 0 };
    double hw = in.poleHandWeight;
    if (shieldPole && haveHand && handPerpMm < 40.0) {
        // El codo que pide el mando cae casi en la linea hombro-muneca: su
        // direccion no es fiable, se mezcla con la del cuerpo.
        hw *= handPerpMm / 40.0;
    }
    if (haveBody && haveHand && !shieldPole) {
        double agree = (Dot(phPerp, pbPerp) + 0.5) / 1.0;   // 1 si coinciden (>= 0,5), 0 si se oponen (<= -0,5)
        if (agree < 0.0) agree = 0.0;
        if (agree > 1.0) agree = 1.0;
        hw *= agree;
    }
    if (in.animPole) {
        // Ronda 22: el codo de la animacion (la mano izquierda en el arma a dos
        // manos). Sin normalizar la del codo de la animacion: si cae cerca de la
        // linea hombro-muneca pesa poco y manda el del cuerpo (40 mm), sin
        // saltos de direccion. La que sugiere la mano no cuenta: con el brazo
        // casi recto cambiaba de lado con milimetros de ruido.
        double pa[3] = { pAnim[0] * animPerpMm, pAnim[1] * animPerpMm, pAnim[2] * animPerpMm };
        if (!haveAnimPole) pa[0] = pa[1] = pa[2] = 0.0;
        Copy3(pa, pole);
        if (haveBody) Madd(pole, pbPerp, 40.0);
    } else {
        if (haveBody) Madd(pole, pbPerp, haveHand ? 1.0 - hw : 1.0);
        if (haveHand) Madd(pole, phPerp, haveBody ? hw : 1.0);
    }
    bool havePole = Norm(pole, 1e-3);
    if (!havePole && haveAnimPole) { Copy3(pAnim, pole); havePole = true; }
    if (!havePole) {
        double t[3] = { in.down[0], in.down[1], in.down[2] };
        if (std::fabs(Dot(t, dh)) > 0.95) Copy3(in.bodyFwd, t);
        Perp(t, dh, pole);
        if (!Norm(pole)) return false;
    }
    if (haveAnimPole && w < 1.0) {
        double mix[3];
        Lerp3(pAnim, pole, w, mix);
        if (Norm(mix, 1e-3)) Copy3(mix, pole);
    }
    if (in.hasPrevPole && in.poleAlpha < 1.0) {
        // Ronda 23: el codo gira alrededor de la linea hombro-muneca hacia el
        // nuevo, una parte cada vez (poleAlpha): con el brazo casi recto su
        // direccion cambiaba mucho con muy poco y el brazo temblaba.
        double pp[3];
        Perp(in.prevPole, dh, pp);
        if (Norm(pp, 1e-3)) {
            double al = in.poleAlpha > 0.0 ? in.poleAlpha : 0.0;
            double rot[9], np[3];
            AxisAngleM3(dh, SignedAngle(pp, pole, dh) * al, rot);
            VecM3(pp, rot, np);
            if (Norm(np, 1e-3)) Copy3(np, pole);
        }
    }
    Copy3(pole, out->pole);
    double cosA = (a2 * a2 + dd * dd - b2 * b2) / (2.0 * a2 * dd);
    if (cosA > 1.0) cosA = 1.0;
    if (cosA < -1.0) cosA = -1.0;
    double sinA = std::sqrt(1.0 - cosA * cosA);
    double pE[3] = { pS[0], pS[1], pS[2] };
    Madd(pE, dh, a2 * cosA);
    Madd(pE, pole, a2 * sinA);

    // 4. Bases de la animacion y de la solucion: hueso + eje del codo.
    // La normal del plano del brazo se firma siempre hacia el eje anatomico
    // (fijo en los ejes de S; en la solucion, ese eje llevado por el giro
    // minimo del brazo). Asi un codo casi recto o pasado hacia atras (la pose
    // de reposo del caballero lo tiene 10 grados al reves) no da la vuelta al
    // brazo, y con peso 0 la animacion sale identica.
    double aHat[3], cHat[3], hA[3], mA[3], nA[3];
    Copy3(vSE, aHat);
    Norm(aHat);
    Copy3(vEW, cHat);
    Norm(cHat);
    VecM3(in.hingeLocal, R[kS], hA);            // ejes de S -> mundo
    if (!Norm(hA)) return false;
    Cross(aHat, cHat, mA);
    double lenA = Len(mA);
    if (lenA > 0.5 && Dot(mA, hA) > 0.3 * lenA) {
        double meas[3] = { mA[0] / lenA, mA[1] / lenA, mA[2] / lenA };
        M3Vec(R[kS], meas, out->hingeLocal);   // mundo -> ejes de S
        Norm(out->hingeLocal);
        out->hingeMeasured = true;
    }
    double tHat[3], ctHat[3], hT[3], mT[3], nT[3];
    Sub(pE, pS, tHat);
    Norm(tHat);
    Sub(pWr, pE, ctHat);
    Norm(ctHat);
    {
        double ax[3];
        Cross(aHat, tHat, ax);
        double sn = Len(ax), cs = Dot(aHat, tHat);
        if (sn > 1e-9) {
            ax[0] /= sn; ax[1] /= sn; ax[2] /= sn;
            double sw[9];
            AxisAngleM3(ax, std::atan2(sn, cs), sw);
            VecM3(hA, sw, hT);
        } else if (cs > 0.0) {
            Copy3(hA, hT);
        } else {
            hT[0] = -hA[0]; hT[1] = -hA[1]; hT[2] = -hA[2];
        }
    }
    Cross(tHat, ctHat, mT);
    if (shieldPole || in.animPole) {
        // Ronda 19: la normal del plano del brazo resuelto va siempre hacia el
        // lado del codo (polo x hombro-muneca, como tHat x ctHat): con la mano
        // cruzando el pecho el eje anatomico llevado por el balanceo minimo
        // quedaba del otro lado y la base daba media vuelta (y el escudo).
        double pdh[3];
        Cross(pole, dh, pdh);
        if (Dot(pdh, hT) < 0.0) { hT[0] = -hT[0]; hT[1] = -hT[1]; hT[2] = -hT[2]; }
    }
    if (!SignedPlaneNormal(mA, hA, nA) || !SignedPlaneNormal(mT, hT, nT)) return false;
    double fa[9], ft[9], rwU[9], rwL[9];
    if (!FrameFromDirNormal(aHat, nA, fa) || !FrameFromDirNormal(tHat, nT, ft)) return false;
    AlignFrames(fa, ft, rwU);
    if (!FrameFromDirNormal(cHat, nA, fa) || !FrameFromDirNormal(ctHat, nT, ft)) return false;
    AlignFrames(fa, ft, rwL);

    double SR[9], ER[9], FR[9];
    M3Mul(R[kS], rwU, SR);
    M3Mul(R[kE], rwL, ER);
    M3Mul(R[kFo], rwL, FR);

    // 5. Giro del antebrazo: una parte (twistShare) del giro extra de la
    // muneca, sin saltos: el angulo se sigue de un fotograma a otro (pasar de
    // +180 a -180 daba la vuelta al escudo, que va en el antebrazo).
    double wrUntw[9];
    M3Mul(R[kWr], rwL, wrUntw);
    if (in.restTwist) {
        // Codo, antebrazo y muneca "sin girar" desde la pose de reposo sobre
        // la base del antebrazo resuelto (ft), mezclados con los de la
        // animacion segun el peso (peso 0 = animacion).
        double eR[9], foR[9], wuR[9];
        M3Mul(in.restRelE, ft, eR);
        M3Mul(in.restRelFo, ft, foR);
        M3Mul(in.restRelWr, ft, wuR);
        double rtw = in.restTwistW < 0.0 ? 0.0 : (in.restTwistW > 1.0 ? 1.0 : in.restTwistW);
        SlerpM3(ER, eR, w * rtw, ER);
        SlerpM3(FR, foR, w * rtw, FR);
        SlerpM3(wrUntw, wuR, w * rtw, wrUntw);
    }
    // Giro alrededor del antebrazo por la descomposicion giro-balanceo del
    // cuaternion de la rotacion relativa (en el mundo). La proyeccion de un
    // eje de la mano fallaba con la muneca doblada mas de ~50 grados (de 40
    // a 50 grados de giro falso y saltos al cambiar de eje).
    double phi = 0.0;
    {
        double wuT[9], Rw[9], q[4];
        M3T(wrUntw, wuT);
        M3Mul(wuT, WR, Rw);
        QuatFromM3(Rw, q);
        double d = q[0] * ctHat[0] + q[1] * ctHat[1] + q[2] * ctHat[2];
        if (std::fabs(d) > 1e-9 || std::fabs(q[3]) > 1e-9) {
            phi = -2.0 * std::atan2(d, q[3]);   // signo: el de SignedAngle / AxisAngleM3 (vectores fila)
            while (phi > kPi) phi -= 2.0 * kPi;
            while (phi <= -kPi) phi += 2.0 * kPi;
        } else if (in.hasPrevTwist) {
            phi = in.prevTwist;                 // balanceo de 180 grados: giro indefinido
        }
    }
    if (in.hasPrevTwist) {
        while (phi - in.prevTwist > kPi) phi -= 2.0 * kPi;
        while (phi - in.prevTwist < -kPi) phi += 2.0 * kPi;
        if (phi > 1.5 * kPi) phi -= 2.0 * kPi;     // vueltas de mas: se vuelve al rango
        if (phi < -1.5 * kPi) phi += 2.0 * kPi;
    }
    out->twistRad = phi;
    out->twistDeg = phi * 180.0 / kPi;
    double share = in.twistShare;
    if (share < 0.0) share = 0.0;
    if (share > 1.0) share = 1.0;
    double fore = phi * share;
    const double kForeMax = (in.foreMaxDeg > 0.0 ? in.foreMaxDeg : 90.0) * kPi / 180.0;
    if (fore > kForeMax) fore = kForeMax;
    if (fore < -kForeMax) fore = -kForeMax;
    double tw[9];
    AxisAngleM3(ctHat, fore, tw);
    M3Mul(FR, tw, FR);
    // La rotacion del antebrazo alrededor de su eje pasa por el codo.
    double rwF[9];
    M3Mul(rwL, tw, rwF);

    // 5b. Escudo: muneca y mano con la relacion de la animacion respecto al
    // antebrazo (R_muneca = rel * R_antebrazo, en filas). La muneca sigue en
    // la del jugador (pWr sale del mando), asi que el antebrazo apunta como el
    // suyo; la mano ya no se dobla con el mando.
    if (in.lockWrist) {
        double foT[9], relW[9], wrAT[9], relH[9];
        M3T(R[kFo], foT);
        M3Mul(R[kWr], foT, relW);
        M3Mul(relW, FR, WR);
        M3T(R[kWr], wrAT);
        M3Mul(R[kH], wrAT, relH);
        M3Mul(relH, WR, HR);
        off[0] = off[1] = off[2] = 0.0;
        for (int kk = 0; kk < 3; ++kk) {
            double row[3] = { WR[kk * 3], WR[kk * 3 + 1], WR[kk * 3 + 2] };
            Madd(off, row, bhT[kk] * sc[kWr][kk]);
        }
    }

    // 6. Posiciones en cadena (malla continua), con el alargamiento.
    double eToFo[3], foToWr[3];
    Sub(P[kFo], P[kE], eToFo);
    Sub(P[kWr], P[kFo], foToWr);
    double pFo[3], pWrC[3], tmp[3];
    VecM3(eToFo, rwL, tmp);
    Add(pE, tmp, pFo);
    VecM3(foToWr, rwF, tmp);
    {
        double dk = Dot(tmp, ctHat) * (kB - 1.0);
        Madd(tmp, ctHat, dk);
    }
    Add(pFo, tmp, pWrC);
    double pHC[3];
    Add(pWrC, off, pHC);
    out->handErrMm = std::sqrt((pHC[0] - in.handPos[0]) * (pHC[0] - in.handPos[0]) +
                               (pHC[1] - in.handPos[1]) * (pHC[1] - in.handPos[1]) +
                               (pHC[2] - in.handPos[2]) * (pHC[2] - in.handPos[2]));
    double u1[3], u2[3];
    Sub(pS, pE, u1);
    Sub(pWr, pE, u2);
    Norm(u1);
    Norm(u2);
    double ce = Dot(u1, u2);
    if (ce > 1.0) ce = 1.0;
    if (ce < -1.0) ce = -1.0;
    out->elbowDeg = std::acos(ce) * 180.0 / kPi;

    ComposeM4(SR, sc[kS], pS, out->m[kS]);
    ComposeM4(ER, sc[kE], pE, out->m[kE]);
    ComposeM4(FR, sc[kFo], pFo, out->m[kFo]);
    ComposeM4(WR, sc[kWr], pWrC, out->m[kWr]);
    ComposeM4(HR, sc[kH], pHC, out->m[kH]);
    // El brazo y el antebrazo se estiran a lo largo de su hueso (la malla va
    // con ellos); el codo y la muneca no.
    StretchAlong(out->m[kS], tHat, kA);
    StretchAlong(out->m[kE], ctHat, kB);   // ronda 36: con el escudo, kB = 1
    StretchAlong(out->m[kFo], ctHat, kB);
    return true;
}

// --- ronda 23: la mano izquierda en el arma a dos manos -----------------------

// Ronda 25: un paso de v hacia target (como mucho step) que se queda en el
// objetivo al llegar. Los fundidos de la ronda 21 (peso a dos manos) y 23
// (pecho en los combos) hacian "si target > v, sube; si no, baja": al llegar
// a 1 bajaban un paso y al siguiente fotograma volvian a subir, alternando
// 1 / 0,93 en cada fotograma (la mano izquierda saltaba ~3 cm y el pecho ~3
// grados de un fotograma a otro: el temblor de las rondas 21-24).
inline double StepToward(double v, double target, double step) {
    if (step < 0.0) step = -step;
    if (v < target) return (v + step < target) ? v + step : target;
    if (v > target) return (v - step > target) ? v - step : target;
    return target;
}

// Factor de un paso de filtro paso bajo de primer orden (frecuencia de corte fc
// en Hz, paso dt en s).
inline double LowPassAlpha(double fc, double dt) {
    if (fc <= 0.0 || dt <= 0.0) return 0.0;
    double tau = 1.0 / (2.0 * kPi * fc);
    return 1.0 / (1.0 + tau / dt);
}

// Cuaternion q llevado al mismo hemisferio que ref (q y -q son la misma rotacion).
inline void QuatAlign(const double ref[4], double q[4]) {
    if (ref[0] * q[0] + ref[1] * q[1] + ref[2] * q[2] + ref[3] * q[3] < 0.0)
        for (int i = 0; i < 4; ++i) q[i] = -q[i];
}

inline void QuatNlerp(const double a[4], const double bIn[4], double t, double o[4]) {
    double b[4] = { bIn[0], bIn[1], bIn[2], bIn[3] };
    QuatAlign(a, b);
    double l = 0.0;
    for (int i = 0; i < 4; ++i) { o[i] = a[i] + (b[i] - a[i]) * t; l += o[i] * o[i]; }
    l = std::sqrt(l);
    if (l < 1e-12) { for (int i = 0; i < 4; ++i) o[i] = b[i]; return; }
    for (int i = 0; i < 4; ++i) o[i] /= l;
}

// Ronda 24: adelante horizontal de una cabeza (o camara) con su adelante y su
// abajo (mundo con y hacia abajo): al adelante se le suma el arriba de la
// cabeza en la medida en que mira hacia abajo (el abajo si mira arriba). Con
// la cabeza recta es la proyeccion de siempre; mirando las manos esta ya no
// vale (casi nada, y su direccion baila con cualquier giro de la cabeza).
inline bool HorizontalHeading(const double fwd[3], const double down[3], double out[3]) {
    out[0] = fwd[0] - fwd[1] * down[0];
    out[1] = 0.0;
    out[2] = fwd[2] - fwd[1] * down[2];
    return Norm(out, 1e-6);
}

} // namespace ik
} // namespace BladeVR
