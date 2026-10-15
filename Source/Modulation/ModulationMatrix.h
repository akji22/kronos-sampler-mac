#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace konos {

enum class ModSource : int {
    None        = 0,
    Lfo1        = 1,
    ModWheelCC1 = 2,
    Velocity    = 3,
    KeyTrack    = 4,
    PitchBend   = 5,
    AmpEnvelope = 6,
    kCount      = 7
};

enum class ModDestination : int {
    None                = 0,
    PitchSemitones      = 1, // +/- 12 semitones at amount = +/-1.0
    FilterCutoffOctaves = 2, // +/- 4 octaves at amount = +/-1.0
    VolumeGain          = 3, // +/- 1.0 linear gain offset
    PanOffset           = 4, // +/- 1.0 stereo pan offset
    Lfo1RateHz          = 5, // +/- 10 Hz LFO speed modulation
    kCount              = 6
};

enum class LfoWaveform : int {
    Sine     = 0,
    Triangle = 1,
    Saw      = 2,
    Square   = 3
};

struct ModulationSlot {
    std::atomic<int>   source{static_cast<int>(ModSource::None)};
    std::atomic<int>   destination{static_cast<int>(ModDestination::None)};
    std::atomic<float> amount{0.0f}; // [-1.0, +1.0]
};

struct ModulatedOffsets {
    float pitchSemitones{0.0f};
    float filterCutoffOctaves{0.0f};
    float volumeScale{1.0f};
    float panOffset{0.0f};
    float lfo1RateDeltaHz{0.0f};
};

/**
 * Lock-free 4-slot Modulation Matrix and LFO 1 generator.
 * Evaluates all active source->destination routings per voice without locks.
 */
class ModulationMatrix {
public:
    static constexpr std::size_t kNumSlots = 4;

    ModulationMatrix() {
        // Default musical modulation routing:
        // Slot 0: ModWheel (CC1) -> Pitch (Vibrato via LFO1 or direct)
        slots_[0].source.store(static_cast<int>(ModSource::Lfo1), std::memory_order_relaxed);
        slots_[0].destination.store(static_cast<int>(ModDestination::PitchSemitones), std::memory_order_relaxed);
        slots_[0].amount.store(0.0f, std::memory_order_relaxed);

        // Slot 1: Velocity -> Filter Cutoff (+1.5 octaves)
        slots_[1].source.store(static_cast<int>(ModSource::Velocity), std::memory_order_relaxed);
        slots_[1].destination.store(static_cast<int>(ModDestination::FilterCutoffOctaves), std::memory_order_relaxed);
        slots_[1].amount.store(0.375f, std::memory_order_relaxed); // 0.375 * 4 = 1.5 octaves

        // Slot 2: KeyTrack -> Filter Cutoff (+0.5 octaves)
        slots_[2].source.store(static_cast<int>(ModSource::KeyTrack), std::memory_order_relaxed);
        slots_[2].destination.store(static_cast<int>(ModDestination::FilterCutoffOctaves), std::memory_order_relaxed);
        slots_[2].amount.store(0.15f, std::memory_order_relaxed);

        // Slot 3: AmpEnvelope -> Filter Cutoff
        slots_[3].source.store(static_cast<int>(ModSource::AmpEnvelope), std::memory_order_relaxed);
        slots_[3].destination.store(static_cast<int>(ModDestination::FilterCutoffOctaves), std::memory_order_relaxed);
        slots_[3].amount.store(0.0f, std::memory_order_relaxed);
    }

    void setSlot(std::size_t idx, ModSource src, ModDestination dst, float amount) noexcept {
        if (idx >= kNumSlots) return;
        slots_[idx].source.store(static_cast<int>(src), std::memory_order_relaxed);
        slots_[idx].destination.store(static_cast<int>(dst), std::memory_order_relaxed);
        slots_[idx].amount.store(std::clamp(amount, -1.0f, 1.0f), std::memory_order_relaxed);
    }

    const ModulationSlot& getSlot(std::size_t idx) const noexcept {
        return slots_[std::min(idx, kNumSlots - 1)];
    }
    ModulationSlot& getSlot(std::size_t idx) noexcept {
        return slots_[std::min(idx, kNumSlots - 1)];
    }

    std::atomic<float> lfo1RateHz{5.5f}; // [0.1, 20.0] Hz
    std::atomic<int>   lfo1Waveform{static_cast<int>(LfoWaveform::Sine)};

    static float computeLfoSample(double phase0to1, LfoWaveform wf) noexcept {
        const double p = phase0to1 - std::floor(phase0to1);
        switch (wf) {
            case LfoWaveform::Sine:
                return static_cast<float>(std::sin(p * 6.283185307179586));
            case LfoWaveform::Triangle:
                return static_cast<float>(1.0 - 4.0 * std::abs(std::round(p - 0.25) - (p - 0.25)));
            case LfoWaveform::Saw:
                return static_cast<float>(2.0 * p - 1.0);
            case LfoWaveform::Square:
                return p < 0.5 ? 1.0f : -1.0f;
        }
        return 0.0f;
    }

    ModulatedOffsets evaluate(float lfo1Val,
                              float modWheelNorm,
                              float velocityNorm,
                              float keyTrackBipolar,
                              float pitchBendBipolar,
                              float ampEnvVal) const noexcept {
        ModulatedOffsets out{};
        for (std::size_t i = 0; i < kNumSlots; ++i) {
            const auto src = static_cast<ModSource>(slots_[i].source.load(std::memory_order_relaxed));
            const auto dst = static_cast<ModDestination>(slots_[i].destination.load(std::memory_order_relaxed));
            const float amt = slots_[i].amount.load(std::memory_order_relaxed);
            if (src == ModSource::None || dst == ModDestination::None || std::abs(amt) < 1.0e-5f) {
                continue;
            }

            float srcSignal = 0.0f;
            switch (src) {
                case ModSource::Lfo1:        srcSignal = lfo1Val; break;
                case ModSource::ModWheelCC1: srcSignal = modWheelNorm; break;
                case ModSource::Velocity:    srcSignal = (velocityNorm - 0.5f) * 2.0f; break;
                case ModSource::KeyTrack:    srcSignal = keyTrackBipolar; break;
                case ModSource::PitchBend:   srcSignal = pitchBendBipolar; break;
                case ModSource::AmpEnvelope: srcSignal = ampEnvVal; break;
                default: break;
            }

            const float modVal = srcSignal * amt;
            switch (dst) {
                case ModDestination::PitchSemitones:
                    out.pitchSemitones += modVal * 12.0f;
                    break;
                case ModDestination::FilterCutoffOctaves:
                    out.filterCutoffOctaves += modVal * 4.0f;
                    break;
                case ModDestination::VolumeGain:
                    out.volumeScale = std::clamp(out.volumeScale + modVal, 0.0f, 2.0f);
                    break;
                case ModDestination::PanOffset:
                    out.panOffset = std::clamp(out.panOffset + modVal, -1.0f, 1.0f);
                    break;
                case ModDestination::Lfo1RateHz:
                    out.lfo1RateDeltaHz += modVal * 10.0f;
                    break;
                default:
                    break;
            }
        }
        return out;
    }

private:
    std::array<ModulationSlot, kNumSlots> slots_{};
};

} // namespace konos
