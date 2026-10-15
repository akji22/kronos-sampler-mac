#include "KontaktFormatInspector.h"
#include "WavAiffFormatLoader.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <filesystem>
#include <algorithm>

namespace konos {

bool KontaktFormatInspector::canHandleExtension(const std::string& extLower) const {
    return extLower == ".nki" || extLower == ".nkm" || extLower == ".nkx" || extLower == ".ncw";
}

namespace {

std::string extractXmlAttribute(const std::string& tagText, const std::string& attrName) {
    const std::string pattern = attrName + "=\"";
    const auto pos = tagText.find(pattern);
    if (pos == std::string::npos) return "";
    const std::size_t valStart = pos + pattern.size();
    const auto endQuote = tagText.find('"', valStart);
    if (endQuote == std::string::npos) return "";
    return tagText.substr(valStart, endQuote - valStart);
}

} // namespace

KontaktInspectionReport KontaktFormatInspector::inspectFile(
    const std::string& filePath,
    std::shared_ptr<InstrumentPatch>* outOptionalPatch) {
    KontaktInspectionReport report;
    const std::filesystem::path fsPath(filePath);
    std::string ext = fsPath.extension().string();
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        report.summary = "Kontakt file not found: " + filePath;
        return report;
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize < 8) {
        report.summary = "File is smaller than the minimum 8-byte Kontakt header.";
        return report;
    }

    const std::size_t readLen = static_cast<std::size_t>(std::min<std::streamsize>(fileSize, 65536));
    std::vector<uint8_t> buf(readLen);
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(readLen));

    // 1. Check for .nkx Encrypted Monolith Container
    if (ext == ".nkx") {
        report.kind = KontaktContainerKind::EncryptedMonolithNkx;
        report.headerRecognized = true;
        report.isEncryptedDrm = true;
        report.usesProprietaryBinaryGraphOrCodec = true;
        report.hasVerifiedPlayableZones = false;
        report.containerFormatLabel = "Kontakt Encrypted Monolith (.nkx)";
        report.verifiedFeatures = {"Container header magic detection"};
        report.unsupportedFeatures = {
            "AES/proprietary DRM payload decryption (prohibited without official Native Access license)",
            "Embedded NCW sample extraction",
            "Locked KSP script resources"
        };
        report.summary =
            "Rejected .nkx container: Native Instruments NKX monoliths are DRM-encrypted archives "
            "governed by Native Access licensing. Circumventing encryption or DRM is prohibited.";
        return report;
    }

    // 2. Check for .ncw Native Compressed Wave
    if (ext == ".ncw" || (buf[0] == 0x01 && buf[1] == 0xA8 && buf[2] == 0x21 && buf[3] == 0x00)) {
        report.kind = KontaktContainerKind::ProprietaryCompressedNcw;
        report.headerRecognized = true;
        report.isEncryptedDrm = false;
        report.usesProprietaryBinaryGraphOrCodec = true;
        report.hasVerifiedPlayableZones = false;
        report.containerFormatLabel = "Native Instruments Compressed Wave (.ncw)";
        report.verifiedFeatures = {"NCW binary header signature recognition"};
        report.unsupportedFeatures = {
            "Proprietary delta-predictor lossless audio bitstream decoding (no public specification or NI SDK)"
        };
        report.summary =
            "Rejected .ncw audio sample: Native Compressed Wave (NCW) is an undocumented proprietary "
            "codec. Resave the instrument in Kontakt with 'Uncompressed Samples' (.wav) to load.";
        return report;
    }

    // 3. Check if the file is an unencrypted XML-structured NKI/NKM patch
    const std::string textView(reinterpret_cast<const char*>(buf.data()), buf.size());
    const bool hasXmlHeader = (textView.find("<?xml") != std::string::npos) ||
                              (textView.find("<K2_Container") != std::string::npos) ||
                              (textView.find("<Koncept") != std::string::npos);

    if (hasXmlHeader) {
        report.kind = KontaktContainerKind::UnencryptedXmlNkiNkm;
        report.headerRecognized = true;
        report.isEncryptedDrm = false;
        report.containerFormatLabel = "Unencrypted XML-Structured Kontakt Patch (" + ext + ")";
        report.verifiedFeatures = {
            "Unencrypted XML patch structure inspection",
            "External WAV/AIFF <Zone> key/velocity/rootNote/tune/volume extraction"
        };
        report.unsupportedFeatures = {
            "Kontakt Script Processor (KSP) bytecode & UI callbacks",
            "Proprietary Kontakt internal insert/send DSP effect chains",
            "NCW compressed sample references"
        };

        auto patch = std::make_shared<InstrumentPatch>();
        patch->name = fsPath.stem().string();
        patch->sourcePath = filePath;
        patch->formatName = "Kontakt XML (" + ext + ")";

        const std::filesystem::path baseDir = fsPath.parent_path();
        std::size_t searchPos = 0;
        bool referencedUnsupportedNcw = false;

        while ((searchPos = textView.find("<Zone", searchPos)) != std::string::npos) {
            const auto closeTag = textView.find('>', searchPos);
            if (closeTag == std::string::npos) break;
            const std::string zoneTag = textView.substr(searchPos, closeTag - searchPos + 1);
            searchPos = closeTag + 1;

            std::string fileAttr = extractXmlAttribute(zoneTag, "file");
            if (fileAttr.empty()) fileAttr = extractXmlAttribute(zoneTag, "sample");
            if (fileAttr.empty()) continue;

            std::replace(fileAttr.begin(), fileAttr.end(), '\\', '/');
            if (fileAttr.find(".ncw") != std::string::npos || fileAttr.find(".nkx") != std::string::npos) {
                referencedUnsupportedNcw = true;
                patch->warnings.push_back("Rejected proprietary sample reference in NKI zone: " + fileAttr);
                continue;
            }

            const std::filesystem::path fullSamplePath = (baseDir / fileAttr).lexically_normal();
            std::shared_ptr<SampleBuffer> sampleBuf;
            FormatLoadResult wavRes = WavAiffFormatLoader::decodeSampleFile(fullSamplePath.string(), sampleBuf);
            if (!wavRes.succeeded || !sampleBuf) {
                patch->warnings.push_back("Missing or unreadable external sample referenced by NKI: " + fullSamplePath.string());
                continue;
            }

            InstrumentZone zone;
            zone.name   = fullSamplePath.filename().string();
            zone.sample = sampleBuf;
            zone.endFrame = sampleBuf->getNumFrames();

            const std::string loKeyStr  = extractXmlAttribute(zoneTag, "loKey");
            const std::string hiKeyStr  = extractXmlAttribute(zoneTag, "hiKey");
            const std::string rootStr   = extractXmlAttribute(zoneTag, "rootKey");
            const std::string loVelStr  = extractXmlAttribute(zoneTag, "loVel");
            const std::string hiVelStr  = extractXmlAttribute(zoneTag, "hiVel");
            const std::string tuneStr   = extractXmlAttribute(zoneTag, "tune");
            const std::string volStr    = extractXmlAttribute(zoneTag, "volume");
            const std::string loopStr   = extractXmlAttribute(zoneTag, "loopMode");
            const std::string lStartStr = extractXmlAttribute(zoneTag, "loopStart");
            const std::string lEndStr   = extractXmlAttribute(zoneTag, "loopEnd");

            if (!loKeyStr.empty()) zone.loKey   = static_cast<uint8_t>(std::clamp(std::stoi(loKeyStr), 0, 127));
            if (!hiKeyStr.empty()) zone.hiKey   = static_cast<uint8_t>(std::clamp(std::stoi(hiKeyStr), 0, 127));
            if (!rootStr.empty())  zone.rootKey = static_cast<uint8_t>(std::clamp(std::stoi(rootStr),  0, 127));
            else                   zone.rootKey = sampleBuf->getRootMidiNote();
            if (!loVelStr.empty()) zone.loVel   = static_cast<uint8_t>(std::clamp(std::stoi(loVelStr), 1, 127));
            if (!hiVelStr.empty()) zone.hiVel   = static_cast<uint8_t>(std::clamp(std::stoi(hiVelStr), 1, 127));
            if (!tuneStr.empty())  zone.tuneCents = std::stof(tuneStr);
            if (!volStr.empty())   zone.volumeDb  = std::stof(volStr);
            if (loopStr == "1" || loopStr == "loop_continuous") {
                zone.loopMode = LoopMode::ForwardLoop;
                zone.loopStartFrame = !lStartStr.empty() ? static_cast<uint64_t>(std::stoull(lStartStr)) : sampleBuf->getLoopStartFrame();
                zone.loopEndFrame   = !lEndStr.empty()   ? static_cast<uint64_t>(std::stoull(lEndStr))   : sampleBuf->getLoopEndFrame();
            }

            patch->zones.push_back(std::move(zone));
        }

        // Requirement 6: Do NOT falsely report a successfully loaded instrument when only metadata or filenames are recognized
        if (patch->zones.empty()) {
            report.hasVerifiedPlayableZones = false;
            report.usesProprietaryBinaryGraphOrCodec = referencedUnsupportedNcw;
            report.summary = referencedUnsupportedNcw
                ? "Unencrypted Kontakt XML metadata recognized, but zones reference proprietary .ncw/.nkx samples that cannot be decoded without an official NI SDK."
                : "Kontakt XML metadata recognized, but zero playable external WAV/AIFF zones were verified on disk. Refusing to report a false positive load.";
            return report;
        }

        report.hasVerifiedPlayableZones = true;
        report.summary = "Loaded unencrypted XML Kontakt patch with " +
                         std::to_string(patch->zones.size()) +
                         " verified external WAV/AIFF zone(s) (KSP scripts and proprietary NI effects are not executed).";
        if (outOptionalPatch) {
            *outOptionalPatch = std::move(patch);
        }
        return report;
    }

    // 4. Modern Binary Compressed .nki / .nkm (Kontakt 4.2.2 through Kontakt 8)
    report.kind = (ext == ".nkm")
                      ? KontaktContainerKind::CompressedProprietaryNkm
                      : KontaktContainerKind::CompressedProprietaryNki;
    report.headerRecognized = true;
    report.isEncryptedDrm = false;
    report.usesProprietaryBinaryGraphOrCodec = true;
    report.hasVerifiedPlayableZones = false;
    report.containerFormatLabel = "Proprietary Binary Kontakt " + std::string(ext == ".nkm" ? "Multi (.nkm)" : "Instrument (.nki)");
    report.verifiedFeatures = {
        "Binary container magic-byte identification (0x" +
        std::to_string(buf[0]) + " 0x" + std::to_string(buf[1]) + ")"
    };
    report.unsupportedFeatures = {
        "Undocumented zlib/zstd serialized C++ patch graph (Kontakt 4.2.2 - Kontakt 8)",
        "Compiled Kontakt Script Processor (KSP) bytecode",
        "Proprietary Kontakt internal DSP effect routing and monolith tables"
    };
    report.summary =
        "Rejected binary " + ext + " file: Native Instruments does not publish an open specification or "
        "third-party decoding SDK for binary NKI/NKM patch graphs. Export samples as uncompressed WAV or convert to SFZ/SF2.";
    return report;
}

FormatLoadResult KontaktFormatInspector::loadFromFile(const std::string& filePath) const {
    FormatLoadResult res;
    std::shared_ptr<InstrumentPatch> verifiedPatch;
    const KontaktInspectionReport rep = inspectFile(filePath, &verifiedPatch);

    res.formatIdentifier = rep.containerFormatLabel.empty() ? "Kontakt" : rep.containerFormatLabel;
    res.diagnosticMessage = rep.summary;

    if (!rep.headerRecognized) {
        res.succeeded = false;
        res.status = FormatSupportStatus::CorruptedOrInvalid;
        return res;
    }

    if (rep.isEncryptedDrm) {
        res.succeeded = false;
        res.status = FormatSupportStatus::UnsupportedEncryptedDrm;
        return res;
    }

    if (rep.hasVerifiedPlayableZones && verifiedPatch && !verifiedPatch->zones.empty()) {
        res.succeeded = true;
        res.status = FormatSupportStatus::PartiallySupported;
        res.warnings = verifiedPatch->warnings;
        res.warnings.push_back("Note: KSP scripts and proprietary Kontakt DSP effects are not executed.");
        res.patch = std::move(verifiedPatch);
        return res;
    }

    res.succeeded = false;
    res.status = FormatSupportStatus::UnsupportedProprietary;
    return res;
}

} // namespace konos
