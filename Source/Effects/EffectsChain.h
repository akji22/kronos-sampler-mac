#pragma once

#include <array>
#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace konos {

struct EffectsParameters {
    // 1. 3-Band Parametric/Shelving EQ
    std::atomic<bool>  eqEnabled{true};
    std::atomic<float> eqLowGainDb{0.0f};      // [-15.0, +15.0] dB @ 120 Hz
    std::atomic<float> eqMidGainDb{0.0f};      // [-15.0, +15.0] dB @ 1000 Hz
    std::atomic<float> eqHighGainDb{0.0f};     // [-15.0, +15.0] dB @ 8000 Hz

    // 2. Quadrature Stereo Chorus
    std::atomic<bool>  chorusEnabled{false};
    std::atomic<float> chorusRateHz{0.8f};     // [0.1, 5.0] Hz
    std::atomic<float> chorusDepth{0.45f};     // [0.0, 1.0]
    std::atomic<float> chorusMix{0.35f};       // [0.0, 1.0]

    // 3. Stereo Ping-Pong Delay
    std::atomic<bool>  delayEnabled{false};
    std::atomic<float> delayTimeMs{320.0f};    // [10.0, 1500.0] ms
    std::atomic<float> delayFeedback{0.35f};   // [0.0, 0.90]
    std::atomic<float> delayMix{0.25f};        // [0.0, 1.0]

    // 4. Algorithmic Stereo Reverb (Schroeder/Freeverb)
    std::atomic<bool>  reverbEnabled{false};
    std::atomic<float> reverbRoomSize{0.65f};  // [0.0, 1.0]
    std::atomic<float> reverbDamping{0.40f};   // [0.0, 1.0]
    std::atomic<float> reverbMix{0.22f};       // [0.0, 1.0]

    // 5. Master Bus Compressor & Limiter
    std::atomic<bool>  compEnabled{true};
    std::atomic<float> compThresholdDb{-12.0f}; // [-40.0, 0.0] dB
    std::atomic<float> compRatio{3.0f};         // [1.0, 20.0]
    std::atomic<float> compMakeupDb{0.0f};      // [0.0, 18.0] dB
};

/**
 * Real-time Master & Rack DSP Effects Chain:
 * 3-Band Biquad EQ -> Stereo Quadrature Chorus -> Stereo Ping-Pong Delay ->
 * Schroeder/Freeverb Stereo Reverb -> Master Bus Compressor.
 * All delay/reverb buffers are pre-allocated during prepare().
 */
class EffectsChain {
public:
    EffectsChain();
    ~EffectsChain() = default;

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    void processStereoBlock(float* inOutLeft, float* inOutRight, int numSamples) noexcept;

    EffectsParameters&       getParameters() noexcept { return params_; }
    const EffectsParameters& getParameters() const noexcept { return params_; }

    float getCompressorGainReductionDb() const noexcept {
        return compGainReductionDb_.load(std::memory_order_relaxed);
    }

private:
    struct BiquadFilter {
        float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
        float z1L{0.0f}, z2L{0.0f}, z1R{0.0f}, z2R{0.0f};

        void reset() noexcept { z1L = z2L = z1R = z2R = 0.0f; }
        void processStereo(float& l, float& r) noexcept;
        void setLowShelf(double sr, float freqHz, float gainDb) noexcept;
        void setPeaking(double sr, float freqHz, float q, float gainDb) noexcept;
        void setHighShelf(double sr, float freqHz, float gainDb) noexcept;
    };

    struct CombFilter {
        std::vector<float> buf{};
        std::size_t idx{0};
        float filterStore{0.0f};

        void init(std::size_t size) { buf.assign(std::max<std::size_t>(16, size), 0.0f); idx = 0; filterStore = 0.0f; }
        void clear() noexcept { std::fill(buf.begin(), buf.end(), 0.0f); filterStore = 0.0f; }
        float process(float input, float feedback, float damp) noexcept {
            const float output = buf[idx];
            filterStore = (output * (1.0f - damp)) + (filterStore * damp);
            if (std::abs(filterStore) < 1.0e-18f) filterStore = 0.0f;
            buf[idx] = input + (filterStore * feedback);
            if (++idx >= buf.size()) idx = 0;
            return output;
        }
    };

    struct AllPassFilter {
        std::vector<float> buf{};
        std::size_t idx{0};

        void init(std::size_t size) { buf.assign(std::max<std::size_t>(16, size), 0.0f); idx = 0; }
        void clear() noexcept { std::fill(buf.begin(), buf.end(), 0.0f); }
        float process(float input) noexcept {
            const float bufOut = buf[idx];
            const float output = -input + bufOut;
            buf[idx] = input + (bufOut * 0.5f);
            if (std::abs(buf[idx]) < 1.0e-18f) buf[idx] = 0.0f;
            if (++idx >= buf.size()) idx = 0;
            return output;
        }
    };

    double sampleRate_{44100.0};
    EffectsParameters params_{};

    // EQ filters
    BiquadFilter lowShelf_{};
    BiquadFilter midPeak_{};
    BiquadFilter highShelf_{};

    // Chorus delay line
    std::vector<float> chorusBufL_{};
    std::vector<float> chorusBufR_{};
    std::size_t chorusWriteIdx_{0};
    double chorusPhase_{0.0};

    // Stereo Ping-Pong Delay line
    std::vector<float> delayBufL_{};
    std::vector<float> delayBufR_{};
    std::size_t delayWriteIdx_{0};
    float delayDampL_{0.0f};
    float delayDampR_{0.0f};

    // Schroeder / Freeverb (4 combs + 2 allpasses per channel)
    std::array<CombFilter, 4>    reverbCombsL_{};
    std::array<CombFilter, 4>    reverbCombsR_{};
    std::array<AllPassFilter, 2> reverbAllPassL_{};
    std::array<AllPassFilter, 2> reverbAllPassR_{};

    // Compressor state
    float compEnvelope_{0.0f};
    std::atomic<float> compGainReductionDb_{0.0f};
};

} // namespace konos
