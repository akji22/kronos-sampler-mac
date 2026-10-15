#include "WavAiffFormatLoader.h"
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>

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

uint16_t readBE16(const uint8_t* p) noexcept {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

uint32_t readBE32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

void writeLE16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void writeLE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

double parseIeee80Extended(const uint8_t* bytes) noexcept {
    const int exponent = ((bytes[0] & 0x7F) << 8) | bytes[1];
    const uint32_t hiMant = readBE32(bytes + 2);
    const uint32_t loMant = readBE32(bytes + 6);
    if (exponent == 0 && hiMant == 0 && loMant == 0) return 0.0;

    const double f = static_cast<double>(hiMant) * std::ldexp(1.0, exponent - 16383 - 31)
                   + static_cast<double>(loMant) * std::ldexp(1.0, exponent - 16383 - 63);
    return (bytes[0] & 0x80) ? -f : f;
}

FormatLoadResult decodeRiffWav(const std::vector<uint8_t>& bytes,
                               const std::string& filePath,
                               std::shared_ptr<SampleBuffer>& outBuffer) {
    FormatLoadResult res;
    res.formatIdentifier = "RIFF WAV";

    if (bytes.size() < 44) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "WAV file is smaller than the minimum 44-byte RIFF header.";
        return res;
    }

    const uint32_t riffSize = readLE32(bytes.data() + 4);
    if (riffSize < 36) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "Invalid RIFF chunk size.";
        return res;
    }

    bool     foundFmt      = false;
    uint16_t audioFormat   = 0;
    uint16_t numChannels   = 0;
    uint32_t sampleRate    = 0;
    uint16_t blockAlign    = 0;
    uint16_t bitsPerSample = 0;

    const uint8_t* dataPtr  = nullptr;
    uint32_t       dataSize = 0;

    uint8_t  unityRootNote  = 60;
    bool     hasLoop        = false;
    uint64_t loopStartFrame = 0;
    uint64_t loopEndFrame   = 0;

    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const char*    chunkId   = reinterpret_cast<const char*>(bytes.data() + offset);
        const uint32_t chunkSize = readLE32(bytes.data() + offset + 4);
        const std::size_t payloadOffset = offset + 8;

        if (payloadOffset + chunkSize > bytes.size()) {
            res.status = FormatSupportStatus::CorruptedOrInvalid;
            res.diagnosticMessage = "Truncated or corrupted WAV chunk ('" +
                                    std::string(chunkId, 4) + "' exceeds file size).";
            return res;
        }

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                res.status = FormatSupportStatus::CorruptedOrInvalid;
                res.diagnosticMessage = "WAV 'fmt ' chunk is smaller than 16 bytes.";
                return res;
            }
            const uint8_t* fmt = bytes.data() + payloadOffset;
            audioFormat   = readLE16(fmt + 0);
            numChannels   = readLE16(fmt + 2);
            sampleRate    = readLE32(fmt + 4);
            blockAlign    = readLE16(fmt + 12);
            bitsPerSample = readLE16(fmt + 14);

            // Handle WAVE_FORMAT_EXTENSIBLE (0xFFFE)
            if (audioFormat == 0xFFFE && chunkSize >= 40) {
                audioFormat = readLE16(fmt + 24);
            }
            foundFmt = true;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataPtr  = bytes.data() + payloadOffset;
            dataSize = chunkSize;
        } else if (std::memcmp(chunkId, "smpl", 4) == 0 && chunkSize >= 36) {
            const uint8_t* smpl = bytes.data() + payloadOffset;
            const uint32_t midiUnityNote = readLE32(smpl + 12);
            if (midiUnityNote <= 127) {
                unityRootNote = static_cast<uint8_t>(midiUnityNote);
            }
            const uint32_t numSampleLoops = readLE32(smpl + 28);
            if (numSampleLoops >= 1 && chunkSize >= 36 + 24) {
                const uint32_t lStart = readLE32(smpl + 36 + 8);
                const uint32_t lEnd   = readLE32(smpl + 36 + 12);
                if (lEnd > lStart) {
                    hasLoop        = true;
                    loopStartFrame = lStart;
                    loopEndFrame   = lEnd;
                }
            }
        }

        // RIFF chunks are word-aligned (padded to even byte boundary)
        offset = payloadOffset + chunkSize + (chunkSize & 1U);
    }

    if (!foundFmt || !dataPtr || dataSize == 0) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "WAV file is missing required 'fmt ' or 'data' chunks.";
        return res;
    }

    if (numChannels == 0 || numChannels > 8 || sampleRate < 4000 || sampleRate > 384000) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "Unsupported WAV channel count or sample rate.";
        return res;
    }

    const uint32_t bytesPerSample = bitsPerSample / 8;
    const uint32_t frameStride    = blockAlign >= numChannels * bytesPerSample
                                        ? blockAlign
                                        : numChannels * bytesPerSample;
    if (frameStride == 0) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "Invalid WAV block alignment.";
        return res;
    }

    const uint64_t numFrames = dataSize / frameStride;
    if (numFrames == 0) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "WAV 'data' chunk contains zero sample frames.";
        return res;
    }

    if (!((audioFormat == 1 && (bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32)) ||
          (audioFormat == 3 && bitsPerSample == 32))) {
        res.status = FormatSupportStatus::UnsupportedProprietary;
        res.diagnosticMessage = "Unsupported WAV encoding format (formatTag=" +
                                std::to_string(audioFormat) + ", bits=" + std::to_string(bitsPerSample) + ").";
        return res;
    }

    outBuffer = std::make_shared<SampleBuffer>(numChannels, numFrames, static_cast<double>(sampleRate), unityRootNote);
    outBuffer->setSourcePath(filePath);
    if (hasLoop) {
        outBuffer->setEmbeddedLoop(true, loopStartFrame, std::min<uint64_t>(loopEndFrame, numFrames));
    }

    for (uint64_t f = 0; f < numFrames; ++f) {
        const uint8_t* frameBase = dataPtr + f * frameStride;
        for (uint16_t ch = 0; ch < numChannels; ++ch) {
            const uint8_t* s = frameBase + ch * bytesPerSample;
            float sampleVal = 0.0f;

            if (audioFormat == 1 && bitsPerSample == 8) {
                sampleVal = (static_cast<int>(s[0]) - 128) / 128.0f;
            } else if (audioFormat == 1 && bitsPerSample == 16) {
                const int16_t v = static_cast<int16_t>(readLE16(s));
                sampleVal = static_cast<float>(v) / 32768.0f;
            } else if (audioFormat == 1 && bitsPerSample == 24) {
                int32_t v = static_cast<int32_t>(s[0]) |
                            (static_cast<int32_t>(s[1]) << 8) |
                            (static_cast<int32_t>(s[2]) << 16);
                if (v & 0x800000) v |= ~0xFFFFFF; // Sign-extend 24-bit to 32-bit
                sampleVal = static_cast<float>(v) / 8388608.0f;
            } else if (audioFormat == 1 && bitsPerSample == 32) {
                const int32_t v = static_cast<int32_t>(readLE32(s));
                sampleVal = static_cast<float>(static_cast<double>(v) / 2147483648.0);
            } else if (audioFormat == 3 && bitsPerSample == 32) {
                float v = 0.0f;
                std::memcpy(&v, s, sizeof(float));
                sampleVal = std::isfinite(v) ? std::clamp(v, -4.0f, 4.0f) : 0.0f;
            }
            outBuffer->getWritePointer(ch)[f] = sampleVal;
        }
    }

    res.succeeded = true;
    res.status = FormatSupportStatus::FullySupported;
    res.diagnosticMessage = "Loaded WAV (" + std::to_string(numChannels) + " ch, " +
                            std::to_string(sampleRate) + " Hz, " +
                            std::to_string(bitsPerSample) + "-bit, " +
                            std::to_string(numFrames) + " frames).";
    return res;
}

FormatLoadResult decodeIffAiff(const std::vector<uint8_t>& bytes,
                               const std::string& filePath,
                               std::shared_ptr<SampleBuffer>& outBuffer) {
    FormatLoadResult res;
    res.formatIdentifier = "IFF AIFF";

    if (bytes.size() < 46) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "AIFF file is smaller than the minimum header size.";
        return res;
    }

    bool     foundComm     = false;
    uint16_t numChannels   = 0;
    uint32_t numFrames     = 0;
    uint16_t bitsPerSample = 0;
    double   sampleRate    = 44100.0;
    uint8_t  rootNote      = 60;

    const uint8_t* ssndDataPtr = nullptr;
    uint32_t       ssndDataBytes = 0;

    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const char*    chunkId   = reinterpret_cast<const char*>(bytes.data() + offset);
        const uint32_t chunkSize = readBE32(bytes.data() + offset + 4);
        const std::size_t payloadOffset = offset + 8;

        if (payloadOffset + chunkSize > bytes.size()) {
            res.status = FormatSupportStatus::CorruptedOrInvalid;
            res.diagnosticMessage = "Truncated AIFF chunk.";
            return res;
        }

        if (std::memcmp(chunkId, "COMM", 4) == 0 && chunkSize >= 18) {
            const uint8_t* comm = bytes.data() + payloadOffset;
            numChannels   = readBE16(comm + 0);
            numFrames     = readBE32(comm + 2);
            bitsPerSample = readBE16(comm + 6);
            sampleRate    = parseIeee80Extended(comm + 8);
            foundComm     = true;
        } else if (std::memcmp(chunkId, "SSND", 4) == 0 && chunkSize >= 8) {
            const uint8_t* ssnd = bytes.data() + payloadOffset;
            const uint32_t dataOffset = readBE32(ssnd + 0);
            if (8 + dataOffset <= chunkSize) {
                ssndDataPtr   = ssnd + 8 + dataOffset;
                ssndDataBytes = chunkSize - 8 - dataOffset;
            }
        } else if (std::memcmp(chunkId, "INST", 4) == 0 && chunkSize >= 20) {
            const uint8_t baseNote = bytes[payloadOffset];
            if (baseNote <= 127) rootNote = baseNote;
        }

        offset = payloadOffset + chunkSize + (chunkSize & 1U);
    }

    if (!foundComm || !ssndDataPtr || numChannels == 0 || numFrames == 0) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "AIFF file is missing required 'COMM' or 'SSND' chunks.";
        return res;
    }

    const uint32_t bytesPerSample = (bitsPerSample + 7) / 8;
    const uint32_t frameStride    = numChannels * bytesPerSample;
    if (ssndDataBytes < numFrames * frameStride || (bitsPerSample != 16 && bitsPerSample != 24)) {
        res.status = FormatSupportStatus::UnsupportedProprietary;
        res.diagnosticMessage = "Unsupported or truncated AIFF sample bit depth.";
        return res;
    }

    outBuffer = std::make_shared<SampleBuffer>(numChannels, numFrames, sampleRate, rootNote);
    outBuffer->setSourcePath(filePath);

    for (uint32_t f = 0; f < numFrames; ++f) {
        const uint8_t* frameBase = ssndDataPtr + f * frameStride;
        for (uint16_t ch = 0; ch < numChannels; ++ch) {
            const uint8_t* s = frameBase + ch * bytesPerSample;
            float val = 0.0f;
            if (bitsPerSample == 16) {
                const int16_t v = static_cast<int16_t>(readBE16(s));
                val = static_cast<float>(v) / 32768.0f;
            } else if (bitsPerSample == 24) {
                int32_t v = (static_cast<int32_t>(s[0]) << 16) |
                            (static_cast<int32_t>(s[1]) << 8) |
                            static_cast<int32_t>(s[2]);
                if (v & 0x800000) v |= ~0xFFFFFF;
                val = static_cast<float>(v) / 8388608.0f;
            }
            outBuffer->getWritePointer(ch)[f] = val;
        }
    }

    res.succeeded = true;
    res.status = FormatSupportStatus::FullySupported;
    res.diagnosticMessage = "Loaded AIFF (" + std::to_string(numChannels) + " ch, " +
                            std::to_string(static_cast<int>(sampleRate)) + " Hz, " +
                            std::to_string(bitsPerSample) + "-bit).";
    return res;
}

} // namespace

bool WavAiffFormatLoader::canHandleExtension(const std::string& extLower) const {
    return extLower == ".wav" || extLower == ".wave" || extLower == ".aif" || extLower == ".aiff";
}

FormatLoadResult WavAiffFormatLoader::decodeSampleFile(const std::string& filePath,
                                                       std::shared_ptr<SampleBuffer>& outBuffer) {
    FormatLoadResult res;
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        res.status = FormatSupportStatus::FileNotFound;
        res.diagnosticMessage = "Unable to open sample file: " + filePath;
        return res;
    }

    const std::streamsize size = file.tellg();
    if (size < 12 || size > static_cast<std::streamsize>(512ULL * 1024ULL * 1024ULL)) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "Sample file size is invalid or exceeds single-sample safety threshold.";
        return res;
    }

    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "I/O error while reading sample file.";
        return res;
    }

    if (std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WAVE", 4) == 0) {
        return decodeRiffWav(bytes, filePath, outBuffer);
    }
    if (std::memcmp(bytes.data(), "FORM", 4) == 0 &&
        (std::memcmp(bytes.data() + 8, "AIFF", 4) == 0 || std::memcmp(bytes.data() + 8, "AIFC", 4) == 0)) {
        return decodeIffAiff(bytes, filePath, outBuffer);
    }

    res.status = FormatSupportStatus::CorruptedOrInvalid;
    res.diagnosticMessage = "File header is neither RIFF WAVE nor FORM AIFF.";
    return res;
}

FormatLoadResult WavAiffFormatLoader::loadFromFile(const std::string& filePath) const {
    std::shared_ptr<SampleBuffer> sampleBuf;
    FormatLoadResult res = decodeSampleFile(filePath, sampleBuf);
    if (res.succeeded && sampleBuf) {
        const std::string stem = std::filesystem::path(filePath).stem().string();
        res.patch = InstrumentPatch::createFromSingleSample(stem, sampleBuf);
    }
    return res;
}

bool WavAiffFormatLoader::writeWavFile(const std::string& filePath,
                                       const SampleBuffer& buffer,
                                       uint16_t bitsPerSample,
                                       bool writeSmplChunk) {
    if (buffer.getNumFrames() == 0 || (bitsPerSample != 16 && bitsPerSample != 24)) return false;

    const uint16_t numChannels   = static_cast<uint16_t>(buffer.getNumChannels());
    const uint32_t sampleRate    = static_cast<uint32_t>(buffer.getSampleRate());
    const uint16_t bytesPerSamp  = bitsPerSample / 8;
    const uint16_t blockAlign    = numChannels * bytesPerSamp;
    const uint32_t byteRate      = sampleRate * blockAlign;
    const uint32_t dataChunkSize = static_cast<uint32_t>(buffer.getNumFrames()) * blockAlign;
    const uint32_t smplChunkSize = writeSmplChunk ? (36 + (buffer.hasEmbeddedLoop() ? 24 : 0)) : 0;
    const uint32_t riffChunkSize = 4 + (8 + 16) + (8 + dataChunkSize) + (writeSmplChunk ? (8 + smplChunkSize) : 0);

    std::vector<uint8_t> out;
    out.reserve(8 + riffChunkSize);

    // RIFF header
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    writeLE32(out, riffChunkSize);
    out.insert(out.end(), {'W', 'A', 'V', 'E'});

    // fmt chunk
    out.insert(out.end(), {'f', 'm', 't', ' '});
    writeLE32(out, 16);
    writeLE16(out, 1); // PCM
    writeLE16(out, numChannels);
    writeLE32(out, sampleRate);
    writeLE32(out, byteRate);
    writeLE16(out, blockAlign);
    writeLE16(out, bitsPerSample);

    // Optional smpl chunk
    if (writeSmplChunk) {
        out.insert(out.end(), {'s', 'm', 'p', 'l'});
        writeLE32(out, smplChunkSize);
        writeLE32(out, 0); // Manufacturer
        writeLE32(out, 0); // Product
        writeLE32(out, static_cast<uint32_t>(1000000000ULL / sampleRate)); // Sample period ns
        writeLE32(out, buffer.getRootMidiNote()); // MIDI unity note
        writeLE32(out, 0); // MIDI pitch fraction
        writeLE32(out, 0); // SMPTE format
        writeLE32(out, 0); // SMPTE offset
        writeLE32(out, buffer.hasEmbeddedLoop() ? 1 : 0); // Num sample loops
        writeLE32(out, 0); // Sampler data
        if (buffer.hasEmbeddedLoop()) {
            writeLE32(out, 0); // Cue point ID
            writeLE32(out, 0); // Type (0 = forward loop)
            writeLE32(out, static_cast<uint32_t>(buffer.getLoopStartFrame()));
            writeLE32(out, static_cast<uint32_t>(buffer.getLoopEndFrame()));
            writeLE32(out, 0); // Fraction
            writeLE32(out, 0); // Play count (0 = infinite)
        }
    }

    // data chunk
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    writeLE32(out, dataChunkSize);

    for (uint64_t f = 0; f < buffer.getNumFrames(); ++f) {
        for (uint16_t ch = 0; ch < numChannels; ++ch) {
            const float clamped = std::clamp(buffer.getReadPointer(ch)[f], -1.0f, 1.0f);
            if (bitsPerSample == 16) {
                const int16_t pcm16 = static_cast<int16_t>(std::lrint(clamped * 32767.0f));
                writeLE16(out, static_cast<uint16_t>(pcm16));
            } else {
                const int32_t pcm24 = static_cast<int32_t>(std::lrint(clamped * 8388607.0f));
                out.push_back(static_cast<uint8_t>(pcm24 & 0xFF));
                out.push_back(static_cast<uint8_t>((pcm24 >> 8) & 0xFF));
                out.push_back(static_cast<uint8_t>((pcm24 >> 16) & 0xFF));
            }
        }
    }

    std::ofstream f(filePath, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return f.good();
}

FormatLoadResult WavAiffFormatLoader::decodeStreamedWavFile(
    const std::string& filePath,
    uint64_t preloadFrames,
    DiskSampleStreamer& streamer,
    std::shared_ptr<SampleBuffer>& outBuffer) {
    // First decode header and preload frames
    std::shared_ptr<SampleBuffer> fullOrPreload;
    FormatLoadResult res = decodeSampleFile(filePath, fullOrPreload);
    if (!res.succeeded || !fullOrPreload) return res;

    const uint64_t totalFrames = fullOrPreload->getNumFrames();
    const uint64_t clampedPreload = std::min(preloadFrames, totalFrames);

    auto streamedBuf = std::make_shared<SampleBuffer>(
        fullOrPreload->getNumChannels(),
        totalFrames,
        fullOrPreload->getSampleRate(),
        fullOrPreload->getRootMidiNote(),
        clampedPreload);
    streamedBuf->setSourcePath(filePath);
    if (fullOrPreload->hasEmbeddedLoop()) {
        streamedBuf->setEmbeddedLoop(true,
                                     fullOrPreload->getLoopStartFrame(),
                                     fullOrPreload->getLoopEndFrame());
    }

    // Copy only the preload head frames into pinned RAM
    for (uint32_t ch = 0; ch < fullOrPreload->getNumChannels(); ++ch) {
        std::memcpy(streamedBuf->getWritePointer(ch),
                    fullOrPreload->getReadPointer(ch),
                    static_cast<std::size_t>(clampedPreload) * sizeof(float));
    }

    if (totalFrames > clampedPreload) {
        auto streamCache = streamer.registerStreamedWavFile(filePath, clampedPreload);
        if (!streamCache) {
            res.succeeded = false;
            res.status = FormatSupportStatus::CorruptedOrInvalid;
            res.diagnosticMessage = "Failed to initialize disk streaming cache for: " + filePath;
            return res;
        }
        streamedBuf->attachStreamCache(std::move(streamCache), &streamer.getUnderrunCounterRef());
    }

    outBuffer = std::move(streamedBuf);
    return res;
}

} // namespace konos
