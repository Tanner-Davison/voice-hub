#pragma once

#include <Arduino.h>

// ─────────────────────────────────────────────────────────────────────────────
// ClassifierBridge.h
//
// Thin wrapper around the Edge Impulse exported model.
//
// After training your model on Edge Impulse and exporting it as an
// Arduino library, it will generate a header like:
//
//   <your-project-name>_inferencing.h
//
// Replace the placeholder include below with your actual exported header.
// The rest of this file should work without modification.
// ─────────────────────────────────────────────────────────────────────────────

// TODO: Replace with your exported Edge Impulse library header
// #include <voice_hub_inferencing.h>

namespace VoiceHub {

constexpr float CONFIDENCE_THRESHOLD = 0.75f;  // Ignore detections below this

struct ClassifierResult {
    const char* label;
    float       confidence;
    bool        valid;  // false = below threshold or noise/unknown
};

class ClassifierBridge {
public:
    ClassifierBridge() = default;

    // Run inference on a buffer of int16 audio samples
    // buffer must contain EI_CLASSIFIER_RAW_SAMPLE_COUNT samples
    // (set by Edge Impulse — typically 16000 for 1 second at 16kHz)
    ClassifierResult classify(int16_t* buffer, size_t length) {
        ClassifierResult result{ nullptr, 0.0f, false };

#ifdef EI_CLASSIFIER_RAW_SAMPLE_COUNT
        // ── Build Edge Impulse signal from buffer ──────────────────────────
        signal_t signal;
        int err = numpy::signal_from_buffer(
            buffer, length, &signal
        );

        if (err != 0) {
            Serial.print("[ML] signal_from_buffer failed: ");
            Serial.println(err);
            return result;
        }

        // ── Run inference ──────────────────────────────────────────────────
        ei_impulse_result_t ei_result = { 0 };
        err = run_classifier(&signal, &ei_result, false);

        if (err != EI_IMPULSE_OK) {
            Serial.print("[ML] run_classifier failed: ");
            Serial.println(err);
            return result;
        }

        // ── Find best classification ───────────────────────────────────────
        float    bestScore = 0.0f;
        uint32_t bestIdx   = 0;

        for (uint32_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            if (ei_result.classification[i].value > bestScore) {
                bestScore = ei_result.classification[i].value;
                bestIdx   = i;
            }
        }

        const char* label = ei_result.classification[bestIdx].label;

        // Skip "noise" and "unknown" labels — Edge Impulse generates these
        bool isBackground = (strcmp(label, "noise")   == 0 ||
                             strcmp(label, "unknown")  == 0 ||
                             strcmp(label, "_noise")   == 0);

        if (!isBackground && bestScore >= CONFIDENCE_THRESHOLD) {
            result.label      = label;
            result.confidence = bestScore;
            result.valid      = true;

            Serial.print("[ML] Detected: ");
            Serial.print(label);
            Serial.print(" (");
            Serial.print(bestScore * 100.0f, 1);
            Serial.println("%)");
        }
#else
        // Edge Impulse library not yet included — print a reminder
        Serial.println("[ML] Edge Impulse library not included yet.");
        Serial.println("     Export your model and add the #include above.");
#endif

        return result;
    }
};

} // namespace VoiceHub
