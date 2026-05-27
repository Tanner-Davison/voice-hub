#pragma once

#include "types.h"
#include <stddef.h> // size_t

// ─────────────────────────────────────────────────────────────────────────────
// config.h  —  All user-configurable settings in one place
// ─────────────────────────────────────────────────────────────────────────────

namespace VoiceHub::Config {

// ── WiFi ─────────────────────────────────────────────────────────────────────
constexpr const char* WIFI_SSID     = "DavisonFamily";
constexpr const char* WIFI_PASSWORD = "Luliann465$";

// ── Dashboard ─────────────────────────────────────────────────────────────────
// Local dev: Windows host IP (board can't reach WSL localhost directly)
// Production: swap for your Vercel URL e.g. "voice-hub-dashboard.vercel.app"
constexpr const char* DASHBOARD_HOST = "10.0.0.161";
constexpr int         DASHBOARD_PORT = 3000;

// How often to send a heartbeat to the dashboard (ms)
constexpr unsigned long HEARTBEAT_MS = 10000;

// ── Debounce ──────────────────────────────────────────────────────────────────
// Minimum ms between two triggers of the same command (prevents rapid re-fire)
constexpr unsigned long DEBOUNCE_MS = 2000;

// ── Webhook Targets ───────────────────────────────────────────────────────────
// Add one entry per keyword you trained in Edge Impulse.
// label must exactly match the Edge Impulse class label.
//
// Home Assistant: host = "homeassistant.local", port = 8123
// IFTTT:          host = "maker.ifttt.com",     path = "/trigger/<event>/with/key/<key>"
// ntfy.sh:        host = "ntfy.sh",             path = "/<topic>", body = nullptr

constexpr WebhookTarget WEBHOOKS[] = {
    {/* command */ "lights on",
     /* host    */ "homeassistant.local",
     /* port    */ 8123,
     /* path    */ "/api/webhook/lights_on",
     /* method  */ "POST",
     /* body    */ nullptr},
    {/* command */ "lights off",
     /* host    */ "homeassistant.local",
     /* port    */ 8123,
     /* path    */ "/api/webhook/lights_off",
     /* method  */ "POST",
     /* body    */ nullptr},
    {/* command */ "good night",
     /* host    */ "homeassistant.local",
     /* port    */ 8123,
     /* path    */ "/api/webhook/good_night",
     /* method  */ "POST",
     /* body    */ nullptr},
};

constexpr size_t WEBHOOK_COUNT = sizeof(WEBHOOKS) / sizeof(WEBHOOKS[0]);

} // namespace VoiceHub::Config
