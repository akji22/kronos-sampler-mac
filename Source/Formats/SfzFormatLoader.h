#pragma once

#include "IFormatAdapter.h"
#include "../SamplePlayback/SampleBuffer.h"
#include <unordered_map>

namespace konos {

/**
 * Production SFZ v1/v2 Instrument Parser and Loader Module.
 *
 * Supported Features:
 * - Headers: <control>, <global>, <master>, <group>, <region>
 * - Preprocessor & Control: #define $MACRO value, default_path=..., // and block comments
 * - Sample paths with spaces (e.g., sample=Audio Samples/Piano C4.wav) and sample cache deduplication
 * - Key mapping: lokey, hikey, key, pitch_keycenter (MIDI 0..127 and note names C-1..G9)
 * - Velocity layers: lovel, hivel, amp_veltrack
 * - Round-robin & groups: seq_length, seq_position, group, off_by
 * - Sample bounds & looping: offset, end, loop_mode/loopmode (no_loop, one_shot,
 *   loop_continuous, loop_sustain), loop_start/loopstart, loop_end/loopend
 * - Pitch & tuning: transpose, tune, pitch_keytrack
 * - Amplitude & pan: volume, amplitude, pan
 * - ADSR Envelopes: ampeg_attack/amp_attack, ampeg_decay/amp_decay,
 *   ampeg_sustain/amp_sustain, ampeg_release/amp_release, ampeg_hold/amp_hold
 * - Filters: cutoff, resonance, fil_type (lpf_2p, hpf_2p, bpf_2p)
 */
class SfzFormatLoader final : public IFormatAdapter {
public:
    std::string getFormatName() const override { return "SFZ v1/v2 Instrument Loader"; }
    bool canHandleExtension(const std::string& extLower) const override { return extLower == ".sfz"; }
    FormatLoadResult loadFromFile(const std::string& filePath) const override;

    static int parseMidiNoteToken(const std::string& token) noexcept;
};

} // namespace konos
