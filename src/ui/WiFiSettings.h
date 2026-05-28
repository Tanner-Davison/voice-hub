#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include <Arduino_GigaDisplay_GFX.h>
#include <Arduino_GigaDisplayTouch.h>
#include <kvstore_global_api.h>
#include "../wifi/WiFiManager.h"

// -----------------------------------------------------------------------------
// WiFiSettings.h  --  Full-screen WiFi settings UI
// Screens: NETWORKS, PASSWORD, CONNECTING, STATS, FAILED
// -----------------------------------------------------------------------------

namespace VoiceHub {

// -- Colors -------------------------------------------------------------------
#define WS_BG        0x0841
#define WS_SURFACE   0x2104
#define WS_BORDER    0x39C7
#define WS_GREEN     0x07E0
#define WS_BLUE      0x051F
#define WS_RED       0xF800
#define WS_WHITE     0xFFFF
#define WS_GRAY      0x8410
#define WS_DARKGRAY  0x4208
#define WS_ACCENT    0x041F
#define WS_ORANGE    0xFD00
#define WS_KEY_PRESS 0x7BEF

// -- Layout -------------------------------------------------------------------
constexpr int SW = 800;
constexpr int SH = 480;
constexpr int HDR = 56;
constexpr int PAD = 10;

constexpr int FIELD_Y = HDR + 12;
constexpr int FIELD_H = 46;

constexpr int KB_Y      = 130;
constexpr int KB_KEY_H  = 52;
constexpr int KB_KEY_W  = 66;
constexpr int KB_GAP    = 5;
constexpr int KB_ROWS_N = 3;

constexpr int KB_ACTION_Y  = KB_Y + KB_ROWS_N * (KB_KEY_H + KB_GAP) + KB_GAP;
constexpr int KB_CONNECT_Y = KB_ACTION_Y + KB_KEY_H + KB_GAP + 6;
constexpr int KB_CONNECT_H = SH - KB_CONNECT_Y - PAD;

constexpr int ACT_SHIFT_W = 90;
constexpr int ACT_BACK_W  = 90;
constexpr int ACT_NUM_W   = 70;
constexpr int ACT_SPACE_X = PAD + ACT_SHIFT_W + KB_GAP;
constexpr int ACT_SPACE_W = SW - PAD * 2 - ACT_SHIFT_W - ACT_BACK_W - ACT_NUM_W - KB_GAP * 3;
constexpr int ACT_NUM_X   = ACT_SPACE_X + ACT_SPACE_W + KB_GAP;
constexpr int ACT_BACK_X  = ACT_NUM_X + ACT_NUM_W + KB_GAP;

enum class WifiScreen { NETWORKS, PASSWORD, CONNECTING, STATS, FAILED };
enum class ShiftState  { OFF, ONCE, LOCK };

static const char* KB_ROWS[]    = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
static const char* KB_ROWS_LO[] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const char* KB_NUMS[]    = { "1234567890", "!@#$%^&*()", "-_=+[]{}|" };

class WiFiSettings {
public:
    WiFiSettings() : _active(false), _screen(WifiScreen::NETWORKS),
                     _selectedNet(-1), _shiftState(ShiftState::OFF), _numMode(false),
                     _scanCount(0), _lastTouchTime(0), _pressedKey(-1),
                     _wifiManager(nullptr) {
        memset(_ssidBuf, 0, sizeof(_ssidBuf));
        memset(_passBuf, 0, sizeof(_passBuf));
        memset(_scannedSSIDs, 0, sizeof(_scannedSSIDs));
    }

    void loadSaved(char* ssidOut, char* passOut) {
        size_t len = 33;
        kv_get("wifi_ssid", ssidOut, len, &len);
        len = 64;
        kv_get("wifi_pass", passOut, len, &len);
    }

    // Pass WiFiManager so we can check current connection and pull live stats
    void open(GigaDisplay_GFX* disp, Arduino_GigaDisplayTouch* touch, WiFiManager* wm = nullptr) {
        _disp        = disp;
        _touch       = touch;
        _wifiManager = wm;
        _active      = true;
        _needsRedraw = false;
        _screen      = WifiScreen::NETWORKS;
        _selectedNet = -1;
        memset(_passBuf, 0, sizeof(_passBuf));
        _passLen = 0;
        _scanNetworks();
        _drawNetworkList();
        _drainTouch();
    }

    bool isActive()    const { return _active; }
    bool needsRedraw() const { return _needsRedraw; }
    void clearRedraw()       { _needsRedraw = false; }

    bool update(char* ssidOut, char* passOut) {
        if (!_active) return false;

        uint8_t contacts;
        GDTpoint_t pts[5];
        contacts = _touch->getTouchPoints(pts);
        unsigned long now = millis();

        if (contacts > 0) {
            if (now - _lastTouchTime > 120) {
                _lastTouchTime = now;
                int sx = pts[0].y;
                int sy = SH - pts[0].x;
                _handleTouch(sx, sy);
            }
        } else {
            _pressedKey = -1;
        }

        if (_connectReady) {
            _connectReady = false;
            _passBuf[_passLen] = '\0';
            _drawConnectingScreen(1, 0);  // initial state
            _instance = this;
            bool ok = _wifiManager ? _wifiManager->connect(_ssidBuf, _passBuf,
                [](int attempt, int tick) {
                    if (_instance) _instance->_drawConnectingScreen(attempt, tick);
                }) : false;
            if (ok) {
                // Save credentials and signal sketch to reconnect
                kv_set("wifi_ssid", _ssidBuf, strlen(_ssidBuf) + 1, 0);
                kv_set("wifi_pass", _passBuf, strlen(_passBuf) + 1, 0);
                strncpy(ssidOut, _ssidBuf, 33);
                strncpy(passOut, _passBuf, 64);
                // Show stats screen on success instead of immediately closing
                _screen = WifiScreen::STATS;
                _drawStatsScreen();
                _drainTouch();
                // Don't close yet -- user will tap "Done" or back
            } else {
                _screen = WifiScreen::FAILED;
                _drawFailedScreen();
                _drainTouch();
            }
            return ok;  // tell sketch to update wifi status
        }
        return false;
    }

private:
    GigaDisplay_GFX*          _disp;
    Arduino_GigaDisplayTouch* _touch;
    WiFiManager*              _wifiManager;
    bool        _active;
    WifiScreen  _screen;
    int         _selectedNet;
    ShiftState  _shiftState;
    bool        _numMode;
    bool        _connectReady = false;
    bool        _showPass     = false;
    static WiFiSettings* _instance;  // for static progress callback trampoline
    char        _ssidBuf[33];
    char        _passBuf[64];
    int         _passLen;
    char        _scannedSSIDs[10][33];
    int8_t      _scannedRSSI[10];
    int         _scanCount;
    unsigned long _lastTouchTime;
    int         _pressedKey;
    int         _netScrollOffset = 0;
    bool        _needsRedraw     = false;

    // -------------------------------------------------------------------------
    void _drainTouch(unsigned long cooldownMs = 50) {
        uint8_t contacts;
        GDTpoint_t pts[5];
        do { contacts = _touch->getTouchPoints(pts); delay(10); } while (contacts > 0);
        delay(cooldownMs);
        _lastTouchTime = 0;
    }

    // -- Scan -----------------------------------------------------------------
    void _scanNetworks() {
        _disp->fillScreen(WS_BG);
        _disp->setTextColor(WS_WHITE);
        _disp->setTextSize(2);
        _disp->setCursor(SW / 2 - 80, SH / 2 - 10);
        _disp->print("Scanning WiFi...");
        int n = WiFi.scanNetworks();
        _scanCount = min(n, 10);
        for (int i = 0; i < _scanCount; i++) {
            strncpy(_scannedSSIDs[i], WiFi.SSID(i), 32);
            _scannedRSSI[i] = WiFi.RSSI(i);
        }
    }

    // -- Network list ---------------------------------------------------------
    void _drawNetworkList() {
        _disp->fillScreen(WS_BG);
        _drawHeader("WiFi Networks", true);

        constexpr int RESCAN_H = 60;
        constexpr int RESCAN_Y = SH - RESCAN_H;
        int rowH       = 52;
        int maxVisible = (RESCAN_Y - HDR - PAD) / rowH;

        if (_scanCount == 0) {
            _disp->setTextColor(WS_GRAY);
            _disp->setTextSize(2);
            _disp->setCursor(PAD * 2, HDR + 40);
            _disp->print("No networks found.");
        } else {
            // Mark currently connected network
            String currentSSID = _wifiManager ? _wifiManager->connectedSSID() : String("");
            for (int i = 0; i < min(_scanCount, maxVisible); i++) {
                int idx = i + _netScrollOffset;
                if (idx >= _scanCount) break;
                bool isConnected = (currentSSID.length() > 0 &&
                                    strcmp(_scannedSSIDs[idx], currentSSID.c_str()) == 0);
                _drawNetworkRow(i, idx, false, isConnected);
            }
        }

        _disp->fillRect(0, RESCAN_Y, SW, RESCAN_H, WS_ACCENT);
        _disp->drawFastHLine(0, RESCAN_Y, SW, WS_BORDER);
        _disp->setTextColor(WS_WHITE);
        _disp->setTextSize(2);
        _disp->setCursor(SW / 2 - 42, RESCAN_Y + RESCAN_H / 2 - 8);
        _disp->print("Rescan");
    }

    void _drawNetworkRow(int row, int idx, bool highlighted, bool isCurrentNetwork = false) {
        int rowH = 52;
        int y    = HDR + 8 + row * rowH;
        uint16_t bg = highlighted ? WS_BLUE : WS_SURFACE;
        _disp->fillRoundRect(PAD, y, SW - PAD * 2, rowH - 4, 6, bg);
        uint16_t border = isCurrentNetwork ? WS_GREEN : WS_BORDER;
        _disp->drawRoundRect(PAD, y, SW - PAD * 2, rowH - 4, 6, border);

        // Signal bars -- anchored to right edge, always drawn first
        int rssi = _scannedRSSI[idx];
        uint16_t sigColor = rssi > -60 ? WS_GREEN : rssi > -75 ? 0xFFE0 : WS_GRAY;
        constexpr int BAR_RIGHT = SW - PAD * 2 - 8;  // right edge of bar group
        constexpr int BAR_W     = 7;
        constexpr int BAR_GAP   = 3;
        constexpr int BAR_COUNT = 4;
        int barsLeft = BAR_RIGHT - BAR_COUNT * (BAR_W + BAR_GAP) + BAR_GAP;
        for (int b = 0; b < BAR_COUNT; b++) {
            int bh = 6 + b * 5;
            int bx = barsLeft + b * (BAR_W + BAR_GAP);
            int by = y + rowH / 2 - bh / 2;
            uint16_t bc = (rssi > -90 + b * 15) ? sigColor : WS_DARKGRAY;
            _disp->fillRect(bx, by, BAR_W, bh, bc);
        }

        // "connected" label -- sits left of the bars with a clear gap
        if (isCurrentNetwork) {
            constexpr int LABEL_GAP = 10;  // gap between label right edge and bars
            // "connected" at textSize(1) = 9 chars * 6px = 54px wide
            int labelX = barsLeft - LABEL_GAP - 54;
            _disp->setTextColor(WS_GREEN);
            _disp->setTextSize(1);
            _disp->setCursor(labelX, y + rowH / 2 - 4);
            _disp->print("connected");
        }

        // SSID -- left side
        _disp->setTextColor(WS_WHITE);
        _disp->setTextSize(2);
        _disp->setCursor(PAD * 2 + 4, y + rowH / 2 - 8);
        _disp->print(_scannedSSIDs[idx]);
    }

    // -- Stats screen ---------------------------------------------------------
    void _drawStatsScreen() {
        _disp->fillScreen(WS_BG);
        _drawHeader(_wifiManager ? _wifiManager->connectedSSID().c_str() : "WiFi Info", true);

        if (!_wifiManager || !_wifiManager->isConnected()) {
            _disp->setTextColor(WS_GRAY);
            _disp->setTextSize(2);
            _disp->setCursor(PAD * 2, HDR + 40);
            _disp->print("Not connected");
            return;
        }

        int32_t rssi    = _wifiManager->rssi();
        String  ip      = _wifiManager->localIP();
        String  gateway = _wifiManager->gatewayIP();
        String  subnet  = _wifiManager->subnetMask();
        String  mac     = _wifiManager->macAddress();

        // Signal strength bar
        int barAreaY = HDR + 20;
        int barMaxW  = SW / 2 - PAD * 4;
        // RSSI typically -30 (great) to -90 (poor)
        int pct      = constrain(map(rssi, -90, -30, 0, 100), 0, 100);
        uint16_t sigColor = pct > 66 ? WS_GREEN : pct > 33 ? 0xFFE0 : WS_RED;

        _disp->setTextColor(WS_GRAY);
        _disp->setTextSize(1);
        _disp->setCursor(PAD * 2, barAreaY);
        _disp->print("SIGNAL");

        _disp->fillRoundRect(PAD * 2, barAreaY + 14, barMaxW, 18, 4, WS_SURFACE);
        _disp->fillRoundRect(PAD * 2, barAreaY + 14, barMaxW * pct / 100, 18, 4, sigColor);
        _disp->drawRoundRect(PAD * 2, barAreaY + 14, barMaxW, 18, 4, WS_BORDER);

        _disp->setTextColor(sigColor);
        _disp->setTextSize(1);
        _disp->setCursor(PAD * 2 + barMaxW + 8, barAreaY + 18);
        _disp->print(rssi);
        _disp->print(" dBm");

        // Stats table
        struct { const char* label; String value; } rows[] = {
            { "IP Address",  ip      },
            { "Gateway",     gateway },
            { "Subnet",      subnet  },
            { "MAC",         mac     },
        };
        int tableY  = barAreaY + 48;
        int rowH    = 44;
        for (int i = 0; i < 4; i++) {
            int ry = tableY + i * rowH;
            uint16_t rowBg = (i % 2 == 0) ? WS_SURFACE : WS_BG;
            _disp->fillRect(PAD, ry, SW - PAD * 2, rowH - 2, rowBg);
            _disp->drawFastHLine(PAD, ry, SW - PAD * 2, WS_BORDER);

            _disp->setTextColor(WS_GRAY);
            _disp->setTextSize(1);
            _disp->setCursor(PAD * 2, ry + 8);
            _disp->print(rows[i].label);

            _disp->setTextColor(WS_WHITE);
            _disp->setTextSize(2);
            _disp->setCursor(PAD * 2, ry + 22);
            _disp->print(rows[i].value);
        }

        // Done button at bottom
        _drawButton(PAD, SH - 56, SW - PAD * 2, 46, "Done", WS_ACCENT);
    }

    // -- Failed screen --------------------------------------------------------
    void _drawFailedScreen() {
        _disp->fillScreen(WS_BG);
        _drawHeader("Connection Failed", true);

        // Big X icon area
        int cx = SW / 2, cy = HDR + 90;
        _disp->fillCircle(cx, cy, 50, WS_RED);
        _disp->setTextColor(WS_WHITE);
        _disp->setTextSize(4);
        _disp->setCursor(cx - 12, cy - 14);
        _disp->print("X");

        _disp->setTextColor(WS_GRAY);
        _disp->setTextSize(2);
        _disp->setCursor(SW / 2 - (int)(strlen(_ssidBuf) * 6), cy + 68);
        _disp->print(_ssidBuf);

        _disp->setTextColor(WS_DARKGRAY);
        _disp->setTextSize(1);
        _disp->setCursor(PAD * 2, cy + 100);
        _disp->print("Could not connect. Check password and try again.");

        // Two buttons: Try Again | Back to List
        int btnY = SH - 66;
        int btnW = SW / 2 - PAD * 2;
        _drawButton(PAD,              btnY, btnW, 56, "Try Again", WS_ACCENT);
        _drawButton(PAD + btnW + PAD, btnY, btnW, 56, "Back",      0x4208);
    }

    // -- Touch handling -------------------------------------------------------
    void _handleTouch(int tx, int ty) {
        switch (_screen) {
            case WifiScreen::NETWORKS:  _handleNetworkTouch(tx, ty);  break;
            case WifiScreen::PASSWORD:  _handlePasswordTouch(tx, ty); break;
            case WifiScreen::STATS:     _handleStatsTouch(tx, ty);    break;
            case WifiScreen::FAILED:    _handleFailedTouch(tx, ty);   break;
            default: break;
        }
    }

    void _handleNetworkTouch(int tx, int ty) {
        // Back
        if (tx < 120 && ty < HDR) {
            _active      = false;
            _needsRedraw = true;
            return;
        }
        // Rescan
        if (ty >= SH - 60) {
            _scanNetworks();
            _drawNetworkList();
            _drainTouch(200);
            return;
        }
        constexpr int RESCAN_Y = SH - 60;
        int rowH = 52;
        int maxVisible = (RESCAN_Y - HDR - PAD) / rowH;
        String currentSSID = _wifiManager ? _wifiManager->connectedSSID() : String("");

        for (int i = 0; i < min(_scanCount, maxVisible); i++) {
            int rowY = HDR + 8 + i * rowH;
            if (ty >= rowY && ty < rowY + rowH - 4) {
                int idx = i + _netScrollOffset;
                _drawNetworkRow(i, idx, true, false);
                delay(60);
                strncpy(_ssidBuf, _scannedSSIDs[idx], 32);

                // Already connected to this network -- show stats instead
                if (_wifiManager && _wifiManager->isConnected() &&
                    strcmp(_ssidBuf, currentSSID.c_str()) == 0) {
                    _screen = WifiScreen::STATS;
                    _drawStatsScreen();
                    _drainTouch();
                    return;
                }

                // New network -- go to password entry
                _passLen    = 0;
                memset(_passBuf, 0, sizeof(_passBuf));
                _screen     = WifiScreen::PASSWORD;
                _shiftState = ShiftState::OFF;
                _numMode    = false;
                _showPass   = false;
                _drawPasswordScreen();
                _drainTouch();
                return;
            }
        }
    }

    void _handlePasswordTouch(int tx, int ty) {
        // Back
        if (tx < 120 && ty < HDR) {
            _screen = WifiScreen::NETWORKS;
            _drawNetworkList();
            _drainTouch();
            return;
        }
        // Show/hide toggle inside the password field
        if (ty >= FIELD_Y && ty < FIELD_Y + FIELD_H) {
            constexpr int TOGGLE_W = 70;
            constexpr int TOGGLE_X = SW - PAD * 2 - TOGGLE_W - 4;
            if (tx >= TOGGLE_X) {
                _lastTouchTime = millis();
                _showPass = !_showPass;
                _drawPasswordField();
            }
            return;
        }

        // Connect
        if (ty >= KB_CONNECT_Y) {
            if (_passLen > 0) {
                _passBuf[_passLen] = '\0';
                _connectReady = true;
            }
            return;
        }
        // Action row
        if (ty >= KB_ACTION_Y && ty < KB_ACTION_Y + KB_KEY_H) {
            if (tx < PAD + ACT_SHIFT_W) {
                _lastTouchTime = millis();
                switch (_shiftState) {
                    case ShiftState::OFF:  _shiftState = ShiftState::ONCE; break;
                    case ShiftState::ONCE: _shiftState = ShiftState::LOCK; break;
                    case ShiftState::LOCK: _shiftState = ShiftState::OFF;  break;
                }
                _drawKeyboardRegion();
                return;
            }
            if (tx >= ACT_NUM_X && tx < ACT_NUM_X + ACT_NUM_W) {
                _lastTouchTime = millis();
                _numMode = !_numMode;
                if (_numMode) _shiftState = ShiftState::OFF;
                _drawKeyboardRegion();
                return;
            }
            if (tx >= ACT_BACK_X) {
                _lastTouchTime = millis();
                if (_passLen > 0) {
                    _passLen--;
                    _passBuf[_passLen] = '\0';
                    _drawPasswordField();
                }
                return;
            }
            if (_passLen < 63) {
                _lastTouchTime = millis();
                _passBuf[_passLen++] = ' ';
                _drawPasswordField();
            }
            return;
        }
        // Letter / number keys
        for (int row = 0; row < KB_ROWS_N; row++) {
            int ky = KB_Y + row * (KB_KEY_H + KB_GAP);
            if (ty < ky || ty >= ky + KB_KEY_H) continue;
            bool upper = (_shiftState != ShiftState::OFF);
            const char* keys = _numMode ? KB_NUMS[row] :
                               (upper   ? KB_ROWS[row] : KB_ROWS_LO[row]);
            int len    = strlen(keys);
            int totalW = len * (KB_KEY_W + KB_GAP) - KB_GAP;
            int startX = (SW - totalW) / 2;
            for (int k = 0; k < len; k++) {
                int kx = startX + k * (KB_KEY_W + KB_GAP);
                if (tx >= kx && tx < kx + KB_KEY_W) {
                    _lastTouchTime = millis();
                    _disp->fillRoundRect(kx, ky, KB_KEY_W, KB_KEY_H, 6, WS_KEY_PRESS);
                    delay(30);
                    _drawKey(kx, ky, KB_KEY_W, KB_KEY_H, String(keys[k]), false);
                    if (_passLen < 63) _passBuf[_passLen++] = keys[k];
                    bool shiftReleased = (!_numMode && _shiftState == ShiftState::ONCE);
                    if (shiftReleased) _shiftState = ShiftState::OFF;
                    _drawPasswordField();
                    if (shiftReleased) _drawKeyboardRegion();
                    return;
                }
            }
        }
    }

    void _handleStatsTouch(int tx, int ty) {
        // Back (header) or Done button (bottom) -- both close
        bool backTapped = (tx < 120 && ty < HDR);
        bool doneTapped = (ty >= SH - 56);
        if (backTapped || doneTapped) {
            _active      = false;
            _needsRedraw = true;
        }
    }

    void _handleFailedTouch(int tx, int ty) {
        // Back (header)
        if (tx < 120 && ty < HDR) {
            _screen = WifiScreen::NETWORKS;
            _drawNetworkList();
            _drainTouch();
            return;
        }
        int btnY = SH - 66;
        if (ty >= btnY) {
            int btnW = SW / 2 - PAD * 2;
            if (tx < PAD + btnW) {
                // Try Again -- go back to password screen with same SSID
                _passLen    = 0;
                memset(_passBuf, 0, sizeof(_passBuf));
                _screen     = WifiScreen::PASSWORD;
                _shiftState = ShiftState::OFF;
                _numMode    = false;
                _showPass   = false;
                _drawPasswordScreen();
                _drainTouch();
            } else {
                // Back to list
                _screen = WifiScreen::NETWORKS;
                _drawNetworkList();
                _drainTouch();
            }
        }
    }

    // -- Password screen ------------------------------------------------------
    void _drawPasswordScreen() {
        _disp->fillScreen(WS_BG);
        _drawHeader(_ssidBuf, true);
        _drawPasswordField();
        _drawKeyboard();
        _drawKeyboardActions();
    }

    void _drawPasswordField() {
        _disp->fillRoundRect(PAD, FIELD_Y, SW - PAD * 2, FIELD_H, 6, WS_SURFACE);
        _disp->drawRoundRect(PAD, FIELD_Y, SW - PAD * 2, FIELD_H, 6, WS_ACCENT);

        // Show/hide toggle -- draw first so text rendering order is clear
        constexpr int TOGGLE_W = 70;
        constexpr int TOGGLE_H = 30;
        constexpr int TOGGLE_X = SW - PAD * 2 - TOGGLE_W - 4;
        constexpr int TOGGLE_Y = FIELD_Y + (FIELD_H - TOGGLE_H) / 2;
        uint16_t toggleBg = _showPass ? WS_BLUE : WS_DARKGRAY;
        _disp->fillRoundRect(TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H, 4, toggleBg);
        _disp->drawRoundRect(TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H, 4, WS_BORDER);
        _disp->setTextColor(WS_WHITE);
        _disp->setTextSize(1);
        const char* lbl = _showPass ? "HIDE" : "SHOW";
        _disp->setCursor(TOGGLE_X + TOGGLE_W / 2 - 12, TOGGLE_Y + TOGGLE_H / 2 - 4);
        _disp->print(lbl);

        // Password text -- max chars that fit before the toggle button
        // textSize(2): each char is 12px wide. Available width = TOGGLE_X - PAD*2 - 4 - cursor_width
        constexpr int TEXT_START_X = PAD * 2;
        constexpr int TEXT_MAX_X   = TOGGLE_X - 8;  // 8px gap before button
        constexpr int CHAR_W       = 12;             // textSize(2) = 6px * 2
        int maxChars = (TEXT_MAX_X - TEXT_START_X) / CHAR_W;

        // Show the tail of the password so the most recent chars are always visible
        int startIdx = max(0, _passLen - maxChars + 1); // +1 to leave room for cursor

        _disp->setTextColor(WS_WHITE);
        _disp->setTextSize(2);
        _disp->setCursor(TEXT_START_X, FIELD_Y + 14);
        if (_showPass) {
            for (int i = startIdx; i < _passLen; i++)
                _disp->print((char)_passBuf[i]);
        } else {
            for (int i = startIdx; i < _passLen; i++)
                _disp->print('*');
        }
        _disp->print('_');
    }

    void _drawKeyboardRegion() {
        _disp->fillRect(0, KB_Y, SW, KB_CONNECT_Y - KB_Y, WS_BG);
        _drawKeyboard();
        _drawKeyboardActions();
    }

    void _drawKeyboard() {
        bool upper = (_shiftState != ShiftState::OFF);
        for (int row = 0; row < KB_ROWS_N; row++) {
            const char* keys = _numMode ? KB_NUMS[row] :
                               (upper   ? KB_ROWS[row] : KB_ROWS_LO[row]);
            int len    = strlen(keys);
            int totalW = len * (KB_KEY_W + KB_GAP) - KB_GAP;
            int startX = (SW - totalW) / 2;
            int ky     = KB_Y + row * (KB_KEY_H + KB_GAP);
            for (int k = 0; k < len; k++) {
                int kx = startX + k * (KB_KEY_W + KB_GAP);
                _drawKey(kx, ky, KB_KEY_W, KB_KEY_H, String(keys[k]), false);
            }
        }
    }

    void _drawKeyboardActions() {
        const char* shiftLbl;
        uint16_t    shiftBg;
        if      (_shiftState == ShiftState::LOCK) { shiftLbl = "LOCK"; shiftBg = WS_ORANGE; }
        else if (_shiftState == ShiftState::ONCE) { shiftLbl = "^ 1";  shiftBg = WS_GREEN;  }
        else                                      { shiftLbl = "^";    shiftBg = WS_ACCENT;  }
        _drawKeyColored(PAD,       KB_ACTION_Y, ACT_SHIFT_W, KB_KEY_H, shiftLbl, shiftBg);
        _drawKey(ACT_SPACE_X,      KB_ACTION_Y, ACT_SPACE_W, KB_KEY_H, "SPACE",  false);
        _drawKeyColored(ACT_NUM_X, KB_ACTION_Y, ACT_NUM_W,   KB_KEY_H,
                        _numMode ? "ABC" : "123", _numMode ? WS_BLUE : WS_ACCENT);
        _drawKey(ACT_BACK_X,       KB_ACTION_Y, ACT_BACK_W,  KB_KEY_H, "<--",    false);
        _drawButton(PAD, KB_CONNECT_Y, SW - PAD * 2, KB_CONNECT_H, "Connect", WS_GREEN);
    }

    void _drawKey(int x, int y, int w, int h, const String& label, bool active) {
        _drawKeyColored(x, y, w, h, label, active ? WS_BLUE : WS_ACCENT);
    }

    void _drawKeyColored(int x, int y, int w, int h, const String& label, uint16_t bg) {
        _disp->fillRoundRect(x, y, w, h, 6, bg);
        _disp->drawRoundRect(x, y, w, h, 6, WS_BORDER);
        _disp->setTextColor(WS_WHITE);
        _disp->setTextSize(2);
        int tx = x + w / 2 - (int)(label.length() * 6);
        int ty = y + h / 2 - 8;
        _disp->setCursor(tx, ty);
        _disp->print(label);
    }

    // -- Shared drawing -------------------------------------------------------
    void _drawHeader(const char* title, bool showBack) {
        _disp->fillRect(0, 0, SW, HDR, 0x0841);
        _disp->drawFastHLine(0, HDR - 1, SW, WS_BORDER);
        _disp->setTextColor(WS_WHITE);
        _disp->setTextSize(2);
        _disp->setCursor(showBack ? 70 : PAD, HDR / 2 - 8);
        _disp->print(title);
        if (showBack) {
            _disp->fillRoundRect(PAD, HDR / 2 - 12, 54, 24, 4, WS_SURFACE);
            _disp->drawRoundRect(PAD, HDR / 2 - 12, 54, 24, 4, WS_BORDER);
            _disp->setCursor(PAD + 8, HDR / 2 - 4);
            _disp->setTextColor(WS_WHITE);
            _disp->setTextSize(1);
            _disp->print("< Back");
        }
    }

    void _drawButton(int x, int y, int w, int h, const char* label, uint16_t color) {
        _disp->fillRoundRect(x, y, w, h, 8, color);
        _disp->setTextColor(WS_WHITE);
        _disp->setTextSize(2);
        int tx = x + w / 2 - (int)(strlen(label) * 6);
        int ty = y + h / 2 - 8;
        _disp->setCursor(tx, ty);
        _disp->print(label);
    }

    void _drawConnectingScreen(int attempt, int tick) {
        // Only do a full clear on the very first call
        if (attempt == 1 && tick == 0) {
            _disp->fillScreen(WS_BG);
            _drawHeader("Connecting...", false);

            // SSID label
            _disp->setTextColor(WS_GRAY);
            _disp->setTextSize(1);
            _disp->setCursor(SW / 2 - (int)(strlen(_ssidBuf) * 3), HDR + 40);
            _disp->print(_ssidBuf);
        }

        // Attempt counter -- clear just that region
        int attY = HDR + 60;
        _disp->fillRect(0, attY, SW, 20, WS_BG);
        _disp->setTextColor(WS_GRAY);
        _disp->setTextSize(1);
        char attBuf[32];
        snprintf(attBuf, sizeof(attBuf), "Attempt %d of 3", attempt);
        _disp->setCursor(SW / 2 - (int)(strlen(attBuf) * 3), attY + 4);
        _disp->print(attBuf);

        // Spinner ring of 12 dots
        int cx = SW / 2, cy = SH / 2 - 10;
        int radius = 46;
        int dotR   = 6;
        int totalDots = 12;
        int lit    = tick % totalDots;
        for (int d = 0; d < totalDots; d++) {
            float angle = d * 2.0f * 3.14159f / totalDots;
            int dx = cx + (int)(radius * cos(angle));
            int dy = cy + (int)(radius * sin(angle));
            // Trail: lit dot bright, 2 behind dimmer, rest dark
            int dist = (totalDots + lit - d) % totalDots;
            uint16_t col;
            if      (dist == 0) col = WS_WHITE;
            else if (dist == 1) col = WS_GRAY;
            else if (dist == 2) col = WS_DARKGRAY;
            else                col = WS_SURFACE;
            _disp->fillCircle(dx, dy, dotR, col);
        }

        // Progress bar across bottom area
        int barY  = cy + radius + 24;
        int barX  = SW / 4;
        int barW  = SW / 2;
        int barH  = 10;
        // Each attempt is 1/3 of the bar, each tick fills within that
        int totalTicks = 3 * 20;  // 3 attempts * 20 ticks max
        int doneTicks  = (attempt - 1) * 20 + tick;
        int fillW = barW * doneTicks / totalTicks;
        _disp->fillRoundRect(barX, barY, barW,  barH, 4, WS_SURFACE);
        if (fillW > 0)
            _disp->fillRoundRect(barX, barY, fillW, barH, 4, WS_BLUE);
        _disp->drawRoundRect(barX, barY, barW,  barH, 4, WS_BORDER);
    }
};

} // namespace VoiceHub

// Static instance pointer for progress callback
VoiceHub::WiFiSettings* VoiceHub::WiFiSettings::_instance = nullptr;
