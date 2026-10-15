#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "SamplerParameters.h"
#include "LockFreeCommandQueue.h"
#include "../Voices/SamplerVoice.h"
#include "../Instruments/InstrumentPatch.h"
#include "../Modulation/ModulationMatrix.h"
#include "../Effects/EffectsChain.h"
#include "../Streaming/DiskSampleStreamer.h"

namespace konos {

struct MidiEvent {
    int     sampleOffset{0};
    uint8_t status{0};
    uint8_t data1{0};
    uint8_t data2{0};

    static MidiEvent noteOn(uint8_t channel, uint8_t note, uint8_t vel, int offset = 0) noexcept {
        return MidiEvent{offset, static_cast<uint8_t>(0x90 | (channel & 0x0F)), note, vel};
    }
    static MidiEvent noteOff(uint8_t channel, uint8_t note, uint8_t vel = 0, int offset = 0) noexcept {
        return MidiEvent{offset, static_cast<uint8_t>(0x80 | (channel & 0x0F)), note, vel};
    }
    static MidiEvent controlChange(uint8_t channel, uint8_t cc, uint8_t value, int offset = 0) noexcept {
        return MidiEvent{offset, static_cast<uint8_t>(0xB0 | (channel & 0x0F)), cc, value};
    }
    static MidiEvent pitchBend(uint8_t channel, uint16_t value14Bit, int offset = 0) noexcept {
        return MidiEvent{
            offset,
            static_cast<uint8_t>(0xE0 | (channel & 0x0F)),
            static_cast<uint8_t>(value14Bit & 0x7F),
            static_cast<uint8_t>((value14Bit >> 7) & 0x7F)
        };
    }
};

class SamplerEngine {
public:
    static constexpr std::size_t kMaxVoices     = 64;
    static constexpr std::size_t kMaxRackSlots  = 4;

    SamplerEngine();
    ~SamplerEngine() = default;

    void prepare(double sampleRate, int maxBlockSize) noexcept;
    void reset() noexcept;

    bool setInstrumentPatchAsync(std::shared_ptr<const InstrumentPatch> patch) noexcept;
    bool setRackSlotPatchAsync(std::size_t slotIndex, std::shared_ptr<const InstrumentPatch> patch) noexcept;
    bool duplicateRackSlotAsync(std::size_t sourceSlot, std::size_t destSlot) noexcept;
    bool clearRackSlotAsync(std::size_t slotIndex) noexcept;
    bool triggerPreviewNoteAsync(uint8_t midiNote = 60, uint8_t velocity = 100) noexcept;
    void reclaimRetiredPatches() noexcept;

    void setSelectedRackSlot(std::size_t slotIndex) noexcept {
        selectedRackSlot_.store(std::min(slotIndex, kMaxRackSlots - 1), std::memory_order_relaxed);
    }
    std::size_t getSelectedRackSlot() const noexcept {
        return selectedRackSlot_.load(std::memory_order_relaxed);
    }

    RackSlotState&       getRackSlotState(std::size_t slotIndex) noexcept {
        return rackSlots_[std::min(slotIndex, kMaxRackSlots - 1)];
    }
    const RackSlotState& getRackSlotState(std::size_t slotIndex) const noexcept {
        return rackSlots_[std::min(slotIndex, kMaxRackSlots - 1)];
    }

    std::shared_ptr<const InstrumentPatch> getRackSlotPatchForUI(std::size_t slotIndex) const noexcept {
        std::lock_guard<std::mutex> lock(nonRealtimeMutex_);
        return uiVisibleSlotPatches_[std::min(slotIndex, kMaxRackSlots - 1)];
    }
    std::shared_ptr<const InstrumentPatch> getCurrentPatchForUI() const noexcept {
        return getRackSlotPatchForUI(getSelectedRackSlot());
    }

    void renderBlock(float* outLeft,
                     float* outRight,
                     int numSamples,
                     const MidiEvent* midiEvents,
                     std::size_t numMidiEvents) noexcept;

    SamplerParameterBlock&       getParameters() noexcept { return params_; }
    const SamplerParameterBlock& getParameters() const noexcept { return params_; }

    ModulationMatrix&       getModulationMatrix() noexcept { return modMatrix_; }
    const ModulationMatrix& getModulationMatrix() const noexcept { return modMatrix_; }

    EffectsChain&       getEffectsChain() noexcept { return effectsChain_; }
    const EffectsChain& getEffectsChain() const noexcept { return effectsChain_; }

    DiskSampleStreamer&       getDiskStreamer() noexcept { return diskStreamer_; }
    const DiskSampleStreamer& getDiskStreamer() const noexcept { return diskStreamer_; }

    int   getActiveVoiceCount() const noexcept { return activeVoiceCount_.load(std::memory_order_relaxed); }
    float getPeakLeft() const noexcept { return peakLeft_.load(std::memory_order_relaxed); }
    float getPeakRight() const noexcept { return peakRight_.load(std::memory_order_relaxed); }
    float getCpuLoadPercent() const noexcept { return cpuLoadPercent_.load(std::memory_order_relaxed); }
    std::size_t getSampleMemoryBytes() const noexcept { return sampleMemoryBytes_.load(std::memory_order_relaxed); }
    bool  isNoteActive(uint8_t midiNote) const noexcept;

    static std::shared_ptr<SampleBuffer> createDefaultSynthPianoSample(double sampleRate = 44100.0);
    static std::shared_ptr<InstrumentPatch> createFactoryPatchById(const std::string& factoryUri,
                                                                   double sampleRate = 44100.0);

private:
    void processPendingCommands() noexcept;
    void handleMidiEvent(const MidiEvent& ev) noexcept;
    void handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) noexcept;
    void handleNoteOff(uint8_t channel, uint8_t note) noexcept;
    void renderSubBlock(float* outLeft, float* outRight, int startSample, int numSamples) noexcept;
    SamplerVoice* allocateVoice(std::size_t slotIndex, uint8_t midiChannel, uint8_t midiNote) noexcept;
    bool isSlotAudible(std::size_t slotIdx, bool anySoloActive) const noexcept;
    void updateTotalSampleMemoryTelemetry() noexcept;

    double sampleRate_{44100.0};
    int    maxBlockSize_{512};

    SamplerParameterBlock params_{};
    SmoothedParameter     smoothedMasterGain_{};
    ModulationMatrix      modMatrix_{};
    EffectsChain          effectsChain_{};
    DiskSampleStreamer    diskStreamer_{};

    std::array<RackSlotState, kMaxRackSlots> rackSlots_{};
    std::array<std::shared_ptr<const InstrumentPatch>, kMaxRackSlots> activeSlotPatches_{};
    std::array<std::shared_ptr<const InstrumentPatch>, kMaxRackSlots> uiVisibleSlotPatches_{};
    std::atomic<std::size_t> selectedRackSlot_{0};

    // Protects ONLY non-real-time producer pushes and UI patch reads (never locked on audio thread)
    mutable std::mutex nonRealtimeMutex_{};

    std::array<SamplerVoice, kMaxVoices> voices_{};
    std::vector<float> slotScratchL_{};
    std::vector<float> slotScratchR_{};
    uint64_t noteOrderCounter_{0};
    uint32_t roundRobinCounter_{0};

    bool  sustainPedalDown_{false};
    float pitchBendNorm_{0.0f};
    float modWheelNorm_{0.0f};
    float channelVolume_{1.0f};
    float expressionGain_{1.0f};

    LockFreeSpscQueue<EngineCommand, 64> commandQueue_{};
    LockFreeSpscQueue<std::shared_ptr<const InstrumentPatch>, 64> retiredPatchQueue_{};

    std::atomic<int>         activeVoiceCount_{0};
    std::atomic<float>       peakLeft_{0.0f};
    std::atomic<float>       peakRight_{0.0f};
    std::atomic<float>       cpuLoadPercent_{0.0f};
    std::atomic<std::size_t> sampleMemoryBytes_{0};
    std::atomic<uint64_t>    activeNotesLow_{0};
    std::atomic<uint64_t>    activeNotesHigh_{0};
};

} // namespace konos
