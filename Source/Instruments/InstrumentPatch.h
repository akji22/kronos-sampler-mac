#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include "../SamplePlayback/SampleBuffer.h"

namespace konos {

enum class LoopMode : uint8_t {
    NoLoop          = 0,
    ForwardLoop     = 1,
    OneShot         = 2
};

/**
 * Defines a single mapped region/zone in an instrument (SFZ region, SF2 zone,
 * or single WAV sample mapped across the keyboard).
 */
struct InstrumentZone {
    std::string name{"Default Zone"};
    std::shared_ptr<const SampleBuffer> sample{};

    // Key & velocity mapping
    uint8_t loKey{0};
    uint8_t hiKey{127};
    uint8_t rootKey{60};
    uint8_t loVel{1};
    uint8_t hiVel{127};

    // Round-robin group sequencing
    uint32_t seqLength{1};
    uint32_t seqPosition{1};

    // Tuning & level offsets
    int32_t transposeSemitones{0};
    float   tuneCents{0.0f};
    float   volumeDb{0.0f};
    float   pan{0.0f}; // [-1.0, +1.0]

    // Sample boundaries & loop points (in sample frames; 0 = use sample defaults)
    uint64_t offsetFrame{0};
    uint64_t endFrame{0};
    LoopMode loopMode{LoopMode::NoLoop};
    uint64_t loopStartFrame{0};
    uint64_t loopEndFrame{0};

    // Optional per-zone envelope / filter overrides (< 0 means use global parameter)
    float attackMsOverride{-1.0f};
    float decayMsOverride{-1.0f};
    float sustainLevelOverride{-1.0f};
    float releaseMsOverride{-1.0f};
    float cutoffHzOverride{-1.0f};
    float resonanceOverride{-1.0f};

    bool matchesNoteAndVelocity(uint8_t midiNote, uint8_t velocity, uint32_t rrCounter) const noexcept {
        if (!sample || sample->getNumFrames() == 0) return false;
        if (midiNote < loKey || midiNote > hiKey) return false;
        if (velocity < loVel || velocity > hiVel) return false;
        if (seqLength > 1) {
            const uint32_t activeStep = (rrCounter % seqLength) + 1;
            if (activeStep != seqPosition) return false;
        }
        return true;
    }
};

/**
 * Immutable instrument patch containing metadata and 1..N mapped sample zones.
 * Shared with the audio thread via std::shared_ptr swaps over a lock-free queue.
 */
struct InstrumentPatch {
    std::string name{"Init Patch"};
    std::string sourcePath{};
    std::string formatName{"WAV"};
    std::vector<InstrumentZone> zones{};
    std::vector<std::string> warnings{};

    std::size_t getTotalSampleMemoryBytes() const noexcept {
        std::size_t total = 0;
        for (const auto& z : zones) {
            if (z.sample) {
                total += z.sample->getMemoryBytes();
            }
        }
        return total;
    }

    static std::shared_ptr<InstrumentPatch> createFromSingleSample(
        const std::string& name,
        std::shared_ptr<const SampleBuffer> buffer) {
        auto patch = std::make_shared<InstrumentPatch>();
        patch->name = name;
        patch->sourcePath = buffer ? buffer->getSourcePath() : "";
        patch->formatName = "WAV/AIFF";

        if (buffer && buffer->getNumFrames() > 0) {
            InstrumentZone zone;
            zone.name = name;
            zone.sample = buffer;
            zone.loKey = 0;
            zone.hiKey = 127;
            zone.rootKey = buffer->getRootMidiNote();
            zone.loVel = 1;
            zone.hiVel = 127;
            zone.endFrame = buffer->getNumFrames();
            if (buffer->hasEmbeddedLoop()) {
                zone.loopMode = LoopMode::ForwardLoop;
                zone.loopStartFrame = buffer->getLoopStartFrame();
                zone.loopEndFrame = buffer->getLoopEndFrame();
            }
            patch->zones.push_back(std::move(zone));
        }
        return patch;
    }
};

} // namespace konos
