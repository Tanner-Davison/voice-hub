#pragma once

#include <Arduino_GigaDisplay_GFX.h>
#include <Arduino_GigaDisplayTouch.h>
#include <Arduino_GigaDisplay.h>
#include "../types.h"

namespace VoiceHub {

// ── Colour palette (RGB565) ───────────────────────────────────────────────────
#define CLR_BG       0x2104
#define CLR_SURFACE  0x2104
#define CLR_BORDER   0x39C7
#define CLR_GREEN    0x07E0
#define CLR_BLUE     0x051F
#define CLR_RED      0xF800
#define CLR_WHITE    0xFFFF
#define CLR_GRAY     0x8410
#define CLR_DARKGRAY 0x4208

// ── Layout constants ──────────────────────────────────────────────────────────
static constexpr int SCREEN_W   = 800;
static constexpr int SCREEN_H   = 480;
static constexpr int HDR_H      = 60;
static constexpr int PADDING    = 12;

static constexpr int LEFT_W     = 480;
static constexpr int DETECT_Y   = HDR_H + 8;
static constexpr int DETECT_H   = 140;
static constexpr int LOG_Y      = DETECT_Y + DETECT_H + 8;
static constexpr int LOG_H      = SCREEN_H - LOG_Y - 8;
static constexpr int LOG_ROWS   = 5;       // visible rows
static constexpr int LOG_BUF    = 20;      // total stored entries
static constexpr int LOG_ROW_H  = LOG_H / LOG_ROWS;

static constexpr int RIGHT_X    = LEFT_W + 8;
static constexpr int RIGHT_W    = SCREEN_W - RIGHT_X - 8;
static constexpr int BTN_Y      = HDR_H + 8;
static constexpr int BTN_H      = 80;

enum class TouchResult { NONE, SCROLL, SETTINGS_TAP, MUTE_TAP, UNMUTE_TAP, STATUS_TAP, ASK_TAP, ASK_WIN_TAP };

struct LogEntry {
    char  label[32];
    float confidence;
    bool  isKeyword;
    bool  used;
};

class DisplayManager {
public:
    DisplayManager() : _display(nullptr), _touch(nullptr),
                       _wifiConnected(false), _micReady(false),
                       _logCount(0), _scrollOffset(0),
                       _lastTouchY(-1), _touchActive(false), _touchVelocity(0) {
        memset(_log, 0, sizeof(_log));
        _lastLabel[0]   = '\0';
        _lastConfidence = 0.0f;
    }

    ~DisplayManager() {
        delete _display;
        delete _touch;
    }

    void begin() {
        _rgb.begin();
        _rgb.off();
        _display = new GigaDisplay_GFX();
        _touch   = new Arduino_GigaDisplayTouch();
        _display->begin();
        _touch->begin();
        delay(500);

        _display->setRotation(1);
        _display->fillScreen(0x18C3);
        delay(100);
        _drawHeader();
        _drawDetectCard();
        _drawLog();
        _drawButtons();
    }

    void setWiFiStatus(bool connected) {
        if (!_display || _wifiConnected == connected) return;
        _wifiConnected = connected;
        _drawHeader();
    }

    // Force header repaint regardless of cached state -- use after returning
    // from an overlay where the underlying value may have changed undetected
    void forceWiFiStatus(bool connected) {
        if (!_display) return;
        _wifiConnected = connected;
        _drawHeader();
    }

    void setMicStatus(bool ready) {
        if (!_display || _micReady == ready) return;
        _micReady = ready;
        _drawHeader();
    }

    void showDetection(const char* label, float confidence) {
        if (!_display) return;
        strncpy(_lastLabel, label, sizeof(_lastLabel) - 1);
        _lastConfidence = confidence;
        _drawDetectCard();
        _addLogEntry(label, confidence, true);
    }

    void showStatus(const char* message) {
        if (!_display) return;
        // Don’t interrupt a touch gesture with a redraw
        if (!_touchActive) _addLogEntry(message, -1.0f, false);
    }

    // Call every loop — handles touch scrolling
    GigaDisplay_GFX* getDisplay() { return _display; }
    Arduino_GigaDisplayTouch* getTouch() { return _touch; }

    // Full repaint — call after returning from an overlay (e.g. WiFiSettings)
    void redraw() {
        if (!_display) return;
        _display->fillScreen(CLR_BG);
        _drawHeader();
        _drawDetectCard();
        _drawLog();
        _drawButtons();
        // Reset touch state so old gestures don’t bleed through
        _touchActive   = false;
        _btnDown       = false;
        _lastBtnFireMs = 0;
        _touchVelocity = 0;
        _lastTouchY    = -1;
        _pendingResult = TouchResult::NONE;
    }

    // Single touch poll — update() owns getTouchPoints(), never call it elsewhere
    TouchResult update() {
        if (!_display || !_touch) return TouchResult::NONE;

        uint8_t contacts;
        GDTpoint_t points[5];
        contacts = _touch->getTouchPoints(points);

        if (contacts > 0) {
            int raw_tx = points[0].x;
            int raw_ty = points[0].y;
            int tx = raw_ty;
            int ty = SCREEN_H - raw_tx;

            // Button region -- fire every time finger is down here,
            // but debounce by time so it doesn't repeat faster than 600ms
            if (tx >= RIGHT_X) {
                unsigned long now = millis();
                if (!_btnDown || (now - _lastBtnFireMs) > 600) {
                    _btnDown = true;
                    _lastBtnFireMs = now;
                    TouchResult btn = _checkButtonTap(tx, ty);
                    Serial.print("[BTN] tx="); Serial.print(tx);
                    Serial.print(" ty="); Serial.print(ty);
                    Serial.print(" btn="); Serial.println((int)btn);
                    if (btn != TouchResult::NONE) {
                        _pendingResult = btn;
                        return btn;
                    }
                }
                _touchActive = false;
                return TouchResult::NONE;
            }
            _btnDown = false;

            // ── Log scroll region ───────────────────────────────────────
            if (tx < LEFT_W && ty > LOG_Y && ty < LOG_Y + LOG_H) {
                if (!_touchActive) {
                    // First contact -- baseline, don't scroll yet
                    _touchActive   = true;
                    _lastTouchY    = ty;
                    _touchVelocity = 0;
                } else {
                    int delta = _lastTouchY - ty;
                    // Always update baseline so we track cumulative movement
                    _lastTouchY = ty;
                    if (abs(delta) > 3) {
                        int maxScroll = max(0, (int)_logCount - LOG_ROWS);
                        int steps = max(1, abs(delta) / 8);
                        _touchVelocity = (delta > 0) ? steps : -steps;
                        int newOffset = constrain(_scrollOffset + _touchVelocity, 0, maxScroll);
                        if (newOffset != _scrollOffset) {
                            _scrollOffset = newOffset;
                            _drawLog();
                        }
                    }
                }
            }
        } else {
            // Finger lifted
            _btnDown = false;
            if (_touchActive && _touchVelocity != 0) {
                int maxScroll = max(0, (int)_logCount - LOG_ROWS);
                int newOffset = constrain(_scrollOffset + _touchVelocity / 2, 0, maxScroll);
                if (newOffset != _scrollOffset) {
                    _scrollOffset = newOffset;
                    _drawLog();
                }
                _touchVelocity /= 2;
            } else {
                _touchVelocity = 0;
            }
            _touchActive = false;
            _lastTouchY  = -1;
        }

        TouchResult pending = _pendingResult;
        if (pending != TouchResult::NONE) {
            _pendingResult = TouchResult::NONE;
            return pending;
        }
        return TouchResult::NONE;
    }

private:
    GigaDisplay_GFX*        _display;
    Arduino_GigaDisplayTouch* _touch;
    GigaDisplayRGB            _rgb;
    bool    _wifiConnected;
    bool    _micReady;
    char    _lastLabel[32];
    float   _lastConfidence;
    LogEntry _log[LOG_BUF];
    int     _logCount;
    int     _scrollOffset; // 0 = newest at top, positive = scroll back in time
    int     _lastTouchY;
    bool    _touchActive;
    int     _touchVelocity;
    bool    _btnDown = false;
    unsigned long _lastBtnFireMs = 0;
    TouchResult _pendingResult = TouchResult::NONE;

    void _drawHeader() {
        _display->fillRect(0, 0, SCREEN_W, HDR_H, CLR_BG);

        _display->setTextColor(CLR_WHITE);
        _display->setTextSize(2);
        _display->setCursor(PADDING, 18);
        _display->print("Voice Hub");

        // WiFi dot
        uint16_t wifiColor = _wifiConnected ? CLR_GREEN : CLR_DARKGRAY;
        _display->fillCircle(SCREEN_W - PADDING - 30, HDR_H / 2, 8, wifiColor);
        _display->setTextSize(1);
        _display->setTextColor(_wifiConnected ? CLR_GREEN : CLR_GRAY);
        _display->setCursor(SCREEN_W - PADDING - 90, HDR_H / 2 - 4);
        _display->print(_wifiConnected ? "Online " : "Offline");

        // Mic dot
        uint16_t micColor = _micReady ? CLR_GREEN : CLR_DARKGRAY;
        _display->fillCircle(SCREEN_W - PADDING - 160, HDR_H / 2, 8, micColor);
        _display->setTextSize(1);
        _display->setTextColor(_micReady ? CLR_GREEN : CLR_GRAY);
        _display->setCursor(SCREEN_W - PADDING - 210, HDR_H / 2 - 4);
        _display->print(_micReady ? "Mic OK" : "No Mic");

        _display->drawFastHLine(0, HDR_H - 1, SCREEN_W, CLR_BORDER);
        _display->drawFastVLine(LEFT_W, HDR_H, SCREEN_H - HDR_H, CLR_BORDER);
    }

    void _drawDetectCard() {
        int x = PADDING, y = DETECT_Y;
        int w = LEFT_W - PADDING * 2, h = DETECT_H;

        _display->fillRoundRect(x, y, w, h, 8, CLR_SURFACE);
        _display->drawRoundRect(x, y, w, h, 8, CLR_BORDER);

        _display->setTextSize(1);
        _display->setTextColor(CLR_GRAY);
        _display->setCursor(x + PADDING, y + 10);
        _display->print("LAST DETECTION");

        if (_lastLabel[0] == '\0') {
            _display->setTextSize(2);
            _display->setTextColor(CLR_DARKGRAY);
            _display->setCursor(x + PADDING, y + 45);
            _display->print("Waiting...");
            return;
        }

        _display->setTextSize(3);
        _display->setTextColor(CLR_WHITE);
        _display->setCursor(x + PADDING, y + 32);
        _display->print(_lastLabel);

        _display->setTextSize(2);
        _display->setTextColor(CLR_GREEN);
        _display->setCursor(x + PADDING, y + 80);
        _display->print((int)(_lastConfidence * 100.0f));
        _display->print("%");

        int barX = x + PADDING, barY = y + h - 22;
        int barW = w - PADDING * 2, barH = 10;
        _display->fillRoundRect(barX, barY, barW, barH, 4, CLR_BORDER);
        int fillW = (int)(barW * _lastConfidence);
        uint16_t barColor = _lastConfidence > 0.8f ? CLR_GREEN :
                            _lastConfidence > 0.5f ? CLR_BLUE  : CLR_RED;
        if (fillW > 0)
            _display->fillRoundRect(barX, barY, fillW, barH, 4, barColor);
    }

    void _drawLog() {
        int x = PADDING, y = LOG_Y;
        int w = LEFT_W - PADDING * 2;

        _display->fillRoundRect(x, y, w, LOG_H, 8, CLR_SURFACE);
        _display->drawRoundRect(x, y, w, LOG_H, 8, CLR_BORDER);

        // Header with scroll indicator
        _display->setTextSize(1);
        _display->setTextColor(CLR_GRAY);
        _display->setCursor(x + PADDING, y + 10);
        _display->print("EVENT LOG");

        if (_logCount > LOG_ROWS) {
            _display->setCursor(x + w - 60, y + 10);
            _display->setTextColor(CLR_DARKGRAY);
            _display->print(_scrollOffset > 0 ? "^ scroll" : "swipe ^");
        }

        if (_logCount == 0) {
            _display->setTextColor(CLR_DARKGRAY);
            _display->setCursor(x + PADDING, y + LOG_H / 2 - 4);
            _display->print("No events yet");
            return;
        }

        // Draw LOG_ROWS entries starting from newest minus scroll offset
        // _logCount-1 is newest, _logCount-1-_scrollOffset is top of visible window
        int topIdx = _logCount - 1 - _scrollOffset;
        for (int row = 0; row < LOG_ROWS; row++) {
            int entryIdx = topIdx - row;
            if (entryIdx < 0) break;
            _drawLogRow(row, _log[entryIdx % LOG_BUF]);
        }

        // Scrollbar
        if (_logCount > LOG_ROWS) {
            int sbX    = x + w - 6;
            int sbH    = LOG_H - 28;
            int sbY    = y + 28;
            int maxScroll = _logCount - LOG_ROWS;
            int thumbH = max(20, sbH * LOG_ROWS / _logCount);
            int thumbY = sbY + (sbH - thumbH) * _scrollOffset / maxScroll;
            _display->fillRect(sbX, sbY, 4, sbH, CLR_BORDER);
            _display->fillRect(sbX, thumbY, 4, thumbH, CLR_GRAY);
        }
    }

    void _drawLogRow(int row, const LogEntry& entry) {
        int x = PADDING * 2;
        int y = LOG_Y + 28 + row * LOG_ROW_H;

        uint16_t rowBg = (row % 2 == 0) ? CLR_SURFACE : CLR_BG;
        _display->fillRect(PADDING + 1, y, LEFT_W - PADDING * 2 - 8, LOG_ROW_H - 2, rowBg);

        uint16_t badgeColor = entry.isKeyword ? CLR_GREEN : CLR_BLUE;
        _display->fillCircle(x + 6, y + LOG_ROW_H / 2, 4, badgeColor);

        _display->setTextSize(1);
        _display->setTextColor(CLR_WHITE);
        _display->setCursor(x + 18, y + LOG_ROW_H / 2 - 4);
        _display->print(entry.label);

        if (entry.isKeyword && entry.confidence >= 0) {
            _display->setTextColor(CLR_GRAY);
            _display->setCursor(LEFT_W - PADDING * 3 - 30, y + LOG_ROW_H / 2 - 4);
            _display->print((int)(entry.confidence * 100));
            _display->print("%");
        }
    }

    void _drawButtons() {
        const char* labels[] = { "Mute", "Unmute", "Ask Windows", "WiFi Settings", "Ask Hub" };
        uint16_t    colors[] = { CLR_BLUE, CLR_GREEN, 0xFD20, 0x4A10, 0xF81F };
        int btnCount = 5;
        int btnW     = RIGHT_W;
        // 5 buttons -- reduce height to fit
        int totalGap = PADDING * (btnCount - 1);
        int btnH     = (SCREEN_H - BTN_Y - PADDING - totalGap) / btnCount;

        for (int i = 0; i < btnCount; i++) {
            int bx = RIGHT_X + PADDING;
            int by = BTN_Y + i * (btnH + PADDING);
            _display->fillRoundRect(bx, by, btnW - PADDING, btnH, 8, colors[i]);
            _display->setTextSize(1);
            _display->setTextColor(CLR_WHITE);
            int tx = bx + (btnW / 2) - (strlen(labels[i]) * 3);
            int ty = by + btnH / 2 - 4;
            _display->setCursor(tx, ty);
            _display->print(labels[i]);
        }
    }

    // Maps a tap in the right-panel button area to a TouchResult.
    // Index: 0=Mute, 1=Unmute, 2=Status, 3=Settings
    TouchResult _checkButtonTap(int tx, int ty) {
        int btnW = RIGHT_W;
        int totalGap = PADDING * 4;
        int btnH = (SCREEN_H - BTN_Y - PADDING - totalGap) / 5;
        int bx   = RIGHT_X + PADDING;
        static const TouchResult results[] = {
            TouchResult::MUTE_TAP,
            TouchResult::UNMUTE_TAP,
            TouchResult::ASK_WIN_TAP,
            TouchResult::SETTINGS_TAP,
            TouchResult::ASK_TAP
        };
        for (int i = 0; i < 5; i++) {
            int by = BTN_Y + i * (btnH + PADDING);
            if (tx >= bx && tx <= bx + btnW - PADDING &&
                ty >= by && ty <= by + btnH) {
                return results[i];
            }
        }
        return TouchResult::NONE;
    }

    void _addLogEntry(const char* label, float confidence, bool isKeyword) {
        int idx = _logCount % LOG_BUF;
        strncpy(_log[idx].label, label, sizeof(_log[idx].label) - 1);
        _log[idx].label[sizeof(_log[idx].label) - 1] = '\0';
        _log[idx].confidence = confidence;
        _log[idx].isKeyword  = isKeyword;
        _log[idx].used       = true;
        _logCount++;
        // Auto-scroll to newest when new entry arrives
        _scrollOffset = 0;
        _drawLog();
    }
};

} // namespace VoiceHub
