#include "FromWorldLocator.h"
#include "HookLogger.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace BladeVR {

// ---------------------------------------------------------------------
// Localizador de la matriz de vista global fromWorld (4x4 doubles, junto
// al objeto de aplicacion apuntado por Blade.exe+0xF4A6A8).
// ---------------------------------------------------------------------
static constexpr uintptr_t kAppPointerOffset = 0xF4A6A8;
static double* g_fromWorldPtr = nullptr;

static bool SafeReadUPtr(uintptr_t addr, uintptr_t* outValue) {
    __try {
        *outValue = *reinterpret_cast<uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCheckMatrixAt(uintptr_t addr, double outTranslation[3]) {
    __try {
        const double* m = reinterpret_cast<const double*>(addr);
        double m03 = m[3], m13 = m[7], m23 = m[11], m33 = m[15];
        if (!std::isfinite(m33) || std::abs(m33 - 1.0) > 0.0001) return false;
        if (!std::isfinite(m03) || !std::isfinite(m13) || !std::isfinite(m23)) return false;
        if (std::abs(m03) > 0.0001 || std::abs(m13) > 0.0001 || std::abs(m23) > 0.0001) return false;

        double row0[3] = { m[0], m[1], m[2] };
        double row1[3] = { m[4], m[5], m[6] };
        double row2[3] = { m[8], m[9], m[10] };
        if (!std::isfinite(row0[0]) || !std::isfinite(row1[0]) || !std::isfinite(row2[0])) return false;

        // Sin lambdas ni objetos con destructor aqui a proposito: __try no
        // puede convivir con ellos en la misma funcion (error C2712).
        double mag0 = std::sqrt(row0[0]*row0[0] + row0[1]*row0[1] + row0[2]*row0[2]);
        double mag1 = std::sqrt(row1[0]*row1[0] + row1[1]*row1[1] + row1[2]*row1[2]);
        double mag2 = std::sqrt(row2[0]*row2[0] + row2[1]*row2[1] + row2[2]*row2[2]);
        if (mag0 < 0.5 || mag0 > 1.5 || mag1 < 0.5 || mag1 > 1.5 || mag2 < 0.5 || mag2 > 1.5) return false;

        double dot01 = row0[0]*row1[0] + row0[1]*row1[1] + row0[2]*row1[2];
        double dot02 = row0[0]*row2[0] + row0[1]*row2[1] + row0[2]*row2[2];
        double dot12 = row1[0]*row2[0] + row1[1]*row2[1] + row1[2]*row2[2];

        double devMag = std::abs(mag0 - 1.0);
        if (std::abs(mag1 - 1.0) > devMag) devMag = std::abs(mag1 - 1.0);
        if (std::abs(mag2 - 1.0) > devMag) devMag = std::abs(mag2 - 1.0);

        double devDot = std::abs(dot01);
        if (std::abs(dot02) > devDot) devDot = std::abs(dot02);
        if (std::abs(dot12) > devDot) devDot = std::abs(dot12);

        double devMax = devMag > devDot ? devMag : devDot;
        if (devMax > 0.01) return false;

        outTranslation[0] = m[12]; outTranslation[1] = m[13]; outTranslation[2] = m[14];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool TryResolveFromWorldInProcess() {
    HMODULE hMain = GetModuleHandleA(nullptr);
    if (!hMain) return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(hMain);

    uintptr_t ptr1 = 0;
    if (!SafeReadUPtr(base + kAppPointerOffset, &ptr1) || ptr1 == 0) return false;

    const intptr_t kWindow = 0x2000;
    uintptr_t regionStart = ptr1 - kWindow;
    uintptr_t regionEnd = ptr1 + kWindow;

    for (uintptr_t addr = regionStart; addr + 256 <= regionEnd; addr += 8) {
        double transA[3], transB[3];
        if (!SafeCheckMatrixAt(addr, transA)) continue;
        if (!SafeCheckMatrixAt(addr + 128, transB)) continue;
        g_fromWorldPtr = reinterpret_cast<double*>(addr);
        HookLogger::Instance().Line("[RENDEROBJ] fromWorld resuelta en proceso, direccion=0x" +
            [&]{ std::ostringstream o; o << std::hex << addr; return o.str(); }());
        return true;
    }
    return false;
}

double* TryGetOrResolveFromWorldPointer() {
    if (!g_fromWorldPtr) {
        TryResolveFromWorldInProcess();
    }
    return g_fromWorldPtr;
}

} // namespace BladeVR
