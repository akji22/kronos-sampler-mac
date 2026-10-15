#include "Sf2FormatLoader.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>

namespace konos {

float Sf2FormatLoader::timecentsToMilliseconds(int16_t timecents) noexcept {
    if (timecents <= -12000) return 0.5f;
    return static_cast<float>(1000.0 * std::pow(2.0, static_cast<double>(timecents) / 1200.0));
}

float Sf2FormatLoader::centsToHz(int16_t cents) noexcept {
    return static_cast<float>(8.1757989156 * std::pow(2.0, static_cast<double>(cents) / 1200.0));
}

namespace {

uint16_t readLE16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

int16_t readLES16(const uint8_t* p) noexcept {
    return static_cast<int16_t>(readLE16(p));
}

uint32_t readLE32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
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

void writeFixedString(std::vector<uint8_t>& out, const std::string& s, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
    }
}

std::string readFixedString(const uint8_t* p, std::size_t maxLen) {
    std::size_t n = 0;
    while (n < maxLen && p[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

// SoundFont 2.04 Hydra Record Structures
struct SfPresetHeader {
    std::string name;
    uint16_t    preset{0};
    uint16_t    bank{0};
    uint16_t    presetBagNdx{0};
};

struct SfBag {
    uint16_t genNdx{0};
    uint16_t modNdx{0};
};

struct SfGen {
    uint16_t oper{0};
    int16_t  amount{0};
    uint8_t  loByte() const noexcept { return static_cast<uint8_t>(static_cast<uint16_t>(amount) & 0xFF); }
    uint8_t  hiByte() const noexcept { return static_cast<uint8_t>((static_cast<uint16_t>(amount) >> 8) & 0xFF); }
};

struct SfInst {
    std::string name;
    uint16_t    instBagNdx{0};
};

struct SfSampleHeader {
    std::string name;
    uint32_t    start{0};
    uint32_t    end{0};
    uint32_t    startLoop{0};
    uint32_t    endLoop{0};
    uint32_t    sampleRate{44100};
    uint8_t     originalPitch{60};
    int8_t      pitchCorrection{0};
    uint16_t    sampleLink{0};
    uint16_t    sampleType{1};
};

// Generator IDs per SoundFont 2.04 Spec Section 8.1.2
enum SfGeneratorId : uint16_t {
    kGenStartAddrsOffset           = 0,
    kGenEndAddrsOffset             = 1,
    kGenStartloopAddrsOffset       = 2,
    kGenEndloopAddrsOffset         = 3,
    kGenStartAddrsCoarseOffset     = 4,
    kGenInitialFilterFc            = 8,
    kGenInitialFilterQ             = 9,
    kGenEndAddrsCoarseOffset       = 12,
    kGenPan                        = 17,
    kGenAttackVolEnv               = 34,
    kGenHoldVolEnv                 = 35,
    kGenDecayVolEnv                = 36,
    kGenSustainVolEnv              = 37,
    kGenReleaseVolEnv              = 38,
    kGenInstrument                 = 41,
    kGenKeyRange                   = 43,
    kGenVelRange                   = 44,
    kGenStartloopAddrsCoarseOffset = 45,
    kGenKeynum                     = 46,
    kGenVelocity                   = 47,
    kGenInitialAttenuation         = 48,
    kGenEndloopAddrsCoarseOffset   = 50,
    kGenCoarseTune                 = 51,
    kGenFineTune                   = 52,
    kGenSampleID                   = 53,
    kGenSampleModes                = 54,
    kGenScaleTuning                = 56,
    kGenOverridingRootKey          = 58
};

struct SfZoneState {
    uint8_t loKey{0};
    uint8_t hiKey{127};
    uint8_t loVel{0};
    uint8_t hiVel{127};

    int32_t startOffset{0};
    int32_t endOffset{0};
    int32_t startLoopOffset{0};
    int32_t endLoopOffset{0};

    int16_t filterFcCents{13500}; // ~19.9 kHz default
    int16_t filterQCb{0};
    int16_t panTenths{0};         // [-500, +500]
    int16_t attenuationCb{0};     // centibels (0.1 dB)

    int16_t attackTc{-12000};
    int16_t holdTc{-12000};
    int16_t decayTc{-12000};
    int16_t sustainCb{0};
    int16_t releaseTc{-12000};

    int16_t coarseTune{0};
    int16_t fineTune{0};
    int16_t overridingRootKey{-1};
    int16_t sampleModes{0};

    int32_t targetInstrument{-1};
    int32_t targetSampleId{-1};

    void applyGenerator(const SfGen& g, bool isAdditivePresetLevel) noexcept {
        switch (g.oper) {
            case kGenKeyRange:
                loKey = std::max(loKey, g.loByte());
                hiKey = std::min(hiKey, g.hiByte());
                break;
            case kGenVelRange:
                loVel = std::max(loVel, g.loByte());
                hiVel = std::min(hiVel, g.hiByte());
                break;
            case kGenStartAddrsOffset:
                startOffset += g.amount;
                break;
            case kGenEndAddrsOffset:
                endOffset += g.amount;
                break;
            case kGenStartloopAddrsOffset:
                startLoopOffset += g.amount;
                break;
            case kGenEndloopAddrsOffset:
                endLoopOffset += g.amount;
                break;
            case kGenStartAddrsCoarseOffset:
                startOffset += static_cast<int32_t>(g.amount) * 32768;
                break;
            case kGenEndAddrsCoarseOffset:
                endOffset += static_cast<int32_t>(g.amount) * 32768;
                break;
            case kGenStartloopAddrsCoarseOffset:
                startLoopOffset += static_cast<int32_t>(g.amount) * 32768;
                break;
            case kGenEndloopAddrsCoarseOffset:
                endLoopOffset += static_cast<int32_t>(g.amount) * 32768;
                break;
            case kGenInitialFilterFc:
                filterFcCents = isAdditivePresetLevel ? static_cast<int16_t>(filterFcCents + g.amount) : g.amount;
                break;
            case kGenInitialFilterQ:
                filterQCb = isAdditivePresetLevel ? static_cast<int16_t>(filterQCb + g.amount) : g.amount;
                break;
            case kGenPan:
                panTenths = isAdditivePresetLevel ? static_cast<int16_t>(panTenths + g.amount) : g.amount;
                break;
            case kGenInitialAttenuation:
                attenuationCb = isAdditivePresetLevel ? static_cast<int16_t>(attenuationCb + g.amount) : g.amount;
                break;
            case kGenAttackVolEnv:
                attackTc = isAdditivePresetLevel ? static_cast<int16_t>(attackTc + g.amount) : g.amount;
                break;
            case kGenHoldVolEnv:
                holdTc = isAdditivePresetLevel ? static_cast<int16_t>(holdTc + g.amount) : g.amount;
                break;
            case kGenDecayVolEnv:
                decayTc = isAdditivePresetLevel ? static_cast<int16_t>(decayTc + g.amount) : g.amount;
                break;
            case kGenSustainVolEnv:
                sustainCb = isAdditivePresetLevel ? static_cast<int16_t>(sustainCb + g.amount) : g.amount;
                break;
            case kGenReleaseVolEnv:
                releaseTc = isAdditivePresetLevel ? static_cast<int16_t>(releaseTc + g.amount) : g.amount;
                break;
            case kGenCoarseTune:
                coarseTune = isAdditivePresetLevel ? static_cast<int16_t>(coarseTune + g.amount) : g.amount;
                break;
            case kGenFineTune:
                fineTune = isAdditivePresetLevel ? static_cast<int16_t>(fineTune + g.amount) : g.amount;
                break;
            case kGenOverridingRootKey:
                if (!isAdditivePresetLevel && g.amount >= 0 && g.amount <= 127) {
                    overridingRootKey = g.amount;
                }
                break;
            case kGenSampleModes:
                if (!isAdditivePresetLevel) sampleModes = g.amount;
                break;
            case kGenInstrument:
                targetInstrument = static_cast<uint16_t>(g.amount);
                break;
            case kGenSampleID:
                targetSampleId = static_cast<uint16_t>(g.amount);
                break;
            default:
                break;
        }
    }
};

struct ParsedSf2Hydra {
    std::string bankName{"SoundFont Bank"};
    const int16_t* smplPcm16{nullptr};
    const uint8_t* sm24Pcm8{nullptr};
    std::size_t    numSamplePoints{0};

    std::vector<SfPresetHeader> phdr;
    std::vector<SfBag>          pbag;
    std::vector<SfGen>          pgen;
    std::vector<SfInst>         inst;
    std::vector<SfBag>          ibag;
    std::vector<SfGen>          igen;
    std::vector<SfSampleHeader> shdr;
};

bool parseSf2Bytes(const std::vector<uint8_t>& bytes,
                   ParsedSf2Hydra& outHydra,
                   std::string& outError) {
    if (bytes.size() < 64 ||
        std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "sfbk", 4) != 0) {
        outError = "File is not a valid RIFF 'sfbk' SoundFont 2 container.";
        return false;
    }

    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const char*    chunkId   = reinterpret_cast<const char*>(bytes.data() + offset);
        const uint32_t chunkSize = readLE32(bytes.data() + offset + 4);
        const std::size_t payloadOffset = offset + 8;

        if (payloadOffset + chunkSize > bytes.size()) {
            outError = "Truncated RIFF chunk inside SoundFont 2 file.";
            return false;
        }

        if (std::memcmp(chunkId, "LIST", 4) == 0 && chunkSize >= 4) {
            const char* listType = reinterpret_cast<const char*>(bytes.data() + payloadOffset);
            std::size_t subOffset = payloadOffset + 4;
            const std::size_t listEnd = payloadOffset + chunkSize;

            while (subOffset + 8 <= listEnd) {
                const char*    subId   = reinterpret_cast<const char*>(bytes.data() + subOffset);
                const uint32_t subSize = readLE32(bytes.data() + subOffset + 4);
                const std::size_t subData = subOffset + 8;

                if (subData + subSize > listEnd) {
                    outError = "Truncated sub-chunk ('" + std::string(subId, 4) + "') in SF2 LIST.";
                    return false;
                }

                const uint8_t* ptr = bytes.data() + subData;
                if (std::memcmp(listType, "INFO", 4) == 0) {
                    if (std::memcmp(subId, "INAM", 4) == 0) {
                        outHydra.bankName = readFixedString(ptr, subSize);
                    }
                } else if (std::memcmp(listType, "sdta", 4) == 0) {
                    if (std::memcmp(subId, "smpl", 4) == 0) {
                        outHydra.smplPcm16 = reinterpret_cast<const int16_t*>(ptr);
                        outHydra.numSamplePoints = subSize / 2;
                    } else if (std::memcmp(subId, "sm24", 4) == 0) {
                        outHydra.sm24Pcm8 = ptr;
                    }
                } else if (std::memcmp(listType, "pdta", 4) == 0) {
                    if (std::memcmp(subId, "phdr", 4) == 0) {
                        if (subSize % 38 != 0) { outError = "Invalid SF2 phdr size."; return false; }
                        const std::size_t count = subSize / 38;
                        outHydra.phdr.resize(count);
                        for (std::size_t i = 0; i < count; ++i) {
                            const uint8_t* r = ptr + i * 38;
                            outHydra.phdr[i].name         = readFixedString(r, 20);
                            outHydra.phdr[i].preset       = readLE16(r + 20);
                            outHydra.phdr[i].bank         = readLE16(r + 22);
                            outHydra.phdr[i].presetBagNdx = readLE16(r + 24);
                        }
                    } else if (std::memcmp(subId, "pbag", 4) == 0) {
                        if (subSize % 4 != 0) { outError = "Invalid SF2 pbag size."; return false; }
                        const std::size_t count = subSize / 4;
                        outHydra.pbag.resize(count);
                        for (std::size_t i = 0; i < count; ++i) {
                            outHydra.pbag[i].genNdx = readLE16(ptr + i * 4);
                            outHydra.pbag[i].modNdx = readLE16(ptr + i * 4 + 2);
                        }
                    } else if (std::memcmp(subId, "pgen", 4) == 0) {
                        if (subSize % 4 != 0) { outError = "Invalid SF2 pgen size."; return false; }
                        const std::size_t count = subSize / 4;
                        outHydra.pgen.resize(count);
                        for (std::size_t i = 0; i < count; ++i) {
                            outHydra.pgen[i].oper   = readLE16(ptr + i * 4);
                            outHydra.pgen[i].amount = readLES16(ptr + i * 4 + 2);
                        }
                    } else if (std::memcmp(subId, "inst", 4) == 0) {
                        if (subSize % 22 != 0) { outError = "Invalid SF2 inst size."; return false; }
                        const std::size_t count = subSize / 22;
                        outHydra.inst.resize(count);
                        for (std::size_t i = 0; i < count; ++i) {
                            const uint8_t* r = ptr + i * 22;
                            outHydra.inst[i].name       = readFixedString(r, 20);
                            outHydra.inst[i].instBagNdx = readLE16(r + 20);
                        }
                    } else if (std::memcmp(subId, "ibag", 4) == 0) {
                        if (subSize % 4 != 0) { outError = "Invalid SF2 ibag size."; return false; }
                        const std::size_t count = subSize / 4;
                        outHydra.ibag.resize(count);
                        for (std::size_t i = 0; i < count; ++i) {
                            outHydra.ibag[i].genNdx = readLE16(ptr + i * 4);
                            outHydra.ibag[i].modNdx = readLE16(ptr + i * 4 + 2);
                        }
                    } else if (std::memcmp(subId, "igen", 4) == 0) {
                        if (subSize % 4 != 0) { outError = "Invalid SF2 igen size."; return false; }
                        const std::size_t count = subSize / 4;
                        outHydra.igen.resize(count);
                        for (std::size_t i = 0; i < count; ++i) {
                            outHydra.igen[i].oper   = readLE16(ptr + i * 4);
                            outHydra.igen[i].amount = readLES16(ptr + i * 4 + 2);
                        }
                    } else if (std::memcmp(subId, "shdr", 4) == 0) {
                        if (subSize % 46 != 0) { outError = "Invalid SF2 shdr size."; return false; }
                        const std::size_t count = subSize / 46;
                        outHydra.shdr.resize(count);
                        for (std::size_t i = 0; i < count; ++i) {
                            const uint8_t* r = ptr + i * 46;
                            outHydra.shdr[i].name            = readFixedString(r, 20);
                            outHydra.shdr[i].start           = readLE32(r + 20);
                            outHydra.shdr[i].end             = readLE32(r + 24);
                            outHydra.shdr[i].startLoop       = readLE32(r + 28);
                            outHydra.shdr[i].endLoop         = readLE32(r + 32);
                            outHydra.shdr[i].sampleRate      = readLE32(r + 36);
                            outHydra.shdr[i].originalPitch   = r[40];
                            outHydra.shdr[i].pitchCorrection = static_cast<int8_t>(r[41]);
                            outHydra.shdr[i].sampleLink      = readLE16(r + 42);
                            outHydra.shdr[i].sampleType      = readLE16(r + 44);
                        }
                    }
                }
                subOffset = subData + subSize + (subSize & 1U);
            }
        }
        offset = payloadOffset + chunkSize + (chunkSize & 1U);
    }

    if (!outHydra.smplPcm16 || outHydra.numSamplePoints == 0 ||
        outHydra.phdr.size() < 2 || outHydra.inst.size() < 2 || outHydra.shdr.size() < 2) {
        outError = "SF2 file is missing required Hydra ('pdta') records or 'smpl' PCM data.";
        return false;
    }
    return true;
}

} // namespace

bool Sf2FormatLoader::listPresets(const std::string& filePath,
                                  std::vector<Sf2PresetInfo>& outPresets,
                                  std::string& outError) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        outError = "Unable to open SF2 file: " + filePath;
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size < 64) {
        outError = "SF2 file is smaller than minimum header size.";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), size);

    ParsedSf2Hydra hydra;
    if (!parseSf2Bytes(bytes, hydra, outError)) return false;

    outPresets.clear();
    // Last phdr record is the terminal EOP record
    for (std::size_t i = 0; i + 1 < hydra.phdr.size(); ++i) {
        outPresets.push_back(Sf2PresetInfo{
            hydra.phdr[i].name,
            hydra.phdr[i].bank,
            hydra.phdr[i].preset
        });
    }
    return true;
}

FormatLoadResult Sf2FormatLoader::loadFromFile(const std::string& filePath) const {
    return loadPresetFromFile(filePath, -1, -1);
}

FormatLoadResult Sf2FormatLoader::loadPresetFromFile(const std::string& filePath,
                                                     int targetBank,
                                                     int targetPreset) const {
    FormatLoadResult res;
    res.formatIdentifier = "SoundFont 2 (.sf2)";

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        res.status = FormatSupportStatus::FileNotFound;
        res.diagnosticMessage = "Unable to open SF2 file: " + filePath;
        return res;
    }

    const std::streamsize size = file.tellg();
    if (size < 64 || size > static_cast<std::streamsize>(1024ULL * 1024ULL * 1024ULL)) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "Invalid or corrupted SF2 file size.";
        return res;
    }

    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "I/O error while reading SF2 file.";
        return res;
    }

    ParsedSf2Hydra hydra;
    std::string parseErr;
    if (!parseSf2Bytes(bytes, hydra, parseErr)) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = parseErr;
        return res;
    }

    // Select requested preset (or first non-terminal preset if targetBank/targetPreset < 0)
    std::size_t selectedPresetIdx = hydra.phdr.size();
    for (std::size_t i = 0; i + 1 < hydra.phdr.size(); ++i) {
        if ((targetBank < 0 || hydra.phdr[i].bank == static_cast<uint16_t>(targetBank)) &&
            (targetPreset < 0 || hydra.phdr[i].preset == static_cast<uint16_t>(targetPreset))) {
            selectedPresetIdx = i;
            break;
        }
    }

    if (selectedPresetIdx >= hydra.phdr.size() - 1) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "Requested SF2 preset not found in bank.";
        return res;
    }

    const auto& presetHdr = hydra.phdr[selectedPresetIdx];
    const uint16_t pbagStart = presetHdr.presetBagNdx;
    const uint16_t pbagEnd   = hydra.phdr[selectedPresetIdx + 1].presetBagNdx;
    if (pbagStart >= pbagEnd || pbagEnd >= hydra.pbag.size()) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "SF2 preset bag indices are out of bounds.";
        return res;
    }

    auto patch = std::make_shared<InstrumentPatch>();
    patch->name = hydra.bankName + " - " + presetHdr.name;
    patch->sourcePath = filePath;
    patch->formatName = "SF2";

    SfZoneState presetGlobalZone{};
    // Reset preset-level additive fields to 0 so they act as relative offsets
    presetGlobalZone.filterFcCents = 0; presetGlobalZone.attackTc = 0; presetGlobalZone.holdTc = 0; presetGlobalZone.decayTc = 0; presetGlobalZone.releaseTc = 0;

    for (uint16_t pb = pbagStart; pb < pbagEnd; ++pb) {
        const uint16_t pgenStart = hydra.pbag[pb].genNdx;
        const uint16_t pgenEnd   = hydra.pbag[pb + 1].genNdx;
        if (pgenEnd > hydra.pgen.size()) break;

        SfZoneState pZone = presetGlobalZone;
        for (uint16_t pg = pgenStart; pg < pgenEnd; ++pg) {
            pZone.applyGenerator(hydra.pgen[pg], true);
        }

        // First zone without an Instrument generator is the Preset Global Zone
        if (pZone.targetInstrument < 0) {
            if (pb == pbagStart) {
                presetGlobalZone = pZone;
            }
            continue;
        }

        const std::size_t instIdx = static_cast<std::size_t>(pZone.targetInstrument);
        if (instIdx + 1 >= hydra.inst.size()) {
            res.warnings.push_back("SF2 preset references out-of-range instrument index.");
            continue;
        }

        const uint16_t ibagStart = hydra.inst[instIdx].instBagNdx;
        const uint16_t ibagEnd   = hydra.inst[instIdx + 1].instBagNdx;
        if (ibagStart >= ibagEnd || ibagEnd >= hydra.ibag.size()) continue;

        SfZoneState instGlobalZone{};
        for (uint16_t ib = ibagStart; ib < ibagEnd; ++ib) {
            const uint16_t igenStart = hydra.ibag[ib].genNdx;
            const uint16_t igenEnd   = hydra.ibag[ib + 1].genNdx;
            if (igenEnd > hydra.igen.size()) break;

            SfZoneState iZone = instGlobalZone;
            for (uint16_t ig = igenStart; ig < igenEnd; ++ig) {
                iZone.applyGenerator(hydra.igen[ig], false);
            }

            // First zone without a SampleID generator is the Instrument Global Zone
            if (iZone.targetSampleId < 0) {
                if (ib == ibagStart) {
                    instGlobalZone = iZone;
                }
                continue;
            }

            const std::size_t shdrIdx = static_cast<std::size_t>(iZone.targetSampleId);
            if (shdrIdx + 1 >= hydra.shdr.size()) {
                res.warnings.push_back("SF2 instrument zone references out-of-range sample header.");
                continue;
            }

            const auto& shdr = hydra.shdr[shdrIdx];
            // Skip ROM samples (bit 15 set in sampleType)
            if ((shdr.sampleType & 0x8000U) != 0) {
                res.warnings.push_back("Skipped SF2 ROM sample '" + shdr.name + "'.");
                continue;
            }

            // Intersect key and velocity ranges between Preset Zone and Instrument Zone
            const uint8_t finalLoKey = std::max(pZone.loKey, iZone.loKey);
            const uint8_t finalHiKey = std::min(pZone.hiKey, iZone.hiKey);
            const uint8_t finalLoVel = std::max<uint8_t>(1, std::max(pZone.loVel, iZone.loVel));
            const uint8_t finalHiVel = std::min(pZone.hiVel, iZone.hiVel);
            if (finalLoKey > finalHiKey || finalLoVel > finalHiVel) continue;

            // Compute sample boundaries inside sdta 'smpl' pool
            const int64_t rawStart = static_cast<int64_t>(shdr.start) + iZone.startOffset;
            const int64_t rawEnd   = static_cast<int64_t>(shdr.end)   + iZone.endOffset;
            const int64_t clampedStart = std::clamp<int64_t>(rawStart, 0, static_cast<int64_t>(hydra.numSamplePoints));
            const int64_t clampedEnd   = std::clamp<int64_t>(rawEnd, clampedStart, static_cast<int64_t>(hydra.numSamplePoints));
            const uint64_t numFrames   = static_cast<uint64_t>(clampedEnd - clampedStart);
            if (numFrames == 0) continue;

            const uint8_t rootNote = (iZone.overridingRootKey >= 0 && iZone.overridingRootKey <= 127)
                                         ? static_cast<uint8_t>(iZone.overridingRootKey)
                                         : (shdr.originalPitch <= 127 ? shdr.originalPitch : 60);

            auto sampleBuf = std::make_shared<SampleBuffer>(
                1, numFrames, static_cast<double>(std::max<uint32_t>(8000, shdr.sampleRate)), rootNote);
            sampleBuf->setSourcePath(filePath + "#" + shdr.name);

            float* dst = sampleBuf->getWritePointer(0);
            for (uint64_t f = 0; f < numFrames; ++f) {
                const std::size_t poolIdx = static_cast<std::size_t>(clampedStart) + static_cast<std::size_t>(f);
                int16_t pcm16 = 0;
                std::memcpy(&pcm16, reinterpret_cast<const uint8_t*>(hydra.smplPcm16) + poolIdx * 2, sizeof(int16_t));
                if (hydra.sm24Pcm8) {
                    const int32_t pcm24 = (static_cast<int32_t>(pcm16) << 8) | static_cast<int32_t>(hydra.sm24Pcm8[poolIdx]);
                    dst[f] = static_cast<float>(pcm24) / 8388608.0f;
                } else {
                    dst[f] = static_cast<float>(pcm16) / 32768.0f;
                }
            }

            InstrumentZone zone;
            zone.name    = shdr.name;
            zone.sample  = sampleBuf;
            zone.loKey   = finalLoKey;
            zone.hiKey   = finalHiKey;
            zone.rootKey = rootNote;
            zone.loVel   = finalLoVel;
            zone.hiVel   = finalHiVel;
            zone.endFrame = numFrames;

            // Loop points relative to zone sample buffer start
            const int64_t loopStartAbs = static_cast<int64_t>(shdr.startLoop) + iZone.startLoopOffset;
            const int64_t loopEndAbs   = static_cast<int64_t>(shdr.endLoop)   + iZone.endLoopOffset;
            const uint64_t relLoopStart = static_cast<uint64_t>(
                std::clamp<int64_t>(loopStartAbs - clampedStart, 0, static_cast<int64_t>(numFrames - 1)));
            const uint64_t relLoopEnd   = static_cast<uint64_t>(
                std::clamp<int64_t>(loopEndAbs - clampedStart, static_cast<int64_t>(relLoopStart + 1), static_cast<int64_t>(numFrames)));

            const int modeFlags = iZone.sampleModes & 3;
            if ((modeFlags == 1 || modeFlags == 3) && relLoopEnd > relLoopStart) {
                zone.loopMode = LoopMode::ForwardLoop;
                zone.loopStartFrame = relLoopStart;
                zone.loopEndFrame   = relLoopEnd;
                sampleBuf->setEmbeddedLoop(true, relLoopStart, relLoopEnd);
            } else {
                zone.loopMode = LoopMode::NoLoop;
            }

            // Combine Instrument + Preset tuning, pan, volume, envelope, and filter
            zone.transposeSemitones = static_cast<int32_t>(iZone.coarseTune) + static_cast<int32_t>(pZone.coarseTune);
            zone.tuneCents          = static_cast<float>(iZone.fineTune + pZone.fineTune + shdr.pitchCorrection);

            const int32_t totalAttenCb = std::max<int32_t>(0, static_cast<int32_t>(iZone.attenuationCb) + static_cast<int32_t>(pZone.attenuationCb));
            zone.volumeDb = -static_cast<float>(totalAttenCb) * 0.1f;

            const int32_t totalPanTenths = std::clamp<int32_t>(
                static_cast<int32_t>(iZone.panTenths) + static_cast<int32_t>(pZone.panTenths), -500, 500);
            zone.pan = static_cast<float>(totalPanTenths) / 500.0f;

            const int16_t atkTc = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(iZone.attackTc) + pZone.attackTc, -12000, 8000));
            const int16_t decTc = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(iZone.decayTc)  + pZone.decayTc,  -12000, 8000));
            const int16_t relTc = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(iZone.releaseTc)+ pZone.releaseTc,-12000, 8000));
            const int32_t susCb = std::clamp<int32_t>(static_cast<int32_t>(iZone.sustainCb) + pZone.sustainCb, 0, 1440);

            zone.attackMsOverride     = timecentsToMilliseconds(atkTc);
            zone.decayMsOverride      = timecentsToMilliseconds(decTc);
            zone.sustainLevelOverride = (susCb >= 1000) ? 0.0f : std::pow(10.0f, -static_cast<float>(susCb) / 200.0f);
            zone.releaseMsOverride    = timecentsToMilliseconds(relTc);

            const int16_t fcCents = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(iZone.filterFcCents) + pZone.filterFcCents, 1500, 13500));
            const int16_t qCb     = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(iZone.filterQCb) + pZone.filterQCb, 0, 960));
            zone.cutoffHzOverride  = std::clamp(centsToHz(fcCents), 20.0f, 20000.0f);
            zone.resonanceOverride = std::clamp(0.707f * std::pow(10.0f, static_cast<float>(qCb) / 200.0f), 0.5f, 12.0f);

            patch->zones.push_back(std::move(zone));
        }
    }

    if (patch->zones.empty()) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "SF2 preset contained no playable sample zones.";
        return res;
    }

    patch->warnings = res.warnings;
    res.succeeded = true;
    res.status = res.warnings.empty() ? FormatSupportStatus::FullySupported : FormatSupportStatus::PartiallySupported;
    res.diagnosticMessage = "Loaded SF2 Preset '" + patch->name + "' with " +
                            std::to_string(patch->zones.size()) + " zone(s).";
    res.patch = std::move(patch);
    return res;
}

bool Sf2FormatLoader::writeTestSf2Bank(const std::string& filePath) {
    // Build a valid binary SoundFont 2.01 (.sf2) file with:
    // - 2 samples in 'smpl': Soft Piano (C4=60, 4410 frames) and Bright Piano (C4=60, 4410 frames)
    // - 1 instrument ("GrandPianoInst") with 2 velocity-layered zones (vel 1..80 and vel 81..127),
    //   looping enabled (sampleModes=1), timecent envelopes, and tuning
    // - 1 preset ("Concert Grand", Bank 0, Preset 0)
    const uint32_t framesPerSample = 4410;
    const uint32_t paddingFrames   = 46; // SF2 spec requires 46 zero sample points after each sample
    const uint32_t totalSmplPoints = (framesPerSample + paddingFrames) * 2;

    std::vector<uint8_t> infoList;
    infoList.insert(infoList.end(), {'I', 'N', 'F', 'O'});
    // ifil (version 2.01)
    infoList.insert(infoList.end(), {'i', 'f', 'i', 'l'});
    writeLE32(infoList, 4);
    writeLE16(infoList, 2);
    writeLE16(infoList, 1);
    // isng ("EMU8000\0")
    infoList.insert(infoList.end(), {'i', 's', 'n', 'g'});
    writeLE32(infoList, 8);
    writeFixedString(infoList, "EMU8000", 8);
    // INAM ("KonosTestBank\0")
    infoList.insert(infoList.end(), {'I', 'N', 'A', 'M'});
    writeLE32(infoList, 14);
    writeFixedString(infoList, "KonosTestBank", 14);

    // sdta LIST
    std::vector<uint8_t> sdtaList;
    sdtaList.insert(sdtaList.end(), {'s', 'd', 't', 'a'});
    sdtaList.insert(sdtaList.end(), {'s', 'm', 'p', 'l'});
    writeLE32(sdtaList, totalSmplPoints * 2);

    constexpr double kTwoPi = 6.283185307179586;
    // Sample 0: Soft fundamental sine at 261.6256 Hz (C4)
    for (uint32_t i = 0; i < framesPerSample; ++i) {
        const double t = static_cast<double>(i) / 44100.0;
        const int16_t s = static_cast<int16_t>(std::lrint(std::sin(kTwoPi * 261.625565 * t) * 18000.0));
        writeLE16(sdtaList, static_cast<uint16_t>(s));
    }
    for (uint32_t i = 0; i < paddingFrames; ++i) writeLE16(sdtaList, 0);

    // Sample 1: Bright harmonic tone at 261.6256 Hz (C4)
    const uint32_t sample1Start = framesPerSample + paddingFrames;
    for (uint32_t i = 0; i < framesPerSample; ++i) {
        const double t = static_cast<double>(i) / 44100.0;
        const double sig = 0.65 * std::sin(kTwoPi * 261.625565 * t) + 0.35 * std::sin(kTwoPi * 523.25113 * t);
        const int16_t s = static_cast<int16_t>(std::lrint(sig * 22000.0));
        writeLE16(sdtaList, static_cast<uint16_t>(s));
    }
    for (uint32_t i = 0; i < paddingFrames; ++i) writeLE16(sdtaList, 0);

    // pdta LIST
    std::vector<uint8_t> pdtaList;
    pdtaList.insert(pdtaList.end(), {'p', 'd', 't', 'a'});

    // 1. phdr (2 records * 38 = 76 bytes)
    pdtaList.insert(pdtaList.end(), {'p', 'h', 'd', 'r'});
    writeLE32(pdtaList, 76);
    writeFixedString(pdtaList, "Concert Grand", 20);
    writeLE16(pdtaList, 0); // preset 0
    writeLE16(pdtaList, 0); // bank 0
    writeLE16(pdtaList, 0); // pbagNdx 0
    writeLE32(pdtaList, 0); writeLE32(pdtaList, 0); writeLE32(pdtaList, 0);
    writeFixedString(pdtaList, "EOP", 20);
    writeLE16(pdtaList, 255); writeLE16(pdtaList, 255); writeLE16(pdtaList, 1);
    writeLE32(pdtaList, 0); writeLE32(pdtaList, 0); writeLE32(pdtaList, 0);

    // 2. pbag (2 records * 4 = 8 bytes)
    pdtaList.insert(pdtaList.end(), {'p', 'b', 'a', 'g'});
    writeLE32(pdtaList, 8);
    writeLE16(pdtaList, 0); writeLE16(pdtaList, 0);
    writeLE16(pdtaList, 1); writeLE16(pdtaList, 0);

    // 3. pmod (1 terminal record * 10 = 10 bytes)
    pdtaList.insert(pdtaList.end(), {'p', 'm', 'o', 'd'});
    writeLE32(pdtaList, 10);
    for (int i = 0; i < 10; ++i) pdtaList.push_back(0);

    // 4. pgen (2 records * 4 = 8 bytes: Gen 41 Instrument=0 + terminal)
    pdtaList.insert(pdtaList.end(), {'p', 'g', 'e', 'n'});
    writeLE32(pdtaList, 8);
    writeLE16(pdtaList, kGenInstrument); writeLE16(pdtaList, 0);
    writeLE16(pdtaList, 0); writeLE16(pdtaList, 0);

    // 5. inst (2 records * 22 = 44 bytes)
    pdtaList.insert(pdtaList.end(), {'i', 'n', 's', 't'});
    writeLE32(pdtaList, 44);
    writeFixedString(pdtaList, "GrandPianoInst", 20);
    writeLE16(pdtaList, 0); // ibagNdx 0
    writeFixedString(pdtaList, "EOI", 20);
    writeLE16(pdtaList, 2); // ibagNdx 2 (2 zones)

    // 6. ibag (3 records * 4 = 12 bytes: Zone 0, Zone 1, Terminal)
    pdtaList.insert(pdtaList.end(), {'i', 'b', 'a', 'g'});
    writeLE32(pdtaList, 12);
    writeLE16(pdtaList, 0);  writeLE16(pdtaList, 0); // Zone 0: igen 0..6
    writeLE16(pdtaList, 6);  writeLE16(pdtaList, 0); // Zone 1: igen 6..12
    writeLE16(pdtaList, 12); writeLE16(pdtaList, 0); // Terminal

    // 7. imod (1 terminal record * 10 = 10 bytes)
    pdtaList.insert(pdtaList.end(), {'i', 'm', 'o', 'd'});
    writeLE32(pdtaList, 10);
    for (int i = 0; i < 10; ++i) pdtaList.push_back(0);

    // 8. igen (13 records * 4 = 52 bytes)
    pdtaList.insert(pdtaList.end(), {'i', 'g', 'e', 'n'});
    writeLE32(pdtaList, 52);
    // Zone 0 (Soft layer: keys 21..108, vel 1..80, loop=1, attack=-7973 tc (~10ms), release=-2400 tc (~250ms), sampleID=0)
    writeLE16(pdtaList, kGenKeyRange);      writeLE16(pdtaList, static_cast<uint16_t>(21 | (108 << 8)));
    writeLE16(pdtaList, kGenVelRange);      writeLE16(pdtaList, static_cast<uint16_t>(1  | (80  << 8)));
    writeLE16(pdtaList, kGenSampleModes);   writeLE16(pdtaList, 1);
    writeLE16(pdtaList, kGenAttackVolEnv);  writeLE16(pdtaList, static_cast<uint16_t>(static_cast<int16_t>(-7973)));
    writeLE16(pdtaList, kGenReleaseVolEnv); writeLE16(pdtaList, static_cast<uint16_t>(static_cast<int16_t>(-2400)));
    writeLE16(pdtaList, kGenSampleID);      writeLE16(pdtaList, 0);

    // Zone 1 (Bright layer: keys 21..108, vel 81..127, loop=1, attack=-9173 tc (~5ms), release=-1200 tc (~500ms), sampleID=1)
    writeLE16(pdtaList, kGenKeyRange);      writeLE16(pdtaList, static_cast<uint16_t>(21 | (108 << 8)));
    writeLE16(pdtaList, kGenVelRange);      writeLE16(pdtaList, static_cast<uint16_t>(81 | (127 << 8)));
    writeLE16(pdtaList, kGenSampleModes);   writeLE16(pdtaList, 1);
    writeLE16(pdtaList, kGenAttackVolEnv);  writeLE16(pdtaList, static_cast<uint16_t>(static_cast<int16_t>(-9173)));
    writeLE16(pdtaList, kGenReleaseVolEnv); writeLE16(pdtaList, static_cast<uint16_t>(static_cast<int16_t>(-1200)));
    writeLE16(pdtaList, kGenSampleID);      writeLE16(pdtaList, 1);

    // Terminal igen
    writeLE16(pdtaList, 0); writeLE16(pdtaList, 0);

    // 9. shdr (3 records * 46 = 138 bytes: Sample 0, Sample 1, EOS)
    pdtaList.insert(pdtaList.end(), {'s', 'h', 'd', 'r'});
    writeLE32(pdtaList, 138);
    // Sample 0
    writeFixedString(pdtaList, "PianoSoft_C4", 20);
    writeLE32(pdtaList, 0);
    writeLE32(pdtaList, framesPerSample);
    writeLE32(pdtaList, 441);
    writeLE32(pdtaList, framesPerSample - 441);
    writeLE32(pdtaList, 44100);
    pdtaList.push_back(60); // C4
    pdtaList.push_back(0);
    writeLE16(pdtaList, 0);
    writeLE16(pdtaList, 1); // monoSample
    // Sample 1
    writeFixedString(pdtaList, "PianoBright_C4", 20);
    writeLE32(pdtaList, sample1Start);
    writeLE32(pdtaList, sample1Start + framesPerSample);
    writeLE32(pdtaList, sample1Start + 441);
    writeLE32(pdtaList, sample1Start + framesPerSample - 441);
    writeLE32(pdtaList, 44100);
    pdtaList.push_back(60); // C4
    pdtaList.push_back(0);
    writeLE16(pdtaList, 0);
    writeLE16(pdtaList, 1); // monoSample
    // EOS
    writeFixedString(pdtaList, "EOS", 20);
    writeLE32(pdtaList, 0); writeLE32(pdtaList, 0); writeLE32(pdtaList, 0); writeLE32(pdtaList, 0);
    writeLE32(pdtaList, 0); pdtaList.push_back(0); pdtaList.push_back(0);
    writeLE16(pdtaList, 0); writeLE16(pdtaList, 0);

    // Assemble top-level RIFF 'sfbk'
    const uint32_t riffPayloadSize = 4 +
                                     (8 + static_cast<uint32_t>(infoList.size())) +
                                     (8 + static_cast<uint32_t>(sdtaList.size())) +
                                     (8 + static_cast<uint32_t>(pdtaList.size()));

    std::vector<uint8_t> riff;
    riff.reserve(8 + riffPayloadSize);
    riff.insert(riff.end(), {'R', 'I', 'F', 'F'});
    writeLE32(riff, riffPayloadSize);
    riff.insert(riff.end(), {'s', 'f', 'b', 'k'});

    riff.insert(riff.end(), {'L', 'I', 'S', 'T'});
    writeLE32(riff, static_cast<uint32_t>(infoList.size()));
    riff.insert(riff.end(), infoList.begin(), infoList.end());

    riff.insert(riff.end(), {'L', 'I', 'S', 'T'});
    writeLE32(riff, static_cast<uint32_t>(sdtaList.size()));
    riff.insert(riff.end(), sdtaList.begin(), sdtaList.end());

    riff.insert(riff.end(), {'L', 'I', 'S', 'T'});
    writeLE32(riff, static_cast<uint32_t>(pdtaList.size()));
    riff.insert(riff.end(), pdtaList.begin(), pdtaList.end());

    std::ofstream f(filePath, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(riff.data()), static_cast<std::streamsize>(riff.size()));
    return f.good();
}

} // namespace konos
