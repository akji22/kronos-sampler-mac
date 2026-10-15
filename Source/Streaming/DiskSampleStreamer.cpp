#include "DiskSampleStreamer.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace konos {

namespace {

uint16_t readLE16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLE32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool inspectWavDataChunk(const std::string& filePath,
                         uint64_t& outDataOffset,
                         uint64_t& outTotalFrames,
                         uint16_t& outChannels,
                         uint16_t& outBitsPerSample,
                         uint16_t& outBlockAlign) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    uint8_t riffHdr[12]{};
    if (!file.read(reinterpret_cast<char*>(riffHdr), 12)) return false;
    if (std::memcmp(riffHdr, "RIFF", 4) != 0 || std::memcmp(riffHdr + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool foundFmt = false;
    while (file.good()) {
        uint8_t chunkHdr[8]{};
        if (!file.read(reinterpret_cast<char*>(chunkHdr), 8)) break;
        const uint32_t chunkSize = readLE32(chunkHdr + 4);
        const std::streamoff payloadPos = file.tellg();

        if (std::memcmp(chunkHdr, "fmt ", 4) == 0 && chunkSize >= 16) {
            uint8_t fmt[16]{};
            if (!file.read(reinterpret_cast<char*>(fmt), 16)) return false;
            const uint16_t formatTag = readLE16(fmt + 0);
            outChannels      = readLE16(fmt + 2);
            outBlockAlign    = readLE16(fmt + 12);
            outBitsPerSample = readLE16(fmt + 14);
            if (formatTag != 1 && formatTag != 3 && formatTag != 0xFFFE) return false;
            foundFmt = true;
        } else if (std::memcmp(chunkHdr, "data", 4) == 0 && foundFmt) {
            if (outBlockAlign == 0 || outChannels == 0) return false;
            outDataOffset  = static_cast<uint64_t>(payloadPos);
            outTotalFrames = chunkSize / outBlockAlign;
            return outTotalFrames > 0;
        }

        const std::streamoff nextPos = payloadPos + static_cast<std::streamoff>(chunkSize + (chunkSize & 1U));
        file.seekg(nextPos, std::ios::beg);
    }
    return false;
}

} // namespace

DiskSampleStreamer::DiskSampleStreamer() {
    startWorker();
}

DiskSampleStreamer::~DiskSampleStreamer() {
    stopWorker();
}

void DiskSampleStreamer::startWorker() {
    if (running_.load(std::memory_order_acquire)) return;
    shutdownRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    workerThread_ = std::thread(&DiskSampleStreamer::workerLoop, this);
}

void DiskSampleStreamer::stopWorker() noexcept {
    shutdownRequested_.store(true, std::memory_order_release);
    wakeCv_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void DiskSampleStreamer::notifyRefillNeeded() noexcept {
    wakeCv_.notify_one();
}

std::shared_ptr<StreamCacheBuffer> DiskSampleStreamer::registerStreamedWavFile(
    const std::string& filePath,
    uint64_t preloadFrames) {
    uint64_t dataOffset  = 0;
    uint64_t totalFrames = 0;
    uint16_t channels    = 0;
    uint16_t bits        = 0;
    uint16_t blockAlign  = 0;

    if (!inspectWavDataChunk(filePath, dataOffset, totalFrames, channels, bits, blockAlign)) {
        return nullptr;
    }

    auto cache = std::make_shared<StreamCacheBuffer>();
    cache->filePath        = filePath;
    cache->dataChunkOffset = dataOffset;
    cache->totalFrames     = totalFrames;
    cache->numChannels     = channels;
    cache->bitsPerSample   = bits;
    cache->blockAlign      = blockAlign;

    const uint64_t initialStart = std::min(preloadFrames, totalFrames > 0 ? totalFrames - 1 : 0);
    if (!fillInactiveWindowFromDisk(*cache, initialStart)) {
        cache->fileError.store(true, std::memory_order_relaxed);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        activeStreams_.push_back(cache);
    }
    return cache;
}

bool DiskSampleStreamer::fillInactiveWindowFromDisk(StreamCacheBuffer& stream,
                                                    uint64_t targetStartFrame) {
    if (shutdownRequested_.load(std::memory_order_acquire)) return false;

    std::ifstream file(stream.filePath, std::ios::binary);
    if (!file.is_open()) {
        stream.fileError.store(true, std::memory_order_relaxed);
        return false;
    }

    const uint64_t clampedStart = std::min(targetStartFrame, stream.totalFrames);
    const uint64_t framesToRead = std::min<uint64_t>(
        StreamCacheBuffer::kWindowFrames,
        stream.totalFrames > clampedStart ? (stream.totalFrames - clampedStart) : 0);

    if (framesToRead == 0) return false;

    const uint64_t byteOffset = stream.dataChunkOffset + clampedStart * stream.blockAlign;
    const std::size_t bytesToRead = static_cast<std::size_t>(framesToRead * stream.blockAlign);

    std::vector<uint8_t> rawBytes(bytesToRead);
    file.seekg(static_cast<std::streamoff>(byteOffset), std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(rawBytes.data()), static_cast<std::streamsize>(bytesToRead))) {
        stream.fileError.store(true, std::memory_order_relaxed);
        return false;
    }

    totalDiskBytesStreamed_.fetch_add(bytesToRead, std::memory_order_relaxed);

    const int currentActive = stream.activeWindowIndex.load(std::memory_order_relaxed);
    const int inactiveIdx   = 1 - currentActive;
    const uint16_t bytesPerSample = stream.bitsPerSample / 8;

    for (uint64_t f = 0; f < framesToRead; ++f) {
        const uint8_t* frameBase = rawBytes.data() + f * stream.blockAlign;
        for (uint16_t ch = 0; ch < std::min<uint16_t>(stream.numChannels, 2); ++ch) {
            const uint8_t* s = frameBase + ch * bytesPerSample;
            float val = 0.0f;
            if (stream.bitsPerSample == 16) {
                const int16_t pcm = static_cast<int16_t>(readLE16(s));
                val = static_cast<float>(pcm) / 32768.0f;
            } else if (stream.bitsPerSample == 24) {
                int32_t pcm = static_cast<int32_t>(s[0]) |
                              (static_cast<int32_t>(s[1]) << 8) |
                              (static_cast<int32_t>(s[2]) << 16);
                if (pcm & 0x800000) pcm |= ~0xFFFFFF;
                val = static_cast<float>(pcm) / 8388608.0f;
            } else if (stream.bitsPerSample == 32) {
                std::memcpy(&val, s, sizeof(float));
                if (!std::isfinite(val)) val = 0.0f;
            }
            stream.windows[inactiveIdx][ch][static_cast<std::size_t>(f)] = val;
        }
        if (stream.numChannels == 1) {
            stream.windows[inactiveIdx][1][static_cast<std::size_t>(f)] =
                stream.windows[inactiveIdx][0][static_cast<std::size_t>(f)];
        }
    }

    stream.windowStartFrame[inactiveIdx].store(clampedStart, std::memory_order_relaxed);
    stream.windowValidFrames[inactiveIdx].store(framesToRead, std::memory_order_relaxed);
    stream.activeWindowIndex.store(inactiveIdx, std::memory_order_release);
    stream.refillPending.store(false, std::memory_order_release);
    return true;
}

void DiskSampleStreamer::workerLoop() {
    while (!shutdownRequested_.load(std::memory_order_acquire)) {
        bool didWork = false;
        {
            std::lock_guard<std::mutex> lock(streamsMutex_);
            for (auto& stream : activeStreams_) {
                if (shutdownRequested_.load(std::memory_order_acquire)) break;
                if (stream && stream->refillPending.load(std::memory_order_acquire)) {
                    const uint64_t target = stream->requestedStartFrame.load(std::memory_order_relaxed);
                    fillInactiveWindowFromDisk(*stream, target);
                    didWork = true;
                }
            }
        }

        if (!didWork && !shutdownRequested_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> wakeLock(wakeMutex_);
            wakeCv_.wait_for(wakeLock, std::chrono::milliseconds(2));
        }
    }
}

} // namespace konos
