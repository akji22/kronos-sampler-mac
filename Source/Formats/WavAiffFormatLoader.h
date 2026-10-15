#pragma once

#include "IFormatAdapter.h"
#include "../SamplePlayback/SampleBuffer.h"
#include <cstdint>
#include <string>

namespace konos {

/**
 * Production RIFF WAV and IFF AIFF audio sample decoder and WAV file writer.
 * Supports:
 * - WAV 16-bit signed PCM, 24-bit packed PCM, 32-bit signed PCM, 32-bit IEEE Float,
 *   and WAVE_FORMAT_EXTENSIBLE headers
 * - WAV 'smpl' chunk parsing (extracts embedded MIDI Unity Root Note and Loop Points)
 * - AIFF 16-bit and 24-bit big-endian signed PCM + 80-bit IEEE 754 extended sample rate
 *   + 'INST' chunk baseNote extraction
 * - Strict bounds checking against truncated or corrupted chunk headers
 */
class WavAiffFormatLoader final : public IFormatAdapter {
public:
    std::string getFormatName() const override { return "WAV/AIFF Audio Sample Loader"; }
    bool canHandleExtension(const std::string& extLower) const override;
    FormatLoadResult loadFromFile(const std::string& filePath) const override;

    static FormatLoadResult decodeSampleFile(const std::string& filePath,
                                             std::shared_ptr<SampleBuffer>& outBuffer);

    static FormatLoadResult decodeStreamedWavFile(const std::string& filePath,
                                                  uint64_t preloadFrames,
                                                  DiskSampleStreamer& streamer,
                                                  std::shared_ptr<SampleBuffer>& outBuffer);

    static bool writeWavFile(const std::string& filePath,
                             const SampleBuffer& buffer,
                             uint16_t bitsPerSample = 24,
                             bool writeSmplChunk = true);
};

} // namespace konos
