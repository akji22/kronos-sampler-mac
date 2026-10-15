#pragma once

#include <cstdint>
#include "../AudioEngine/SamplerParameters.h"
#include "../Instruments/InstrumentPatch.h"
#include "../Modulation/ModulationMatrix.h"
#include "AdsrEnvelope.h"
#include "StateVariableFilter.h"

namespace konos {

/**
 * Trivially copyable, zero-heap POD snapshot of a zone's playback parameters.
 * Guarantees that SamplerVoice never copies std::string or decrements a
 * std::shared_ptr<const SampleBuffer> reference count on the real-time audio thread.
 */
struct VoiceZoneParams {
    uint8_t  loKey{0};
    uint8_t  hiKey{127};
    uint8_t  rootKey{60};
    uint8_t  loVel{1};
    uint8_t  hiVel{127};
    int32_t  transposeSemitones{0};
    float    tuneCents{0.0f};
    float    volumeDb{0.0f};
    float    pan{0.0f};
    uint64_t offsetFrame{0};
    uint64_t endFrame{0};
    LoopMode loopMode{LoopMode::NoLoop};
    uint64_t loopStartFrame{0};
    uint64_t loopEndFrame{0};
    float    attackMsOverride{-1.0f};
    float    decayMsOverride{-1.0f};
    float    sustainLevelOverride{-1.0f};
    float    releaseMsOverride{-1.0f};
    float    cutoffHzOverride{-1.0f};
    float    resonanceOverride{-1.0f};
};

class SamplerVoice {
public:
    SamplerVoice() = default;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void startVoice(std::size_t rackSlotIndex,
                    const InstrumentZone& zone,
                    uint8_t midiChannel,
                    uint8_t midiNote,
                    uint8_t velocity,
                    uint64_t noteOnOrderId,
                    const SamplerParameterBlock& params) noexcept;

    void noteOff(bool sustainPedalDown) noexcept;
    void releaseSustainPedal() noexcept;
    void triggerSteal() noexcept;

    void renderNextBlock(float* outLeft,
                         float* outRight,
                         int numSamples,
                         const SamplerParameterBlock& params,
                         const RackSlotState& slotState,
                         const ModulationMatrix& modMatrix,
                         float pitchBendNorm,
                         float modWheelNorm,
                         float channelGain) noexcept;

    bool        isActive() const noexcept { return envelope_.isActive() && activeZoneSample_ != nullptr; }
    bool        isReleasing() const noexcept {
        return envelope_.getStage() == EnvelopeStage::Release ||
               envelope_.getStage() == EnvelopeStage::StealingFade;
    }
    std::size_t getRackSlotIndex() const noexcept { return rackSlotIndex_; }
    uint8_t     getMidiNote() const noexcept { return midiNote_; }
    uint8_t     getMidiChannel() const noexcept { return midiChannel_; }
    uint64_t    getOrderId() const noexcept { return orderId_; }
    float       getCurrentEnvelopeLevel() const noexcept { return envelope_.getCurrentValue(); }

private:
    double hostSampleRate_{44100.0};

    std::size_t         rackSlotIndex_{0};
    const SampleBuffer* activeZoneSample_{nullptr};
    VoiceZoneParams     zoneParams_{};

    uint8_t  midiChannel_{0};
    uint8_t  midiNote_{60};
    uint8_t  velocity_{100};
    uint64_t orderId_{0};
    bool     sustainedByPedal_{false};

    double   playbackPhase_{0.0};
    float    velocityGain_{1.0f};
    float    zoneLinearGain_{1.0f};
    double   lfoPhase0to1_{0.0};

    AdsrEnvelope        envelope_{};
    StateVariableFilter filter_{};
};

} // namespace konos
