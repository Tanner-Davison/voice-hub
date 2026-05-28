#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <mbed.h>

using namespace arduino;

namespace VoiceHub {

// Dashboard -- all HTTP calls run on a background thread so M7 never blocks.
// If a previous send is still in flight, new calls are silently dropped.
// If the server fails, a 30s backoff prevents hammering a dead host.

class Dashboard {
public:
    Dashboard(const char* host, int port = 80)
        : _host(host), _port(port),
          _reachable(true), _lastFailMs(0), _thread(nullptr) {}

    bool sendStatus() {
        if (WiFi.status() != WL_CONNECTED) return false;
        StaticJsonDocument<128> doc;
        doc["ip"]   = WiFi.localIP().toString();
        doc["ssid"] = WiFi.SSID();
        char body[128];
        serializeJson(doc, body);
        return _post("/api/status", body);
    }

    bool sendEvent(const char* type, const char* label, float confidence = -1.0f) {
        if (WiFi.status() != WL_CONNECTED) return false;
        StaticJsonDocument<192> doc;
        doc["type"]  = type;
        doc["label"] = label;
        doc["ip"]    = WiFi.localIP().toString();
        doc["ssid"]  = WiFi.SSID();
        if (confidence >= 0.0f) doc["confidence"] = confidence;
        char body[192];
        serializeJson(doc, body);
        return _post("/api/event", body);
    }

private:
    const char*   _host;
    int           _port;
    bool          _reachable;
    unsigned long _lastFailMs;
    rtos::Thread* _thread;

    static constexpr unsigned long BACKOFF_MS = 30000;

    // Shared between caller and thread -- only one thread runs at a time
    static char        _tPath[32];
    static char        _tBody[256];
    static const char* _tHost;
    static int         _tPort;
    static volatile bool _tBusy;

    bool _post(const char* path, const char* body) {
        // Don't retry a recently-failed host
        if (!_reachable && (millis() - _lastFailMs) < BACKOFF_MS) return false;

        // Drop if thread still running
        if (_tBusy) return false;

        strncpy(_tPath, path, sizeof(_tPath) - 1);
        strncpy(_tBody, body, sizeof(_tBody) - 1);
        _tHost = _host;
        _tPort = _port;
        _tBusy = true;

        // Clean up previous thread
        if (_thread) { _thread->join(); delete _thread; }

        _thread = new rtos::Thread(osPriorityBelowNormal, 4096);
        _thread->start([]() {
            WiFiClient wc;
            HttpClient http(static_cast<Client&>(wc), _tHost, _tPort);
            http.setHttpResponseTimeout(2000);
            http.beginRequest();
            if (http.post(_tPath) == 0) {
                http.sendHeader("Content-Type", "application/json");
                http.sendHeader("Content-Length", strlen(_tBody));
                http.beginBody();
                http.print(_tBody);
                http.endRequest();
                http.responseStatusCode();
            }
            http.stop();
            _tBusy = false;
        });

        _reachable = true;
        return true;
    }
};

// Static member definitions
char        Dashboard::_tPath[32]  = {};
char        Dashboard::_tBody[256] = {};
const char* Dashboard::_tHost      = nullptr;
int         Dashboard::_tPort      = 0;
volatile bool Dashboard::_tBusy    = false;

} // namespace VoiceHub
