#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <Arduino_AdvancedAnalog.h>
#include <Arduino_GigaDisplay_GFX.h>
#include <Arduino_GigaDisplayTouch.h>
#include <PDM.h>
#include <mbed.h>
#include <math.h>

// -----------------------------------------------------------------------------
// AskHub.h  --  Non-blocking Record -> POST -> Play pipeline
// -----------------------------------------------------------------------------

namespace VoiceHub {

static constexpr int    AH_SW          = 800;
static constexpr int    AH_SH          = 480;
static constexpr int    AH_PAD         = 20;
static constexpr int    AH_SAMPLE_RATE = 16000;
static constexpr int    AH_MAX_SECONDS = 5;
static constexpr size_t AH_MAX_SAMPLES = AH_SAMPLE_RATE * AH_MAX_SECONDS;
static constexpr size_t AH_MAX_BYTES   = 200000;  // 200KB

#define AH_BG       0x0841
#define AH_RED      0xF800
#define AH_GREEN    0x07E0
#define AH_WHITE    0xFFFF
#define AH_GRAY     0x8410
#define AH_DARKGRAY 0x4208
#define AH_SURFACE  0x2104

enum class AskState {
    IDLE, READY, RECORDING,
    SENDING, WAITING, RECEIVING,
    PLAYING, ERROR_STATE
};

static volatile AskState _ahState      = AskState::IDLE;
static volatile bool     _ahThreadDone = false;
static char              _ahErrorMsg[64]    = {0};
static char              _ahTranscript[128] = {0};
static char              _ahReply[208]      = {0};
static int16_t*          _ahRecBuf          = nullptr;
static size_t            _ahSamples         = 0;
static char              _ahBridgeHost[64]  = {0};
static int               _ahBridgePort      = 0;

// -- Network thread -----------------------------------------------------------
static void _ahNetworkThread() {
    _ahState = AskState::SENDING;

    // Use raw WiFiClient -- much faster than ArduinoHttpClient for large responses
    WiFiClient wc;
    if (!wc.connect(_ahBridgeHost, _ahBridgePort)) {
        snprintf(_ahErrorMsg, sizeof(_ahErrorMsg), "Cannot reach server");
        _ahState = AskState::ERROR_STATE;
        _ahThreadDone = true;
        return;
    }

    // Build multipart body
    size_t pcmBytes = _ahSamples * 2;
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

    // Send HTTP request manually
    wc.print("POST /ask HTTP/1.1\r\n");
    wc.print("Host: "); wc.print(_ahBridgeHost); wc.print(":"); wc.println(_ahBridgePort);
    wc.print("Content-Type: multipart/form-data; boundary="); wc.println(boundary);
    wc.print("Content-Length: "); wc.println((int)bodyLen);
    wc.println("Connection: close");
    wc.println();
    wc.print(partHead);

    // Send PCM in large chunks
    uint8_t* ptr = (uint8_t*)_ahRecBuf;
    size_t   rem = pcmBytes;
    while (rem > 0) {
        size_t chunk = min(rem, (size_t)4096);
        wc.write(ptr, chunk);
        ptr += chunk; rem -= chunk;
    }
    wc.print(tail);

    _ahState = AskState::WAITING;

    // Read HTTP response line
    wc.setTimeout(30000);
    String statusLine = wc.readStringUntil('\n');
    if (statusLine.indexOf("200") < 0) {
        wc.stop();
        snprintf(_ahErrorMsg, sizeof(_ahErrorMsg), "Bad status: %.40s", statusLine.c_str());
        _ahState = AskState::ERROR_STATE;
        _ahThreadDone = true;
        return;
    }

    // Read headers, extract Content-Length + custom headers
    int contentLen = -1;
    _ahTranscript[0] = '\0';
    _ahReply[0]      = '\0';
    while (true) {
        String line = wc.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
        if (line.startsWith("Content-Length:")) {
            String val = line.substring(15); val.trim();
            contentLen = val.toInt();
        } else if (line.startsWith("X-Transcript:")) {
            String val = line.substring(13); val.trim();
            strncpy(_ahTranscript, val.c_str(), sizeof(_ahTranscript) - 1);
        } else if (line.startsWith("X-Reply:")) {
            String val = line.substring(8); val.trim();
            strncpy(_ahReply, val.c_str(), sizeof(_ahReply) - 1);
        }
    }

    Serial.print("[AskHub] Content-Length: "); Serial.println(contentLen);
    if (contentLen <= 0 || contentLen > (int)AH_MAX_BYTES) {
        wc.stop();
        snprintf(_ahErrorMsg, sizeof(_ahErrorMsg), "Bad response size: %d", contentLen);
        _ahState = AskState::ERROR_STATE;
        _ahThreadDone = true;
        return;
    }

    _ahState = AskState::RECEIVING;

    // Read body directly -- no ArduinoHttpClient overhead
    uint8_t*      wavBuf   = (uint8_t*)_ahRecBuf;
    int           received = 0;
    unsigned long deadline = millis() + 30000;
    static uint8_t rxChunk[4096];

    while (received < contentLen && millis() < deadline) {
        int avail = wc.available();
        if (avail > 0) {
            int want = min(avail, min((int)sizeof(rxChunk), contentLen - received));
            int got  = wc.read(rxChunk, want);
            if (got > 0) {
                memcpy(wavBuf + received, rxChunk, got);
                received += got;
            }
        } else if (!wc.connected()) {
            break;  // connection closed by server
        } else {
            rtos::ThisThread::sleep_for(1);
        }
    }
    wc.stop();

    Serial.print("[AskHub] Received: "); Serial.print(received);
    Serial.print("/"); Serial.println(contentLen);

    if (received < contentLen) {
        strncpy(_ahErrorMsg, "Response cut short.", sizeof(_ahErrorMsg) - 1);
        _ahState = AskState::ERROR_STATE;
        _ahThreadDone = true;
        return;
    }

    _ahSamples    = (size_t)received;
    _ahState      = AskState::PLAYING;
    _ahThreadDone = true;
}

// =============================================================================
class AskHub {
public:
    AskHub() : _disp(nullptr), _touch(nullptr), _active(false),
               _lastTouchTime(0), _lastMeterUpdate(0), _recordStartMs(0),
               _levelSmooth(0.0f), _sampleCount(0), _thread(nullptr) {}

    ~AskHub() {
        if (_ahRecBuf) { free(_ahRecBuf); _ahRecBuf = nullptr; }
        delete _thread;
    }

    bool preallocate() {
        if (_ahRecBuf) return true;
        _ahRecBuf = (int16_t*)malloc(AH_MAX_BYTES);
        if (!_ahRecBuf) { Serial.println("[AskHub] preallocate failed"); return false; }
        Serial.print("[AskHub] Buffer: "); Serial.print(AH_MAX_BYTES); Serial.println(" bytes");
        return true;
    }

    void open(GigaDisplay_GFX* disp, Arduino_GigaDisplayTouch* touch,
              const char* host, int port) {
        if (_active) return;
        _disp = disp; _touch = touch;
        strncpy(_ahBridgeHost, host, sizeof(_ahBridgeHost) - 1);
        _ahBridgePort = port;
        if (!_ahRecBuf) { Serial.println("[AskHub] No buffer"); return; }

        // Don't touch _record_ready -- classifier keeps running normally
        // We'll arm _raw_buf when START is tapped
        _raw_buf   = nullptr;
        _raw_count = 0;
        _raw_max   = 0;

        _ahState = AskState::READY;
        _ahThreadDone = false;
        _ahErrorMsg[0] = '\0';
        _ahTranscript[0] = '\0';
        _ahReply[0] = '\0';
        _sampleCount = 0; _ahSamples = 0;
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

        switch (_ahState) {
            case AskState::READY:     _updateReady();     break;
            case AskState::RECORDING: _updateRecording(); break;
            case AskState::SENDING:
            case AskState::WAITING:
            case AskState::RECEIVING: _updateSending();   break;

            case AskState::PLAYING:
                if (_ahThreadDone) {
                    _ahThreadDone = false;
                    Serial.print("[AskHub] Playing "); Serial.print(_ahSamples); Serial.println(" bytes");
                    _drawResponseScreen(_ahTranscript, _ahReply);
                    _playWav((uint8_t*)_ahRecBuf, (int)_ahSamples);
                    _close();
                    return _ahReply[0] ? _ahReply : "Hub responded";
                }
                break;

            case AskState::ERROR_STATE:
                if (_ahThreadDone) {
                    _ahThreadDone = false;
                    _drawError(_ahErrorMsg);
                    delay(2000);
                    const char* err = _ahErrorMsg;
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
        _raw_buf  = nullptr;  // disarm PDM raw capture
        _active   = false;
        _ahState  = AskState::IDLE;
        _record_ready = false;
        if (_thread) { _thread->join(); delete _thread; _thread = nullptr; }
    }

    void _drainTouch(unsigned long ms = 50) {
        if (!_touch) return;
        uint8_t c; GDTpoint_t p[5];
        do { c = _touch->getTouchPoints(p); delay(10); } while (c > 0);
        delay(ms);
    }

    // -- Ready screen ---------------------------------------------------------
    void _updateReady() {
        if (!_touch) return;
        unsigned long now = millis();
        if (now - _lastTouchTime < 300) return;
        uint8_t c; GDTpoint_t p[5];
        if (_touch->getTouchPoints(p) == 0) return;
        _lastTouchTime = now;
        int tx = p[0].y, ty = AH_SH - p[0].x;
        int bx = AH_SW / 2 - 150, by = AH_SH - 130;
        if (tx >= bx && tx <= bx + 300 && ty >= by && ty <= by + 90) {
            _sampleCount  = 0;
            _levelSmooth  = 0.0f;
            // Arm raw capture in PDM callback
            _raw_count = 0;
            _raw_max   = AH_MAX_SAMPLES;
            _raw_buf   = _ahRecBuf;  // arm last -- callback checks this
            Serial.println("[AskHub] START -- raw capture armed");
            _recordStartMs   = millis();
            _lastMeterUpdate = millis();
            _lastTouchTime   = millis() + 200;
            _ahState = AskState::RECORDING;
            _drawRecordingScreen();
            _drainTouch(50);
        } else {
            _close();
        }
    }

    // -- Recording ------------------------------------------------------------
    void _updateRecording() {
        size_t captured = _raw_count;  // written by PDM callback

        unsigned long now     = millis();
        unsigned long elapsed = now - _recordStartMs;
        unsigned long maxMs   = (unsigned long)AH_MAX_SECONDS * 1000;

        if (elapsed >= maxMs || captured >= AH_MAX_SAMPLES) {
            _startSend(); return;
        }

        if (now - _lastMeterUpdate > 50) {
            _lastMeterUpdate = now;
            if (((int)(elapsed/1000)) != ((int)((elapsed-50)/1000))) {
                Serial.print("[AskHub] t="); Serial.print(elapsed/1000);
                Serial.print("s samples="); Serial.println(captured);
            }
            // RMS from last 512 samples
            if (captured > 0) {
                size_t window = min(captured, (size_t)512);
                size_t start  = captured - window;
                float sum = 0;
                for (size_t i = start; i < captured; i++) {
                    float s = _ahRecBuf[i];
                    sum += s * s;
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
                int tx = p[0].y, ty = AH_SH - p[0].x;
                int bx = AH_SW / 2 - 150, by = AH_SH - 100;
                if (tx >= bx && tx <= bx + 300 && ty >= by && ty <= by + 80)
                    _startSend();
            }
        }
    }

    void _feedMic() {
        // PDM.available() works because _pdm_callback is still registered and
        // calls PDM.read() -- but with _record_ready=false it discards the data
        // after reading. We re-read here to get our own copy.
        // Actually on mbed PDM, the data is read inside _pdm_callback already.
        // We need to read BEFORE the callback does, which isn't possible.
        // Solution: use the _sampleBuffer that the callback already filled.
        // The callback always reads into _sampleBuffer regardless of _record_ready.
        // We just need to copy from there.
        // Since _sampleBuffer is only 2048 shorts and the callback fires ~every 4ms,
        // we call this every loop iteration to drain it quickly.
        extern int16_t _sampleBuffer[];
        int avail = PDM.available();
        if (avail <= 0) return;
        // Don't call PDM.read() -- the callback already did. Just use _sampleBuffer.
        // But we don't know how many fresh samples are there without reading.
        // Simplest: call PDM.read() here too -- on mbed this is safe to call
        // from main loop as well as callback, it just reads whatever is available.
        static int16_t localBuf[2048];
        int bytes  = PDM.read((char*)localBuf, min(avail, (int)sizeof(localBuf)));
        int samples = bytes >> 1;
        if (samples <= 0) return;

        size_t toStore = min((size_t)samples, AH_MAX_SAMPLES - _sampleCount);
        if (toStore > 0) {
            memcpy(_ahRecBuf + _sampleCount, localBuf, toStore * 2);
            _sampleCount += toStore;
        }

        float sum = 0;
        for (int i = 0; i < samples; i++) sum += (float)localBuf[i] * localBuf[i];
        float rms = sqrtf(sum / samples) / 32768.0f;
        _levelSmooth = _levelSmooth * 0.5f + rms * 0.5f;
    }

    void _startSend() {
        // Disarm raw capture
        _raw_buf = nullptr;
        size_t captured = _raw_count;
        if (captured == 0) { _close(); return; }
        Serial.print("[AskHub] Recorded "); Serial.print(captured); Serial.println(" samples");
        _ahSamples    = captured;
        _ahThreadDone = false;
        _drawSendingScreen("Sending audio...");
        delete _thread;
        _thread = new rtos::Thread(osPriorityNormal, 8192);
        _thread->start(_ahNetworkThread);
    }

    unsigned long _lastSpinUpdate = 0;
    void _updateSending() {
        unsigned long now = millis();
        if (now - _lastSpinUpdate > 100) {
            _lastSpinUpdate = now;
            const char* msg = "Sending audio...";
            if (_ahState == AskState::WAITING)   msg = "Thinking...";
            if (_ahState == AskState::RECEIVING) msg = "Receiving response...";
            _drawSendingScreen(msg);
        }
    }

    void _playWav(uint8_t* buf, int len) {
        if (len < 44) { Serial.println("[AskHub] WAV too short"); return; }
        if (memcmp(buf, "RIFF", 4) != 0) { Serial.println("[AskHub] Not a WAV"); return; }

        uint16_t channels      = *(uint16_t*)(buf + 22);
        uint32_t sampleRate    = *(uint32_t*)(buf + 24);
        uint16_t bitsPerSample = *(uint16_t*)(buf + 34);

        int      dataOffset = 12;
        uint32_t dataSize   = 0;
        while (dataOffset + 8 < len) {
            if (memcmp(buf + dataOffset, "data", 4) == 0) {
                dataSize = *(uint32_t*)(buf + dataOffset + 4);
                dataOffset += 8; break;
            }
            dataOffset += 8 + *(uint32_t*)(buf + dataOffset + 4);
        }
        if (dataSize == 0) { Serial.println("[AskHub] No data chunk"); return; }

        Serial.print("[AskHub] WAV: "); Serial.print(sampleRate);
        Serial.print("Hz "); Serial.print(channels); Serial.println("ch");

        AdvancedDAC dac0(A12);
        if (!dac0.begin(AN_RESOLUTION_12, sampleRate, 256, 16)) {
            Serial.println("[AskHub] DAC begin failed"); return;
        }

        int16_t* samples  = (int16_t*)(buf + dataOffset);
        uint32_t numSamps = dataSize / (bitsPerSample / 8);
        uint32_t idx      = 0;

        while (idx < numSamps) {
            if (dac0.available()) {
                SampleBuffer sbuf = dac0.dequeue();
                for (size_t i = 0; i < sbuf.size() && idx < numSamps; i++) {
                    int32_t s = (int32_t)samples[idx++];
                    if (channels == 2 && idx < numSamps)
                        s = (s + (int32_t)samples[idx++]) / 2;
                    sbuf.data()[i] = (uint16_t)((s + 32768) >> 4);
                }
                dac0.write(sbuf);
            }
        }
        delay(300);
        dac0.stop();
        Serial.println("[AskHub] Playback done");
    }

    void _drawReadyScreen() {
        _disp->fillScreen(AH_BG);
        _disp->fillRect(0, 0, AH_SW, 56, AH_SURFACE);
        _disp->drawFastHLine(0, 55, AH_SW, AH_GRAY);
        _disp->setTextColor(AH_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AH_PAD, 18); _disp->print("Ask Hub");
        _disp->setTextColor(AH_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AH_PAD, 38); _disp->print("Tap anywhere outside START to cancel");

        int cx = AH_SW / 2, cy = 130;
        _disp->fillCircle(cx, cy, 44, AH_SURFACE);
        _disp->drawCircle(cx, cy, 44, AH_GRAY);
        _disp->fillRoundRect(cx - 10, cy - 26, 20, 32, 8, AH_GREEN);
        _disp->fillRect(cx - 16, cy + 10, 32, 5, AH_GREEN);
        _disp->fillRect(cx - 2, cy + 15, 4, 8, AH_GREEN);

        _disp->setTextColor(AH_GRAY); _disp->setTextSize(2);
        _disp->setCursor(AH_SW / 2 - 126, 188);
        _disp->print("Press START to record");

        _disp->setTextColor(AH_DARKGRAY); _disp->setTextSize(1);
        char buf[32]; snprintf(buf, sizeof(buf), "Max %d seconds", AH_MAX_SECONDS);
        _disp->setCursor(AH_SW / 2 - (int)(strlen(buf) * 3), 212);
        _disp->print(buf);

        int bx = AH_SW / 2 - 150, by = AH_SH - 130;
        _disp->fillRoundRect(bx, by, 300, 90, 14, AH_GREEN);
        _disp->drawRoundRect(bx, by, 300, 90, 14, 0x07C0);
        _disp->setTextColor(0x0000); _disp->setTextSize(3);
        _disp->setCursor(bx + 60, by + 30); _disp->print("START");
    }

    void _drawRecordingScreen() {
        _disp->fillScreen(AH_BG);
        _disp->fillRect(0, 0, AH_SW, 56, AH_SURFACE);
        _disp->drawFastHLine(0, 55, AH_SW, AH_GRAY);
        _disp->setTextColor(AH_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AH_PAD, 18); _disp->print("Ask Hub");
        _disp->fillRoundRect(AH_SW - 90, 10, 78, 36, 6, AH_RED);
        _disp->setTextColor(AH_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AH_SW - 78, 20); _disp->print("REC");
        _disp->setTextColor(AH_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AH_PAD, 76); _disp->print("Speak now -- tap STOP when done");
        _disp->setTextColor(AH_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AH_PAD, 112); _disp->print("MIC LEVEL");
        _disp->fillRoundRect(AH_PAD, 120, AH_SW - AH_PAD * 2, 60, 8, 0x1082);
        _disp->drawRoundRect(AH_PAD, 120, AH_SW - AH_PAD * 2, 60, 8, AH_GRAY);
        _disp->setTextColor(AH_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AH_PAD, 196); _disp->print("TIME REMAINING");
        _disp->fillRoundRect(AH_PAD, 208, AH_SW - AH_PAD * 2, 20, 4, 0x1082);
        _disp->drawRoundRect(AH_PAD, 208, AH_SW - AH_PAD * 2, 20, 4, AH_GRAY);
        _disp->setTextColor(AH_WHITE); _disp->setTextSize(5);
        _disp->setCursor(AH_SW / 2 - 15, 248); _disp->print(AH_MAX_SECONDS);
        _disp->setTextColor(AH_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AH_SW / 2 - 18, 298); _disp->print("seconds");
        int bx = AH_SW / 2 - 150, by = AH_SH - 100;
        _disp->fillRoundRect(bx, by, 300, 80, 12, AH_RED);
        _disp->drawRoundRect(bx, by, 300, 80, 12, 0xFB00);
        _disp->setTextColor(AH_WHITE); _disp->setTextSize(3);
        _disp->setCursor(bx + 300/2 - 36, by + 26); _disp->print("STOP");
        _lastMeterUpdate = millis();
    }

    void _drawMeter() {
        int bx = AH_PAD + 3, by = 123;
        int bw = AH_SW - AH_PAD * 2 - 6, bh = 54;
        _disp->fillRoundRect(bx, by, bw, bh, 6, 0x1082);
        float level = constrain(_levelSmooth * 12.0f, 0.0f, 1.0f);
        int   fillW = (int)(bw * level);
        uint16_t col = level > 0.75f ? AH_RED : level > 0.4f ? 0xFFE0 : AH_GREEN;
        if (fillW > 4) _disp->fillRoundRect(bx, by, fillW, bh, 6, col);
        for (int t = 1; t < 10; t++)
            _disp->drawFastVLine(bx + bw * t / 10, by + bh - 10, 10, 0x39C7);
    }

    void _drawCountdown(unsigned long elapsedMs, unsigned long maxMs) {
        float remaining = max(0.0f, (float)AH_MAX_SECONDS - elapsedMs / 1000.0f);
        _disp->fillRect(AH_SW / 2 - 60, 240, 120, 60, AH_BG);
        _disp->setTextColor(remaining < 2.0f ? AH_RED : AH_WHITE); _disp->setTextSize(5);
        char buf[8]; snprintf(buf, sizeof(buf), "%d", (int)ceilf(remaining));
        _disp->setCursor(AH_SW / 2 - (int)(strlen(buf) * 15), 248); _disp->print(buf);
        _disp->fillRect(AH_SW / 2 - 30, 298, 60, 10, AH_BG);
        _disp->setTextColor(AH_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AH_SW / 2 - 18, 298); _disp->print("seconds");
        int barX = AH_PAD + 2, barW = AH_SW - AH_PAD * 2 - 4;
        float pct = constrain((float)elapsedMs / (float)maxMs, 0.0f, 1.0f);
        int fillW = (int)(barW * pct);
        _disp->fillRect(barX, 210, barW, 16, 0x1082);
        if (fillW > 0)
            _disp->fillRoundRect(barX, 210, fillW, 16, 4,
                pct > 0.8f ? AH_RED : pct > 0.5f ? 0xFFE0 : AH_GREEN);
    }

    int _spinIdx = 0;
    void _drawSendingScreen(const char* msg) {
        static AskState lastState = AskState::IDLE;
        if (_ahState != lastState) {
            lastState = _ahState;
            _disp->fillScreen(AH_BG);
            _disp->fillRect(0, 0, AH_SW, 56, AH_SURFACE);
            _disp->drawFastHLine(0, 55, AH_SW, AH_GRAY);
            _disp->setTextColor(AH_WHITE); _disp->setTextSize(2);
            _disp->setCursor(AH_PAD, 18); _disp->print("Ask Hub");
        }
        int cx = AH_SW / 2, cy = AH_SH / 2 - 20;
        for (int d = 0; d < 10; d++) {
            float angle = d * 2.0f * 3.14159f / 10;
            int dx = cx + (int)(44 * cosf(angle));
            int dy = cy + (int)(44 * sinf(angle));
            int dist = (10 + _spinIdx - d) % 10;
            uint16_t col = dist == 0 ? AH_WHITE : dist == 1 ? AH_GRAY :
                           dist == 2 ? AH_DARKGRAY : AH_SURFACE;
            _disp->fillCircle(dx, dy, 5, col);
        }
        _spinIdx = (_spinIdx + 1) % 10;
        _disp->fillRect(0, cy + 54, AH_SW, 20, AH_BG);
        _disp->setTextColor(AH_GRAY); _disp->setTextSize(2);
        _disp->setCursor(AH_SW / 2 - (int)(strlen(msg) * 6), cy + 56);
        _disp->print(msg);
    }

    void _drawError(const char* msg) {
        _disp->fillScreen(AH_BG);
        _disp->setTextColor(AH_RED); _disp->setTextSize(2);
        _disp->setCursor(AH_SW / 2 - (int)(strlen(msg) * 6), AH_SH / 2 - 8);
        _disp->print(msg);
    }

    void _drawResponseScreen(const char* transcript, const char* reply) {
        _disp->fillScreen(AH_BG);
        _disp->fillRect(0, 0, AH_SW, 56, AH_SURFACE);
        _disp->drawFastHLine(0, 55, AH_SW, AH_GRAY);
        _disp->setTextColor(AH_WHITE); _disp->setTextSize(2);
        _disp->setCursor(AH_PAD, 18); _disp->print("Ask Hub");
        _disp->fillRoundRect(AH_SW - 110, 10, 100, 36, 6, AH_GREEN);
        _disp->setTextColor(0x0000); _disp->setTextSize(1);
        _disp->setCursor(AH_SW - 100, 24); _disp->print("PLAYING...");

        _disp->setTextColor(AH_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AH_PAD, 72); _disp->print("YOU ASKED:");
        _disp->fillRoundRect(AH_PAD, 84, AH_SW - AH_PAD * 2, 80, 6, AH_SURFACE);
        _disp->drawRoundRect(AH_PAD, 84, AH_SW - AH_PAD * 2, 80, 6, AH_GRAY);
        _disp->setTextColor(AH_WHITE); _disp->setTextSize(2);
        _drawWrapped(transcript, AH_PAD + 8, 96, AH_SW - AH_PAD * 2 - 16, 2);

        _disp->setTextColor(AH_GRAY); _disp->setTextSize(1);
        _disp->setCursor(AH_PAD, 178); _disp->print("HUB SAYS:");
        _disp->fillRoundRect(AH_PAD, 190, AH_SW - AH_PAD * 2, 200, 6, 0x1842);
        _disp->drawRoundRect(AH_PAD, 190, AH_SW - AH_PAD * 2, 200, 6, AH_GREEN);
        _disp->setTextColor(AH_WHITE); _disp->setTextSize(2);
        _drawWrapped(reply, AH_PAD + 8, 202, AH_SW - AH_PAD * 2 - 16, 2);
    }

    void _drawWrapped(const char* text, int x, int y, int maxW, int textSize) {
        if (!text || !text[0]) return;
        _disp->setTextSize(textSize);
        int charW = textSize * 6;
        int lineH = textSize * 8 + 4;
        int cx = x, cy = y;
        char word[32]; int wi = 0;
        const char* p = text;
        auto flushWord = [&]() {
            if (wi == 0) return;
            word[wi] = '\0';
            int wordPx = wi * charW;
            if (cx + wordPx > x + maxW) { cx = x; cy += lineH; }
            _disp->setCursor(cx, cy);
            _disp->print(word);
            cx += wordPx + charW;
            wi = 0;
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
