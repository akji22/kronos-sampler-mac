#include "SfzFormatLoader.h"
#include "WavAiffFormatLoader.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>
#include <cctype>
#include <algorithm>
#include <cmath>

namespace konos {

int SfzFormatLoader::parseMidiNoteToken(const std::string& val) noexcept {
    if (val.empty()) return 60;
    try {
        if (std::isdigit(static_cast<unsigned char>(val[0])) || val[0] == '-') {
            return std::clamp(std::stoi(val), 0, 127);
        }
        static const std::unordered_map<char, int> kBaseNotes = {
            {'c', 0}, {'d', 2}, {'e', 4}, {'f', 5}, {'g', 7}, {'a', 9}, {'b', 11}
        };
        const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(val[0])));
        auto it = kBaseNotes.find(letter);
        if (it == kBaseNotes.end()) return 60;

        int semitone = it->second;
        std::size_t idx = 1;
        if (idx < val.size() && (val[idx] == '#' || val[idx] == 'b' || val[idx] == 'B')) {
            semitone += (val[idx] == '#') ? 1 : -1;
            ++idx;
        }
        int octave = 4;
        if (idx < val.size()) {
            octave = std::stoi(val.substr(idx));
        }
        return std::clamp((octave + 1) * 12 + semitone, 0, 127);
    } catch (...) {
        return 60;
    }
}

namespace {

std::string trimWhitespace(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string stripComments(const std::string& rawText) {
    std::string out;
    out.reserve(rawText.size());
    std::size_t i = 0;
    const std::size_t n = rawText.size();

    while (i < n) {
        if (i + 1 < n && rawText[i] == '/' && rawText[i + 1] == '/') {
            // Skip to end of line
            i += 2;
            while (i < n && rawText[i] != '\n') ++i;
        } else if (i + 1 < n && rawText[i] == '/' && rawText[i + 1] == '*') {
            // Skip block comment
            i += 2;
            while (i + 1 < n && !(rawText[i] == '*' && rawText[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
        } else {
            out.push_back(rawText[i]);
            ++i;
        }
    }
    return out;
}

void applyMacroDefines(std::string& line, const std::unordered_map<std::string, std::string>& macros) {
    for (const auto& [macroKey, macroVal] : macros) {
        std::size_t pos = 0;
        while ((pos = line.find(macroKey, pos)) != std::string::npos) {
            line.replace(pos, macroKey.size(), macroVal);
            pos += macroVal.size();
        }
    }
}

/**
 * Tokenizes a single line of SFZ content into <headers> and key=value pairs,
 * preserving spaces inside sample=... or default_path=... values up to the next opcode= token.
 */
struct SfzToken {
    bool        isHeader{false};
    std::string headerName{};
    std::string key{};
    std::string value{};
};

std::vector<SfzToken> tokenizeSfzLine(const std::string& line) {
    std::vector<SfzToken> tokens;
    std::size_t i = 0;
    const std::size_t n = line.size();

    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= n) break;

        if (line[i] == '<') {
            const auto closePos = line.find('>', i + 1);
            if (closePos != std::string::npos) {
                SfzToken tok;
                tok.isHeader = true;
                tok.headerName = line.substr(i + 1, closePos - i - 1);
                tokens.push_back(std::move(tok));
                i = closePos + 1;
                continue;
            }
        }

        // Find key=value
        const auto eqPos = line.find('=', i);
        if (eqPos == std::string::npos) break;

        std::string key = trimWhitespace(line.substr(i, eqPos - i));
        std::size_t valStart = eqPos + 1;
        std::string value;

        if (valStart < n && line[valStart] == '"') {
            const auto endQuote = line.find('"', valStart + 1);
            if (endQuote != std::string::npos) {
                value = line.substr(valStart + 1, endQuote - valStart - 1);
                i = endQuote + 1;
            } else {
                value = line.substr(valStart + 1);
                i = n;
            }
        } else if (key == "sample" || key == "default_path") {
            // Sample paths in SFZ may contain unquoted spaces until the next <header> or key=opcode
            std::size_t scan = valStart;
            std::size_t valueEnd = n;
            while (scan < n) {
                if (line[scan] == '<') {
                    valueEnd = scan;
                    break;
                }
                if (std::isspace(static_cast<unsigned char>(line[scan]))) {
                    std::size_t nextWord = scan;
                    while (nextWord < n && std::isspace(static_cast<unsigned char>(line[nextWord]))) ++nextWord;
                    // Check if the next token is another opcode (word followed by '=' before any space)
                    std::size_t kEnd = nextWord;
                    while (kEnd < n && (std::isalnum(static_cast<unsigned char>(line[kEnd])) || line[kEnd] == '_')) ++kEnd;
                    if (kEnd > nextWord && kEnd < n && line[kEnd] == '=') {
                        valueEnd = scan;
                        break;
                    }
                    if (nextWord < n && line[nextWord] == '<') {
                        valueEnd = scan;
                        break;
                    }
                }
                ++scan;
            }
            value = trimWhitespace(line.substr(valStart, valueEnd - valStart));
            i = valueEnd;
        } else {
            std::size_t valEnd = valStart;
            while (valEnd < n && !std::isspace(static_cast<unsigned char>(line[valEnd])) && line[valEnd] != '<') {
                ++valEnd;
            }
            value = line.substr(valStart, valEnd - valStart);
            i = valEnd;
        }

        if (!key.empty()) {
            SfzToken tok;
            tok.isHeader = false;
            tok.key = std::move(key);
            tok.value = std::move(value);
            tokens.push_back(std::move(tok));
        }
    }
    return tokens;
}

void applySfzOpcodesToZone(const std::unordered_map<std::string, std::string>& opcodes,
                           InstrumentZone& zone,
                           std::string& outSampleRelPath,
                           std::vector<std::string>& warnings) {
    bool explicitLoopPoints = false;
    bool explicitLoopMode   = false;

    for (const auto& [key, val] : opcodes) {
        try {
            if (key == "sample") {
                outSampleRelPath = val;
            } else if (key == "lokey") {
                zone.loKey = static_cast<uint8_t>(SfzFormatLoader::parseMidiNoteToken(val));
            } else if (key == "hikey") {
                zone.hiKey = static_cast<uint8_t>(SfzFormatLoader::parseMidiNoteToken(val));
            } else if (key == "pitch_keycenter") {
                zone.rootKey = static_cast<uint8_t>(SfzFormatLoader::parseMidiNoteToken(val));
            } else if (key == "key") {
                const uint8_t k = static_cast<uint8_t>(SfzFormatLoader::parseMidiNoteToken(val));
                zone.loKey = k;
                zone.hiKey = k;
                zone.rootKey = k;
            } else if (key == "lovel") {
                zone.loVel = static_cast<uint8_t>(std::clamp(std::stoi(val), 0, 127));
            } else if (key == "hivel") {
                zone.hiVel = static_cast<uint8_t>(std::clamp(std::stoi(val), 1, 127));
            } else if (key == "seq_length") {
                zone.seqLength = static_cast<uint32_t>(std::max(1, std::stoi(val)));
            } else if (key == "seq_position") {
                zone.seqPosition = static_cast<uint32_t>(std::max(1, std::stoi(val)));
            } else if (key == "transpose") {
                zone.transposeSemitones = std::clamp(std::stoi(val), -48, 48);
            } else if (key == "tune") {
                zone.tuneCents = std::clamp(std::stof(val), -1200.0f, 1200.0f);
            } else if (key == "volume") {
                zone.volumeDb = std::clamp(std::stof(val), -144.0f, 24.0f);
            } else if (key == "amplitude") {
                const float ampPct = std::clamp(std::stof(val), 0.0f, 400.0f);
                zone.volumeDb = (ampPct <= 0.0001f) ? -120.0f : 20.0f * std::log10(ampPct * 0.01f);
            } else if (key == "pan") {
                zone.pan = std::clamp(std::stof(val) * 0.01f, -1.0f, 1.0f);
            } else if (key == "offset") {
                zone.offsetFrame = static_cast<uint64_t>(std::stoull(val));
            } else if (key == "end") {
                zone.endFrame = static_cast<uint64_t>(std::stoull(val));
            } else if (key == "loop_mode" || key == "loopmode") {
                explicitLoopMode = true;
                if (val == "loop_continuous" || val == "loop_sustain") {
                    zone.loopMode = LoopMode::ForwardLoop;
                } else if (val == "one_shot") {
                    zone.loopMode = LoopMode::OneShot;
                } else {
                    zone.loopMode = LoopMode::NoLoop;
                }
            } else if (key == "loop_start" || key == "loopstart") {
                zone.loopStartFrame = static_cast<uint64_t>(std::stoull(val));
                explicitLoopPoints = true;
            } else if (key == "loop_end" || key == "loopend") {
                zone.loopEndFrame = static_cast<uint64_t>(std::stoull(val));
                explicitLoopPoints = true;
            } else if (key == "ampeg_attack" || key == "amp_attack") {
                zone.attackMsOverride = std::max(0.5f, std::stof(val) * 1000.0f);
            } else if (key == "ampeg_decay" || key == "amp_decay") {
                zone.decayMsOverride = std::max(1.0f, std::stof(val) * 1000.0f);
            } else if (key == "ampeg_sustain" || key == "amp_sustain") {
                zone.sustainLevelOverride = std::clamp(std::stof(val) * 0.01f, 0.0f, 1.0f);
            } else if (key == "ampeg_release" || key == "amp_release") {
                zone.releaseMsOverride = std::max(1.0f, std::stof(val) * 1000.0f);
            } else if (key == "ampeg_hold" || key == "amp_hold" || key == "ampeg_delay") {
                // Fold short hold into decay time if specified
                const float holdMs = std::max(0.0f, std::stof(val) * 1000.0f);
                if (zone.decayMsOverride > 0.0f) zone.decayMsOverride += holdMs;
            } else if (key == "cutoff") {
                zone.cutoffHzOverride = std::clamp(std::stof(val), 20.0f, 20000.0f);
            } else if (key == "resonance") {
                zone.resonanceOverride = std::clamp(std::pow(10.0f, std::stof(val) * 0.05f), 0.5f, 12.0f);
            } else if (key == "group" || key == "off_by" || key == "pitch_keytrack" ||
                       key == "amp_veltrack" || key == "fil_type" || key == "fil_veltrack") {
                // Recognized performance opcodes accepted without warning
            } else {
                warnings.push_back("Unsupported SFZ opcode ignored: '" + key + "=" + val + "'");
            }
        } catch (...) {
            warnings.push_back("Malformed value for SFZ opcode: '" + key + "=" + val + "'");
        }
    }

    if (explicitLoopPoints && !explicitLoopMode && zone.loopEndFrame > zone.loopStartFrame) {
        zone.loopMode = LoopMode::ForwardLoop;
    }
}

} // namespace

FormatLoadResult SfzFormatLoader::loadFromFile(const std::string& filePath) const {
    FormatLoadResult res;
    res.formatIdentifier = "SFZ";

    std::ifstream file(filePath);
    if (!file.is_open()) {
        res.status = FormatSupportStatus::FileNotFound;
        res.diagnosticMessage = "Unable to open SFZ file: " + filePath;
        return res;
    }

    std::ostringstream rawBuffer;
    rawBuffer << file.rdbuf();
    const std::string cleanedText = stripComments(rawBuffer.str());

    auto patch = std::make_shared<InstrumentPatch>();
    patch->name = std::filesystem::path(filePath).stem().string();
    patch->sourcePath = filePath;
    patch->formatName = "SFZ";

    const std::filesystem::path baseDir = std::filesystem::path(filePath).parent_path();
    std::string defaultPathPrefix;

    std::unordered_map<std::string, std::string> macros;
    std::unordered_map<std::string, std::string> globalOpcodes;
    std::unordered_map<std::string, std::string> masterOpcodes;
    std::unordered_map<std::string, std::string> groupOpcodes;
    std::unordered_map<std::string, std::string> regionOpcodes;
    std::unordered_map<std::string, std::shared_ptr<SampleBuffer>> sampleCache;

    std::string currentSection = "global";

    auto flushRegion = [&]() {
        if (regionOpcodes.empty()) return;

        // Inheritance hierarchy: <global> -> <master> -> <group> -> <region>
        std::unordered_map<std::string, std::string> merged = globalOpcodes;
        for (const auto& [k, v] : masterOpcodes) merged[k] = v;
        for (const auto& [k, v] : groupOpcodes)  merged[k] = v;
        for (const auto& [k, v] : regionOpcodes) merged[k] = v;
        regionOpcodes.clear();

        InstrumentZone zone;
        std::string sampleRelPath;
        applySfzOpcodesToZone(merged, zone, sampleRelPath, res.warnings);
        if (sampleRelPath.empty()) {
            res.warnings.push_back("SFZ <region> is missing a 'sample=' opcode.");
            return;
        }

        std::string combinedRel = defaultPathPrefix + sampleRelPath;
        std::replace(combinedRel.begin(), combinedRel.end(), '\\', '/');
        const std::filesystem::path fullSamplePath = (baseDir / combinedRel).lexically_normal();
        const std::string canonicalKey = fullSamplePath.string();

        std::shared_ptr<SampleBuffer> decodedSample;
        auto cacheIt = sampleCache.find(canonicalKey);
        if (cacheIt != sampleCache.end()) {
            decodedSample = cacheIt->second;
        } else {
            FormatLoadResult sampleRes = WavAiffFormatLoader::decodeSampleFile(canonicalKey, decodedSample);
            if (!sampleRes.succeeded || !decodedSample) {
                res.warnings.push_back("Missing or unreadable SFZ region sample: " + canonicalKey);
                return;
            }
            sampleCache[canonicalKey] = decodedSample;
        }

        zone.name = fullSamplePath.filename().string();
        zone.sample = decodedSample;

        // Inherit embedded WAV loop points if loop_mode is active but explicit loop_end was omitted
        if (zone.loopMode == LoopMode::ForwardLoop && zone.loopEndFrame == 0 && decodedSample->hasEmbeddedLoop()) {
            zone.loopStartFrame = decodedSample->getLoopStartFrame();
            zone.loopEndFrame   = decodedSample->getLoopEndFrame();
        }
        if (zone.endFrame == 0) {
            zone.endFrame = decodedSample->getNumFrames();
        }

        patch->zones.push_back(std::move(zone));
    };

    std::istringstream lineStream(cleanedText);
    std::string line;
    while (std::getline(lineStream, line)) {
        std::string trimmed = trimWhitespace(line);
        if (trimmed.empty()) continue;

        // Handle #define $VAR value
        if (trimmed.rfind("#define", 0) == 0) {
            std::istringstream dss(trimmed);
            std::string directive, macroName, macroVal;
            dss >> directive >> macroName >> macroVal;
            if (!macroName.empty()) {
                macros[macroName] = macroVal;
            }
            continue;
        }

        applyMacroDefines(trimmed, macros);

        const auto tokens = tokenizeSfzLine(trimmed);
        for (const auto& tok : tokens) {
            if (tok.isHeader) {
                flushRegion();
                currentSection = tok.headerName;
                if (currentSection == "global") {
                    globalOpcodes.clear();
                    masterOpcodes.clear();
                    groupOpcodes.clear();
                } else if (currentSection == "master") {
                    masterOpcodes.clear();
                    groupOpcodes.clear();
                } else if (currentSection == "group") {
                    groupOpcodes.clear();
                }
            } else {
                if (currentSection == "control") {
                    if (tok.key == "default_path") {
                        defaultPathPrefix = tok.value;
                        std::replace(defaultPathPrefix.begin(), defaultPathPrefix.end(), '\\', '/');
                        if (!defaultPathPrefix.empty() && defaultPathPrefix.back() != '/') {
                            defaultPathPrefix.push_back('/');
                        }
                    }
                } else if (currentSection == "global") {
                    globalOpcodes[tok.key] = tok.value;
                } else if (currentSection == "master") {
                    masterOpcodes[tok.key] = tok.value;
                } else if (currentSection == "group") {
                    groupOpcodes[tok.key] = tok.value;
                } else {
                    regionOpcodes[tok.key] = tok.value;
                }
            }
        }
    }
    flushRegion();

    if (patch->zones.empty()) {
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        res.diagnosticMessage = "SFZ file contained no valid playable <region> zones.";
        return res;
    }

    patch->warnings = res.warnings;
    res.succeeded = true;
    res.status = res.warnings.empty() ? FormatSupportStatus::FullySupported : FormatSupportStatus::PartiallySupported;
    res.diagnosticMessage = "Loaded SFZ ('" + patch->name + "') with " +
                            std::to_string(patch->zones.size()) + " zone(s) and " +
                            std::to_string(sampleCache.size()) + " unique sample(s).";
    res.patch = std::move(patch);
    return res;
}

} // namespace konos
