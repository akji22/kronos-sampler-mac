#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <string>
#include "../AudioEngine/SamplerEngine.h"
#include "../Browser/LibraryBrowserModel.h"

namespace konos {

class KonosPluginProcessor final : public juce::AudioProcessor {
public:
    KonosPluginProcessor();
    ~KonosPluginProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    void loadInstrumentFileAsync(const juce::File& file);
    void loadInstrumentFileToSlotAsync(const juce::File& file, std::size_t slotIndex);
    void loadLibraryItemToSlotAsync(const LibraryItem& item, std::size_t slotIndex);

    juce::AudioProcessorValueTreeState& getApvts() noexcept { return apvts_; }
    juce::MidiKeyboardState& getKeyboardState() noexcept { return keyboardState_; }
    SamplerEngine& getSamplerEngine() noexcept { return engine_; }
    const SamplerEngine& getSamplerEngine() const noexcept { return engine_; }
    LibraryBrowserModel& getLibraryBrowser() noexcept { return libraryBrowser_; }

    juce::String getLastLoadStatusText() const;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    void syncApvtsToEngineParameters() noexcept;
    void syncEngineParametersToApvts() noexcept;

    SamplerEngine engine_{};
    LibraryBrowserModel libraryBrowser_{};
    juce::AudioProcessorValueTreeState apvts_;
    juce::MidiKeyboardState keyboardState_{};
    juce::ThreadPool loaderThreadPool_{1};

    mutable juce::CriticalSection statusLock_{};
    juce::String lastLoadStatusText_{"Ready - Factory Electric Grand (C4) Loaded"};
    std::array<std::string, SamplerEngine::kMaxRackSlots> slotLoadedPaths_{
        "Factory://ElectricGrand", "", "", ""
    };

    std::vector<MidiEvent> blockMidiEvents_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KonosPluginProcessor)
};

} // namespace konos
