#include "EffectsChain.h"
#include "../AudioEngine/SamplerParameters.h"

namespace konos {

void EffectsChain::BiquadFilter::processStereo(float& l, float& r) noexcept {
    const float outL = b0 * l + z1L;
    z1L = b1 * l - a1 * outL + z2L;
    z2L = b2 * l - a2 * outL;
    if (std::abs(z1L) < 1.0e-18f) z1L = 0.0f;
    if (std::abs(z2L) < 1.0e-18f) z2L = 0.0f;
    l = outL;

    const float outR = b0 * r + z1R;
    z1R = b1 * r - a1 * outR + z2R;
    z2R = b2 * r - a2 * outR;
    if (std::abs(z1R) < 1.0e-18f) z1R = 0.0f;
    if (std::abs(z2R) < 1.0e-18f) z2R = 0.0f;
    r = outR;
}

void EffectsChain::BiquadFilter::setLowShelf(double sr, float freqHz, float gainDb) noexcept {
    const double A  = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
    const double w0 = 6.283185307179586 * std::clamp(static_cast<double>(freqHz), 20.0, sr * 0.45) / sr;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw * 0.5 * std::sqrt(2.0);
    const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    const double a0 = (A + 1.0) + (A - 1.0) * cw + twoSqrtAAlpha;
    b0 = static_cast<float>((A * ((A + 1.0) - (A - 1.0) * cw + twoSqrtAAlpha)) / a0);
    b1 = static_cast<float>((2.0 * A * ((A - 1.0) - (A + 1.0) * cw)) / a0);
    b2 = static_cast<float>((A * ((A + 1.0) - (A - 1.0) * cw - twoSqrtAAlpha)) / a0);
    a1 = static_cast<float>((-2.0 * ((A - 1.0) + (A + 1.0) * cw)) / a0);
    a2 = static_cast<float>(((A + 1.0) + (A - 1.0) * cw - twoSqrtAAlpha) / a0);
}

void EffectsChain::BiquadFilter::setPeaking(double sr, float freqHz, float q, float gainDb) noexcept {
    const double A  = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
    const double w0 = 6.283185307179586 * std::clamp(static_cast<double>(freqHz), 20.0, sr * 0.45) / sr;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * std::max(0.1, static_cast<double>(q)));

    const double a0 = 1.0 + alpha / A;
    b0 = static_cast<float>((1.0 + alpha * A) / a0);
    b1 = static_cast<float>((-2.0 * cw) / a0);
    b2 = static_cast<float>((1.0 - alpha * A) / a0);
    a1 = b1;
    a2 = static_cast<float>((1.0 - alpha / A) / a0);
}

void EffectsChain::BiquadFilter::setHighShelf(double sr, float freqHz, float gainDb) noexcept {
    const double A  = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
    const double w0 = 6.283185307179586 * std::clamp(static_cast<double>(freqHz), 20.0, sr * 0.45) / sr;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw * 0.5 * std::sqrt(2.0);
    const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    const double a0 = (A + 1.0) - (A - 1.0) * cw + twoSqrtAAlpha;
    b0 = static_cast<float>((A * ((A + 1.0) + (A - 1.0) * cw + twoSqrtAAlpha)) / a0);
    b1 = static_cast<float>((-2.0 * A * ((A - 1.0) + (A + 1.0) * cw)) / a0);
    b2 = static_cast<float>((A * ((A + 1.0) + (A - 1.0) * cw - twoSqrtAAlpha)) / a0);
    a1 = static_cast<float>((2.0 * ((A - 1.0) - (A + 1.0) * cw)) / a0);
    a2 = static_cast<float>(((A + 1.0) - (A - 1.0) * cw - twoSqrtAAlpha) / a0);
}

EffectsChain::EffectsChain() {
    prepare(44100.0, 512);
}

void EffectsChain::prepare(double sampleRate, int /*maxBlockSize*/) {
    sampleRate_ = std::max(8000.0, sampleRate);

    // 50 ms chorus buffer
    const std::size_t chorusLen = static_cast<std::size_t>(sampleRate_ * 0.050) + 8;
    chorusBufL_.assign(chorusLen, 0.0f);
    chorusBufR_.assign(chorusLen, 0.0f);
    chorusWriteIdx_ = 0;

    // 2.0 second stereo delay buffer
    const std::size_t delayLen = static_cast<std::size_t>(sampleRate_ * 2.0) + 8;
    delayBufL_.assign(delayLen, 0.0f);
    delayBufR_.assign(delayLen, 0.0f);
    delayWriteIdx_ = 0;

    // Freeverb/Schroeder comb tunings scaled to sample rate
    const double srScale = sampleRate_ / 44100.0;
    constexpr std::size_t kCombBase[4] = {1116, 1188, 1277, 1356};
    constexpr std::size_t kAllPassBase[2] = {556, 441};
    for (std::size_t i = 0; i < 4; ++i) {
        reverbCombsL_[i].init(static_cast<std::size_t>(kCombBase[i] * srScale));
        reverbCombsR_[i].init(static_cast<std::size_t>((kCombBase[i] + 23) * srScale));
    }
    for (std::size_t i = 0; i < 2; ++i) {
        reverbAllPassL_[i].init(static_cast<std::size_t>(kAllPassBase[i] * srScale));
        reverbAllPassR_[i].init(static_cast<std::size_t>((kAllPassBase[i] + 23) * srScale));
    }

    reset();
}

void EffectsChain::reset() noexcept {
    lowShelf_.reset();
    midPeak_.reset();
    highShelf_.reset();
    std::fill(chorusBufL_.begin(), chorusBufL_.end(), 0.0f);
    std::fill(chorusBufR_.begin(), chorusBufR_.end(), 0.0f);
    std::fill(delayBufL_.begin(), delayBufL_.end(), 0.0f);
    std::fill(delayBufR_.begin(), delayBufR_.end(), 0.0f);
    delayDampL_ = delayDampR_ = 0.0f;
    for (auto& c : reverbCombsL_) c.clear();
    for (auto& c : reverbCombsR_) c.clear();
    for (auto& a : reverbAllPassL_) a.clear();
    for (auto& a : reverbAllPassR_) a.clear();
    compEnvelope_ = 0.0f;
    compGainReductionDb_.store(0.0f, std::memory_order_relaxed);
}

void EffectsChain::processStereoBlock(float* inOutLeft, float* inOutRight, int numSamples) noexcept {
    if (!inOutLeft || !inOutRight || numSamples <= 0) return;

    // 1. 3-Band EQ
    if (params_.eqEnabled.load(std::memory_order_relaxed)) {
        const float lowDb  = std::clamp(params_.eqLowGainDb.load(std::memory_order_relaxed), -15.0f, 15.0f);
        const float midDb  = std::clamp(params_.eqMidGainDb.load(std::memory_order_relaxed), -15.0f, 15.0f);
        const float highDb = std::clamp(params_.eqHighGainDb.load(std::memory_order_relaxed), -15.0f, 15.0f);
        lowShelf_.setLowShelf(sampleRate_, 120.0f, lowDb);
        midPeak_.setPeaking(sampleRate_, 1000.0f, 0.8f, midDb);
        highShelf_.setHighShelf(sampleRate_, 8000.0f, highDb);

        for (int i = 0; i < numSamples; ++i) {
            lowShelf_.processStereo(inOutLeft[i], inOutRight[i]);
            midPeak_.processStereo(inOutLeft[i], inOutRight[i]);
            highShelf_.processStereo(inOutLeft[i], inOutRight[i]);
        }
    }

    // 2. Quadrature Stereo Chorus
    if (params_.chorusEnabled.load(std::memory_order_relaxed) && !chorusBufL_.empty()) {
        const float rateHz = std::clamp(params_.chorusRateHz.load(std::memory_order_relaxed), 0.1f, 5.0f);
        const float depth  = std::clamp(params_.chorusDepth.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float mix    = std::clamp(params_.chorusMix.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const double phaseInc = static_cast<double>(rateHz) / sampleRate_;
        const std::size_t bufSize = chorusBufL_.size();
        const double baseDelaySamples = 0.012 * sampleRate_;
        const double modDepthSamples  = 0.007 * static_cast<double>(depth) * sampleRate_;

        for (int i = 0; i < numSamples; ++i) {
            const float dryL = inOutLeft[i];
            const float dryR = inOutRight[i];
            chorusBufL_[chorusWriteIdx_] = dryL;
            chorusBufR_[chorusWriteIdx_] = dryR;

            const double lfoL = std::sin(chorusPhase_ * 6.283185307179586);
            const double lfoR = std::cos(chorusPhase_ * 6.283185307179586);
            chorusPhase_ += phaseInc;
            if (chorusPhase_ >= 1.0) chorusPhase_ -= 1.0;

            auto readDelay = [&](const std::vector<float>& buf, double delaySamps) -> float {
                double readPos = static_cast<double>(chorusWriteIdx_) - delaySamps;
                while (readPos < 0.0) readPos += static_cast<double>(bufSize);
                const std::size_t idx0 = static_cast<std::size_t>(readPos) % bufSize;
                const std::size_t idx1 = (idx0 + 1) % bufSize;
                const float frac = static_cast<float>(readPos - std::floor(readPos));
                return buf[idx0] * (1.0f - frac) + buf[idx1] * frac;
            };

            const float wetL = readDelay(chorusBufL_, baseDelaySamples + lfoL * modDepthSamples);
            const float wetR = readDelay(chorusBufR_, baseDelaySamples + lfoR * modDepthSamples);

            inOutLeft[i]  = dryL * (1.0f - mix) + wetL * mix;
            inOutRight[i] = dryR * (1.0f - mix) + wetR * mix;
            if (++chorusWriteIdx_ >= bufSize) chorusWriteIdx_ = 0;
        }
    }

    // 3. Stereo Ping-Pong Delay
    if (params_.delayEnabled.load(std::memory_order_relaxed) && !delayBufL_.empty()) {
        const float timeMs = std::clamp(params_.delayTimeMs.load(std::memory_order_relaxed), 10.0f, 1500.0f);
        const float fb     = std::clamp(params_.delayFeedback.load(std::memory_order_relaxed), 0.0f, 0.90f);
        const float mix    = std::clamp(params_.delayMix.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const std::size_t bufSize = delayBufL_.size();
        const std::size_t delaySamples = std::clamp<std::size_t>(
            static_cast<std::size_t>((timeMs * 0.001f) * static_cast<float>(sampleRate_)), 1, bufSize - 2);

        for (int i = 0; i < numSamples; ++i) {
            const std::size_t readIdx = (delayWriteIdx_ + bufSize - delaySamples) % bufSize;
            const float delayedL = delayBufL_[readIdx];
            const float delayedR = delayBufR_[readIdx];

            // Gentle one-pole high-frequency damping in feedback loop
            delayDampL_ = 0.7f * delayedL + 0.3f * delayDampL_;
            delayDampR_ = 0.7f * delayedR + 0.3f * delayDampR_;
            if (std::abs(delayDampL_) < 1.0e-18f) delayDampL_ = 0.0f;
            if (std::abs(delayDampR_) < 1.0e-18f) delayDampR_ = 0.0f;

            // Cross-feed L -> R and R -> L for ping-pong stereo image
            delayBufL_[delayWriteIdx_] = inOutLeft[i]  + delayDampR_ * fb;
            delayBufR_[delayWriteIdx_] = inOutRight[i] + delayDampL_ * fb;

            inOutLeft[i]  = inOutLeft[i]  * (1.0f - mix) + delayedL * mix;
            inOutRight[i] = inOutRight[i] * (1.0f - mix) + delayedR * mix;
            if (++delayWriteIdx_ >= bufSize) delayWriteIdx_ = 0;
        }
    }

    // 4. Algorithmic Stereo Reverb (Schroeder / Freeverb)
    if (params_.reverbEnabled.load(std::memory_order_relaxed)) {
        const float room = std::clamp(params_.reverbRoomSize.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float damp = std::clamp(params_.reverbDamping.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float mix  = std::clamp(params_.reverbMix.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float feedback = 0.70f + room * 0.26f;

        for (int i = 0; i < numSamples; ++i) {
            const float inputMono = (inOutLeft[i] + inOutRight[i]) * 0.015f;
            float outL = 0.0f;
            float outR = 0.0f;
            for (std::size_t c = 0; c < 4; ++c) {
                outL += reverbCombsL_[c].process(inputMono, feedback, damp);
                outR += reverbCombsR_[c].process(inputMono, feedback, damp);
            }
            for (std::size_t a = 0; a < 2; ++a) {
                outL = reverbAllPassL_[a].process(outL);
                outR = reverbAllPassR_[a].process(outR);
            }
            inOutLeft[i]  = inOutLeft[i]  * (1.0f - mix) + outL * mix * 3.5f;
            inOutRight[i] = inOutRight[i] * (1.0f - mix) + outR * mix * 3.5f;
        }
    }

    // 5. Master Bus Compressor & Soft Saturation
    if (params_.compEnabled.load(std::memory_order_relaxed)) {
        const float threshDb = std::clamp(params_.compThresholdDb.load(std::memory_order_relaxed), -40.0f, 0.0f);
        const float ratio    = std::clamp(params_.compRatio.load(std::memory_order_relaxed), 1.0f, 20.0f);
        const float makeup   = dbToLinear(std::clamp(params_.compMakeupDb.load(std::memory_order_relaxed), 0.0f, 18.0f));
        const float atkCoeff = static_cast<float>(std::exp(-1.0 / (0.008 * sampleRate_)));
        const float relCoeff = static_cast<float>(std::exp(-1.0 / (0.120 * sampleRate_)));
        const float slope    = 1.0f - (1.0f / ratio);

        float maxGrDb = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            const float peakIn = std::max(std::abs(inOutLeft[i]), std::abs(inOutRight[i]));
            if (peakIn > compEnvelope_) {
                compEnvelope_ = atkCoeff * compEnvelope_ + (1.0f - atkCoeff) * peakIn;
            } else {
                compEnvelope_ = relCoeff * compEnvelope_ + (1.0f - relCoeff) * peakIn;
            }
            if (compEnvelope_ < 1.0e-18f) compEnvelope_ = 0.0f;

            float gainLinear = makeup;
            if (compEnvelope_ > 0.0001f) {
                const float envDb = 20.0f * std::log10(compEnvelope_);
                if (envDb > threshDb) {
                    const float grDb = (envDb - threshDb) * slope;
                    maxGrDb = std::max(maxGrDb, grDb);
                    gainLinear *= dbToLinear(-grDb);
                }
            }
            inOutLeft[i]  = std::tanh(inOutLeft[i]  * gainLinear);
            inOutRight[i] = std::tanh(inOutRight[i] * gainLinear);
        }
        compGainReductionDb_.store(maxGrDb, std::memory_order_relaxed);
    } else {
        compGainReductionDb_.store(0.0f, std::memory_order_relaxed);
    }
}

} // namespace konos
