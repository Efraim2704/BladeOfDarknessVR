#include "HookLogger.h"
#include <windows.h>
#include <sstream>
#include <iomanip>

namespace BladeVR {

static std::string Timestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << st.wYear << "-" << std::setw(2) << st.wMonth << "-" << std::setw(2) << st.wDay << " "
        << std::setw(2) << st.wHour << ":" << std::setw(2) << st.wMinute << ":" << std::setw(2) << st.wSecond
        << "." << std::setw(3) << st.wMilliseconds;
    return oss.str();
}

HookLogger& HookLogger::Instance() {
    static HookLogger instance;
    return instance;
}

void HookLogger::Init(unsigned long pid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return;

    // Escribimos junto al ejecutable del proceso objetivo (no dependemos del CWD,
    // que dentro del proceso inyectado puede ser cualquier cosa).
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    std::string dir(exePath);
    size_t slash = dir.find_last_of("\\/");
    dir = (slash != std::string::npos) ? dir.substr(0, slash) : ".";

    // Marcador de depuracion: sin el, no se escribe ningun log.
    std::string marker = dir + "\\BladeVR_debug.txt";
    if (GetFileAttributesA(marker.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    dir += "\\BladeVR_logs";

    if (!CreateDirectoryA(dir.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            // Fallback: %TEMP%
            char tempPath[MAX_PATH] = {0};
            GetTempPathA(MAX_PATH, tempPath);
            dir = std::string(tempPath) + "BladeVR_logs";
            CreateDirectoryA(dir.c_str(), nullptr);
        }
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::ostringstream name;
    name << dir << "\\BladeVR_" << pid << "_"
         << std::setfill('0')
         << std::setw(4) << st.wYear << std::setw(2) << st.wMonth << std::setw(2) << st.wDay << "_"
         << std::setw(2) << st.wHour << std::setw(2) << st.wMinute << std::setw(2) << st.wSecond << ".log";

    m_file.open(name.str(), std::ios::out | std::ios::app);
    m_initialized = m_file.is_open();

    if (m_initialized) {
        m_file << "==================================================\n";
        m_file << "BladeVR " << kBladeVRVersion << " - log (inyectado en PID " << pid << ")\n";
        m_file << "Iniciado: " << Timestamp() << "\n";
        m_file << "==================================================\n";
        m_file.flush();
    }
}

void HookLogger::Line(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return;
    m_file << Timestamp() << " " << text << "\n";
    m_file.flush();
}

} // namespace BladeVR
