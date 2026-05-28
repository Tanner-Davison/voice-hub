#pragma once

#include <Arduino.h>
#include <PDM.h>
#include <voice-hub_inferencing.h>

// ─────────────────────────────────────────────────────────────────────────────
// I2SMicrophone.h  —  PDM mic with sliding window for continuous inference
//
// Uses EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW (4) to slide the window
// by SLICE_SIZE samples (500ms) each inference — so the full 2s phrase
// is much more likely to be captured regardless of when you start speaking.
// ─────────────────────────────────────────────────────────────────────────────

namespace VoiceHub {

constexpr uint32_t SAMPLE_RATE     = EI_CLASSIFIER_FREQUENCY;
constexpr size_t   CAPTURE_SAMPLES = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
constexpr size_t   SLICE_SIZE      = EI_CLASSIFIER_SLICE_SIZE; // 8000 samples = 500ms

typedef struct {
    int16_t  *buffer;
    uint8_t   buf_ready;
    uint32_t  buf_count;
    uint32_t  n_samples;
} inference_t;

static inference_t   _inference;
static int16_t       _sampleBuffer[2048];
static volatile bool _record_ready = false;

// Secondary raw capture -- set buf pointer + max to capture outside classifier
static int16_t*       _raw_buf      = nullptr;
static size_t         _raw_max      = 0;
static volatile size_t _raw_count   = 0;

static void _pdm_callback(void) {
    int bytesAvailable = PDM.available();
    int bytesRead = PDM.read((char*)&_sampleBuffer[0], bytesAvailable);
    int samples = bytesRead >> 1;

    // Feed classifier sliding window
    if (_inference.buf_ready == 0 && _record_ready == true) {
        for (int i = 0; i < samples; i++) {
            _inference.buffer[_inference.buf_count++] = _sampleBuffer[i];
            if (_inference.buf_count >= _inference.n_samples) {
                _inference.buf_count = 0;
                _inference.buf_ready = 1;
                break;
            }
        }
    }

    // Feed secondary raw buffer (used by AskHub)
    if (_raw_buf != nullptr && _raw_count < _raw_max) {
        size_t toStore = min((size_t)samples, _raw_max - _raw_count);
        memcpy(_raw_buf + _raw_count, _sampleBuffer, toStore * 2);
        _raw_count += toStore;
    }
}

class I2SMicrophone {
public:
    I2SMicrophone() = default;

    bool begin() {
        // Allocate a full window buffer
        _inference.buffer = (int16_t*)malloc(CAPTURE_SAMPLES * sizeof(int16_t));
        if (_inference.buffer == nullptr) {
            Serial.println("[MIC] malloc failed");
            return false;
        }

        _inference.buf_count = 0;
        _inference.n_samples = CAPTURE_SAMPLES;
        _inference.buf_ready = 0;

        PDM.onReceive(_pdm_callback);
        PDM.setBufferSize(2048);

        if (!PDM.begin(1, EI_CLASSIFIER_FREQUENCY)) {
            Serial.println("[MIC] PDM begin() failed");
            free(_inference.buffer);
            return false;
        }

        Serial.println("[MIC] PDM started OK");
        return true;
    }

    // Sliding window capture -- NO poll callback, zero blocking inside
    bool captureSlice(int16_t* output, size_t fullLength) {
        memmove(_inference.buffer,
                _inference.buffer + SLICE_SIZE,
                (CAPTURE_SAMPLES - SLICE_SIZE) * sizeof(int16_t));

        _inference.buf_count = CAPTURE_SAMPLES - SLICE_SIZE;
        _inference.n_samples = CAPTURE_SAMPLES;
        _inference.buf_ready = 0;
        _record_ready = true;

        // Pure busy-wait -- no I2C, no display, no callbacks
        // PDM interrupt fills the buffer in background; we just yield CPU
        while (_inference.buf_ready == 0) {
            delay(1);
        }

        _record_ready = false;
        memcpy(output, _inference.buffer, fullLength * sizeof(int16_t));
        return true;
    }

    // Full blocking capture (used for first fill)
    bool capture(int16_t* output, size_t length) {
        _record_ready = true;
        while (_inference.buf_ready == 0) {
            delay(10);
        }
        memcpy(output, _inference.buffer, length * sizeof(int16_t));
        _inference.buf_ready = 0;
        _record_ready = false;
        return true;
    }
};

} // namespace VoiceHub
