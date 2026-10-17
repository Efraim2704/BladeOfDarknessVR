#include "VrInput.h"
#include "HookLogger.h"
#include "OpenVRHook.h"
#include "openvr.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace BladeVR {

namespace {

// --- ficheros que se dan a SteamVR (se escriben junto a la DLL) -------------
const char* const kActionsJson = R"JSON({
  "default_bindings": [
    { "controller_type": "oculus_touch", "binding_url": "bindings_oculus_touch.json" },
    { "controller_type": "knuckles", "binding_url": "bindings_knuckles.json" }
  ],
  "actions": [
    { "name": "/actions/blade/in/left_stick", "type": "vector2" },
    { "name": "/actions/blade/in/right_stick", "type": "vector2" },
    { "name": "/actions/blade/in/left_stick_click", "type": "boolean" },
    { "name": "/actions/blade/in/right_stick_click", "type": "boolean" },
    { "name": "/actions/blade/in/left_trigger", "type": "vector1" },
    { "name": "/actions/blade/in/right_trigger", "type": "vector1" },
    { "name": "/actions/blade/in/left_grip", "type": "boolean" },
    { "name": "/actions/blade/in/right_grip", "type": "boolean" },
    { "name": "/actions/blade/in/button_x", "type": "boolean" },
    { "name": "/actions/blade/in/button_y", "type": "boolean" },
    { "name": "/actions/blade/in/button_a", "type": "boolean" },
    { "name": "/actions/blade/in/button_b", "type": "boolean" },
    { "name": "/actions/blade/in/left_thumbrest", "type": "boolean" },
    { "name": "/actions/blade/in/right_thumbrest", "type": "boolean" },
    { "name": "/actions/blade/in/left_stick_touch", "type": "boolean" },
    { "name": "/actions/blade/in/right_stick_touch", "type": "boolean" },
    { "name": "/actions/blade/out/haptic_left", "type": "vibration" },
    { "name": "/actions/blade/out/haptic_right", "type": "vibration" }
  ],
  "action_sets": [
    { "name": "/actions/blade", "usage": "single" }
  ],
  "localization": [
    {
      "language_tag": "en_US",
      "/actions/blade": "Blade of Darkness VR",
      "/actions/blade/in/left_stick": "Left stick (move)",
      "/actions/blade/in/right_stick": "Right stick (camera; D-pad with the left thumb resting)",
      "/actions/blade/in/left_stick_click": "Left stick click (sprint)",
      "/actions/blade/in/right_stick_click": "Right stick click (recenter; diorama with the left thumb resting)",
      "/actions/blade/in/left_trigger": "Left trigger",
      "/actions/blade/in/right_trigger": "Right trigger",
      "/actions/blade/in/left_grip": "Left grip (block)",
      "/actions/blade/in/right_grip": "Right grip (throw)",
      "/actions/blade/in/button_x": "X (use)",
      "/actions/blade/in/button_y": "Y (turn around)",
      "/actions/blade/in/button_a": "A (attack; Start with the left thumb resting)",
      "/actions/blade/in/button_b": "B (jump; Back with the left thumb resting)",
      "/actions/blade/in/left_thumbrest": "Left thumb resting (modifier)",
      "/actions/blade/in/right_thumbrest": "Right thumb resting",
      "/actions/blade/in/left_stick_touch": "Left thumb on the stick (ignores stick drift)",
      "/actions/blade/in/right_stick_touch": "Right thumb on the stick",
      "/actions/blade/out/haptic_left": "Left haptics",
      "/actions/blade/out/haptic_right": "Right haptics"
    },
    {
      "language_tag": "es_ES",
      "/actions/blade": "Blade of Darkness VR",
      "/actions/blade/in/left_stick": "Joystick izquierdo (moverse)",
      "/actions/blade/in/right_stick": "Joystick derecho (camara; cruceta con el pulgar izquierdo en reposo)",
      "/actions/blade/in/left_stick_click": "Clic del joystick izquierdo (sprint)",
      "/actions/blade/in/right_stick_click": "Clic del joystick derecho (recentrar; diorama con el pulgar izquierdo en reposo)",
      "/actions/blade/in/left_trigger": "Gatillo izquierdo",
      "/actions/blade/in/right_trigger": "Gatillo derecho",
      "/actions/blade/in/left_grip": "Agarre izquierdo (bloquear)",
      "/actions/blade/in/right_grip": "Agarre derecho (lanzar)",
      "/actions/blade/in/button_x": "X (usar)",
      "/actions/blade/in/button_y": "Y (darse la vuelta)",
      "/actions/blade/in/button_a": "A (atacar; Start con el pulgar izquierdo en reposo)",
      "/actions/blade/in/button_b": "B (saltar; Back con el pulgar izquierdo en reposo)",
      "/actions/blade/in/left_thumbrest": "Pulgar izquierdo en reposo (modificador)",
      "/actions/blade/in/right_thumbrest": "Pulgar derecho en reposo",
      "/actions/blade/in/left_stick_touch": "Pulgar izquierdo sobre el joystick (ignora la deriva)",
      "/actions/blade/in/right_stick_touch": "Pulgar derecho sobre el joystick",
      "/actions/blade/out/haptic_left": "Vibracion izquierda",
      "/actions/blade/out/haptic_right": "Vibracion derecha"
    }
  ]
}
)JSON";

const char* const kTouchJson = R"JSON({
  "action_manifest_version": 0,
  "alias_info": {},
  "bindings": {
    "/actions/blade": {
      "haptics": [
        { "output": "/actions/blade/out/haptic_left", "path": "/user/hand/left/output/haptic" },
        { "output": "/actions/blade/out/haptic_right", "path": "/user/hand/right/output/haptic" }
      ],
      "poses": [],
      "skeleton": [],
      "sources": [
        { "path": "/user/hand/left/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/blade/in/left_stick" }, "click": { "output": "/actions/blade/in/left_stick_click" },
                      "touch": { "output": "/actions/blade/in/left_stick_touch" } } },
        { "path": "/user/hand/right/input/joystick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/blade/in/right_stick" }, "click": { "output": "/actions/blade/in/right_stick_click" },
                      "touch": { "output": "/actions/blade/in/right_stick_touch" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/blade/in/left_trigger" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/blade/in/right_trigger" } } },
        { "path": "/user/hand/left/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/left_grip" } } },
        { "path": "/user/hand/right/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/right_grip" } } },
        { "path": "/user/hand/left/input/x", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/button_x" } } },
        { "path": "/user/hand/left/input/y", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/button_y" } } },
        { "path": "/user/hand/right/input/a", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/button_a" } } },
        { "path": "/user/hand/right/input/b", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/button_b" } } },
        { "path": "/user/hand/left/input/thumbrest", "mode": "button",
          "inputs": { "touch": { "output": "/actions/blade/in/left_thumbrest" } } },
        { "path": "/user/hand/right/input/thumbrest", "mode": "button",
          "inputs": { "touch": { "output": "/actions/blade/in/right_thumbrest" } } }
      ]
    }
  },
  "controller_type": "oculus_touch",
  "description": "Blade of Darkness VR (BladeVR 2.0)",
  "name": "BladeVR",
  "options": { "returnBindingHints": false },
  "simulated_actions": []
}
)JSON";

const char* const kKnucklesJson = R"JSON({
  "action_manifest_version": 0,
  "alias_info": {},
  "bindings": {
    "/actions/blade": {
      "haptics": [
        { "output": "/actions/blade/out/haptic_left", "path": "/user/hand/left/output/haptic" },
        { "output": "/actions/blade/out/haptic_right", "path": "/user/hand/right/output/haptic" }
      ],
      "poses": [],
      "skeleton": [],
      "sources": [
        { "path": "/user/hand/left/input/thumbstick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/blade/in/left_stick" }, "click": { "output": "/actions/blade/in/left_stick_click" },
                      "touch": { "output": "/actions/blade/in/left_stick_touch" } } },
        { "path": "/user/hand/right/input/thumbstick", "mode": "joystick",
          "inputs": { "position": { "output": "/actions/blade/in/right_stick" }, "click": { "output": "/actions/blade/in/right_stick_click" },
                      "touch": { "output": "/actions/blade/in/right_stick_touch" } } },
        { "path": "/user/hand/left/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/blade/in/left_trigger" } } },
        { "path": "/user/hand/right/input/trigger", "mode": "trigger",
          "inputs": { "pull": { "output": "/actions/blade/in/right_trigger" } } },
        { "path": "/user/hand/left/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/left_grip" } } },
        { "path": "/user/hand/right/input/grip", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/right_grip" } } },
        { "path": "/user/hand/left/input/a", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/button_x" } } },
        { "path": "/user/hand/left/input/b", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/button_y" } } },
        { "path": "/user/hand/right/input/a", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/button_a" } } },
        { "path": "/user/hand/right/input/b", "mode": "button",
          "inputs": { "click": { "output": "/actions/blade/in/button_b" } } },
        { "path": "/user/hand/left/input/trackpad", "mode": "button",
          "inputs": { "touch": { "output": "/actions/blade/in/left_thumbrest" } } },
        { "path": "/user/hand/right/input/trackpad", "mode": "button",
          "inputs": { "touch": { "output": "/actions/blade/in/right_thumbrest" } } }
      ]
    }
  },
  "controller_type": "knuckles",
  "description": "Blade of Darkness VR (BladeVR 2.0)",
  "name": "BladeVR",
  "options": { "returnBindingHints": false },
  "simulated_actions": []
}
)JSON";

vr::IVRInput* g_input = nullptr;
bool g_ready = false;          // acciones listas
bool g_manifestSet = false;    // SteamVR ya tiene el manifiesto (sin entrada heredada)
bool g_failed = false;         // no se usa IVRInput en esta sesion
int g_attempts = 0;
unsigned long long g_nextTry = 0;

vr::VRActionSetHandle_t g_set = vr::k_ulInvalidActionSetHandle;
enum ActionId {
    A_LSTICK, A_RSTICK, A_LCLICK, A_RCLICK, A_LTRIG, A_RTRIG, A_LGRIP, A_RGRIP, A_X, A_Y, A_A, A_B,
    A_LREST, A_RREST, A_LTOUCH, A_RTOUCH, A_HAPL, A_HAPR, A_COUNT
};
const char* const kActionNames[A_COUNT] = {
    "/actions/blade/in/left_stick", "/actions/blade/in/right_stick", "/actions/blade/in/left_stick_click",
    "/actions/blade/in/right_stick_click", "/actions/blade/in/left_trigger", "/actions/blade/in/right_trigger",
    "/actions/blade/in/left_grip", "/actions/blade/in/right_grip", "/actions/blade/in/button_x",
    "/actions/blade/in/button_y", "/actions/blade/in/button_a", "/actions/blade/in/button_b",
    "/actions/blade/in/left_thumbrest", "/actions/blade/in/right_thumbrest", "/actions/blade/in/left_stick_touch",
    "/actions/blade/in/right_stick_touch", "/actions/blade/out/haptic_left",
    "/actions/blade/out/haptic_right"
};
vr::VRActionHandle_t g_action[A_COUNT];

void Log(const std::string& s) { HookLogger::Instance().Line(s); }

bool OwnDir(std::string* dir) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&OwnDir), &self) || !self) {
        return false;
    }
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::string d(path, n);
    size_t slash = d.find_last_of("\\/");
    if (slash == std::string::npos) return false;
    d.resize(slash + 1);
    *dir = d;
    return true;
}

// Escribe el fichero solo si no existe o es distinto.
bool WriteIfDifferent(const std::string& path, const char* content) {
    size_t len = std::strlen(content);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f) {
        std::string old;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) old.append(buf, n);
        std::fclose(f);
        if (old.size() == len && std::memcmp(old.data(), content, len) == 0) return true;
    }
    f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(content, 1, len, f) == len;
    std::fclose(f);
    return ok;
}

bool WriteBindings(std::string* manifest) {
    std::string dir;
    if (!OwnDir(&dir)) return false;
    dir += "BladeVR_bindings";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\";
    bool ok = WriteIfDifferent(dir + "actions.json", kActionsJson) &&
              WriteIfDifferent(dir + "bindings_oculus_touch.json", kTouchJson) &&
              WriteIfDifferent(dir + "bindings_knuckles.json", kKnucklesJson);
    *manifest = dir + "actions.json";
    return ok;
}

bool EnsureReady() {
    if (g_ready) return true;
    if (g_failed) return false;
    unsigned long long now = GetTickCount64();
    if (now < g_nextTry) return false;
    g_nextTry = now + 100;
    if (!g_input) {
        g_input = static_cast<vr::IVRInput*>(GetOpenVRInterface(vr::IVRInput_Version));
        if (!g_input) return false;   // OpenVR aun no esta listo
    }
    if (!g_manifestSet) {
        std::string manifest;
        if (!WriteBindings(&manifest)) {
            Log("[ENTRADA] No se pudo escribir BladeVR_bindings junto a BladeVR.dll: mandos por la entrada heredada "
                "(sin reposapulgares).");
            g_failed = true;
            return false;
        }
        vr::EVRInputError err = g_input->SetActionManifestPath(manifest.c_str());
        if (err != vr::VRInputError_None) {
            // Puede fallar mientras SteamVR termina de arrancar: se reintenta unos segundos.
            if (++g_attempts < 100) return false;
            Log("[ENTRADA] SetActionManifestPath fallo (" + std::to_string(static_cast<int>(err)) + ") con " + manifest +
                ": mandos por la entrada heredada.");
            g_failed = true;
            return false;
        }
        g_manifestSet = true;
        Log("[ENTRADA] Manifiesto de acciones dado a SteamVR: " + manifest);
    }
    if (g_input->GetActionSetHandle("/actions/blade", &g_set) != vr::VRInputError_None) {
        if (++g_attempts < 100) return false;
        Log("[ENTRADA] ERROR: no se pudo obtener el conjunto de acciones /actions/blade.");
        g_failed = true;
        return false;
    }
    for (int i = 0; i < A_COUNT; ++i) {
        if (g_input->GetActionHandle(kActionNames[i], &g_action[i]) != vr::VRInputError_None ||
            g_action[i] == vr::k_ulInvalidActionHandle) {
            Log(std::string("[ENTRADA] ERROR: falta la accion ") + kActionNames[i]);
            g_failed = true;
            return false;
        }
    }
    g_ready = true;
    Log("[ENTRADA] Mandos por las acciones de OpenVR (con reposapulgares).");
    return true;
}

bool Digital(int a, bool* active) {
    vr::InputDigitalActionData_t d{};
    if (g_input->GetDigitalActionData(g_action[a], &d, sizeof(d), vr::k_ulInvalidInputValueHandle) != vr::VRInputError_None) {
        return false;
    }
    if (d.bActive && active) *active = true;
    return d.bActive && d.bState;
}

// Como Digital, pero dice si la accion tiene enlace (bActive): con unos enlaces
// guardados de antes de la ronda 17 el toque del joystick no esta enlazado.
bool DigitalBound(int a, bool* state) {
    vr::InputDigitalActionData_t d{};
    *state = false;
    if (g_input->GetDigitalActionData(g_action[a], &d, sizeof(d), vr::k_ulInvalidInputValueHandle) != vr::VRInputError_None) {
        return false;
    }
    *state = d.bActive && d.bState;
    return d.bActive;
}

void Analog(int a, float* x, float* y, bool* active) {
    vr::InputAnalogActionData_t d{};
    *x = 0.0f;
    if (y) *y = 0.0f;
    if (g_input->GetAnalogActionData(g_action[a], &d, sizeof(d), vr::k_ulInvalidInputValueHandle) != vr::VRInputError_None) {
        return;
    }
    if (!d.bActive) return;
    if (active) *active = true;
    *x = d.x;
    if (y) *y = d.y;
}

} // namespace

bool VrInputRead(VrInputHands* out) {
    std::memset(out, 0, sizeof(*out));
    if (!EnsureReady()) return g_manifestSet;   // con el manifiesto puesto la entrada heredada ya no llega
    vr::VRActiveActionSet_t set{};
    set.ulActionSet = g_set;
    set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    set.nPriority = 0;
    if (g_input->UpdateActionState(&set, sizeof(set), 1) != vr::VRInputError_None) return true;
    for (int h = 0; h < 2; ++h) {
        bool* act = &out->active[h];
        Analog(h == 0 ? A_LSTICK : A_RSTICK, &out->stickX[h], &out->stickY[h], act);
        out->stickClick[h] = Digital(h == 0 ? A_LCLICK : A_RCLICK, act);
        Analog(h == 0 ? A_LTRIG : A_RTRIG, &out->trigger[h], nullptr, act);
        out->grip[h] = Digital(h == 0 ? A_LGRIP : A_RGRIP, act);
        out->buttonA[h] = Digital(h == 0 ? A_X : A_A, act);
        out->buttonB[h] = Digital(h == 0 ? A_Y : A_B, act);
        out->thumbrest[h] = Digital(h == 0 ? A_LREST : A_RREST, act);
        out->stickTouchBound[h] = DigitalBound(h == 0 ? A_LTOUCH : A_RTOUCH, &out->stickTouch[h]);
    }
    return true;
}

bool VrInputManifestActive() { return g_manifestSet; }

void VrInputHaptic(int hand, float seconds, float amplitude) {
    if (!g_ready || !g_input || hand < 0 || hand > 1) return;
    if (amplitude <= 0.0f) return;
    if (amplitude > 1.0f) amplitude = 1.0f;
    g_input->TriggerHapticVibrationAction(g_action[hand == 0 ? A_HAPL : A_HAPR], 0.0f, seconds, 160.0f, amplitude,
                                          vr::k_ulInvalidInputValueHandle);
}

} // namespace BladeVR
