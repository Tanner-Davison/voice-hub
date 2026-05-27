#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// types.h  —  Shared plain data types (no Arduino.h dependency)
// Keeping these here avoids circular includes between config.h and WiFiManager.h
// ─────────────────────────────────────────────────────────────────────────────

namespace VoiceHub {

struct WebhookTarget {
    const char* command;  // keyword label from Edge Impulse
    const char* host;     // e.g. "homeassistant.local"
    int         port;     // e.g. 8123
    const char* path;     // e.g. "/api/webhook/lights_on"
    const char* method;   // "POST" or "GET"
    const char* body;     // optional JSON body, nullptr for none
};

} // namespace VoiceHub
