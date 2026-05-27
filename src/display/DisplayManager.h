#pragma once

#include <Arduino_GigaDisplay_GFX.h>
#include "../types.h"

// ─────────────────────────────────────────────────────────────────────────────
// DisplayManager.h  —  Touchscreen UI for the Voice Hub
//
// Layout (480 x 800):
//   ┌─────────────────────────────┐
//   │  Header: title + WiFi dot   │  ~60px
//   ├─────────────────────────────┤
//   │  Last Detection card        │  ~160px
//   │  label + confidence bar     │
//   ├─────────────────────────────┤
//   │  Event log (scrolling)      │  ~460px
//   │  last 8 events              │
//   ├─────────────────────────────┤
//   │  Command buttons row        │  ~120px
//   └─────────────────────────────┘
//
// Libraries required (Arduino Library Manager):
//   - Arduino_GigaDisplay_GFX
//   - Arduino_GigaDisplayTouch
// ─────────────────────────────────────────────────────────────────────────────

namespace VoiceHub {

// ── Colour palette (RGB565) ───────────────────────────────────────────────────
#define CLR_BG       0x0861
#define CLR_SURFACE  0x2104
#define CLR_BORDER   0x39C7
#define CLR_GREEN    0x07E0
#define CLR_BLUE     0x051F
#define CLR_RED      0xF800
#define CLR_WHITE    0xFFFF
#define CLR_GRAY     0x8410
#define CLR_DARKGRAY 0x4208

// ── Layout constants ──────────────────────────────────────────────────────────
static constexpr int SCREEN_W  = 480;
static constexpr int SCREEN_H  = 800;
static constexpr int HDR_H     = 60;
static constexpr int DETECT_Y  = HDR_H + 8;
static constexpr int DETECT_H  = 160;
static constexpr int LOG_Y     = DETECT_Y + DETECT_H + 8;
static constexpr int LOG_H     = 460;
static constexpr int BTN_Y     = LOG_Y + LOG_H + 8;
static constexpr int BTN_H     = 100;
static constexpr int PADDING   = 12;
static constexpr int LOG_ROWS  = 8;
static constexpr int LOG_ROW_H = LOG_H / LOG_ROWS;

struct LogEntry {
    char  label[32];
    float confidence;
    bool  isKeyword;
};

class DisplayManager {
public:
    DisplayManager() : _display(), _wifiConnected(false), _logCount(0) {
        memset(_log, 0, sizeof(_log));
        _lastLabel[0]   = '\0';
        _lastConfidence = 0.0f;
    }

    void begin() {
        _display.begin();
        _display.setRotation(0);
        _display.fillScreen(CLR_BG);
        _drawHeader();
        _drawDetectCard();
        _drawLog();
        _drawButtons();
    }

    void setWiFiStatus(bool connected) {
        if (_wifiConnected == connected) return;
        _wifiConnected = connected;
        _drawHeader();
    }

    void showDetection(const char* label, float confidence) {
        strncpy(_lastLabel, label, sizeof(_lastLabel) - 1);
        _lastConfidence = confidence;
        _drawDetectCard();
        _addLogEntry(label, confidence, true);
    }

    void showStatus(const char* message) {
        _addLogEntry(message, -1.0f, false);
    }

    // Call every loop
    void update() { }

private:
    GigaDisplay_GFX _display;
    bool            _wifiConnected;
    char            _lastLabel[32];
    float           _lastConfidence;
    LogEntry        _log[LOG_ROWS];
    int             _logCount;

    void _drawHeader() {
        _display.fillRect(0, 0, SCREEN_W, HDR_H, CLR_BG);

        _display.setTextColor(CLR_WHITE);
        _display.setTextSize(2);
        _display.setCursor(PADDING, 18);
        _display.print("Voice Hub");

        uint16_t dotColor = _wifiConnected ? CLR_GREEN : CLR_DARKGRAY;
        _display.fillCircle(SCREEN_W - PADDING - 30, HDR_H / 2, 8, dotColor);

        _display.setTextSize(1);
        _display.setTextColor(_wifiConnected ? CLR_GREEN : CLR_GRAY);
        _display.setCursor(SCREEN_W - PADDING - 90, HDR_H / 2 - 4);
        _display.print(_wifiConnected ? "Online " : "Offline");

        _display.drawFastHLine(0, HDR_H - 1, SCREEN_W, CLR_BORDER);
    }

    void _drawDetectCard() {
        int x = PADDING, y = DETECT_Y;
        int w = SCREEN_W - PADDING * 2, h = DETECT_H;

        _display.fillRoundRect(x, y, w, h, 8, CLR_SURFACE);
        _display.drawRoundRect(x, y, w, h, 8, CLR_BORDER);

        _display.setTextSize(1);
        _display.setTextColor(CLR_GRAY);
        _display.setCursor(x + PADDING, y + 10);
        _display.print("LAST DETECTION");

        if (_lastLabel[0] == '\0') {
            _display.setTextSize(2);
            _display.setTextColor(CLR_DARKGRAY);
            _display.setCursor(x + PADDING, y + 50);
            _display.print("Waiting...");
            return;
        }

        _display.setTextSize(3);
        _display.setTextColor(CLR_WHITE);
        _display.setCursor(x + PADDING, y + 38);
        _display.print(_lastLabel);

        _display.setTextSize(2);
        _display.setTextColor(CLR_GREEN);
        _display.setCursor(x + PADDING, y + 90);
        _display.print((int)(_lastConfidence * 100.0f));
        _display.print("%");

        // Confidence bar
        int barX = x + PADDING, barY = y + h - 28;
        int barW = w - PADDING * 2, barH = 12;
        _display.fillRoundRect(barX, barY, barW, barH, 4, CLR_BORDER);

        int fillW = (int)(barW * _lastConfidence);
        uint16_t barColor = _lastConfidence > 0.8f ? CLR_GREEN :
                            _lastConfidence > 0.5f ? CLR_BLUE  : CLR_RED;
        if (fillW > 0)
            _display.fillRoundRect(barX, barY, fillW, barH, 4, barColor);
    }

    void _drawLog() {
        int x = PADDING, y = LOG_Y;
        int w = SCREEN_W - PADDING * 2;

        _display.fillRoundRect(x, y, w, LOG_H, 8, CLR_SURFACE);
        _display.drawRoundRect(x, y, w, LOG_H, 8, CLR_BORDER);

        _display.setTextSize(1);
        _display.setTextColor(CLR_GRAY);
        _display.setCursor(x + PADDING, y + 10);
        _display.print("EVENT LOG");

        if (_logCount == 0) {
            _display.setTextColor(CLR_DARKGRAY);
            _display.setCursor(x + PADDING, y + LOG_H / 2 - 4);
            _display.print("No events yet");
            return;
        }

        int startIdx = (_logCount < LOG_ROWS) ? 0 : (_logCount - LOG_ROWS);
        int drawn    = 0;
        for (int i = _logCount - 1; i >= startIdx && drawn < LOG_ROWS; i--, drawn++) {
            _drawLogRow(drawn, _log[i % LOG_ROWS]);
        }
    }

    void _drawLogRow(int row, const LogEntry& entry) {
        int x = PADDING * 2;
        int y = LOG_Y + 28 + row * LOG_ROW_H;

        uint16_t rowBg = (row % 2 == 0) ? CLR_SURFACE : CLR_BG;
        _display.fillRect(PADDING + 1, y, SCREEN_W - PADDING * 2 - 2, LOG_ROW_H - 2, rowBg);

        uint16_t badgeColor = entry.isKeyword ? CLR_GREEN : CLR_BLUE;
        _display.fillCircle(x + 6, y + LOG_ROW_H / 2, 4, badgeColor);

        _display.setTextSize(1);
        _display.setTextColor(CLR_WHITE);
        _display.setCursor(x + 18, y + LOG_ROW_H / 2 - 4);
        _display.print(entry.label);

        if (entry.isKeyword && entry.confidence >= 0) {
            _display.setTextColor(CLR_GRAY);
            _display.setCursor(SCREEN_W - PADDING * 4 - 30, y + LOG_ROW_H / 2 - 4);
            _display.print((int)(entry.confidence * 100));
            _display.print("%");
        }
    }

    void _drawButtons() {
        const char* labels[] = { "Mute", "Unmute", "Status", "Reboot" };
        uint16_t    colors[] = { CLR_BLUE, CLR_GREEN, CLR_GRAY, CLR_RED };
        int btnCount = 4;
        int btnW     = (SCREEN_W - PADDING * (btnCount + 1)) / btnCount;
        int btnH     = BTN_H - PADDING * 2;

        for (int i = 0; i < btnCount; i++) {
            int bx = PADDING + i * (btnW + PADDING);
            int by = BTN_Y + PADDING;
            _display.fillRoundRect(bx, by, btnW, btnH, 8, colors[i]);
            _display.setTextSize(1);
            _display.setTextColor(CLR_WHITE);
            int tx = bx + (btnW / 2) - (strlen(labels[i]) * 3);
            int ty = by + btnH / 2 - 4;
            _display.setCursor(tx, ty);
            _display.print(labels[i]);
        }
    }

    void _addLogEntry(const char* label, float confidence, bool isKeyword) {
        int idx = _logCount % LOG_ROWS;
        strncpy(_log[idx].label, label, sizeof(_log[idx].label) - 1);
        _log[idx].confidence = confidence;
        _log[idx].isKeyword  = isKeyword;
        _logCount++;
        _drawLog();
    }
};

} // namespace VoiceHub
