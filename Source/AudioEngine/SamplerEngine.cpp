#include "SamplerEngine.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace konos {

SamplerEngine::SamplerEngine() {
    slotScratchL_.resize(4096, 0.0f);
    slotScratchR_.resize(4096, 0.0f);

    auto defaultSample = createDefaultSynthPianoSample(44100.0);
    auto defaultPatch  = InstrumentPatch::createFromSingleSample("Factory Electric Grand (C4)", defaultSample);

    activeSlotPatches_[0]    = defaultPatch;
    uiVisibleSlotPatches_[0] = defaultPatch;
    rackSlots_[0].enabled.store(true, std::memory_order_relaxed);
    rackSlots_[0].midiChannel.store(0, std::memory_order_relaxed);
    rackSlots_[0].sampleMemoryBytes.store(defaultPatch->getTotalSampleMemoryBytes(), std::memory_order_relaxed);

    for (std::size_t s = 1; s < kMaxRackSlots; ++s) {
        rackSlots_[s].enabled.store(false, std::memory_order_relaxed);
        rackSlots_[s].midiChannel.store(static_cast<uint8_t>(s + 1), std::memory_order_relaxed);
    }

    updateTotalSampleMemoryTelemetry();
}

void SamplerEngine::prepare(double sampleRate, int maxBlockSize) noexcept {
    sampleRate_   = std::max(8000.0, sampleRate);
    maxBlockSize_ = std::max(1, maxBlockSize);

    if (static_cast<std::size_t>(maxBlockSize_) > slotScratchL_.size()) {
        slotScratchL_.resize(static_cast<std::size_t>(maxBlockSize_), 0.0f);
        slotScratchR_.resize(static_cast<std::size_t>(maxBlockSize_), 0.0f);
    }

    smoothedMasterGain_.reset(sampleRate_, 10.0, dbToLinear(params_.masterGainDb.load(std::memory_order_relaxed)));
    effectsChain_.prepare(sampleRate_, maxBlockSize_);

    for (auto& voice : voices_) {
        voice.prepare(sampleRate_);
    }
}

void SamplerEngine::reset() noexcept {
    for (auto& voice : voices_) {
        voice.reset();
    }
    effectsChain_.reset();
    sustainPedalDown_ = false;
    pitchBendNorm_    = 0.0f;
    modWheelNorm_     = 0.0f;
    channelVolume_    = 1.0f;
    expressionGain_   = 1.0f;
    activeVoiceCount_.store(0, std::memory_order_relaxed);
    activeNotesLow_.store(0, std::memory_order_relaxed);
    activeNotesHigh_.store(0, std::memory_order_relaxed);
    for (auto& slot : rackSlots_) {
        slot.activeVoices.store(0, std::memory_order_relaxed);
        slot.peakLeft.store(0.0f, std::memory_order_relaxed);
        slot.peakRight.store(0.0f, std::memory_order_relaxed);
    }
}

bool SamplerEngine::setInstrumentPatchAsync(std::shared_ptr<const InstrumentPatch> patch) noexcept {
    return setRackSlotPatchAsync(getSelectedRackSlot(), std::move(patch));
}

bool SamplerEngine::setRackSlotPatchAsync(std::size_t slotIndex,
                                          std::shared_ptr<const InstrumentPatch> patch) noexcept {
    if (slotIndex >= kMaxRackSlots) return false;
    std::lock_guard<std::mutex> lock(nonRealtimeMutex_);

    std::shared_ptr<const InstrumentPatch> oldPatch;
    while (retiredPatchQueue_.pop(oldPatch)) {
        oldPatch.reset();
    }

    uiVisibleSlotPatches_[slotIndex] = patch;
    rackSlots_[slotIndex].enabled.store(patch != nullptr, std::memory_order_relaxed);

    EngineCommand cmd;
    cmd.type      = EngineCommandType::SetInstrumentPatch;
    cmd.slotIndex = slotIndex;
    cmd.patch     = std::move(patch);
    return commandQueue_.push(std::move(cmd));
}

bool SamplerEngine::duplicateRackSlotAsync(std::size_t sourceSlot, std::size_t destSlot) noexcept {
    if (sourceSlot >= kMaxRackSlots || destSlot >= kMaxRackSlots || sourceSlot == destSlot) return false;
    std::shared_ptr<const InstrumentPatch> patch;
    {
        std::lock_guard<std::mutex> lock(nonRealtimeMutex_);
        patch = uiVisibleSlotPatches_[sourceSlot];
    }
    if (!patch) return false;

    rackSlots_[destSlot].volumeDb.store(rackSlots_[sourceSlot].volumeDb.load(std::memory_order_relaxed));
    rackSlots_[destSlot].pan.store(rackSlots_[sourceSlot].pan.load(std::memory_order_relaxed));
    rackSlots_[destSlot].tuneSemitones.store(rackSlots_[sourceSlot].tuneSemitones.load(std::memory_order_relaxed));
    rackSlots_[destSlot].midiChannel.store(rackSlots_[sourceSlot].midiChannel.load(std::memory_order_relaxed));
    return setRackSlotPatchAsync(destSlot, patch);
}

bool SamplerEngine::clearRackSlotAsync(std::size_t slotIndex) noexcept {
    return setRackSlotPatchAsync(slotIndex, nullptr);
}

bool SamplerEngine::triggerPreviewNoteAsync(uint8_t midiNote, uint8_t velocity) noexcept {
    std::lock_guard<std::mutex> lock(nonRealtimeMutex_);
    EngineCommand cmd;
    cmd.type            = EngineCommandType::TriggerPreviewNote;
    cmd.slotIndex       = getSelectedRackSlot();
    cmd.previewNote     = midiNote;
    cmd.previewVelocity = velocity;
    return commandQueue_.push(std::move(cmd));
}

void SamplerEngine::reclaimRetiredPatches() noexcept {
    std::lock_guard<std::mutex> lock(nonRealtimeMutex_);
    std::shared_ptr<const InstrumentPatch> oldPatch;
    while (retiredPatchQueue_.pop(oldPatch)) {
        oldPatch.reset();
    }
}

void SamplerEngine::updateTotalSampleMemoryTelemetry() noexcept {
    std::size_t totalBytes = 0;
    for (std::size_t s = 0; s < kMaxRackSlots; ++s) {
        const std::size_t slotBytes = activeSlotPatches_[s] ? activeSlotPatches_[s]->getTotalSampleMemoryBytes() : 0;
        rackSlots_[s].sampleMemoryBytes.store(slotBytes, std::memory_order_relaxed);
        totalBytes += slotBytes;
    }
    sampleMemoryBytes_.store(totalBytes, std::memory_order_relaxed);
}

void SamplerEngine::processPendingCommands() noexcept {
    EngineCommand cmd;
    while (commandQueue_.pop(cmd)) {
        if (cmd.type == EngineCommandType::SetInstrumentPatch) {
            const std::size_t s = std::min(cmd.slotIndex, kMaxRackSlots - 1);
            for (auto& voice : voices_) {
                if (voice.isActive() && voice.getRackSlotIndex() == s) {
                    voice.reset();
                }
            }
            if (activeSlotPatches_[s]) {
                retiredPatchQueue_.push(std::move(activeSlotPatches_[s]));
            }
            activeSlotPatches_[s] = std::move(cmd.patch);
            rackSlots_[s].enabled.store(activeSlotPatches_[s] != nullptr, std::memory_order_relaxed);
            updateTotalSampleMemoryTelemetry();
        } else if (cmd.type == EngineCommandType::TriggerPreviewNote) {
            handleNoteOn(0, cmd.previewNote, cmd.previewVelocity);
        } else if (cmd.type == EngineCommandType::AllNotesOff) {
            sustainPedalDown_ = false;
            for (auto& voice : voices_) {
                voice.noteOff(false);
            }
        } else if (cmd.type == EngineCommandType::ResetEngine) {
            reset();
        }
    }
}

bool SamplerEngine::isSlotAudible(std::size_t slotIdx, bool anySoloActive) const noexcept {
    if (!rackSlots_[slotIdx].enabled.load(std::memory_order_relaxed) || !activeSlotPatches_[slotIdx]) {
        return false;
    }
    if (rackSlots_[slotIdx].mute.load(std::memory_order_relaxed)) {
        return false;
    }
    if (anySoloActive && !rackSlots_[slotIdx].solo.load(std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

void SamplerEngine::renderBlock(float* outLeft,
                                float* outRight,
                                int numSamples,
                                const MidiEvent* midiEvents,
                                std::size_t numMidiEvents) noexcept {
    if (!outLeft || !outRight || numSamples <= 0) return;
    const auto tStart = std::chrono::steady_clock::now();

    std::memset(outLeft,  0, static_cast<std::size_t>(numSamples) * sizeof(float));
    std::memset(outRight, 0, static_cast<std::size_t>(numSamples) * sizeof(float));

    processPendingCommands();

    smoothedMasterGain_.setTarget(dbToLinear(params_.masterGainDb.load(std::memory_order_relaxed)));

    int currentFrame = 0;
    for (std::size_t idx = 0; idx < numMidiEvents; ++idx) {
        const MidiEvent& ev = midiEvents[idx];
        const int eventFrame = std::clamp(ev.sampleOffset, currentFrame, numSamples);
        if (eventFrame > currentFrame) {
            renderSubBlock(outLeft, outRight, currentFrame, eventFrame - currentFrame);
            currentFrame = eventFrame;
        }
        handleMidiEvent(ev);
    }

    if (currentFrame < numSamples) {
        renderSubBlock(outLeft, outRight, currentFrame, numSamples - currentFrame);
    }

    // Notify disk streamer worker if any voice requested a window refill
    diskStreamer_.notifyRefillNeeded();

    effectsChain_.processStereoBlock(outLeft, outRight, numSamples);

    float blockPeakL = 0.0f;
    float blockPeakR = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        const float masterGain = smoothedMasterGain_.getNextValue();
        outLeft[i]  *= masterGain;
        outRight[i] *= masterGain;
        blockPeakL = std::max(blockPeakL, std::abs(outLeft[i]));
        blockPeakR = std::max(blockPeakR, std::abs(outRight[i]));
    }

    peakLeft_.store(blockPeakL, std::memory_order_relaxed);
    peakRight_.store(blockPeakR, std::memory_order_relaxed);

    int totalActiveVoices = 0;
    std::array<int, kMaxRackSlots> slotVoiceCounts{};
    uint64_t lowMask  = 0;
    uint64_t highMask = 0;
    for (const auto& voice : voices_) {
        if (voice.isActive()) {
            ++totalActiveVoices;
            const std::size_t s = std::min(voice.getRackSlotIndex(), kMaxRackSlots - 1);
            ++slotVoiceCounts[s];
            const uint8_t note = voice.getMidiNote();
            if (note < 64) {
                lowMask |= (uint64_t{1} << note);
            } else if (note < 128) {
                highMask |= (uint64_t{1} << (note - 64));
            }
        }
    }
    for (std::size_t s = 0; s < kMaxRackSlots; ++s) {
        rackSlots_[s].activeVoices.store(slotVoiceCounts[s], std::memory_order_relaxed);
    }
    activeVoiceCount_.store(totalActiveVoices, std::memory_order_relaxed);
    activeNotesLow_.store(lowMask, std::memory_order_relaxed);
    activeNotesHigh_.store(highMask, std::memory_order_relaxed);

    const auto tEnd = std::chrono::steady_clock::now();
    const double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();
    const double bufferPeriodSec = static_cast<double>(numSamples) / sampleRate_;
    const float loadPct = bufferPeriodSec > 0.0 ? static_cast<float>((elapsedSec / bufferPeriodSec) * 100.0) : 0.0f;
    cpuLoadPercent_.store(loadPct, std::memory_order_relaxed);
}

bool SamplerEngine::isNoteActive(uint8_t midiNote) const noexcept {
    if (midiNote < 64) {
        return (activeNotesLow_.load(std::memory_order_relaxed) & (uint64_t{1} << midiNote)) != 0;
    }
    if (midiNote < 128) {
        return (activeNotesHigh_.load(std::memory_order_relaxed) & (uint64_t{1} << (midiNote - 64))) != 0;
    }
    return false;
}

void SamplerEngine::renderSubBlock(float* outLeft, float* outRight, int startSample, int numSamples) noexcept {
    if (numSamples <= 0) return;
    const float combinedChannelGain = channelVolume_ * expressionGain_;

    bool anySolo = false;
    for (std::size_t s = 0; s < kMaxRackSlots; ++s) {
        if (rackSlots_[s].enabled.load(std::memory_order_relaxed) &&
            rackSlots_[s].solo.load(std::memory_order_relaxed)) {
            anySolo = true;
            break;
        }
    }

    const int safeSamples = std::min(numSamples, static_cast<int>(slotScratchL_.size()));

    for (std::size_t s = 0; s < kMaxRackSlots; ++s) {
        if (!isSlotAudible(s, anySolo)) {
            rackSlots_[s].peakLeft.store(0.0f, std::memory_order_relaxed);
            rackSlots_[s].peakRight.store(0.0f, std::memory_order_relaxed);
            continue;
        }

        std::memset(slotScratchL_.data(), 0, static_cast<std::size_t>(safeSamples) * sizeof(float));
        std::memset(slotScratchR_.data(), 0, static_cast<std::size_t>(safeSamples) * sizeof(float));

        for (auto& voice : voices_) {
            if (voice.isActive() && voice.getRackSlotIndex() == s) {
                voice.renderNextBlock(slotScratchL_.data(),
                                      slotScratchR_.data(),
                                      safeSamples,
                                      params_,
                                      rackSlots_[s],
                                      modMatrix_,
                                      pitchBendNorm_,
                                      modWheelNorm_,
                                      combinedChannelGain);
            }
        }

        float sPeakL = 0.0f;
        float sPeakR = 0.0f;
        for (int i = 0; i < safeSamples; ++i) {
            outLeft[startSample + i]  += slotScratchL_[i];
            outRight[startSample + i] += slotScratchR_[i];
            sPeakL = std::max(sPeakL, std::abs(slotScratchL_[i]));
            sPeakR = std::max(sPeakR, std::abs(slotScratchR_[i]));
        }
        rackSlots_[s].peakLeft.store(sPeakL, std::memory_order_relaxed);
        rackSlots_[s].peakRight.store(sPeakR, std::memory_order_relaxed);
    }
}

void SamplerEngine::handleMidiEvent(const MidiEvent& ev) noexcept {
    const uint8_t statusType = ev.status & 0xF0;
    const uint8_t channel    = ev.status & 0x0F;

    switch (statusType) {
        case 0x90:
            if (ev.data2 > 0) {
                handleNoteOn(channel, ev.data1 & 0x7F, ev.data2 & 0x7F);
            } else {
                handleNoteOff(channel, ev.data1 & 0x7F);
            }
            break;

        case 0x80:
            handleNoteOff(channel, ev.data1 & 0x7F);
            break;

        case 0xB0: {
            const uint8_t cc  = ev.data1 & 0x7F;
            const uint8_t val = ev.data2 & 0x7F;
            if (cc == 1) {
                modWheelNorm_ = static_cast<float>(val) / 127.0f;
            } else if (cc == 7) {
                channelVolume_ = static_cast<float>(val) / 127.0f;
            } else if (cc == 11) {
                expressionGain_ = static_cast<float>(val) / 127.0f;
            } else if (cc == 64) {
                const bool pedalDown = (val >= 64);
                if (sustainPedalDown_ && !pedalDown) {
                    for (auto& voice : voices_) {
                        voice.releaseSustainPedal();
                    }
                }
                sustainPedalDown_ = pedalDown;
            } else if (cc == 120) {
                // CC120: All Sound Off (immediate emergency silence)
                sustainPedalDown_ = false;
                for (auto& voice : voices_) {
                    voice.reset();
                }
            } else if (cc == 123) {
                // CC123: All Notes Off (respects sustain pedal & release stage)
                for (auto& voice : voices_) {
                    voice.noteOff(sustainPedalDown_);
                }
            }
            break;
        }

        case 0xE0: {
            const int bend14 = static_cast<int>(ev.data1 & 0x7F) | (static_cast<int>(ev.data2 & 0x7F) << 7);
            pitchBendNorm_ = std::clamp(static_cast<float>(bend14 - 8192) / 8192.0f, -1.0f, 1.0f);
            break;
        }

        default:
            break;
    }
}

void SamplerEngine::handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
    bool anySolo = false;
    for (std::size_t s = 0; s < kMaxRackSlots; ++s) {
        if (rackSlots_[s].enabled.load(std::memory_order_relaxed) &&
            rackSlots_[s].solo.load(std::memory_order_relaxed)) {
            anySolo = true;
            break;
        }
    }

    const uint32_t rrStep = roundRobinCounter_++;
    const uint8_t incomingMidiCh1Based = static_cast<uint8_t>((channel & 0x0F) + 1);

    for (std::size_t s = 0; s < kMaxRackSlots; ++s) {
        if (!isSlotAudible(s, anySolo)) continue;
        const uint8_t slotCh = rackSlots_[s].midiChannel.load(std::memory_order_relaxed);
        if (slotCh != 0 && slotCh != incomingMidiCh1Based) continue;

        rackSlots_[s].midiActivityCounter.fetch_add(1, std::memory_order_relaxed);
        const auto& patch = activeSlotPatches_[s];
        if (!patch) continue;

        for (const auto& zone : patch->zones) {
            if (zone.matchesNoteAndVelocity(note, velocity, rrStep)) {
                if (SamplerVoice* voice = allocateVoice(s, channel, note)) {
                    voice->startVoice(s, zone, channel, note, velocity, ++noteOrderCounter_, params_);
                }
            }
        }
    }
}

void SamplerEngine::handleNoteOff(uint8_t channel, uint8_t note) noexcept {
    for (auto& voice : voices_) {
        if (voice.isActive() &&
            voice.getMidiNote() == note &&
            voice.getMidiChannel() == (channel & 0x0F)) {
            voice.noteOff(sustainPedalDown_);
        }
    }
}

SamplerVoice* SamplerEngine::allocateVoice(std::size_t slotIndex,
                                           uint8_t midiChannel,
                                           uint8_t midiNote) noexcept {
    const std::size_t allowedVoices = static_cast<std::size_t>(
        std::clamp(params_.maxPolyphony.load(std::memory_order_relaxed), 1, static_cast<int>(kMaxVoices)));

    // Prevent phase-locked voice accumulation when retriggering the same note on the same slot
    for (std::size_t i = 0; i < allowedVoices; ++i) {
        if (voices_[i].isActive() &&
            voices_[i].getRackSlotIndex() == slotIndex &&
            voices_[i].getMidiChannel() == midiChannel &&
            voices_[i].getMidiNote() == midiNote) {
            voices_[i].triggerSteal();
        }
    }

    // 1. Prefer an idle voice within the configured polyphony limit
    std::size_t activeCount = 0;
    SamplerVoice* freeVoice = nullptr;
    for (std::size_t i = 0; i < allowedVoices; ++i) {
        if (voices_[i].isActive()) {
            ++activeCount;
        } else if (!freeVoice) {
            freeVoice = &voices_[i];
        }
    }
    if (freeVoice && activeCount < allowedVoices) {
        return freeVoice;
    }

    // 2. Steal a voice playing the same MIDI note on the same slot
    for (std::size_t i = 0; i < allowedVoices; ++i) {
        if (voices_[i].isActive() &&
            voices_[i].getRackSlotIndex() == slotIndex &&
            voices_[i].getMidiNote() == midiNote) {
            voices_[i].triggerSteal();
            return &voices_[i];
        }
    }

    // 3. Steal the quietest voice already in Release/StealingFade stage
    SamplerVoice* bestReleasing = nullptr;
    float lowestEnv = 2.0f;
    for (std::size_t i = 0; i < allowedVoices; ++i) {
        if (voices_[i].isActive() && voices_[i].isReleasing()) {
            if (voices_[i].getCurrentEnvelopeLevel() < lowestEnv) {
                lowestEnv = voices_[i].getCurrentEnvelopeLevel();
                bestReleasing = &voices_[i];
            }
        }
    }
    if (bestReleasing) {
        return bestReleasing;
    }

    // 4. Steal the oldest active voice
    SamplerVoice* oldestVoice = &voices_[0];
    uint64_t oldestOrder = voices_[0].getOrderId();
    for (std::size_t i = 1; i < allowedVoices; ++i) {
        if (voices_[i].getOrderId() < oldestOrder) {
            oldestOrder = voices_[i].getOrderId();
            oldestVoice = &voices_[i];
        }
    }
    oldestVoice->triggerSteal();
    return oldestVoice;
}

std::shared_ptr<SampleBuffer> SamplerEngine::createDefaultSynthPianoSample(double sampleRate) {
    const uint64_t numFrames = static_cast<uint64_t>(sampleRate * 2.5);
    auto buffer = std::make_shared<SampleBuffer>(2, numFrames, sampleRate, 60);
    buffer->setSourcePath("Factory://ElectricGrand");
    buffer->setEmbeddedLoop(true,
                            static_cast<uint64_t>(sampleRate * 0.5),
                            static_cast<uint64_t>(sampleRate * 2.2));

    float* left  = buffer->getWritePointer(0);
    float* right = buffer->getWritePointer(1);

    constexpr double kTwoPi = 6.283185307179586;
    const double f0 = 261.6255653;

    for (uint64_t i = 0; i < numFrames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double envFundamental = std::exp(-t * 1.1);
        const double envHarmonics   = std::exp(-t * 3.2);
        const double attackRamp     = std::min(1.0, t / 0.003);

        const double h1 = 0.58 * std::sin(kTwoPi * f0 * t);
        const double h2 = 0.24 * std::sin(kTwoPi * (f0 * 2.0) * t + 0.3);
        const double h3 = 0.11 * std::sin(kTwoPi * (f0 * 3.0) * t + 0.7);
        const double h4 = 0.05 * std::sin(kTwoPi * (f0 * 4.0) * t + 1.2);
        const double chorusDetune = 0.08 * std::sin(kTwoPi * (f0 * 1.0015) * t);

        const double sigL = attackRamp * (envFundamental * (h1 + chorusDetune) + envHarmonics * (h2 + h3 + h4));
        const double sigR = attackRamp * (envFundamental * (h1 - chorusDetune) + envHarmonics * (h2 - 0.5 * h3 + h4));

        left[i]  = static_cast<float>(std::clamp(sigL, -0.98, 0.98));
        right[i] = static_cast<float>(std::clamp(sigR, -0.98, 0.98));
    }
    return buffer;
}

std::shared_ptr<InstrumentPatch> SamplerEngine::createFactoryPatchById(const std::string& factoryUri,
                                                                       double sampleRate) {
    if (factoryUri == "Factory://WarmPad") {
        const uint64_t numFrames = static_cast<uint64_t>(sampleRate * 3.0);
        auto buf = std::make_shared<SampleBuffer>(2, numFrames, sampleRate, 60);
        buf->setSourcePath(factoryUri);
        buf->setEmbeddedLoop(true, static_cast<uint64_t>(sampleRate * 0.4), static_cast<uint64_t>(sampleRate * 2.8));
        constexpr double kTwoPi = 6.283185307179586;
        const double f0 = 261.6255653;
        for (uint64_t i = 0; i < numFrames; ++i) {
            const double t = static_cast<double>(i) / sampleRate;
            const double sL = 0.45 * std::sin(kTwoPi * f0 * t) + 0.28 * std::sin(kTwoPi * (f0 * 1.003) * t) + 0.18 * std::sin(kTwoPi * (f0 * 1.998) * t);
            const double sR = 0.45 * std::sin(kTwoPi * f0 * t) + 0.28 * std::sin(kTwoPi * (f0 * 0.997) * t) + 0.18 * std::sin(kTwoPi * (f0 * 2.002) * t);
            buf->getWritePointer(0)[i] = static_cast<float>(sL);
            buf->getWritePointer(1)[i] = static_cast<float>(sR);
        }
        auto patch = InstrumentPatch::createFromSingleSample("Factory Warm Analog Pad", buf);
        patch->sourcePath = factoryUri;
        patch->zones[0].attackMsOverride = 220.0f;
        patch->zones[0].releaseMsOverride = 650.0f;
        patch->zones[0].cutoffHzOverride = 3800.0f;
        return patch;
    }
    if (factoryUri == "Factory://ConcertHarp") {
        auto buf = createDefaultSynthPianoSample(sampleRate);
        buf->setSourcePath(factoryUri);
        buf->setEmbeddedLoop(false, 0, buf->getNumFrames());
        auto patch = InstrumentPatch::createFromSingleSample("Factory Concert Harp Pluck", buf);
        patch->sourcePath = factoryUri;
        patch->zones[0].attackMsOverride = 1.5f;
        patch->zones[0].decayMsOverride = 420.0f;
        patch->zones[0].sustainLevelOverride = 0.15f;
        patch->zones[0].releaseMsOverride = 550.0f;
        return patch;
    }
    if (factoryUri == "Factory://ChamberStrings") {
        auto buf = createDefaultSynthPianoSample(sampleRate);
        buf->setSourcePath(factoryUri);
        auto patch = InstrumentPatch::createFromSingleSample("Factory Chamber Strings", buf);
        patch->sourcePath = factoryUri;
        patch->zones[0].attackMsOverride = 140.0f;
        patch->zones[0].sustainLevelOverride = 0.92f;
        patch->zones[0].releaseMsOverride = 480.0f;
        return patch;
    }
    auto buf = createDefaultSynthPianoSample(sampleRate);
    auto patch = InstrumentPatch::createFromSingleSample("Factory Electric Grand (C4)", buf);
    patch->sourcePath = "Factory://ElectricGrand";
    return patch;
}

} // namespace konos
