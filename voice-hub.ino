// ─────────────────────────────────────────────────────────────────────────────
// voice_hub.ino  —  Main sketch
//
// Board:   Arduino Giga R1 WiFi
// Target:  M7 core (primary)
//
// Dependencies (install via Arduino Library Manager):
//   - ArduinoHttpClient
//   - ArduinoJson
//   - Arduino_AdvancedAnalog  (for I2S on Giga R1)
//   - Arduino_GigaDisplay_GFX
//   - Arduino_GigaDisplayTouch
//   - <your-project>_inferencing  (exported from Edge Impulse)
//
// Setup order:
//   1. Attach u.FL antenna to the board
//   2. Plug GIGA Display Shield onto the board
//   3. Wire INMP441 microphone (see I2SMicrophone.h for pinout) — OR use shield mic
//   4. Fill in WiFi credentials and dashboard URL in config.h
//   5. Train keyword model on Edge Impulse, export as Arduino library
//   6. Replace the placeholder include in ClassifierBridge.h
//   7. Upload to Giga R1 M7 core
// ─────────────────────────────────────────────────────────────────────────────

#include "src/audio/I2SMicrophone.h"
#include "src/classifier/ClassifierBridge.h"
#include "src/config.h"
#include "src/display/DisplayManager.h"
#include "src/wifi/Dashboard.h"
#include "src/wifi/WiFiManager.h"

using namespace VoiceHub;

// ── Globals ───────────────────────────────────────────────────────────────────
I2SMicrophone    mic;
WiFiManager      wifi(Config::WIFI_SSID, Config::WIFI_PASSWORD);
Dashboard        dashboard(Config::DASHBOARD_HOST, Config::DASHBOARD_PORT);
DisplayManager   display;
ClassifierBridge classifier;
bool             micReady = false;

// Audio capture buffer — sized to what Edge Impulse expects (1s @ 16kHz)
int16_t audioBuffer[CAPTURE_SAMPLES];

// Debounce tracking
unsigned long lastTriggerTime  = 0;
const char*   lastTriggerLabel = nullptr;

// Heartbeat tracking
unsigned long lastHeartbeat = 0;

// ── Forward declarations ───────────────────────────────────────────────────────
void                 handleDetection(const ClassifierResult& result);
const WebhookTarget* findWebhook(const char* label);

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(3000);
    while (!Serial && millis() < 5000)
        ;

    Serial.println("=== Voice Hub — Arduino Giga R1 WiFi ===");
    Serial.println("[WiFi] Antenna ready");

    if (!mic.begin()) {
        Serial.println("[WARN] Microphone init failed — running without mic.");
    } else {
        micReady = true;
    }

    if (wifi.connect()) {
        Serial.println("[Dashboard] Sending connect status...");
        dashboard.sendEvent("status", "connected");
        dashboard.sendStatus();
        lastHeartbeat = millis();
    } else {
        Serial.println("[WARN] WiFi not connected.");
    }

    // Init display last so we enter loop() quickly
    display.begin();
    display.setWiFiStatus(wifi.isConnected());
    display.showStatus("Listening...");

    Serial.println("[READY] Listening for keywords...");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    // 1. Keep display responsive
    display.update();

    // 2. Heartbeat — keep the dashboard showing us as Online
    if (wifi.isConnected() && (millis() - lastHeartbeat >= Config::HEARTBEAT_MS)) {
        dashboard.sendStatus();
        lastHeartbeat = millis();
    }

    // 3. Capture 1 second of audio into buffer
    if (!micReady) { delay(10); return; }
    mic.capture(audioBuffer, CAPTURE_SAMPLES);

    // 4. Run ML inference
    ClassifierResult result = classifier.classify(audioBuffer, CAPTURE_SAMPLES);

    // 5. Handle valid detections
    if (result.valid) {
        handleDetection(result);
    }

    delay(10);
}

// ─────────────────────────────────────────────────────────────────────────────
void handleDetection(const ClassifierResult& result) {
    unsigned long now = millis();

    bool sameLabel = (lastTriggerLabel != nullptr && strcmp(result.label, lastTriggerLabel) == 0);

    if (sameLabel && (now - lastTriggerTime) < Config::DEBOUNCE_MS) {
        Serial.println("[DEBOUNCE] Skipping repeated command");
        return;
    }

    lastTriggerTime  = now;
    lastTriggerLabel = result.label;

    Serial.print("[DETECT] ");
    Serial.print(result.label);
    Serial.print(" (");
    Serial.print(result.confidence * 100.0f, 1);
    Serial.println("%)");

    // Update display
    display.showDetection(result.label, result.confidence);

    // Post to dashboard
    dashboard.sendEvent("keyword", result.label, result.confidence);

    // Find and fire matching webhook
    const WebhookTarget* target = findWebhook(result.label);

    if (target == nullptr) {
        Serial.print("[WARN] No webhook configured for: ");
        Serial.println(result.label);
        return;
    }

    bool ok = wifi.trigger(*target);

    if (ok) {
        Serial.print("[OK] Webhook fired for: ");
        Serial.println(result.label);
    } else {
        Serial.print("[FAIL] Webhook failed for: ");
        Serial.println(result.label);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
const WebhookTarget* findWebhook(const char* label) {
    for (size_t i = 0; i < Config::WEBHOOK_COUNT; i++) {
        if (strcmp(Config::WEBHOOKS[i].command, label) == 0) {
            return &Config::WEBHOOKS[i];
        }
    }
    return nullptr;
}
