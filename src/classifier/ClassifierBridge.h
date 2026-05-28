#pragma once

// Save 10KB RAM
#define EIDSP_QUANTIZE_FILTERBANK 0

#include <Arduino.h>
#include <voice-hub_inferencing.h>

// ─────────────────────────────────────────────────────────────────────────────
// ClassifierBridge.h — Edge Impulse inference using portenta_h7 pattern
// ─────────────────────────────────────────────────────────────────────────────

namespace VoiceHub {

constexpr float CONFIDENCE_THRESHOLD = 0.80f;  // keyword must score at least this
constexpr float MARGIN_THRESHOLD     = 0.20f;  // keyword must beat 2nd place by at least this
constexpr float NOISE_CEILING        = 0.40f;  // reject if noise/unknown scores above this

struct ClassifierResult {
    const char* label;
    float       confidence;
    bool        valid;
};

// Pointer to audio buffer — set before calling get_data callback
static int16_t* _classifyBuffer = nullptr;

static int _audio_signal_get_data(size_t offset, size_t length, float* out_ptr) {
    numpy::int16_to_float(&_classifyBuffer[offset], out_ptr, length);
    return 0;
}

static bool _isNoise(const char* label) {
    return (strcmp(label, "noise")            == 0 ||
            strcmp(label, "background")       == 0 ||
            strcmp(label, "background_noise") == 0 ||
            strcmp(label, "unknown")          == 0 ||
            strcmp(label, "_noise")           == 0 ||
            strcmp(label, "_unknown")         == 0 ||
            label[0] == '_');  // Edge Impulse prefixes noise classes with _
}

class ClassifierBridge {
public:
    ClassifierBridge() = default;

    ClassifierResult classify(int16_t* buffer, size_t length) {
        ClassifierResult result{ nullptr, 0.0f, false };

        _classifyBuffer = buffer;

        signal_t signal;
        signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
        signal.get_data     = &_audio_signal_get_data;

        ei_impulse_result_t ei_result = { 0 };
        EI_IMPULSE_ERROR err = run_classifier(&signal, &ei_result, false);

        if (err != EI_IMPULSE_OK) {
            ei_printf("ERR: run_classifier failed (%d)\n", err);
            return result;
        }

        // Find best keyword score and best noise score separately
        float    bestKeyScore  = 0.0f;  uint32_t bestKeyIdx   = 0;
        float    secondScore   = 0.0f;
        float    bestNoiseScore = 0.0f;

        for (uint32_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            float score = ei_result.classification[i].value;
            const char* lbl = ei_result.classification[i].label;

            if (_isNoise(lbl)) {
                if (score > bestNoiseScore) bestNoiseScore = score;
            } else {
                if (score > bestKeyScore) {
                    secondScore  = bestKeyScore;  // previous best becomes second
                    bestKeyScore = score;
                    bestKeyIdx   = i;
                } else if (score > secondScore) {
                    secondScore = score;
                }
            }
        }

        float margin = bestKeyScore - secondScore;
        const char* label = ei_result.classification[bestKeyIdx].label;

        Serial.print("[ML] top=" );
        Serial.print(label);
        Serial.print(" ");
        Serial.print(bestKeyScore * 100.0f, 1);
        Serial.print("% margin=");
        Serial.print(margin * 100.0f, 1);
        Serial.print("% noise=");
        Serial.print(bestNoiseScore * 100.0f, 1);
        Serial.println("%");

        if (bestKeyScore  >= CONFIDENCE_THRESHOLD &&
            margin        >= MARGIN_THRESHOLD     &&
            bestNoiseScore < NOISE_CEILING) {
            result.label      = label;
            result.confidence = bestKeyScore;
            result.valid      = true;

            Serial.print("[ML] ACCEPTED: ");
            Serial.print(label);
            Serial.print(" (");
            Serial.print(bestKeyScore * 100.0f, 1);
            Serial.println("%)");
        } else {
            Serial.println("[ML] rejected");
        }

        return result;
    }
};

} // namespace VoiceHub
