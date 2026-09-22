#include "ProjectionHook.h"
#include "HookLogger.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace BladeVR {

// Indices de la vtable de ID3D11DeviceContext (layout publico del SDK).
namespace ContextVTable {
    constexpr int VSSetConstantBuffers = 7;
    constexpr int Map = 14;
    constexpr int Unmap = 15;
    constexpr int UpdateSubresource = 48;
}

using PFN_VSSetConstantBuffers = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using PFN_Map = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_UpdateSubresource = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);

static PFN_VSSetConstantBuffers g_originalVSSetConstantBuffers = nullptr;
static PFN_Map g_originalMap = nullptr;
static PFN_Unmap g_originalUnmap = nullptr;
static PFN_UpdateSubresource g_originalUpdateSubresource = nullptr;
static bool g_installed = false;

// Solo interesan constant buffers pequenos (una o dos matrices y poco mas).
static constexpr UINT kMaxTrackedBufferBytes = 512;
static constexpr size_t kMaxTrackedBuffers = 4096;

// Ultima subida conocida de cada constant buffer: si contiene un bloque de
// 64 bytes que parece una matriz de perspectiva, se guarda ese bloque.
struct BufferRecord {
    bool hasPerspective = false;
    float matrix[16] = {};
};
static std::mutex g_recordsMutex;
static std::unordered_map<uintptr_t, BufferRecord> g_records;

// Map en curso por recurso (memoria mapeada y tamano), hasta su Unmap.
struct PendingMap { void* data; UINT bytes; };
static std::unordered_map<uintptr_t, PendingMap> g_pendingMaps;

static std::atomic<uintptr_t> g_boundVSSlot0{0};


// Perspectiva pura con la convencion del juego (vector fila): m[11] = 1
// (w = z), m[15] = 0, escala en m[0] y m[5], near/far en m[10] y m[14], y el
// resto ceros. m[5] es negativo en este juego (Y de pantalla hacia abajo).
static bool IsPerspectiveMatrix(const float* m) {
    if (m[11] != 1.0f || m[15] != 0.0f) return false;
    if (m[0] <= 0.0f || m[5] == 0.0f) return false;
    if (m[1] != 0.0f || m[2] != 0.0f || m[3] != 0.0f) return false;
    if (m[4] != 0.0f || m[6] != 0.0f || m[7] != 0.0f) return false;
    if (m[8] != 0.0f || m[9] != 0.0f) return false;
    return true;
}

// true si 'buffer' es un constant buffer de tamano razonable; devuelve su tamano.
static bool IsTrackableConstantBuffer(ID3D11Resource* resource, UINT* outBytes) {
    if (!resource) return false;
    ID3D11Buffer* buffer = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || !buffer) {
        return false;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    buffer->Release();
    if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) return false;
    if (desc.ByteWidth < 64 || desc.ByteWidth > kMaxTrackedBufferBytes) return false;
    *outBytes = desc.ByteWidth;
    return true;
}

// Registra el contenido recien subido a un constant buffer: busca, en
// bloques alineados a 16 bytes, la primera matriz de perspectiva.
static void RecordUpload(ID3D11Resource* resource, const uint8_t* data, size_t len) {
    BufferRecord rec;
    for (size_t off = 0; off + 64 <= len; off += 16) {
        float m[16];
        memcpy(m, data + off, 64);
        if (IsPerspectiveMatrix(m)) {
            rec.hasPerspective = true;
            memcpy(rec.matrix, m, 64);
            break;
        }
    }
    uintptr_t key = reinterpret_cast<uintptr_t>(resource);
    std::lock_guard<std::mutex> lock(g_recordsMutex);
    if (g_records.size() >= kMaxTrackedBuffers && g_records.find(key) == g_records.end()) return;
    g_records[key] = rec;
}

static void STDMETHODCALLTYPE HookedVSSetConstantBuffers(
    ID3D11DeviceContext* self, UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) {
    if (ppConstantBuffers && StartSlot == 0 && NumBuffers > 0) {
        g_boundVSSlot0.store(reinterpret_cast<uintptr_t>(ppConstantBuffers[0]), std::memory_order_relaxed);
    }
    g_originalVSSetConstantBuffers(self, StartSlot, NumBuffers, ppConstantBuffers);
}

static HRESULT STDMETHODCALLTYPE HookedMap(
    ID3D11DeviceContext* self, ID3D11Resource* pResource, UINT Subresource,
    D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) {
    HRESULT hr = g_originalMap(self, pResource, Subresource, MapType, MapFlags, pMappedResource);
    if (SUCCEEDED(hr) && pResource && pMappedResource && pMappedResource->pData &&
        (MapType == D3D11_MAP_WRITE || MapType == D3D11_MAP_WRITE_DISCARD || MapType == D3D11_MAP_WRITE_NO_OVERWRITE)) {
        UINT bytes = 0;
        if (IsTrackableConstantBuffer(pResource, &bytes)) {
            std::lock_guard<std::mutex> lock(g_recordsMutex);
            g_pendingMaps[reinterpret_cast<uintptr_t>(pResource)] = PendingMap{pMappedResource->pData, bytes};
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE HookedUnmap(ID3D11DeviceContext* self, ID3D11Resource* pResource, UINT Subresource) {
    if (pResource) {
        PendingMap pending{nullptr, 0};
        {
            std::lock_guard<std::mutex> lock(g_recordsMutex);
            auto it = g_pendingMaps.find(reinterpret_cast<uintptr_t>(pResource));
            if (it != g_pendingMaps.end()) {
                pending = it->second;
                g_pendingMaps.erase(it);
            }
        }
        if (pending.data) {
            // El juego ya ha terminado de escribir: se lee ANTES del Unmap
            // real, mientras la memoria mapeada sigue siendo valida.
            RecordUpload(pResource, static_cast<const uint8_t*>(pending.data), pending.bytes);
        }
    }
    g_originalUnmap(self, pResource, Subresource);
}

static void STDMETHODCALLTYPE HookedUpdateSubresource(
    ID3D11DeviceContext* self, ID3D11Resource* pDstResource, UINT DstSubresource,
    const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) {
    UINT bytes = 0;
    if (pDstResource && pSrcData && (!pDstBox || pDstBox->left == 0) && IsTrackableConstantBuffer(pDstResource, &bytes)) {
        size_t len = bytes;
        if (pDstBox && pDstBox->right > pDstBox->left && (pDstBox->right - pDstBox->left) < len) {
            len = pDstBox->right - pDstBox->left;
        }
        RecordUpload(pDstResource, static_cast<const uint8_t*>(pSrcData), len);
    }
    g_originalUpdateSubresource(self, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

bool InstallProjectionHook(ID3D11DeviceContext* context) {
    if (g_installed || !context) return g_installed;
    void** vtable = *reinterpret_cast<void***>(context);
    void* vsSetAddr = vtable[ContextVTable::VSSetConstantBuffers];
    void* mapAddr = vtable[ContextVTable::Map];
    void* unmapAddr = vtable[ContextVTable::Unmap];
    void* updateAddr = vtable[ContextVTable::UpdateSubresource];

    bool ok = true;
    ok &= (MH_CreateHook(vsSetAddr, &HookedVSSetConstantBuffers, reinterpret_cast<void**>(&g_originalVSSetConstantBuffers)) == MH_OK);
    ok &= (MH_CreateHook(mapAddr, &HookedMap, reinterpret_cast<void**>(&g_originalMap)) == MH_OK);
    ok &= (MH_CreateHook(unmapAddr, &HookedUnmap, reinterpret_cast<void**>(&g_originalUnmap)) == MH_OK);
    ok &= (MH_CreateHook(updateAddr, &HookedUpdateSubresource, reinterpret_cast<void**>(&g_originalUpdateSubresource)) == MH_OK);
    ok &= (MH_EnableHook(vsSetAddr) == MH_OK);
    ok &= (MH_EnableHook(mapAddr) == MH_OK);
    ok &= (MH_EnableHook(unmapAddr) == MH_OK);
    ok &= (MH_EnableHook(updateAddr) == MH_OK);

    g_installed = ok;
    HookLogger::Instance().Line(ok ? "[PROJECTION] Hooks de VSSetConstantBuffers/Map/Unmap/UpdateSubresource instalados."
                                   : "[PROJECTION] ERROR: fallo al instalar uno o mas hooks del contexto.");
    return ok;
}

void UninstallProjectionHook() {
    // Los hooks se deshabilitan en bloque (MH_DisableHook(MH_ALL_HOOKS)) en
    // UninstallDx11Hook; aqui solo se marca el estado.
    g_installed = false;
}

bool ProjectionGetBoundVSSlot0Matrix(float out[16]) {
    uintptr_t buf = g_boundVSSlot0.load(std::memory_order_relaxed);
    if (!buf) return false;
    std::lock_guard<std::mutex> lock(g_recordsMutex);
    auto it = g_records.find(buf);
    if (it == g_records.end() || !it->second.hasPerspective) return false;
    memcpy(out, it->second.matrix, 64);
    return true;
}

ID3D11Buffer* ProjectionGetBoundVSSlot0Buffer() {
    return reinterpret_cast<ID3D11Buffer*>(g_boundVSSlot0.load(std::memory_order_relaxed));
}

void ProjectionBindVSSlot0Raw(ID3D11DeviceContext* ctx, ID3D11Buffer* buf) {
    if (!ctx || !g_originalVSSetConstantBuffers) return;
    ID3D11Buffer* one[1] = { buf };
    g_originalVSSetConstantBuffers(ctx, 0, 1, one);
}

} // namespace BladeVR
