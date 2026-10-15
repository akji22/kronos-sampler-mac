#include "SamplerVoice.h"
#include <cmath>
#include <algorithm>

namespace konos {

void SamplerVoice::prepare(double sampleRate) noexcept {
    hostSampleRate_ = std::max(8000.0, sampleRate);
    envelope_.setSampleRate(hostSampleRate_);
    filter_.reset();
}

void SamplerVoice::reset() noexcept {
    activeZoneSample_ = nullptr;
    zoneParams_       = VoiceZoneParams{};
    sustainedByPedal_ = false;
    playbackPhase_    = 0.0;
    lfoPhase0to1_     = 0.0;
    envelope_.reset();
    filter_.reset();
}

void SamplerVoice::startVoice(std::size_t rackSlotIndex,
                              const InstrumentZone& zone,
                              uint8_t midiChannel,
                              uint8_t midiNote,
                              uint8_t velocity,
                              uint64_t noteOnOrderId,
                              const SamplerParameterBlock& params) noexcept {
    if (!zone.sample || zone.sample->getNumFrames() == 0) {
        reset();
        return;
    }

    rackSlotIndex_    = rackSlotIndex;
    activeZoneSample_ = zone.sample.get();

    // Copy only POD parameters — zero heap allocations, zero shared_ptr ref-count ops
    zoneParams_.loKey                = zone.loKey;
    zoneParams_.hiKey                = zone.hiKey;
    zoneParams_.rootKey              = zone.rootKey;
    zoneParams_.loVel                = zone.loVel;
    zoneParams_.hiVel                = zone.hiVel;
    zoneParams_.transposeSemitones   = zone.transposeSemitones;
    zoneParams_.tuneCents            = zone.tuneCents;
    zoneParams_.volumeDb             = zone.volumeDb;
    zoneParams_.pan                  = zone.pan;
    zoneParams_.offsetFrame          = zone.offsetFrame;
    zoneParams_.endFrame             = zone.endFrame;
    zoneParams_.loopMode             = zone.loopMode;
    zoneParams_.loopStartFrame       = zone.loopStartFrame;
    zoneParams_.loopEndFrame         = zone.loopEndFrame;
    zoneParams_.attackMsOverride     = zone.attackMsOverride;
    zoneParams_.decayMsOverride      = zone.decayMsOverride;
    zoneParams_.sustainLevelOverride = zone.sustainLevelOverride;
    zoneParams_.releaseMsOverride    = zone.releaseMsOverride;
    zoneParams_.cutoffHzOverride     = zone.cutoffHzOverride;
    zoneParams_.resonanceOverride    = zone.resonanceOverride;

    midiChannel_      = midiChannel;
    midiNote_         = midiNote;
    velocity_         = velocity;
    orderId_          = noteOnOrderId;
    sustainedByPedal_ = false;
    lfoPhase0to1_     = 0.0;

    const uint64_t totalFrames = activeZoneSample_->getNumFrames();
    const float startNorm = std::clamp(params.sampleStartNorm.load(std::memory_order_relaxed), 0.0f, 0.99f);
    const float endNorm   = std::clamp(params.sampleEndNorm.load(std::memory_order_relaxed), startNorm + 0.01f, 1.0f);

    const uint64_t paramStartFrame = static_cast<uint64_t>(startNorm * static_cast<float>(totalFrames - 1));
    const uint64_t paramEndFrame   = static_cast<uint64_t>(endNorm   * static_cast<float>(totalFrames));

    const uint64_t effectiveStart = std::max(zoneParams_.offsetFrame, paramStartFrame);
    const uint64_t effectiveEnd   = zoneParams_.endFrame > 0
                                        ? std::min(zoneParams_.endFrame, paramEndFrame)
                                        : paramEndFrame;

    const bool reverse = params.reversePlayback.load(std::memory_order_relaxed);
    playbackPhase_ = reverse
                         ? static_cast<double>(std::max<uint64_t>(effectiveStart, effectiveEnd - 1))
                         : static_cast<double>(effectiveStart);

    const float velNorm = static_cast<float>(std::clamp<uint8_t>(velocity, 1, 127)) / 127.0f;
    const float sens    = std::clamp(params.velocitySensitivity.load(std::memory_order_relaxed), 0.0f, 1.0f);
    velocityGain_       = (1.0f - sens) + sens * (velNorm * velNorm);
    zoneLinearGain_     = dbToLinear(zoneParams_.volumeDb);

    const float atk = zoneParams_.attackMsOverride  >= 0.0f ? zoneParams_.attackMsOverride  : params.attackMs.load(std::memory_order_relaxed);
    const float dec = zoneParams_.decayMsOverride   >= 0.0f ? zoneParams_.decayMsOverride   : params.decayMs.load(std::memory_order_relaxed);
    const float sus = zoneParams_.sustainLevelOverride >= 0.0f ? zoneParams_.sustainLevelOverride : params.sustainLevel.load(std::memory_order_relaxed);
    const float rel = zoneParams_.releaseMsOverride >= 0.0f ? zoneParams_.releaseMsOverride : params.releaseMs.load(std::memory_order_relaxed);

    envelope_.setSampleRate(hostSampleRate_);
    envelope_.setParameters(atk, dec, sus, rel);
    envelope_.noteOn();

    filter_.reset();
}

void SamplerVoice::noteOff(bool sustainPedalDown) noexcept {
    if (!isActive()) return;
    if (zoneParams_.loopMode == LoopMode::OneShot) return;

    if (sustainPedalDown) {
        sustainedByPedal_ = true;
    } else {
        envelope_.noteOff();
    }
}

void SamplerVoice::releaseSustainPedal() noexcept {
    if (sustainedByPedal_) {
        sustainedByPedal_ = false;
        envelope_.noteOff();
    }
}

void SamplerVoice::triggerSteal() noexcept {
    envelope_.triggerStealFade();
}

void SamplerVoice::renderNextBlock(float* outLeft,
                                   float* outRight,
                                   int numSamples,
                                   const SamplerParameterBlock& params,
                                   const RackSlotState& slotState,
                                   const ModulationMatrix& modMatrix,
                                   float pitchBendNorm,
                                   float modWheelNorm,
                                   float channelGain) noexcept {
    if (!isActive() || !activeZoneSample_ || numSamples <= 0) return;

    const uint64_t totalFrames = activeZoneSample_->getNumFrames();
    if (totalFrames == 0) {
        reset();
        return;
    }

    const float atk = zoneParams_.attackMsOverride  >= 0.0f ? zoneParams_.attackMsOverride  : params.attackMs.load(std::memory_order_relaxed);
    const float dec = zoneParams_.decayMsOverride   >= 0.0f ? zoneParams_.decayMsOverride   : params.decayMs.load(std::memory_order_relaxed);
    const float sus = zoneParams_.sustainLevelOverride >= 0.0f ? zoneParams_.sustainLevelOverride : params.sustainLevel.load(std::memory_order_relaxed);
    const float rel = zoneParams_.releaseMsOverride >= 0.0f ? zoneParams_.releaseMsOverride : params.releaseMs.load(std::memory_order_relaxed);
    envelope_.setParameters(atk, dec, sus, rel);

    const float baseCutoff = zoneParams_.cutoffHzOverride >= 0.0f
                                 ? zoneParams_.cutoffHzOverride
                                 : params.filterCutoffHz.load(std::memory_order_relaxed);
    const float resonance  = zoneParams_.resonanceOverride >= 0.0f
                                 ? zoneParams_.resonanceOverride
                                 : params.filterResonance.load(std::memory_order_relaxed);
    const auto  filterMode = static_cast<FilterType>(
        std::clamp(params.filterMode.load(std::memory_order_relaxed), 0, 2));

    const float velNorm = static_cast<float>(velocity_) / 127.0f;
    const float keyTrackBipolar = static_cast<float>(static_cast<int>(midiNote_) - 60) / 60.0f;
    const float baseLfoRateHz   = std::clamp(modMatrix.lfo1RateHz.load(std::memory_order_relaxed), 0.1f, 20.0f);
    const auto  lfoWave         = static_cast<LfoWaveform>(
        std::clamp(modMatrix.lfo1Waveform.load(std::memory_order_relaxed), 0, 3));

    const float lfoBlockSample = ModulationMatrix::computeLfoSample(lfoPhase0to1_, lfoWave);
    const ModulatedOffsets modOffsets = modMatrix.evaluate(
        lfoBlockSample, modWheelNorm, velNorm, keyTrackBipolar, pitchBendNorm, envelope_.getCurrentValue());

    const float modulatedCutoff = baseCutoff * std::pow(2.0f, modOffsets.filterCutoffOctaves);
    filter_.setParameters(hostSampleRate_, modulatedCutoff, resonance, filterMode);

    const float slotGainLinear = dbToLinear(slotState.volumeDb.load(std::memory_order_relaxed));
    const float combinedPan = std::clamp(
        zoneParams_.pan +
        slotState.pan.load(std::memory_order_relaxed) +
        params.masterPan.load(std::memory_order_relaxed) +
        modOffsets.panOffset,
        -1.0f, 1.0f);
    constexpr float kQuarterPi = 0.7853981633974483f;
    const float panAngle = (combinedPan + 1.0f) * kQuarterPi;
    const float panLeft  = std::cos(panAngle);
    const float panRight = std::sin(panAngle);

    const double sampleRateRatio = activeZoneSample_->getSampleRate() / hostSampleRate_;
    const double baseNoteDelta =
        static_cast<double>(static_cast<int>(midiNote_) - static_cast<int>(zoneParams_.rootKey))
        + static_cast<double>(zoneParams_.transposeSemitones)
        + static_cast<double>(zoneParams_.tuneCents) * 0.01
        + static_cast<double>(slotState.tuneSemitones.load(std::memory_order_relaxed))
        + static_cast<double>(params.masterTuneSemitones.load(std::memory_order_relaxed))
        + static_cast<double>(pitchBendNorm * params.pitchBendRangeSemitones.load(std::memory_order_relaxed));

    const float startNorm = std::clamp(params.sampleStartNorm.load(std::memory_order_relaxed), 0.0f, 0.99f);
    const float endNorm   = std::clamp(params.sampleEndNorm.load(std::memory_order_relaxed), startNorm + 0.01f, 1.0f);
    const double startBound = std::max<double>(
        static_cast<double>(zoneParams_.offsetFrame),
        static_cast<double>(startNorm) * static_cast<double>(totalFrames - 1));
    const double endBound = std::min<double>(
        zoneParams_.endFrame > 0 ? static_cast<double>(zoneParams_.endFrame) : static_cast<double>(totalFrames),
        static_cast<double>(endNorm) * static_cast<double>(totalFrames));

    const bool loopActive = params.loopEnabled.load(std::memory_order_relaxed) ||
                            (zoneParams_.loopMode == LoopMode::ForwardLoop);
    double loopStart = static_cast<double>(params.loopStartNorm.load(std::memory_order_relaxed)) * static_cast<double>(totalFrames);
    double loopEnd   = static_cast<double>(params.loopEndNorm.load(std::memory_order_relaxed))   * static_cast<double>(totalFrames);
    if (zoneParams_.loopMode == LoopMode::ForwardLoop && zoneParams_.loopEndFrame > zoneParams_.loopStartFrame) {
        loopStart = static_cast<double>(zoneParams_.loopStartFrame);
        loopEnd   = static_cast<double>(zoneParams_.loopEndFrame);
    }
    loopStart = std::clamp(loopStart, startBound, std::max(startBound, endBound - 2.0));
    loopEnd   = std::clamp(loopEnd, loopStart + 1.0, endBound);
    const double loopLength = std::max(1.0, loopEnd - loopStart);

    const bool reverse = params.reversePlayback.load(std::memory_order_relaxed);
    const uint32_t numChannels = activeZoneSample_->getNumChannels();
    const float voiceGain = velocityGain_ * zoneLinearGain_ * slotGainLinear * channelGain * modOffsets.volumeScale;

    const double effectiveLfoHz = std::clamp(baseLfoRateHz + modOffsets.lfo1RateDeltaHz, 0.05f, 30.0f);
    const double lfoPhaseInc = effectiveLfoHz / hostSampleRate_;

    for (int i = 0; i < numSamples; ++i) {
        const float env = envelope_.getNextSample();
        if (!envelope_.isActive()) {
            activeZoneSample_ = nullptr;
            break;
        }

        const float lfoSample = ModulationMatrix::computeLfoSample(lfoPhase0to1_, lfoWave);
        lfoPhase0to1_ += lfoPhaseInc;
        if (lfoPhase0to1_ >= 1.0) lfoPhase0to1_ -= 1.0;

        const double cc1Vibrato = static_cast<double>(lfoSample * modWheelNorm) * 0.35;
        const double totalNoteDelta = baseNoteDelta + cc1Vibrato + static_cast<double>(modOffsets.pitchSemitones * lfoSample);
        const double phaseIncrement = sampleRateRatio * std::pow(2.0, totalNoteDelta / 12.0);

        float sampleL = activeZoneSample_->interpolateCubicHermite(0, playbackPhase_);
        float sampleR = (numChannels > 1)
                            ? activeZoneSample_->interpolateCubicHermite(1, playbackPhase_)
                            : sampleL;

        filter_.processStereo(sampleL, sampleR);

        const float gain = env * voiceGain;
        outLeft[i]  += sampleL * gain * panLeft;
        outRight[i] += sampleR * gain * panRight;

        if (!reverse) {
            playbackPhase_ += phaseIncrement;
            if (loopActive && playbackPhase_ >= loopEnd) {
                playbackPhase_ = loopStart + std::fmod(playbackPhase_ - loopStart, loopLength);
            } else if (playbackPhase_ >= endBound) {
                reset();
                break;
            }
        } else {
            playbackPhase_ -= phaseIncrement;
            if (loopActive && playbackPhase_ <= loopStart) {
                playbackPhase_ = loopEnd - std::fmod(loopEnd - playbackPhase_, loopLength);
            } else if (playbackPhase_ <= startBound) {
                reset();
                break;
            }
        }
    }
}

} // namespace konos
