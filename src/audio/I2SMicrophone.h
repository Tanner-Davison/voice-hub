#pragma once

#include <Arduino.h>
#include <Arduino_AdvancedAnalog.h>

// ─────────────────────────────────────────────────────────────────────────────
// I2SMicrophone.h  —  INMP441 input via AdvancedI2S (Giga R1)
//
// Wiring:
//   INMP441 SD   → Giga R1 PG_9  (SDI / data in)
//   INMP441 WS   → Giga R1 PG_10 (word select)
//   INMP441 SCK  → Giga R1 PG_11 (bit clock)
//   INMP441 VDD  → 3.3V
//   INMP441 GND  → GND
//   INMP441 L/R  → GND  (selects left channel)
//   MCK not connected — INMP441 doesn't need a master clock
// ─────────────────────────────────────────────────────────────────────────────

namespace VoiceHub {

constexpr uint32_t SAMPLE_RATE     = 16000;
constexpr size_t   SAMPLES_PER_BUF = 512;           // DMA chunk size
constexpr size_t   CAPTURE_SAMPLES = SAMPLE_RATE;   // 1 second of audio

// WS, CK, SDI, SDO, MCK
// SDO and MCK unused for input-only — NC keeps the pin disconnected
static AdvancedI2S i2s(PG_10, PG_11, PG_9, NC, NC);

class I2SMicrophone {
public:
    I2SMicrophone() = default;

    bool begin() {
        // AN_I2S_MODE_IN = input only (microphone)
        // 16kHz, 512 samples per DMA buffer, 8 buffers queued
        if (!i2s.begin(AN_I2S_MODE_IN, SAMPLE_RATE, SAMPLES_PER_BUF, 8)) {
            Serial.println("[MIC] Failed to start I2S");
            return false;
        }
        Serial.println("[MIC] I2S started OK");
        return true;
    }

    // Fill output buffer with `length` int16 samples (blocking).
    // Accumulates DMA chunks until the buffer is full.
    bool capture(int16_t* output, size_t length) {
        size_t filled = 0;

        while (filled < length) {
            if (!i2s.available()) {
                delay(1);
                continue;
            }

            SampleBuffer buf = i2s.read();

            for (size_t i = 0; i < buf.size() && filled < length; i++) {
                // buf samples are uint16 — reinterpret as int16 for Edge Impulse
                output[filled++] = static_cast<int16_t>(buf[i]);
            }

            buf.release();
        }

        return true;
    }
};

} // namespace VoiceHub
