#pragma once

#include "IFormatAdapter.h"
#include <cstdint>
#include <string>
#include <vector>

namespace konos {

struct Sf2PresetInfo {
    std::string name{};
    uint16_t    bankNumber{0};
    uint16_t    presetNumber{0};
};

/**
 * SoundFont 2.01 / 2.04 (.sf2) Binary RIFF 'sfbk' Loader Module.
 *
 * Implements the E-mu / Creative SoundFont 2.04 Technical Specification:
 * - Parses LIST 'INFO', LIST 'sdta' ('smpl' 16-bit PCM + optional 'sm24' 24-bit LSBs),
 *   and LIST 'pdta' ('phdr', 'pbag', 'pmod', 'pgen', 'inst', 'ibag', 'imod', 'igen', 'shdr')
 * - Supports preset & bank selection (loadPresetFromFile) and preset enumeration (listPresets)
 * - Resolves Global Preset Zone + Local Preset Zone additive generator offsets combined
 *   with Global Instrument Zone + Local Instrument Zone generators
 * - Supports keyRange (43), velRange (44), sampleModes (54: NoLoop, ForwardLoop, LoopSustain),
 *   start/end/loop address offsets (fine & coarse 32k offsets), overridingRootKey (58),
 *   coarseTune (51), fineTune (52), VolEnv ADSR timecents (34, 35, 36, 37, 38),
 *   initialFilterFc (8), initialFilterQ (9), pan (17), and initialAttenuation (48)
 */
class Sf2FormatLoader final : public IFormatAdapter {
public:
    std::string getFormatName() const override { return "SoundFont 2 (.sf2) Bank Loader"; }
    bool canHandleExtension(const std::string& extLower) const override { return extLower == ".sf2"; }

    /**
     * Loads the first playable preset from the SoundFont bank.
     */
    FormatLoadResult loadFromFile(const std::string& filePath) const override;

    /**
     * Loads a specific (bankNumber, presetNumber) preset from a SoundFont (.sf2) file.
     * Pass bankNumber < 0 or presetNumber < 0 to load the first available preset.
     */
    FormatLoadResult loadPresetFromFile(const std::string& filePath,
                                        int targetBank = -1,
                                        int targetPreset = -1) const;

    /**
     * Lists all non-terminal presets contained in a SoundFont (.sf2) file.
     */
    static bool listPresets(const std::string& filePath,
                            std::vector<Sf2PresetInfo>& outPresets,
                            std::string& outError);

    /**
     * Utility for automated testing: writes a valid binary SoundFont 2.01 (.sf2) bank
     * containing two velocity-layered presets, looped 16-bit PCM audio samples,
     * key ranges, tuning, and timecent ADSR envelopes.
     */
    static bool writeTestSf2Bank(const std::string& filePath);

    static float timecentsToMilliseconds(int16_t timecents) noexcept;
    static float centsToHz(int16_t cents) noexcept;
};

} // namespace konos
