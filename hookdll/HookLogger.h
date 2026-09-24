#pragma once
#include <string>
#include <fstream>
#include <mutex>

// Logger minimo para BladeVR.dll.
// Se ejecuta DENTRO del proceso de Blade.exe, asi que debe ser lo mas simple
// y robusto posible: un unico archivo por PID para no mezclar sesiones, y
// sin lanzar excepciones.
//
// APAGADO por defecto: solo escribe si existe un fichero llamado
// BladeVR_debug.txt junto a Blade.exe (su contenido da igual). Asi el mod no
// deja ficheros en la carpeta del juego salvo que se quiera depurar.

namespace BladeVR {

// Version del mod, en la cabecera de cada log.
constexpr const char* kBladeVRVersion = "1.2";

class HookLogger {
public:
    static HookLogger& Instance();

    // Si existe BladeVR_debug.txt junto a Blade.exe, crea
    // BladeVR_logs/BladeVR_<pid>_<timestamp>.log en esa misma carpeta (o en
    // %TEMP% si no puede escribir ahi). Si no existe, no hace nada y Line()
    // se convierte en una operacion nula.
    void Init(unsigned long pid);

    void Line(const std::string& text);

private:
    HookLogger() = default;
    std::ofstream m_file;
    std::mutex m_mutex;
    bool m_initialized = false;
};

} // namespace BladeVR
