#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include "../types.h"

using namespace arduino;  // WiFiClient/Client live in arduino:: on mbed core

namespace VoiceHub {

class WiFiManager {
public:
    WiFiManager(const char* ssid, const char* password)
        : _ssid(ssid), _password(password) {}

    bool connect() {
        return connect(_ssid, _password);
    }

    // onProgress(attempt, tick) called every 500ms tick during connection
    bool connect(const char* ssid, const char* password,
                 void (*onProgress)(int attempt, int tick) = nullptr) {
        Serial.print("[WiFi] Connecting to ");
        Serial.print(ssid);

        for (int attempt = 1; attempt <= 3; attempt++) {
            WiFi.disconnect();
            delay(200);
            WiFi.begin(ssid, password);

            int ticks = 0;
            while (WiFi.status() != WL_CONNECTED && ticks < 20) {
                delay(500);
                Serial.print(".");
                if (onProgress) onProgress(attempt, ticks);
                ticks++;
            }

            if (WiFi.status() == WL_CONNECTED) {
                Serial.print("\n[WiFi] Connected! IP: ");
                Serial.println(WiFi.localIP().toString());
                return true;
            }

            Serial.print("\n[WiFi] Attempt ");
            Serial.print(attempt);
            Serial.println(" failed, retrying...");
        }

        Serial.println("[WiFi] Connection FAILED after 3 attempts");
        return false;
    }

    bool isConnected() const {
        return WiFi.status() == WL_CONNECTED;
    }

    // Returns the SSID of the currently connected network (empty string if not connected)
    String connectedSSID() const {
        return isConnected() ? String(WiFi.SSID()) : String("");
    }

    // Stats for the stats screen
    String localIP()  const { return isConnected() ? WiFi.localIP().toString()  : "-"; }
    String gatewayIP()const { return isConnected() ? WiFi.gatewayIP().toString(): "-"; }
    String subnetMask()const{ return isConnected() ? WiFi.subnetMask().toString(): "-"; }
    int32_t rssi()    const { return isConnected() ? WiFi.RSSI() : 0; }
    String macAddress() const {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return String(buf);
    }

    void reconnectIfNeeded() {
        if (!isConnected()) {
            Serial.println("[WiFi] Reconnecting...");
            connect();
        }
    }

    bool trigger(const WebhookTarget& target) {
        if (!isConnected()) {
            Serial.println("[WiFi] Skipping trigger -- not connected");
            return false;
        }

        WiFiClient wifi;
        HttpClient client(static_cast<Client&>(wifi), target.host, static_cast<uint16_t>(target.port));
        client.setHttpResponseTimeout(3000);  // 3s max -- don't freeze the main loop

        Serial.print("[WiFi] Triggering: ");
        Serial.println(target.command);

        int err;
        if (strcmp(target.method, "POST") == 0) {
            client.beginRequest();
            err = client.post(target.path);
            if (err != 0) {
                Serial.print("[WiFi] Connect error: ");
                Serial.println(err);
                client.stop();
                return false;
            }
            client.sendHeader("Content-Type", "application/json");
            if (target.body) {
                client.sendHeader("Content-Length", strlen(target.body));
                client.beginBody();
                client.print(target.body);
            }
            client.endRequest();
        } else {
            err = client.get(target.path);
            if (err != 0) {
                Serial.print("[WiFi] Connect error: ");
                Serial.println(err);
                client.stop();
                return false;
            }
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
