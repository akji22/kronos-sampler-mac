#pragma once

#include <atomic>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace konos {

/**
 * Per-slot parameters for each channel in the Multi-Instrument Rack.
 */
struct RackSlotState {
    std::atomic<bool>    enabled{false};
    std::atomic<uint8_t> midiChannel{0};       // 0 = Omni, 1..16 = Ch 1..16
    std::atomic<float>   volumeDb{0.0f};       // [-60.0, +12.0] dB
    std::atomic<float>   pan{0.0f};            // [-1.0, +1.0]
    std::atomic<float>   tuneSemitones{0.0f};  // [-24.0, +24.0] st
    std::atomic<bool>    mute{false};
    std::atomic<bool>    solo{false};
    std::atomic<int>     maxPolyphony{32};     // [1, 64]

    // Real-time telemetry per slot
    std::atomic<int>         activeVoices{0};
    std::atomic<float>       peakLeft{0.0f};
    std::atomic<float>       peakRight{0.0f};
    std::atomic<uint32_t>    midiActivityCounter{0};
    std::atomic<std::size_t> sampleMemoryBytes{0};
};

/**
 * Thread-safe atomic parameter block shared between the host/UI thread
 * and the real-time audio rendering thread.
 */
struct SamplerParameterBlock {
    std::atomic<float> masterGainDb{0.0f};            // [-60.0, +12.0] dB
    std::atomic<float> masterPan{0.0f};               // [-1.0, +1.0]
    std::atomic<float> masterTuneSemitones{0.0f};     // [-24.0, +24.0] semitones
    std::atomic<float> pitchBendRangeSemitones{2.0f}; // [1.0, 24.0] semitones

    // Amplitude ADSR Envelope parameters
    std::atomic<float> attackMs{5.0f};                // [0.5, 5000.0] ms
    std::atomic<float> decayMs{180.0f};               // [1.0, 10000.0] ms
    std::atomic<float> sustainLevel{0.80f};           // [0.0, 1.0]
    std::atomic<float> releaseMs{250.0f};             // [2.0, 10000.0] ms

    // Per-voice State-Variable Filter parameters
    std::atomic<float> filterCutoffHz{18000.0f};      // [20.0, 20000.0] Hz
    std::atomic<float> filterResonance{0.707f};       // [0.5, 10.0] Q
    std::atomic<int>   filterMode{0};                 // 0 = LowPass, 1 = HighPass, 2 = BandPass
    std::atomic<float> velToFilterOctaves{1.5f};      // [0.0, 4.0] octaves

    // Sample playback & loop bounds (normalized [0.0, 1.0])
    std::atomic<float> sampleStartNorm{0.0f};         // [0.0, 0.99]
    std::atomic<float> sampleEndNorm{1.0f};           // [0.01, 1.0]
    std::atomic<bool>  loopEnabled{false};
    std::atomic<float> loopStartNorm{0.10f};          // [0.0, 0.98]
    std::atomic<float> loopEndNorm{0.90f};            // [0.02, 1.0]
    std::atomic<bool>  reversePlayback{false};

    // Polyphony management
    std::atomic<int>   maxPolyphony{32};              // [1, 64]
    std::atomic<float> velocitySensitivity{0.85f};    // [0.0, 1.0]
};

class SmoothedParameter {
public:
    void reset(double sampleRate, double rampTimeMs, float initialValue = 0.0f) noexcept {
        currentValue_ = initialValue;
        targetValue_ = initialValue;
        if (sampleRate <= 0.0 || rampTimeMs <= 0.0) {
            coeff_ = 1.0f;
        } else {
            const double numSamples = (rampTimeMs * 0.001) * sampleRate;
            coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, numSamples)));
        }
    }

    void setTarget(float target) noexcept { targetValue_ = target; }

    float getNextValue() noexcept {
        currentValue_ += coeff_ * (targetValue_ - currentValue_);
        if (std::abs(targetValue_ - currentValue_) < 1.0e-6f) {
            currentValue_ = targetValue_;
        }
        return currentValue_;
    }

    float getCurrentValue() const noexcept { return currentValue_; }

private:
    float currentValue_{0.0f};
    float targetValue_{0.0f};
    float coeff_{1.0f};
};

inline float dbToLinear(float db) noexcept {
    if (db <= -60.0f) return 0.0f;
    return std::pow(10.0f, db * 0.05f);
}

} // namespace konos
