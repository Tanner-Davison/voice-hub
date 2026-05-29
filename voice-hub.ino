// ─────────────────────────────────────────────────────────────────────────────
// voice_hub.ino  —  Main sketch
//
// Board:   Arduino Giga R1 WiFi
// Target:  M7 core (primary)
// ─────────────────────────────────────────────────────────────────────────────

// Save 10KB RAM for Edge Impulse filterbank
#define EIDSP_QUANTIZE_FILTERBANK 0

#include "src/audio/I2SMicrophone.h"
#include "src/classifier/ClassifierBridge.h"
#include "src/config.h"
#include "src/display/DisplayManager.h"
#include "src/ui/AskHub.h"
#include "src/ui/AskWindows.h"
#include "src/ui/WiFiSettings.h"
#include "src/wifi/Dashboard.h"
#include "src/wifi/WiFiManager.h"

using namespace VoiceHub;

// ── Globals ───────────────────────────────────────────────────────────────────
I2SMicrophone    mic;
WiFiManager      wifi(Config::WIFI_SSID, Config::WIFI_PASSWORD);
Dashboard        dashboard(Config::DASHBOARD_HOST, Config::DASHBOARD_PORT);
DisplayManager   display;
ClassifierBridge classifier;
WiFiSettings     wifiSettings;
AskHub           askHub;
AskWindows       askWin;
bool             micReady = false;

// Audio buffer — heap allocated in I2SMicrophone, captured into here
static int16_t audioBuffer[EI_CLASSIFIER_RAW_SAMPLE_COUNT];

// Debounce
unsigned long lastTriggerTime  = 0;
const char*   lastTriggerLabel = nullptr;

// Heartbeat
unsigned long lastHeartbeat = 0;

// ── Forward declarations ───────────────────────────────────────────────────────
void                 handleDetection(const char* label, float confidence);
const WebhookTarget* findWebhook(const char* label);

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(3000);
    while (!Serial && millis() < 5000);

    Serial.println("=== Voice Hub --- Arduino Giga R1 WiFi ===");

    // Init mic first so its buffer gets priority on the heap
    if (!mic.begin()) {
        Serial.println("[WARN] Microphone init failed.");
    } else {
        micReady = true;
    }

    // Grab AskHub buffer after mic -- still before Edge Impulse
    askHub.preallocate();
    // Share AskHub's buffer -- AskWindows never runs simultaneously
    askWin.preallocate(_ahRecBuf);

    // Try saved credentials first, fall back to compiled-in config
    static char savedSSID[33] = {0};
    static char savedPass[64] = {0};
    wifiSettings.loadSaved(savedSSID, savedPass);

    bool hasSaved = (savedSSID[0] != '\0');
    Serial.print("[WiFi] Using ");
    Serial.println(hasSaved ? "saved credentials" : "config credentials");

    bool connected = hasSaved
        ? wifi.connect(savedSSID, savedPass)
        : wifi.connect();

    if (connected) {
        Serial.println("[Dashboard] Sending connect status...");
        dashboard.sendEvent("status", "connected");
        dashboard.sendStatus();
        lastHeartbeat = millis();
    } else {
        Serial.println("[WARN] WiFi not connected.");
    }

    display.begin();
    display.setWiFiStatus(wifi.isConnected());
    display.setMicStatus(micReady);
    display.showStatus("Listening...");

    Serial.println("[READY] Listening for keywords...");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    // AskHub takes full control while recording or sending
    if (askWin.isActive()) {
        const char* status = askWin.update();
        if (status) {
            display.redraw();
            display.forceWiFiStatus(wifi.isConnected());
            display.setMicStatus(micReady);
            display.showStatus(status);
        }
        return;
    }

    if (askHub.isActive()) {
        const char* status = askHub.update();
        if (status) {
            // AskHub just finished -- repaint and log the result
            display.redraw();
            display.forceWiFiStatus(wifi.isConnected());
            display.setMicStatus(micReady);
            display.showStatus(status);
        }
        return;
    }

    if (wifiSettings.isActive()) {
        static char newSSID[33] = {0};
        static char newPass[64] = {0};
        wifiSettings.update(newSSID, newPass);  // drives the UI; result handled via needsRedraw
        return;
    }

    // WiFiSettings closed (back, Done, or successful connect) -- repaint main screen
    if (wifiSettings.needsRedraw()) {
        wifiSettings.clearRedraw();
        display.redraw();                          // full repaint
        display.forceWiFiStatus(wifi.isConnected()); // always update header dot
        display.setMicStatus(micReady);
        if (wifi.isConnected()) display.showStatus("Connected!");
        else                    display.showStatus("Offline");
        return;
    }

    // Single touch poll — update() owns getTouchPoints(), never call it elsewhere
    TouchResult touch = display.update();

    switch (touch) {
        case TouchResult::ASK_TAP:
            askHub.open(display.getDisplay(), display.getTouch(),
                        Config::BRIDGE_HOST, Config::BRIDGE_PORT);
            return;
        case TouchResult::ASK_WIN_TAP:
            askWin.open(display.getDisplay(), display.getTouch(),
                        Config::BRIDGE_HOST, Config::BRIDGE_PORT);
            return;
        case TouchResult::SETTINGS_TAP:
            wifiSettings.open(display.getDisplay(), display.getTouch(), &wifi);
            return;
        case TouchResult::MUTE_TAP:
            micReady = false;
            display.setMicStatus(false);
            display.showStatus("Muted");
            break;
        case TouchResult::UNMUTE_TAP:
            micReady = true;
            display.setMicStatus(true);
            display.showStatus("Unmuted");
            break;
        case TouchResult::STATUS_TAP:
            display.showStatus(wifi.isConnected() ? "Online" : "Offline");
            break;
        default:
            break;
    }

    if (wifi.isConnected() && millis() - lastHeartbeat >= Config::HEARTBEAT_MS) {
        if (dashboard.sendStatus()) lastHeartbeat = millis();
        else lastHeartbeat = millis();  // reset regardless so we don't spam
    }

    if (!micReady) { delay(10); return; }

    // Sliding window capture — poll touch during the 500ms audio wait.
    // Button taps are latched in _pendingResult and flushed next update() call.
    // Capture one 500ms audio slice -- pure PDM, no touch/display calls inside
    mic.captureSlice(audioBuffer, EI_CLASSIFIER_RAW_SAMPLE_COUNT);

    // Check touch immediately after capture -- catches taps made during the 500ms window
    {
        TouchResult postCapture = display.update();
        switch (postCapture) {
            case TouchResult::ASK_TAP:
                askHub.open(display.getDisplay(), display.getTouch(),
                            Config::BRIDGE_HOST, Config::BRIDGE_PORT);
                return;
            case TouchResult::ASK_WIN_TAP:
                askWin.open(display.getDisplay(), display.getTouch(),
                            Config::BRIDGE_HOST, Config::BRIDGE_PORT);
                return;
            case TouchResult::SETTINGS_TAP:
                wifiSettings.open(display.getDisplay(), display.getTouch(), &wifi);
                return;
            case TouchResult::MUTE_TAP:
                micReady = false;
                display.setMicStatus(false);
                display.showStatus("Muted");
                break;
            case TouchResult::UNMUTE_TAP:
                micReady = true;
                display.setMicStatus(true);
                display.showStatus("Unmuted");
                break;
            case TouchResult::STATUS_TAP:
                display.showStatus(wifi.isConnected() ? "Online" : "Offline");
                break;
            default:
                break;
        }
    }

    ClassifierResult result = classifier.classify(audioBuffer, EI_CLASSIFIER_RAW_SAMPLE_COUNT);

    if (result.valid) {
        handleDetection(result.label, result.confidence);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void handleDetection(const char* label, float confidence) {
    unsigned long now = millis();

    bool sameLabel = (lastTriggerLabel != nullptr && strcmp(label, lastTriggerLabel) == 0);
    if (sameLabel && (now - lastTriggerTime) < Config::DEBOUNCE_MS) {
        Serial.println("[DEBOUNCE] Skipping");
        return;
    }

    lastTriggerTime  = now;
    lastTriggerLabel = label;

    Serial.print("[DETECT] ");
    Serial.print(label);
    Serial.print(" (");
    Serial.print(confidence * 100.0f, 1);
    Serial.println("%)");

    display.showDetection(label, confidence);

    // Only hit network if we're connected -- skip dashboard/webhook silently if not
    if (!wifi.isConnected()) return;

    dashboard.sendEvent("keyword", label, confidence);

    const WebhookTarget* target = findWebhook(label);
    if (target == nullptr) return;

    bool ok = wifi.trigger(*target);
    Serial.println(ok ? "[OK] Webhook fired" : "[FAIL] Webhook failed");
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
