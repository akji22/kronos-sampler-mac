#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace konos {

enum class EnvelopeStage : uint8_t {
    Idle         = 0,
    Attack       = 1,
    Decay        = 2,
    Sustain      = 3,
    Release      = 4,
    StealingFade = 5
};

/**
 * Sample-accurate, click-free ADSR amplitude envelope with a dedicated
 * fast 5 ms StealingFade stage to prevent clicks during voice stealing.
 */
class AdsrEnvelope {
public:
    void setSampleRate(double sampleRate) noexcept {
        sampleRate_ = std::max(1000.0, sampleRate);
        recalculateRates();
    }

    void setParameters(float attackMs, float decayMs, float sustainLevel, float releaseMs) noexcept {
        attackMs_     = std::clamp(attackMs, 0.5f, 10000.0f);
        decayMs_      = std::clamp(decayMs, 1.0f, 15000.0f);
        sustainLevel_ = std::clamp(sustainLevel, 0.0f, 1.0f);
        releaseMs_    = std::clamp(releaseMs, 1.0f, 15000.0f);
        recalculateRates();
    }

    void noteOn() noexcept {
        recalculateRates();
        stage_ = EnvelopeStage::Attack;
    }

    void noteOff() noexcept {
        if (stage_ != EnvelopeStage::Idle && stage_ != EnvelopeStage::StealingFade) {
            stage_ = EnvelopeStage::Release;
            // Release from whatever current envelope level we have reached
            const double releaseSamples = std::max(1.0, (releaseMs_ * 0.001) * sampleRate_);
            releaseDelta_ = static_cast<float>( std::max(0.0001f, envelopeValue_) / releaseSamples );
        }
    }

    void triggerStealFade() noexcept {
        if (stage_ != EnvelopeStage::Idle) {
            stage_ = EnvelopeStage::StealingFade;
            const double fadeSamples = std::max(1.0, 0.004 * sampleRate_); // 4 ms click-free fade
            stealDelta_ = static_cast<float>(std::max(0.001f, envelopeValue_) / fadeSamples);
        }
    }

    void reset() noexcept {
        stage_ = EnvelopeStage::Idle;
        envelopeValue_ = 0.0f;
    }

    float getNextSample() noexcept {
        switch (stage_) {
            case EnvelopeStage::Idle:
                envelopeValue_ = 0.0f;
                break;

            case EnvelopeStage::Attack:
                envelopeValue_ += attackDelta_;
                if (envelopeValue_ >= 1.0f) {
                    envelopeValue_ = 1.0f;
                    stage_ = EnvelopeStage::Decay;
                }
                break;

            case EnvelopeStage::Decay:
                envelopeValue_ -= decayDelta_;
                if (envelopeValue_ <= sustainLevel_) {
                    envelopeValue_ = sustainLevel_;
                    stage_ = EnvelopeStage::Sustain;
                }
                break;

            case EnvelopeStage::Sustain:
                envelopeValue_ = sustainLevel_;
                break;

            case EnvelopeStage::Release:
                envelopeValue_ -= releaseDelta_;
                if (envelopeValue_ <= 0.0f) {
                    envelopeValue_ = 0.0f;
                    stage_ = EnvelopeStage::Idle;
                }
                break;

            case EnvelopeStage::StealingFade:
                envelopeValue_ -= stealDelta_;
                if (envelopeValue_ <= 0.0f) {
                    envelopeValue_ = 0.0f;
                    stage_ = EnvelopeStage::Idle;
                }
                break;
        }
        return envelopeValue_;
    }

    bool isActive() const noexcept { return stage_ != EnvelopeStage::Idle; }
    EnvelopeStage getStage() const noexcept { return stage_; }
    float getCurrentValue() const noexcept { return envelopeValue_; }

private:
    void recalculateRates() noexcept {
        const double attackSamples  = std::max(1.0, (attackMs_ * 0.001) * sampleRate_);
        const double decaySamples   = std::max(1.0, (decayMs_ * 0.001) * sampleRate_);
        const double releaseSamples = std::max(1.0, (releaseMs_ * 0.001) * sampleRate_);

        attackDelta_  = static_cast<float>(1.0 / attackSamples);
        decayDelta_   = static_cast<float>(std::max(0.001, 1.0 - sustainLevel_) / decaySamples);
        releaseDelta_ = static_cast<float>(std::max(0.05, static_cast<double>(sustainLevel_)) / releaseSamples);
    }

    double sampleRate_{44100.0};
    float  attackMs_{5.0f};
    float  decayMs_{180.0f};
    float  sustainLevel_{0.80f};
    float  releaseMs_{250.0f};

    float  attackDelta_{0.001f};
    float  decayDelta_{0.0005f};
    float  releaseDelta_{0.0005f};
    float  stealDelta_{0.005f};

    float         envelopeValue_{0.0f};
    EnvelopeStage stage_{EnvelopeStage::Idle};
};

} // namespace konos
