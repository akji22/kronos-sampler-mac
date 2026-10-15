#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace konos {

/**
 * Lock-free double-buffered streaming window for a single streamed sample source.
 * The real-time audio thread reads from the active window or preload head buffer
 * without locking and posts ahead-of-time refill requests via atomic targetStartFrame.
 */
struct StreamCacheBuffer {
    static constexpr uint64_t kWindowFrames = 8192;

    std::string filePath{};
    uint64_t    dataChunkOffset{44};
    uint64_t    totalFrames{0};
    uint16_t    numChannels{2};
    uint16_t    bitsPerSample{16};
    uint16_t    blockAlign{4};

    // Double-buffered planar float windows [bufferIndex 0..1][channel 0..1][frame 0..kWindowFrames-1]
    std::array<std::array<std::vector<float>, 2>, 2> windows{};
    std::array<std::atomic<uint64_t>, 2> windowStartFrame{0, 0};
    std::array<std::atomic<uint64_t>, 2> windowValidFrames{0, 0};
    std::atomic<int>      activeWindowIndex{0};
    std::atomic<uint64_t> requestedStartFrame{0};
    std::atomic<bool>     refillPending{false};
    std::atomic<bool>     fileError{false};

    StreamCacheBuffer() {
        for (int b = 0; b < 2; ++b) {
            for (int ch = 0; ch < 2; ++ch) {
                windows[b][ch].assign(kWindowFrames, 0.0f);
            }
        }
    }

    /**
     * Lock-free audio-thread read: checks the active cache window, triggers an
     * asynchronous background refill when playback enters the second half of the
     * current window, and returns false only on a cache miss/underrun.
     */
    bool tryReadSampleLockFree(uint32_t channel,
                               uint64_t frameIndex,
                               float& outSample,
                               std::atomic<uint64_t>& underrunCounter) noexcept {
        if (fileError.load(std::memory_order_relaxed)) {
            outSample = 0.0f;
            return false;
        }

        const int activeIdx = activeWindowIndex.load(std::memory_order_acquire);
        const uint64_t winStart = windowStartFrame[activeIdx].load(std::memory_order_relaxed);
        const uint64_t winValid = windowValidFrames[activeIdx].load(std::memory_order_relaxed);
        const uint32_t ch = std::min<uint32_t>(channel, 1);

        if (frameIndex >= winStart && frameIndex < winStart + winValid) {
            outSample = windows[activeIdx][ch][static_cast<std::size_t>(frameIndex - winStart)];
            // Request lookahead refill when passing 50% of the current window
            if (frameIndex >= winStart + (kWindowFrames / 2) &&
                winStart + kWindowFrames < totalFrames &&
                !refillPending.load(std::memory_order_relaxed)) {
                requestedStartFrame.store(winStart + (kWindowFrames / 2), std::memory_order_relaxed);
                refillPending.store(true, std::memory_order_release);
            }
            return true;
        }

        // Cache miss: request immediate refill at frameIndex and report underrun
        if (!refillPending.load(std::memory_order_relaxed)) {
            requestedStartFrame.store(frameIndex, std::memory_order_relaxed);
            refillPending.store(true, std::memory_order_release);
        }
        underrunCounter.fetch_add(1, std::memory_order_relaxed);
        outSample = 0.0f;
        return false;
    }
};

/**
 * Background disk-streaming manager for large WAV sample files.
 * Pre-populates the initial streaming window and services lock-free lookahead
 * refill requests on a dedicated worker thread with clean shutdown cancellation.
 */
class DiskSampleStreamer {
public:
    DiskSampleStreamer();
    ~DiskSampleStreamer();

    DiskSampleStreamer(const DiskSampleStreamer&) = delete;
    DiskSampleStreamer& operator=(const DiskSampleStreamer&) = delete;

    void startWorker();
    void stopWorker() noexcept;

    /**
     * Registers a WAV file for disk streaming and synchronously primes the first
     * streaming cache window after preloadFrames.
     */
    std::shared_ptr<StreamCacheBuffer> registerStreamedWavFile(const std::string& filePath,
                                                               uint64_t preloadFrames);

    void notifyRefillNeeded() noexcept;

    uint64_t getUnderrunCount() const noexcept {
        return underrunCount_.load(std::memory_order_relaxed);
    }
    uint64_t getTotalDiskBytesStreamed() const noexcept {
        return totalDiskBytesStreamed_.load(std::memory_order_relaxed);
    }
    std::atomic<uint64_t>& getUnderrunCounterRef() noexcept { return underrunCount_; }

private:
    void workerLoop();
    bool fillInactiveWindowFromDisk(StreamCacheBuffer& stream, uint64_t targetStartFrame);

    mutable std::mutex streamsMutex_{};
    std::vector<std::shared_ptr<StreamCacheBuffer>> activeStreams_{};

    std::thread             workerThread_{};
    std::mutex              wakeMutex_{};
    std::condition_variable wakeCv_{};
    std::atomic<bool>       running_{false};
    std::atomic<bool>       shutdownRequested_{false};

    std::atomic<uint64_t>   underrunCount_{0};
    std::atomic<uint64_t>   totalDiskBytesStreamed_{0};
};

} // namespace konos
