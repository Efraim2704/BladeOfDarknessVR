#include "BladeVRIni.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace BladeVR {

namespace {

struct IniKey {
    const char* key;
    const char* def;       // nullptr: la linea solo se escribe si hay valor
    const char* comment;
};

// Ronda 55: la calibracion de las manos (las cuatro claves comparten comentario).
const char* const kGripComment =
    "; Agarre calibrado con Ctrl+F8 (brazos estirados al frente, pulgares arriba): ejes de cada\n"
    "; mando con el brazo colgando (9 numeros) y alcance del brazo en metros. Sin estas lineas,\n"
    "; el agarre de serie (el de un mando de Quest cogido con naturalidad).";

// Las claves conocidas, en el orden en que se escriben, con su comentario.
const IniKey kKnown[] = {
    { "modo_combate", "1",
      "; Modo de combate al arrancar (el ultimo elegido): 0 VR basico, 1 mandos de movimiento,\n"
      "; 2 mandos + combos del juego, 3 manos flotantes (sin cuerpo)." },
    { "diorama", "0",
      "; 1 = al empezar a jugar se entra en el modo diorama (estaba activado al salir)." },
    { "altura_ojos_cm", "0",
      "; Altura anadida a los ojos en el modo de manos flotantes, en cm (Mayus + Re Pag / Av Pag)." },
    { "resistencia", "1",
      "; 1 = los tajos y punetazos con los mandos gastan la resistencia como el ataque del juego\n"
      "; (0 = no la gastan). Mayus + F8 lo cambia." },
    { "agarre_izquierda", nullptr, kGripComment },
    { "agarre_derecha", nullptr, kGripComment },
    { "alcance_izquierda", nullptr, kGripComment },
    { "alcance_derecha", nullptr, kGripComment },
};

struct Entry {
    std::string key;
    std::string value;
};

std::mutex g_iniMutex;
bool g_loaded = false;
std::vector<Entry> g_entries;   // lo leido (y lo cambiado) del fichero

std::string IniPath() {
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p.resize(slash + 1);
    else p.clear();
    return p + "BladeVR.ini";
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

Entry* Find(const char* key) {
    for (Entry& e : g_entries)
        if (_stricmp(e.key.c_str(), key) == 0) return &e;
    return nullptr;
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    FILE* f = nullptr;
    if (fopen_s(&f, IniPath().c_str(), "r") != 0 || !f) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = Trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#' || s[0] == '[') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(s.substr(0, eq)), v = Trim(s.substr(eq + 1));
        if (k.empty() || v.empty()) continue;
        Entry* e = Find(k.c_str());
        if (e) e->value = v;
        else g_entries.push_back({ k, v });
    }
    std::fclose(f);
}

bool SaveLocked() {
    FILE* f = nullptr;
    if (fopen_s(&f, IniPath().c_str(), "w") != 0 || !f) return false;
    bool ok = std::fprintf(f, "; BladeVR: ajustes que se recuerdan entre partidas.\n[BladeVR]\n") > 0;
    const char* lastComment = nullptr;
    for (const IniKey& k : kKnown) {
        const Entry* e = Find(k.key);
        if (!e && !k.def) continue;
        if (k.comment && k.comment != lastComment) ok = std::fprintf(f, "%s\n", k.comment) > 0 && ok;
        lastComment = k.comment;
        ok = std::fprintf(f, "%s=%s\n", k.key, e ? e->value.c_str() : k.def) > 0 && ok;
    }
    for (const Entry& e : g_entries) {
        bool known = false;
        for (const IniKey& k : kKnown)
            if (_stricmp(e.key.c_str(), k.key) == 0) known = true;
        if (!known) ok = std::fprintf(f, "%s=%s\n", e.key.c_str(), e.value.c_str()) > 0 && ok;
    }
    std::fclose(f);
    return ok;
}

} // namespace

bool IniGetString(const char* key, std::string* out) {
    std::lock_guard<std::mutex> lock(g_iniMutex);
    LoadLocked();
    const Entry* e = Find(key);
    if (!e) return false;
    if (out) *out = e->value;
    return true;
}

bool IniSetString(const char* key, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_iniMutex);
    LoadLocked();
    Entry* e = Find(key);
    if (e) {
        if (e->value == value) return true;
        e->value = value;
    } else {
        g_entries.push_back({ key, value });
    }
    return SaveLocked();
}

int IniGetInt(const char* key, int def) {
    std::string v;
    if (!IniGetString(key, &v)) return def;
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    return end == v.c_str() ? def : static_cast<int>(n);
}

bool IniSetInt(const char* key, int value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", value);
    return IniSetString(key, buf);
}

} // namespace BladeVR
