#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>

using namespace arduino;

// ─────────────────────────────────────────────────────────────────────────────
// Dashboard.h  —  Posts events and heartbeats to the Voice Hub dashboard
//
// Usage:
//   Dashboard dash("your-app.vercel.app", 80);
//   dash.sendStatus();                           // on WiFi connect
//   dash.sendEvent("keyword", "lights on", 0.97); // on detection
//   dash.sendStatus();                           // in loop as heartbeat
// ─────────────────────────────────────────────────────────────────────────────

namespace VoiceHub {

class Dashboard {
public:
    Dashboard(const char* host, int port = 80)
        : _host(host), _port(port) {}

    // Send a heartbeat with current WiFi info — call on connect + every ~10s
    bool sendStatus() {
        StaticJsonDocument<128> doc;
        doc["ip"]   = WiFi.localIP().toString();
        doc["ssid"] = WiFi.SSID();

        char body[128];
        serializeJson(doc, body);
        return post("/api/status", body);
    }

    // Send a keyword detection event
    // type: "keyword", "status", or "error"
    bool sendEvent(const char* type, const char* label, float confidence = -1.0f) {
        StaticJsonDocument<192> doc;
        doc["type"]  = type;
        doc["label"] = label;
        doc["ip"]    = WiFi.localIP().toString();
        doc["ssid"]  = WiFi.SSID();
        if (confidence >= 0.0f) {
            doc["confidence"] = confidence;
        }

        char body[192];
        serializeJson(doc, body);
        return post("/api/event", body);
    }

private:
    const char* _host;
    int         _port;

    bool post(const char* path, const char* body) {
        if (WiFi.status() != WL_CONNECTED) return false;

        WiFiClient wifi;
        HttpClient client(static_cast<Client&>(wifi), _host, static_cast<uint16_t>(_port));

        client.beginRequest();
        int err = client.post(path);
        client.sendHeader("Content-Type", "application/json");
        client.sendHeader("Content-Length", strlen(body));
        client.beginBody();
        client.print(body);
        client.endRequest();

        if (err != 0) {
            Serial.print("[Dashboard] HTTP error: ");
            Serial.println(err);
            return false;
        }

        int status = client.responseStatusCode();
        client.stop();

        if (status < 200 || status >= 300) {
            Serial.print("[Dashboard] Bad response: ");
            Serial.println(status);
            return false;
        }

        return true;
    }
};

} // namespace VoiceHub
