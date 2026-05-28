#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <Arduino_GigaDisplay_GFX.h>
#include <Arduino_GigaDisplayTouch.h>
#include <PDM.h>
#include <mbed.h>
#include <math.h>

// -----------------------------------------------------------------------------
// AskWindows.h  --  Like AskHub but bridge plays audio on Windows.
//                   Giga only receives and displays text -- no WAV transfer.
// -----------------------------------------------------------------------------

namespace VoiceHub {

// Reuse AskHub constants
static constexpr int    AW_SW          = 800;
static constexpr int    AW_SH          = 480;
static constexpr int    AW_PAD         = 20;
static constexpr int    AW_SAMPLE_RATE = 16000;
static constexpr int    AW_MAX_SECONDS = 5;
static constexpr size_t AW_MAX_SAMPLES = AW_SAMPLE_RATE * AW_MAX_SECONDS;

#define AW_BG       0x0841
#define AW_ORANGE   0xFD20
#define AW_GREEN    0x07E0
#define AW_WHITE    0xFFFF
#define AW_GRAY     0x8410
#define AW_DARKGRAY 0x4208
#define AW_SURFACE  0x2104
#define AW_RED      0xF800

enum class AwState { IDLE, READY, RECORDING, SENDING, WAITING, DONE, ERROR_STATE };

static volatile AwState  _awState      = AwState::IDLE;
static volatile bool     _awThreadDone = false;
static char              _awErrorMsg[64]    = {0};
static char              _awTranscript[128] = {0};
static char              _awReply[208]      = {0};
static int16_t*          _awRecBuf          = nullptr;
static size_t            _awSamples         = 0;
static char              _awBridgeHost[64]  = {0};
static int               _awBridgePort      = 0;

// -- Network thread: POST audio to /ask_windows, get back text only -----------
static void _awNetworkThread() {
    _awState = AwState::SENDING;

    WiFiClient wc;
    if (!wc.connect(_awBridgeHost, _awBridgePort)) {
        strncpy(_awErrorMsg, "Cannot reach server", sizeof(_awErrorMsg) - 1);
        _awState = AwState::ERROR_STATE;
        _awThreadDone = true;
        return;
    }

    size_t pcmBytes = _awSamples * 2;
    String boundary = "----VoiceHubBoundary";
    String partHead =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.raw\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n";
    String tail =
        "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"sample_rate\"\r\n\r\n16000"
        "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"channels\"\r\n\r\n1"
        "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"bits\"\r\n\r\n16"
        "\r\n--" + boundary + "--\r\n";
    size_t bodyLen = partHead.length() + pcmBytes + tail.length();

    wc.print("POST /ask_windows HTTP/1.1\r\n");
    wc.print("Host: "); wc.print(_awBridgeHost); wc.print(":"); wc.println(_awBridgePort);
    wc.print("Content-Type: multipart/form-data; boundary="); wc.println(boundary);
    wc.print("Content-Length: "); wc.println((int)bodyLen);
    wc.println("Connection: close");
    wc.println();
    wc.print(partHead);

    uint8_t* ptr = (uint8_t*)_awRecBuf;
    size_t   rem = pcmBytes;
    while (rem > 0) {
        size_t chunk = min(rem, (size_t)4096);
        wc.write(ptr, chunk);
        ptr += chunk; rem -= chunk;
    }
    wc.print(tail);

    _awState = AwState::WAITING;

    wc.setTimeout(30000);
    String statusLine = wc.readStringUntil('\n');
    if (statusLine.indexOf("200") < 0) {
        wc.stop();
        snprintf(_awErrorMsg, sizeof(_awErrorMsg), "Bad status: %.40s", statusLine.c_str());
        _awState = AwState::ERROR_STATE;
        _awThreadDone = true;
        return;
    }

    // Read headers -- get X-Transcript and X-Reply
    _awTranscript[0] = '\0';
    _awReply[0]      = '\0';
    while (true) {
        String line = wc.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
        if (line.startsWith("X-Transcript:")) {
            String val = line.substring(13); val.trim();
            strncpy(_awTranscript, val.c_str(), sizeof(_awTranscript) - 1);
        } else if (line.startsWith("X-Reply:")) {
            String val = line.substring(8); val.trim();
            strncpy(_awReply, val.c_str(), sizeof(_awReply) - 1);
        }
    }
    wc.stop();

    // Bridge plays audio on Windows -- we just show the text
    Serial.print("[AskWin] Q: "); Serial.println(_awTranscript);
    Serial.print("[AskWin] A: "); Serial.println(_awReply);

    _awState      = AwState::DONE;
    _awThreadDone = true;
}

// =============================================================================
class AskWindows {
public:
    AskWindows() : _disp(nullptr), _touch(nullptr), _active(false),
                   _lastTouchTime(0), _lastMeterUpdate(0), _recordStartMs(0),
                   _levelSmooth(0.0f), _sampleCount(0), _thread(nullptr) {}

    bool preallocate(int16_t* sharedBuf) {
        // Share the AskHub buffer -- they never run simultaneously
        _awRecBuf = sharedBuf;
        return sharedBuf != nullptr;
    }

    void open(GigaDisplay_GFX* disp, Arduino_GigaDisplayTouch* touch,
              const char* host, int port) {
        if (_active) return;
        _disp = disp; _touch = touch;
        strncpy(_awBridgeHost, host, sizeof(_awBridgeHost) - 1);
        _awBridgePort = port;
        if (!_awRecBuf) { Serial.println("[AskWin] No buffer"); return; }

        _record_ready = false;
        _raw_buf      = nullptr;
        _raw_count    = 0;
        _raw_max      = 0;

        _awState = AwState::READY;
        _awThreadDone = false;
        _awErrorMsg[0] = '\0';
        _awTranscript[0] = '\0';
        _awReply[0] = '\0';
        _sampleCount = 0; _awSamples = 0;
        _levelSmooth = 0.0f; _active = true;
        _recordStartMs = 0;
        _lastMeterUpdate = millis();
        _lastTouchTime   = millis() + 300;

        _drawReadyScreen();
        _drainTouch();
    }

    bool isActive() const { return _active; }

    const char* update() {
        if (!_active) return nullptr;

        switch (_awState) {
            case AwState::READY:     _updateReady();     break;
            case AwState::RECORDING: _updateRecording(); break;
            case AwState::SENDING:
            case AwState::WAITING:   _updateSending();   break;

            case AwState::DONE:
                if (_awThreadDone) {
                    _awThreadDone = false;
                    _drawResponseScreen(_awTranscript, _awReply);
                    delay(4000);  // show text for 4s then return
                    _close();
                    return _awReply[0] ? _awReply : "Windows played response";
                }
                break;

            case AwState::ERROR_STATE:
                if (_awThreadDone) {
                    _awThreadDone = false;
                    _drawError(_awErrorMsg);
                    delay(2000);
                    const char* err = _awErrorMsg;
                    _close();
                    return err;
                }
                break;

            default: break;
        }
        return nullptr;
    }

private:
    GigaDisplay_GFX*          _disp;
    Arduino_GigaDisplayTouch* _touch;
    bool          _active;
    unsigned long _lastTouchTime;
    unsigned long _lastMeterUpdate;
    unsigned long _recordStartMs;
    float         _levelSmooth;
    size_t        _sampleCount;
    rtos::Thread* _thread;

    void _close() {
        _raw_buf  = nullptr;
        _active   = false;
        _awState  = AwState::IDLE;
        _record_ready = false;
        if (_thread) { _thread->join(); delete _thread; _thread = nullptr; }
    }

    void _drainTouch(unsigned long ms = 50) {
        if (!_touch) return;
        uint8_t c; GDTpoint_t p[5];
        do { c = _touch->getTouchPoints(p); delay(10); } while (c > 0);
        delay(ms);
    }

    void _updateReady() {
        if (!_touch) return;
        unsigned long now = millis();
        if (now - _lastTouchTime < 300) return;
        uint8_t c; GDTpoint_t p[5];
        if (_touch->getTouchPoints(p) == 0) return;
        _lastTouchTime = now;
        int tx = p[0].y, ty = AW_SH - p[0].x;
        int bx = AW_SW / 2 - 150, by = AW_SH - 130;
        if (tx >= bx && tx <= bx + 300 && ty >= by && ty <= by + 90) {
            _sampleCount = 0; _levelSmooth = 0.0f;
            _raw_count = 0; _raw_max = AW_MAX_SAMPLES;
            _raw_buf   = _awRecBuf;
            Serial.println("[AskWin] START -- recording");
            _recordStartMs   = millis();
            _lastMeterUpdate = millis();
            _lastTouchTime   = millis() + 200;
            _awState = AwState::RECORDING;
            _drawRecordingScreen();
            _drainTouch(50);
        } else {
            _close();
        }
    }

    void _updateRecording() {
        size_t captured = _raw_count;
        unsigned long now     = millis();
        unsigned long elapsed = now - _recordStartMs;
        unsigned long maxMs   = (unsigned long)AW_MAX_SECONDS * 1000;

        if (elapsed >= maxMs || captured >= AW_MAX_SAMPLES) {
            _startSend(); return;
        }

        if (now - _lastMeterUpdate > 50) {
            _lastMeterUpdate = now;
            if (captured > 0) {
                size_t window = min(captured, (size_t)512);
                size_t start  = captured - window;
                float sum = 0;
                for (size_t i = start; i < captured; i++) {
                    float s = _awRecBuf[i]; sum += s * s;
                }
                _levelSmooth = _levelSmooth * 0.5f +
                               (sqrtf(sum / window) / 32768.0f) * 0.5f;
            }
            _drawMeter();
            _drawCountdown(elapsed, maxMs);
        }

        if (_touch) {
            uint8_t c; GDTpoint_t p[5];
            if (_touch->getTouchPoints(p) > 0 && now - _lastTouchTime > 200) {
                _lastTouchTime = now;
                int tx = p[0].y, ty = AW_SH - p[0].x;
                int bx = AW_SW / 2 - 150, by = AW_SH - 100;
                if (tx >= bx && tx <= bx + 300 && ty >= by && ty <= by + 80)
                    _startSend();
            }
        }
    }

    void _startSend() {
        _raw_buf = nullptr;
        size_t captured = _raw_count;
        if (captured == 0) { _close(); return; }
        Serial.print("[AskWin] Recorded "); Serial.print(captured); Serial.println(" samples");
        _awSamples    = captured;
        _awThreadDone = false;
        _drawSendingScreen("Sending to Windows...");
        delete _thread;
        _thread = new rtos::Thread(osPriorityNormal, 8192);
        _thread->start(_awNetworkThread);
    }

    unsigned long _lastSpinUpdate = 0;
    void _updateSending() {
        unsigned long now = millis();
        if (now - _lastSpinUpdate > 100) {
            _lastSpinUpdate = now;
            const char* msg = _awState == AwState::WAITING ? "Playing on Windows..." : "Sending audio...";
            _drawSendingScreen(msg);
        }
    }

    // -- Screens --------------------------------------------------------------
    void _drawReadyScreen() {
        _disp->fillScreen(AW_BG);
        _disp->fillRect(0, 0, AW_SW, 56, AW_SURFACE);
        _disp->drawFastHLine(0, 55, AW_SW, AW_GRAY);
        _disp->setTextColor(AW_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AW_PAD, 18); _disp->print("Ask Windows");
        _disp->setTextColor(AW_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AW_PAD, 38); _disp->print("Audio plays on Windows speaker  |  tap outside START to cancel");

        // Mic icon
        int cx = AW_SW / 2, cy = 130;
        _disp->fillCircle(cx, cy, 44, AW_SURFACE);
        _disp->drawCircle(cx, cy, 44, AW_ORANGE);
        _disp->fillRoundRect(cx - 10, cy - 26, 20, 32, 8, AW_ORANGE);
        _disp->fillRect(cx - 16, cy + 10, 32, 5, AW_ORANGE);
        _disp->fillRect(cx - 2, cy + 15, 4, 8, AW_ORANGE);

        _disp->setTextColor(AW_GRAY); _disp->setTextSize(2);
        _disp->setCursor(AW_SW / 2 - 126, 188);
        _disp->print("Press START to record");

        int bx = AW_SW / 2 - 150, by = AW_SH - 130;
        _disp->fillRoundRect(bx, by, 300, 90, 14, AW_ORANGE);
        _disp->setTextColor(0x0000); _disp->setTextSize(3);
        _disp->setCursor(bx + 60, by + 30); _disp->print("START");
    }

    void _drawRecordingScreen() {
        _disp->fillScreen(AW_BG);
        _disp->fillRect(0, 0, AW_SW, 56, AW_SURFACE);
        _disp->drawFastHLine(0, 55, AW_SW, AW_GRAY);
        _disp->setTextColor(AW_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AW_PAD, 18); _disp->print("Ask Windows");
        _disp->fillRoundRect(AW_SW - 90, 10, 78, 36, 6, AW_RED);
        _disp->setTextColor(AW_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AW_SW - 78, 20); _disp->print("REC");
        _disp->setTextColor(AW_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AW_PAD, 76); _disp->print("Speak now -- tap STOP when done");
        _disp->setTextColor(AW_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AW_PAD, 112); _disp->print("MIC LEVEL");
        _disp->fillRoundRect(AW_PAD, 120, AW_SW - AW_PAD * 2, 60, 8, 0x1082);
        _disp->drawRoundRect(AW_PAD, 120, AW_SW - AW_PAD * 2, 60, 8, AW_GRAY);
        _disp->setTextColor(AW_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AW_PAD, 196); _disp->print("TIME REMAINING");
        _disp->fillRoundRect(AW_PAD, 208, AW_SW - AW_PAD * 2, 20, 4, 0x1082);
        _disp->drawRoundRect(AW_PAD, 208, AW_SW - AW_PAD * 2, 20, 4, AW_GRAY);
        _disp->setTextColor(AW_WHITE); _disp->setTextSize(5);
        _disp->setCursor(AW_SW / 2 - 15, 248); _disp->print(AW_MAX_SECONDS);
        _disp->setTextColor(AW_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AW_SW / 2 - 18, 298); _disp->print("seconds");
        int bx = AW_SW / 2 - 150, by = AW_SH - 100;
        _disp->fillRoundRect(bx, by, 300, 80, 12, AW_RED);
        _disp->setTextColor(AW_WHITE); _disp->setTextSize(3);
        _disp->setCursor(bx + 300/2 - 36, by + 26); _disp->print("STOP");
        _lastMeterUpdate = millis();
    }

    void _drawMeter() {
        int bx = AW_PAD + 3, by = 123, bw = AW_SW - AW_PAD * 2 - 6, bh = 54;
        _disp->fillRoundRect(bx, by, bw, bh, 6, 0x1082);
        float level = constrain(_levelSmooth * 12.0f, 0.0f, 1.0f);
        int   fillW = (int)(bw * level);
        uint16_t col = level > 0.75f ? AW_RED : level > 0.4f ? AW_ORANGE : AW_GREEN;
        if (fillW > 4) _disp->fillRoundRect(bx, by, fillW, bh, 6, col);
        for (int t = 1; t < 10; t++)
            _disp->drawFastVLine(bx + bw * t / 10, by + bh - 10, 10, 0x39C7);
    }

    void _drawCountdown(unsigned long elapsedMs, unsigned long maxMs) {
        float remaining = max(0.0f, (float)AW_MAX_SECONDS - elapsedMs / 1000.0f);
        _disp->fillRect(AW_SW / 2 - 60, 240, 120, 60, AW_BG);
        _disp->setTextColor(remaining < 2.0f ? AW_RED : AW_WHITE); _disp->setTextSize(5);
        char buf[8]; snprintf(buf, sizeof(buf), "%d", (int)ceilf(remaining));
        _disp->setCursor(AW_SW / 2 - (int)(strlen(buf) * 15), 248); _disp->print(buf);
        _disp->fillRect(AW_SW / 2 - 30, 298, 60, 10, AW_BG);
        _disp->setTextColor(AW_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AW_SW / 2 - 18, 298); _disp->print("seconds");
        int barX = AW_PAD + 2, barW = AW_SW - AW_PAD * 2 - 4;
        float pct = constrain((float)elapsedMs / (float)maxMs, 0.0f, 1.0f);
        int fillW = (int)(barW * pct);
        _disp->fillRect(barX, 210, barW, 16, 0x1082);
        if (fillW > 0)
            _disp->fillRoundRect(barX, 210, fillW, 16, 4,
                pct > 0.8f ? AW_RED : pct > 0.5f ? AW_ORANGE : AW_GREEN);
    }

    int _spinIdx = 0;
    void _drawSendingScreen(const char* msg) {
        static AwState lastState = AwState::IDLE;
        if (_awState != lastState) {
            lastState = _awState;
            _disp->fillScreen(AW_BG);
            _disp->fillRect(0, 0, AW_SW, 56, AW_SURFACE);
            _disp->drawFastHLine(0, 55, AW_SW, AW_GRAY);
            _disp->setTextColor(AW_WHITE); _disp->setTextSize(2);
            _disp->setCursor(AW_PAD, 18); _disp->print("Ask Windows");
        }
        int cx = AW_SW / 2, cy = AW_SH / 2 - 20;
        for (int d = 0; d < 10; d++) {
            float angle = d * 2.0f * 3.14159f / 10;
            int dx = cx + (int)(44 * cosf(angle));
            int dy = cy + (int)(44 * sinf(angle));
            int dist = (10 + _spinIdx - d) % 10;
            uint16_t col = dist == 0 ? AW_WHITE : dist == 1 ? AW_GRAY :
                           dist == 2 ? AW_DARKGRAY : AW_SURFACE;
            _disp->fillCircle(dx, dy, 5, col);
        }
        _spinIdx = (_spinIdx + 1) % 10;
        _disp->fillRect(0, cy + 54, AW_SW, 20, AW_BG);
        _disp->setTextColor(AW_GRAY); _disp->setTextSize(2);
        _disp->setCursor(AW_SW / 2 - (int)(strlen(msg) * 6), cy + 56);
        _disp->print(msg);
    }

    void _drawError(const char* msg) {
        _disp->fillScreen(AW_BG);
        _disp->setTextColor(AW_RED); _disp->setTextSize(2);
        _disp->setCursor(AW_SW / 2 - (int)(strlen(msg) * 6), AW_SH / 2 - 8);
        _disp->print(msg);
    }

    void _drawResponseScreen(const char* transcript, const char* reply) {
        _disp->fillScreen(AW_BG);
        _disp->fillRect(0, 0, AW_SW, 56, AW_SURFACE);
        _disp->drawFastHLine(0, 55, AW_SW, AW_GRAY);
        _disp->setTextColor(AW_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AW_PAD, 18); _disp->print("Ask Windows");
        _disp->fillRoundRect(AW_SW - 130, 10, 118, 36, 6, AW_ORANGE);
        _disp->setTextColor(0x0000); _disp->setTextSize(1);
        _disp->setCursor(AW_SW - 122, 24); _disp->print("PLAYING ON PC");

        _disp->setTextColor(AW_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AW_PAD, 72); _disp->print("YOU ASKED:");
        _disp->fillRoundRect(AW_PAD, 84, AW_SW - AW_PAD * 2, 80, 6, AW_SURFACE);
        _disp->drawRoundRect(AW_PAD, 84, AW_SW - AW_PAD * 2, 80, 6, AW_GRAY);
        _disp->setTextColor(AW_WHITE); _disp->setTextSize(2);
        _drawWrapped(transcript, AW_PAD + 8, 96, AW_SW - AW_PAD * 2 - 16, 2);

        _disp->setTextColor(AW_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AW_PAD, 178); _disp->print("WINDOWS SAYS:");
        _disp->fillRoundRect(AW_PAD, 190, AW_SW - AW_PAD * 2, 200, 6, 0x2900);
        _disp->drawRoundRect(AW_PAD, 190, AW_SW - AW_PAD * 2, 200, 6, AW_ORANGE);
        _disp->setTextColor(AW_WHITE); _disp->setTextSize(2);
        _drawWrapped(reply, AW_PAD + 8, 202, AW_SW - AW_PAD * 2 - 16, 2);
    }

    void _drawWrapped(const char* text, int x, int y, int maxW, int textSize) {
        if (!text || !text[0]) return;
        _disp->setTextSize(textSize);
        int charW = textSize * 6, lineH = textSize * 8 + 4;
        int cx = x, cy = y;
        char word[32]; int wi = 0;
        const char* p = text;
        auto flushWord = [&]() {
            if (wi == 0) return;
            word[wi] = '\0';
            int wordPx = wi * charW;
            if (cx + wordPx > x + maxW) { cx = x; cy += lineH; }
            _disp->setCursor(cx, cy); _disp->print(word);
            cx += wordPx + charW; wi = 0;
        };
        while (*p) {
            if (*p == ' ' || *p == '\n') flushWord();
            else if (wi < 31) word[wi++] = *p;
            p++;
        }
        flushWord();
    }
};

} // namespace VoiceHub
