#pragma once

#include "IFormatAdapter.h"
#include <cstdint>
#include <string>
#include <vector>

namespace konos {

enum class KontaktContainerKind : uint8_t {
    UnknownOrCorrupted         = 0,
    UnencryptedXmlNkiNkm       = 1, // Open/legacy XML patch with external WAV/AIFF zone mappings
    CompressedProprietaryNki   = 2, // Kontakt 4.2.2 - Kontakt 8 binary/zlib/zstd patch graph
    CompressedProprietaryNkm   = 3, // Kontakt binary multi-instrument bank
    EncryptedMonolithNkx       = 4, // DRM-encrypted commercial Kontakt Player container
    ProprietaryCompressedNcw   = 5  // Native Compressed Wave delta-predictor audio file
};

struct KontaktInspectionReport {
    KontaktContainerKind kind{KontaktContainerKind::UnknownOrCorrupted};
    bool                 headerRecognized{false};
    bool                 isEncryptedDrm{false};
    bool                 usesProprietaryBinaryGraphOrCodec{false};
    bool                 hasVerifiedPlayableZones{false};
    std::string          containerFormatLabel{};
    std::string          summary{};
    std::vector<std::string> verifiedFeatures{};
    std::vector<std::string> unsupportedFeatures{};
};

/**
 * Native Kontakt Format Inspector & Lawful Interoperability Adapter (.nki, .nkm, .nkx, .ncw).
 *
 * Strictly enforces legal and engineering requirements:
 * 1. Never assumes that recognizing a file extension or header means its contents can be decoded.
 * 2. Does NOT bypass encryption, DRM, library activation, or Native Access licensing (.nkx).
 * 3. Does NOT falsely report a loaded instrument when only metadata or filenames are recognized.
 * 4. Decodes unencrypted XML-structured NKI patches ONLY when explicit <Zone> mappings and
 *    readable external WAV/AIFF sample files are verified on disk; rejects proprietary binary
 *    NKI/NKM patch graphs, KSP bytecode, .ncw compressed samples, and DRM-protected .nkx containers
 *    with a structured KontaktInspectionReport.
 */
class KontaktFormatInspector final : public IFormatAdapter {
public:
    std::string getFormatName() const override { return "Kontakt Format Inspector & Adapter"; }
    bool canHandleExtension(const std::string& extLower) const override;
    FormatLoadResult loadFromFile(const std::string& filePath) const override;

    static KontaktInspectionReport inspectFile(const std::string& filePath,
                                               std::shared_ptr<InstrumentPatch>* outOptionalPatch = nullptr);
};

} // namespace konos
