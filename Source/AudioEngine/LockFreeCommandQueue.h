#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include "../Instruments/InstrumentPatch.h"

namespace konos {

enum class EngineCommandType : uint8_t {
    SetInstrumentPatch = 0,
    AllNotesOff        = 1,
    ResetEngine        = 2,
    TriggerPreviewNote = 3
};

struct EngineCommand {
    EngineCommandType type{EngineCommandType::AllNotesOff};
    std::size_t       slotIndex{0};
    uint8_t           previewNote{60};
    uint8_t           previewVelocity{100};
    std::shared_ptr<const InstrumentPatch> patch{};
};

template <typename T, std::size_t Capacity = 64>
class LockFreeSpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

public:
    bool push(T item) noexcept {
        const std::size_t writeIdx = writeIndex_.load(std::memory_order_relaxed);
        const std::size_t nextIdx  = (writeIdx + 1) & (Capacity - 1);
        if (nextIdx == readIndex_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[writeIdx] = std::move(item);
        writeIndex_.store(nextIdx, std::memory_order_release);
        return true;
    }

    bool pop(T& outItem) noexcept {
        const std::size_t readIdx = readIndex_.load(std::memory_order_relaxed);
        if (readIdx == writeIndex_.load(std::memory_order_acquire)) {
            return false;
        }
        outItem = std::move(buffer_[readIdx]);
        buffer_[readIdx] = T{};
        readIndex_.store((readIdx + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<std::size_t> writeIndex_{0};
    alignas(64) std::atomic<std::size_t> readIndex_{0};
};

} // namespace konos
