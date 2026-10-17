#pragma once

namespace BladeVR {

// ---------------------------------------------------------------------
// Mandos de VR por la API de acciones de OpenVR (IVRInput), version 2.0.
//
// Hace falta para el reposapulgares (la entrada heredada no lo da): con el
// pulgar izquierdo en reposo, el joystick derecho es la cruceta y A / B son
// Start / Back (VrPadHook.cpp).
//
// La DLL escribe el manifiesto y los enlaces por defecto (Touch de Meta e
// Index) en BladeVR_bindings\ junto a ella y se los da a SteamVR. Con un
// manifiesto, SteamVR deja de dar la entrada heredada a la aplicacion, asi
// que OpenVRHook rellena entonces VrHandState desde aqui con el mismo
// significado de siempre (el diorama no nota la diferencia). Si algo falla,
// se sigue con la entrada heredada como hasta ahora.
//
// Todo en el hilo de Present (PumpOpenVRFrameTiming, con el compositor
// tomado).
// ---------------------------------------------------------------------

struct VrInputHands {
    bool active[2];        // el mando da senal (0 izquierdo, 1 derecho)
    float stickX[2];
    float stickY[2];
    bool stickClick[2];
    float trigger[2];      // 0..1
    bool grip[2];
    bool buttonA[2];       // A (derecho) / X (izquierdo)
    bool buttonB[2];       // B (derecho) / Y (izquierdo)
    bool thumbrest[2];     // pulgar en el reposapulgares
    bool stickTouch[2];    // pulgar sobre el joystick (ronda 17)
    bool stickTouchBound[2];  // el toque del joystick tiene enlace en SteamVR
};

// true si se leyeron las acciones (IVRInput en uso). false = usar la
// entrada heredada (OpenVR aun no listo, sin manifiesto o fallo).
bool VrInputRead(VrInputHands* out);

// true si el manifiesto ya esta puesto: la entrada heredada ya no llega.
bool VrInputManifestActive();

// Vibracion corta en un mando (0 izquierdo, 1 derecho). Hilo de Present.
void VrInputHaptic(int hand, float seconds, float amplitude);

} // namespace BladeVR
