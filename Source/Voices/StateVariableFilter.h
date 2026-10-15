#pragma once

#include <cmath>
#include <algorithm>
#include <cstdint>

namespace konos {

enum class FilterType : int {
    LowPass  = 0,
    HighPass = 1,
    BandPass = 2
};

/**
 * Topology-Preserving Transform (TPT) Zero-Delay Feedback State-Variable Filter
 * based on Andrew Simper (Cytomic) trapezoidal integrated SVF equations.
 * Unconditionally stable under fast cutoff/resonance modulation with denormal protection.
 */
class StateVariableFilter {
public:
    void reset() noexcept {
        ic1eq_[0] = 0.0f;
        ic1eq_[1] = 0.0f;
        ic2eq_[0] = 0.0f;
        ic2eq_[1] = 0.0f;
    }

    void setParameters(double sampleRate, float cutoffHz, float resonanceQ, FilterType type) noexcept {
        const double sr = std::max(8000.0, sampleRate);
        const float nyquistLimit = static_cast<float>(sr * 0.48);
        const float clampedCutoff = std::clamp(cutoffHz, 20.0f, nyquistLimit);
        const float clampedQ = std::clamp(resonanceQ, 0.5f, 12.0f);

        constexpr float kPi = 3.14159265358979323846f;
        const float g = std::tan(kPi * clampedCutoff / static_cast<float>(sr));
        const float k = 1.0f / clampedQ;

        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
        k_  = k;
        type_ = type;
    }

    void processStereo(float& left, float& right) noexcept {
        left  = processChannel(0, left);
        right = processChannel(1, right);
    }

    float processChannel(int ch, float v0) noexcept {
        // Flush denormals
        constexpr float kAntiDenormal = 1.0e-20f;
        const float v3 = v0 - ic2eq_[ch];
        const float v1 = a1_ * ic1eq_[ch] + a2_ * v3;
        const float v2 = ic2eq_[ch] + a2_ * ic1eq_[ch] + a3_ * v3;

        ic1eq_[ch] = 2.0f * v1 - ic1eq_[ch] + kAntiDenormal;
        ic2eq_[ch] = 2.0f * v2 - ic2eq_[ch] - kAntiDenormal;

        if (std::abs(ic1eq_[ch]) < 1.0e-18f) ic1eq_[ch] = 0.0f;
        if (std::abs(ic2eq_[ch]) < 1.0e-18f) ic2eq_[ch] = 0.0f;

        switch (type_) {
            case FilterType::LowPass:  return v2;
            case FilterType::HighPass: return v0 - k_ * v1 - v2;
            case FilterType::BandPass: return v1;
        }
        return v2;
    }

private:
    float a1_{0.0f};
    float a2_{0.0f};
    float a3_{0.0f};
    float k_{1.414f};
    FilterType type_{FilterType::LowPass};
    float ic1eq_[2]{0.0f, 0.0f};
    float ic2eq_[2]{0.0f, 0.0f};
};

} // namespace konos
