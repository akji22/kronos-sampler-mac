#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <memory>
#include "../Streaming/DiskSampleStreamer.h"

namespace konos {

/**
 * Decoded audio sample buffer supporting both full-RAM samples and hybrid
 * Preload Head + Lock-Free Disk Streaming for large orchestral samples.
 */
class SampleBuffer {
public:
    SampleBuffer() = default;

    SampleBuffer(uint32_t numChannels,
                 uint64_t numFrames,
                 double sampleRate,
                 uint8_t embeddedRootNote = 60,
                 uint64_t residentFrames = 0)
        : numChannels_(std::max<uint32_t>(1, numChannels)),
          numFrames_(numFrames),
          residentFrames_(residentFrames > 0 ? std::min(residentFrames, numFrames) : numFrames),
          sampleRate_(sampleRate > 0.0 ? sampleRate : 44100.0),
          rootMidiNote_(embeddedRootNote) {
        channels_.resize(numChannels_);
        for (auto& ch : channels_) {
            ch.resize(static_cast<std::size_t>(residentFrames_), 0.0f);
        }
        loopEndFrame_ = numFrames_;
    }

    uint32_t getNumChannels() const noexcept { return numChannels_; }
    uint64_t getNumFrames() const noexcept { return numFrames_; }
    uint64_t getResidentFrames() const noexcept { return residentFrames_; }
    bool     isDiskStreamed() const noexcept { return streamCache_ != nullptr; }
    double   getSampleRate() const noexcept { return sampleRate_; }
    uint8_t  getRootMidiNote() const noexcept { return rootMidiNote_; }
    void     setRootMidiNote(uint8_t note) noexcept { rootMidiNote_ = std::clamp<uint8_t>(note, 0, 127); }

    bool     hasEmbeddedLoop() const noexcept { return hasEmbeddedLoop_; }
    uint64_t getLoopStartFrame() const noexcept { return loopStartFrame_; }
    uint64_t getLoopEndFrame() const noexcept { return loopEndFrame_; }

    void setEmbeddedLoop(bool enabled, uint64_t startFrame, uint64_t endFrame) noexcept {
        hasEmbeddedLoop_ = enabled;
        loopStartFrame_  = std::min(startFrame, numFrames_ > 0 ? numFrames_ - 1 : 0);
        loopEndFrame_    = std::clamp(endFrame, loopStartFrame_ + 1, numFrames_);
    }

    const std::string& getSourcePath() const noexcept { return sourcePath_; }
    void setSourcePath(std::string path) { sourcePath_ = std::move(path); }

    void attachStreamCache(std::shared_ptr<StreamCacheBuffer> cache,
                           std::atomic<uint64_t>* underrunCounter) noexcept {
        streamCache_     = std::move(cache);
        underrunCounter_ = underrunCounter;
    }

    float* getWritePointer(uint32_t channel) noexcept {
        return channels_[std::min(channel, numChannels_ - 1)].data();
    }

    const float* getReadPointer(uint32_t channel) const noexcept {
        return channels_[std::min(channel, numChannels_ - 1)].data();
    }

    float getSampleClamped(uint32_t channel, int64_t frameIndex) const noexcept {
        if (numFrames_ == 0 || residentFrames_ == 0) return 0.0f;
        const uint64_t clamped = static_cast<uint64_t>(
            std::clamp<int64_t>(frameIndex, 0, static_cast<int64_t>(numFrames_) - 1));
        const uint32_t ch = std::min(channel, numChannels_ - 1);

        // 1. Always serve from pinned RAM preload buffer if within residentFrames_
        if (clamped < residentFrames_) {
            return channels_[ch][static_cast<std::size_t>(clamped)];
        }

        // 2. Otherwise serve lock-free from the double-buffered disk streaming window
        if (streamCache_ && underrunCounter_) {
            float streamedVal = 0.0f;
            if (streamCache_->tryReadSampleLockFree(ch, clamped, streamedVal, *underrunCounter_)) {
                return streamedVal;
            }
        }
        return 0.0f;
    }

    float interpolateCubicHermite(uint32_t channel, double phase) const noexcept {
        if (numFrames_ == 0) return 0.0f;

        const int64_t i1   = static_cast<int64_t>(std::floor(phase));
        const float   frac = static_cast<float>(phase - static_cast<double>(i1));

        const float y0 = getSampleClamped(channel, i1 - 1);
        const float y1 = getSampleClamped(channel, i1);
        const float y2 = getSampleClamped(channel, i1 + 1);
        const float y3 = getSampleClamped(channel, i1 + 2);

        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    std::size_t getMemoryBytes() const noexcept {
        std::size_t ramBytes = static_cast<std::size_t>(numChannels_) *
                               static_cast<std::size_t>(residentFrames_) * sizeof(float);
        if (streamCache_) {
            ramBytes += 4 * StreamCacheBuffer::kWindowFrames * sizeof(float);
        }
        return ramBytes;
    }

private:
    uint32_t numChannels_{1};
    uint64_t numFrames_{0};
    uint64_t residentFrames_{0};
    double   sampleRate_{44100.0};
    uint8_t  rootMidiNote_{60};
    bool     hasEmbeddedLoop_{false};
    uint64_t loopStartFrame_{0};
    uint64_t loopEndFrame_{0};
    std::string sourcePath_{};
    std::vector<std::vector<float>> channels_{};
    std::shared_ptr<StreamCacheBuffer> streamCache_{};
    std::atomic<uint64_t>* underrunCounter_{nullptr};
};

} // namespace konos
