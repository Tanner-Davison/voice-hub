#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include "../types.h"

namespace VoiceHub {

class WiFiManager {
public:
    WiFiManager(const char* ssid, const char* password)
        : _ssid(ssid), _password(password) {}

    bool connect() {
        Serial.print("[WiFi] Connecting to ");
        Serial.print(_ssid);

        WiFi.begin(_ssid, _password);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            Serial.print(".");
            attempts++;
        }

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("\n[WiFi] Connection FAILED");
            return false;
        }

        Serial.print("\n[WiFi] Connected! IP: ");
        Serial.println(WiFi.localIP());
        return true;
    }

    bool isConnected() const {
        return WiFi.status() == WL_CONNECTED;
    }

    void reconnectIfNeeded() {
        if (!isConnected()) {
            Serial.println("[WiFi] Reconnecting...");
            connect();
        }
    }

    bool trigger(const WebhookTarget& target) {
        reconnectIfNeeded();

        WiFiClient wifi;
        HttpClient client(wifi, target.host, target.port);

        Serial.print("[WiFi] Triggering: ");
        Serial.println(target.command);

        int err;
        if (strcmp(target.method, "POST") == 0) {
            client.beginRequest();
            err = client.post(target.path);
            client.sendHeader("Content-Type", "application/json");
            if (target.body) {
                client.sendHeader("Content-Length", strlen(target.body));
                client.beginBody();
                client.print(target.body);
            }
            client.endRequest();
        } else {
            err = client.get(target.path);
        }

        if (err != 0) {
            Serial.print("[WiFi] HTTP error: ");
            Serial.println(err);
            return false;
        }

        int statusCode = client.responseStatusCode();
        Serial.print("[WiFi] Response: ");
        Serial.println(statusCode);

        client.stop();
        return (statusCode >= 200 && statusCode < 300);
    }

private:
    const char* _ssid;
    const char* _password;
};

} // namespace VoiceHub
