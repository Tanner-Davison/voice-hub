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
//   - <your-project>_inferencing  (exported from Edge Impulse)
//
// Setup order:
//   1. Wire INMP441 microphone (see I2SMicrophone.h for pinout)
//   2. Fill in WiFi credentials and webhook URLs in config.h
//   3. Train keyword model on Edge Impulse, export as Arduino library
//   4. Replace the placeholder include in ClassifierBridge.h
//   5. Upload to Giga R1 M7 core
// ─────────────────────────────────────────────────────────────────────────────

#include "src/config.h"
#include "src/audio/I2SMicrophone.h"
#include "src/wifi/WiFiManager.h"
#include "src/classifier/ClassifierBridge.h"

using namespace VoiceHub;

// ── Globals ───────────────────────────────────────────────────────────────────
I2SMicrophone   mic;
WiFiManager     wifi(Config::WIFI_SSID, Config::WIFI_PASSWORD);
ClassifierBridge classifier;

// Audio capture buffer — sized to what Edge Impulse expects (1s @ 16kHz)
int16_t audioBuffer[CAPTURE_SAMPLES];

// Debounce tracking
unsigned long lastTriggerTime = 0;
const char*   lastTriggerLabel = nullptr;

// ── Forward declarations ───────────────────────────────────────────────────────
void handleDetection(const ClassifierResult& result);
const WebhookTarget* findWebhook(const char* label);

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);  // Wait up to 3s for serial monitor

    Serial.println("=== Voice Hub — Arduino Giga R1 WiFi ===");

    if (!mic.begin()) {
        Serial.println("[FATAL] Microphone init failed. Halting.");
        while (true);
    }

    if (!wifi.connect()) {
        // Non-fatal — will retry on first trigger attempt
        Serial.println("[WARN] WiFi not connected. Will retry on detection.");
    }

    Serial.println("[READY] Listening for keywords...");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    // 1. Capture 1 second of audio into buffer
    mic.capture(audioBuffer, CAPTURE_SAMPLES);

    // 2. Run ML inference
    ClassifierResult result = classifier.classify(audioBuffer, CAPTURE_SAMPLES);

    // 3. Handle valid detections
    if (result.valid) {
        handleDetection(result);
    }

    // Small yield to keep WiFi stack healthy
    delay(10);
}

// ─────────────────────────────────────────────────────────────────────────────
void handleDetection(const ClassifierResult& result) {
    unsigned long now = millis();

    // Debounce — ignore same command within DEBOUNCE_MS
    bool sameLabel = (lastTriggerLabel != nullptr &&
                      strcmp(result.label, lastTriggerLabel) == 0);

    if (sameLabel && (now - lastTriggerTime) < Config::DEBOUNCE_MS) {
        Serial.println("[DEBOUNCE] Skipping repeated command");
        return;
    }

    lastTriggerTime  = now;
    lastTriggerLabel = result.label;

    // Find matching webhook
    const WebhookTarget* target = findWebhook(result.label);

    if (target == nullptr) {
        Serial.print("[WARN] No webhook configured for: ");
        Serial.println(result.label);
        return;
    }

    // Fire it
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
