#pragma once

#include <array>
#include <string>
#include <sstream>
#include <iomanip>
#include "../AudioEngine/SamplerEngine.h"

namespace konos {

/**
 * Versioned State Serializer (schemaVersion=2, backward-compatible with v1).
 * Saves and restores:
 * - Global SamplerParameterBlock
 * - All 4 Multi-Instrument Rack Slots (midiChannel, volumeDb, pan, tune, mute, solo, sourcePath)
 * - All 4 Modulation Matrix Slots + LFO 1 Rate & Waveform
 * - All 5 Master Effects Chain Modules (EQ, Chorus, Delay, Reverb, Compressor)
 */
class PresetStateManager {
public:
    static std::string serializeEngineState(const SamplerEngine& engine,
                                            std::array<std::string, SamplerEngine::kMaxRackSlots>* optionalSlotPaths = nullptr) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);
        oss << "schemaVersion=2\n";

        const auto& p = engine.getParameters();
        oss << "selectedRackSlot=" << engine.getSelectedRackSlot() << "\n";
        oss << "masterGainDb=" << p.masterGainDb.load() << "\n";
        oss << "masterPan=" << p.masterPan.load() << "\n";
        oss << "masterTuneSemitones=" << p.masterTuneSemitones.load() << "\n";
        oss << "attackMs=" << p.attackMs.load() << "\n";
        oss << "decayMs=" << p.decayMs.load() << "\n";
        oss << "sustainLevel=" << p.sustainLevel.load() << "\n";
        oss << "releaseMs=" << p.releaseMs.load() << "\n";
        oss << "filterCutoffHz=" << p.filterCutoffHz.load() << "\n";
        oss << "filterResonance=" << p.filterResonance.load() << "\n";
        oss << "filterMode=" << p.filterMode.load() << "\n";
        oss << "sampleStartNorm=" << p.sampleStartNorm.load() << "\n";
        oss << "sampleEndNorm=" << p.sampleEndNorm.load() << "\n";
        oss << "loopEnabled=" << (p.loopEnabled.load() ? 1 : 0) << "\n";
        oss << "loopStartNorm=" << p.loopStartNorm.load() << "\n";
        oss << "loopEndNorm=" << p.loopEndNorm.load() << "\n";
        oss << "maxPolyphony=" << p.maxPolyphony.load() << "\n";

        // Modulation Matrix & LFO 1
        const auto& mod = engine.getModulationMatrix();
        oss << "lfo1RateHz=" << mod.lfo1RateHz.load() << "\n";
        oss << "lfo1Waveform=" << mod.lfo1Waveform.load() << "\n";
        for (std::size_t m = 0; m < ModulationMatrix::kNumSlots; ++m) {
            const auto& slot = mod.getSlot(m);
            const std::string pre = "mod" + std::to_string(m) + "_";
            oss << pre << "src=" << slot.source.load() << "\n";
            oss << pre << "dst=" << slot.destination.load() << "\n";
            oss << pre << "amt=" << slot.amount.load() << "\n";
        }

        // Effects Chain
        const auto& fx = engine.getEffectsChain().getParameters();
        oss << "eqEnabled=" << (fx.eqEnabled.load() ? 1 : 0) << "\n";
        oss << "eqLowGainDb=" << fx.eqLowGainDb.load() << "\n";
        oss << "eqMidGainDb=" << fx.eqMidGainDb.load() << "\n";
        oss << "eqHighGainDb=" << fx.eqHighGainDb.load() << "\n";
        oss << "chorusEnabled=" << (fx.chorusEnabled.load() ? 1 : 0) << "\n";
        oss << "chorusRateHz=" << fx.chorusRateHz.load() << "\n";
        oss << "chorusDepth=" << fx.chorusDepth.load() << "\n";
        oss << "chorusMix=" << fx.chorusMix.load() << "\n";
        oss << "delayEnabled=" << (fx.delayEnabled.load() ? 1 : 0) << "\n";
        oss << "delayTimeMs=" << fx.delayTimeMs.load() << "\n";
        oss << "delayFeedback=" << fx.delayFeedback.load() << "\n";
        oss << "delayMix=" << fx.delayMix.load() << "\n";
        oss << "reverbEnabled=" << (fx.reverbEnabled.load() ? 1 : 0) << "\n";
        oss << "reverbRoomSize=" << fx.reverbRoomSize.load() << "\n";
        oss << "reverbDamping=" << fx.reverbDamping.load() << "\n";
        oss << "reverbMix=" << fx.reverbMix.load() << "\n";
        oss << "compEnabled=" << (fx.compEnabled.load() ? 1 : 0) << "\n";
        oss << "compThresholdDb=" << fx.compThresholdDb.load() << "\n";
        oss << "compRatio=" << fx.compRatio.load() << "\n";
        oss << "compMakeupDb=" << fx.compMakeupDb.load() << "\n";

        // Multi-Instrument Rack Slots
        for (std::size_t s = 0; s < SamplerEngine::kMaxRackSlots; ++s) {
            const auto& st = engine.getRackSlotState(s);
            const auto patch = engine.getRackSlotPatchForUI(s);
            const std::string pre = "slot" + std::to_string(s) + "_";
            std::string pathStr = patch ? patch->sourcePath : "";
            if (optionalSlotPaths && !(*optionalSlotPaths)[s].empty()) {
                pathStr = (*optionalSlotPaths)[s];
            }
            oss << pre << "enabled=" << (st.enabled.load() ? 1 : 0) << "\n";
            oss << pre << "midiCh=" << static_cast<int>(st.midiChannel.load()) << "\n";
            oss << pre << "volDb=" << st.volumeDb.load() << "\n";
            oss << pre << "pan=" << st.pan.load() << "\n";
            oss << pre << "tune=" << st.tuneSemitones.load() << "\n";
            oss << pre << "mute=" << (st.mute.load() ? 1 : 0) << "\n";
            oss << pre << "solo=" << (st.solo.load() ? 1 : 0) << "\n";
            oss << pre << "path=" << pathStr << "\n";
        }

        return oss.str();
    }

    static bool deserializeEngineState(
        const std::string& data,
        SamplerEngine& engine,
        std::array<std::string, SamplerEngine::kMaxRackSlots>& outSlotPaths) {
        std::istringstream iss(data);
        std::string line;
        bool foundSchema = false;

        auto& p   = engine.getParameters();
        auto& mod = engine.getModulationMatrix();
        auto& fx  = engine.getEffectsChain().getParameters();

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const std::string val = line.substr(eq + 1);

            try {
                if (key == "schemaVersion") {
                    const int ver = std::stoi(val);
                    if (ver != 1 && ver != 2) return false;
                    foundSchema = true;
                } else if (key == "loadedFilePath") {
                    outSlotPaths[0] = val;
                } else if (key == "selectedRackSlot") {
                    engine.setSelectedRackSlot(static_cast<std::size_t>(std::max(0, std::stoi(val))));
                } else if (key == "masterGainDb") {
                    p.masterGainDb.store(std::stof(val));
                } else if (key == "masterPan") {
                    p.masterPan.store(std::stof(val));
                } else if (key == "masterTuneSemitones") {
                    p.masterTuneSemitones.store(std::stof(val));
                } else if (key == "attackMs") {
                    p.attackMs.store(std::stof(val));
                } else if (key == "decayMs") {
                    p.decayMs.store(std::stof(val));
                } else if (key == "sustainLevel") {
                    p.sustainLevel.store(std::stof(val));
                } else if (key == "releaseMs") {
                    p.releaseMs.store(std::stof(val));
                } else if (key == "filterCutoffHz") {
                    p.filterCutoffHz.store(std::stof(val));
                } else if (key == "filterResonance") {
                    p.filterResonance.store(std::stof(val));
                } else if (key == "filterMode") {
                    p.filterMode.store(std::stoi(val));
                } else if (key == "sampleStartNorm") {
                    p.sampleStartNorm.store(std::stof(val));
                } else if (key == "sampleEndNorm") {
                    p.sampleEndNorm.store(std::stof(val));
                } else if (key == "loopEnabled") {
                    p.loopEnabled.store(std::stoi(val) != 0);
                } else if (key == "loopStartNorm") {
                    p.loopStartNorm.store(std::stof(val));
                } else if (key == "loopEndNorm") {
                    p.loopEndNorm.store(std::stof(val));
                } else if (key == "maxPolyphony") {
                    p.maxPolyphony.store(std::stoi(val));
                } else if (key == "lfo1RateHz") {
                    mod.lfo1RateHz.store(std::stof(val));
                } else if (key == "lfo1Waveform") {
                    mod.lfo1Waveform.store(std::stoi(val));
                } else if (key == "eqEnabled") {
                    fx.eqEnabled.store(std::stoi(val) != 0);
                } else if (key == "eqLowGainDb") {
                    fx.eqLowGainDb.store(std::stof(val));
                } else if (key == "eqMidGainDb") {
                    fx.eqMidGainDb.store(std::stof(val));
                } else if (key == "eqHighGainDb") {
                    fx.eqHighGainDb.store(std::stof(val));
                } else if (key == "chorusEnabled") {
                    fx.chorusEnabled.store(std::stoi(val) != 0);
                } else if (key == "chorusRateHz") {
                    fx.chorusRateHz.store(std::stof(val));
                } else if (key == "chorusDepth") {
                    fx.chorusDepth.store(std::stof(val));
                } else if (key == "chorusMix") {
                    fx.chorusMix.store(std::stof(val));
                } else if (key == "delayEnabled") {
                    fx.delayEnabled.store(std::stoi(val) != 0);
                } else if (key == "delayTimeMs") {
                    fx.delayTimeMs.store(std::stof(val));
                } else if (key == "delayFeedback") {
                    fx.delayFeedback.store(std::stof(val));
                } else if (key == "delayMix") {
                    fx.delayMix.store(std::stof(val));
                } else if (key == "reverbEnabled") {
                    fx.reverbEnabled.store(std::stoi(val) != 0);
                } else if (key == "reverbRoomSize") {
                    fx.reverbRoomSize.store(std::stof(val));
                } else if (key == "reverbDamping") {
                    fx.reverbDamping.store(std::stof(val));
                } else if (key == "reverbMix") {
                    fx.reverbMix.store(std::stof(val));
                } else if (key == "compEnabled") {
                    fx.compEnabled.store(std::stoi(val) != 0);
                } else if (key == "compThresholdDb") {
                    fx.compThresholdDb.store(std::stof(val));
                } else if (key == "compRatio") {
                    fx.compRatio.store(std::stof(val));
                } else if (key == "compMakeupDb") {
                    fx.compMakeupDb.store(std::stof(val));
                } else {
                    // Check mod0..3 and slot0..3 keys
                    for (std::size_t m = 0; m < ModulationMatrix::kNumSlots; ++m) {
                        const std::string pre = "mod" + std::to_string(m) + "_";
                        if (key == pre + "src") mod.getSlot(m).source.store(std::stoi(val));
                        else if (key == pre + "dst") mod.getSlot(m).destination.store(std::stoi(val));
                        else if (key == pre + "amt") mod.getSlot(m).amount.store(std::stof(val));
                    }
                    for (std::size_t s = 0; s < SamplerEngine::kMaxRackSlots; ++s) {
                        const std::string pre = "slot" + std::to_string(s) + "_";
                        auto& st = engine.getRackSlotState(s);
                        if (key == pre + "enabled") st.enabled.store(std::stoi(val) != 0);
                        else if (key == pre + "midiCh") st.midiChannel.store(static_cast<uint8_t>(std::stoi(val)));
                        else if (key == pre + "volDb") st.volumeDb.store(std::stof(val));
                        else if (key == pre + "pan") st.pan.store(std::stof(val));
                        else if (key == pre + "tune") st.tuneSemitones.store(std::stof(val));
                        else if (key == pre + "mute") st.mute.store(std::stoi(val) != 0);
                        else if (key == pre + "solo") st.solo.store(std::stoi(val) != 0);
                        else if (key == pre + "path") outSlotPaths[s] = val;
                    }
                }
            } catch (...) {
                return false;
            }
        }
        return foundSchema;
    }

    // Legacy v1 helpers preserved for compatibility
    static std::string serializeToKeyValueString(const SamplerParameterBlock& params,
                                                 const std::string& currentSamplePath) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);
        oss << "schemaVersion=1\n";
        oss << "loadedFilePath=" << currentSamplePath << "\n";
        oss << "masterGainDb=" << params.masterGainDb.load() << "\n";
        oss << "masterPan=" << params.masterPan.load() << "\n";
        oss << "masterTuneSemitones=" << params.masterTuneSemitones.load() << "\n";
        oss << "attackMs=" << params.attackMs.load() << "\n";
        oss << "decayMs=" << params.decayMs.load() << "\n";
        oss << "sustainLevel=" << params.sustainLevel.load() << "\n";
        oss << "releaseMs=" << params.releaseMs.load() << "\n";
        oss << "filterCutoffHz=" << params.filterCutoffHz.load() << "\n";
        oss << "filterResonance=" << params.filterResonance.load() << "\n";
        oss << "filterMode=" << params.filterMode.load() << "\n";
        oss << "sampleStartNorm=" << params.sampleStartNorm.load() << "\n";
        oss << "sampleEndNorm=" << params.sampleEndNorm.load() << "\n";
        oss << "loopEnabled=" << (params.loopEnabled.load() ? 1 : 0) << "\n";
        oss << "loopStartNorm=" << params.loopStartNorm.load() << "\n";
        oss << "loopEndNorm=" << params.loopEndNorm.load() << "\n";
        oss << "maxPolyphony=" << params.maxPolyphony.load() << "\n";
        return oss.str();
    }

    static bool deserializeFromKeyValueString(const std::string& data,
                                              SamplerParameterBlock& params,
                                              std::string& outSamplePath) {
        std::istringstream iss(data);
        std::string line;
        bool foundSchema = false;

        while (std::getline(iss, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const std::string val = line.substr(eq + 1);

            if (key == "schemaVersion") {
                const int ver = std::stoi(val);
                if (ver != 1 && ver != 2) return false;
                foundSchema = true;
            } else if (key == "loadedFilePath" || key == "slot0_path") {
                outSamplePath = val;
            } else if (key == "masterGainDb") {
                params.masterGainDb.store(std::stof(val));
            } else if (key == "masterPan") {
                params.masterPan.store(std::stof(val));
            } else if (key == "masterTuneSemitones") {
                params.masterTuneSemitones.store(std::stof(val));
            } else if (key == "attackMs") {
                params.attackMs.store(std::stof(val));
            } else if (key == "decayMs") {
                params.decayMs.store(std::stof(val));
            } else if (key == "sustainLevel") {
                params.sustainLevel.store(std::stof(val));
            } else if (key == "releaseMs") {
                params.releaseMs.store(std::stof(val));
            } else if (key == "filterCutoffHz") {
                params.filterCutoffHz.store(std::stof(val));
            } else if (key == "filterResonance") {
                params.filterResonance.store(std::stof(val));
            } else if (key == "filterMode") {
                params.filterMode.store(std::stoi(val));
            } else if (key == "sampleStartNorm") {
                params.sampleStartNorm.store(std::stof(val));
            } else if (key == "sampleEndNorm") {
                params.sampleEndNorm.store(std::stof(val));
            } else if (key == "loopEnabled") {
                params.loopEnabled.store(std::stoi(val) != 0);
            } else if (key == "loopStartNorm") {
                params.loopStartNorm.store(std::stof(val));
            } else if (key == "loopEndNorm") {
                params.loopEndNorm.store(std::stof(val));
            } else if (key == "maxPolyphony") {
                params.maxPolyphony.store(std::stoi(val));
            }
        }
        return foundSchema;
    }
};

} // namespace konos
