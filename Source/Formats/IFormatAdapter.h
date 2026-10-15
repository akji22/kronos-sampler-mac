#pragma once

#include <memory>
#include <string>
#include <vector>
#include "../Instruments/InstrumentPatch.h"

namespace konos {

enum class FormatSupportStatus : uint8_t {
    FullySupported          = 0,
    PartiallySupported      = 1,
    UnsupportedProprietary  = 2,
    UnsupportedEncryptedDrm = 3,
    CorruptedOrInvalid      = 4,
    FileNotFound            = 5
};

struct FormatLoadResult {
    bool                succeeded{false};
    FormatSupportStatus status{FormatSupportStatus::CorruptedOrInvalid};
    std::string         formatIdentifier{};
    std::string         diagnosticMessage{};
    std::vector<std::string> warnings{};
    std::shared_ptr<InstrumentPatch> patch{};
};

/**
 * Abstract base interface for all sample and instrument format loaders.
 */
class IFormatAdapter {
public:
    virtual ~IFormatAdapter() = default;
    virtual std::string getFormatName() const = 0;
    virtual bool canHandleExtension(const std::string& extLower) const = 0;
    virtual FormatLoadResult loadFromFile(const std::string& filePath) const = 0;
};

} // namespace konos
