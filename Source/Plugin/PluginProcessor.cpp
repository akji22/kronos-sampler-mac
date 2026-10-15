#include "PluginProcessor.h"
#include "../UI/PluginEditor.h"
#include "../Formats/WavAiffFormatLoader.h"
#include "../Formats/SfzFormatLoader.h"
#include "../Formats/Sf2FormatLoader.h"
#include "../Formats/KontaktFormatInspector.h"
#include "../State/PresetStateManager.h"
#include <algorithm>

namespace konos {

KonosPluginProcessor::KonosPluginProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "KonosParameters", createParameterLayout()) {
    blockMidiEvents_.reserve(512);
}

KonosPluginProcessor::~KonosPluginProcessor() {
    loaderThreadPool_.removeAllJobs(true, 4000);
    engine_.getDiskStreamer().stopWorker();
    engine_.reclaimRetiredPatches();
}

juce::AudioProcessorValueTreeState::ParameterLayout KonosPluginProcessor::createParameterLayout() {
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"masterGainDb", 1}, "Master Volume",
        juce::NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"masterPan", 1}, "Master Pan",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"masterTuneSemitones", 1}, "Master Tune",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"attackMs", 1}, "Attack (ms)",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.5f, 0.35f), 5.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"decayMs", 1}, "Decay (ms)",
        juce::NormalisableRange<float>(1.0f, 10000.0f, 1.0f, 0.35f), 180.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sustainLevel", 1}, "Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.80f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"releaseMs", 1}, "Release (ms)",
        juce::NormalisableRange<float>(2.0f, 10000.0f, 1.0f, 0.35f), 250.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"filterCutoffHz", 1}, "Filter Cutoff (Hz)",
        juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.28f), 18000.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"filterResonance", 1}, "Filter Resonance (Q)",
        juce::NormalisableRange<float>(0.5f, 10.0f, 0.01f, 0.5f), 0.707f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"filterMode", 1}, "Filter Mode", 0, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"lfo1RateHz", 1}, "LFO 1 Rate (Hz)",
        juce::NormalisableRange<float>(0.1f, 20.0f, 0.05f, 0.5f), 5.5f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sampleStartNorm", 1}, "Sample Start",
        juce::NormalisableRange<float>(0.0f, 0.95f, 0.001f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sampleEndNorm", 1}, "Sample End",
        juce::NormalisableRange<float>(0.05f, 1.0f, 0.001f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"loopEnabled", 1}, "Loop Enabled", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopStartNorm", 1}, "Loop Start",
        juce::NormalisableRange<float>(0.0f, 0.95f, 0.001f), 0.10f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopEndNorm", 1}, "Loop End",
        juce::NormalisableRange<float>(0.05f, 1.0f, 0.001f), 0.90f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"maxPolyphony", 1}, "Max Polyphony", 1, 64, 32));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"eqEnabled", 1}, "EQ Enabled", true));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"eqLowGainDb", 1}, "EQ Low Gain (dB)",
        juce::NormalisableRange<float>(-15.0f, 15.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"eqMidGainDb", 1}, "EQ Mid Gain (dB)",
        juce::NormalisableRange<float>(-15.0f, 15.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"eqHighGainDb", 1}, "EQ High Gain (dB)",
        juce::NormalisableRange<float>(-15.0f, 15.0f, 0.1f), 0.0f));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"chorusEnabled", 1}, "Chorus Enabled", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"chorusRateHz", 1}, "Chorus Rate (Hz)",
        juce::NormalisableRange<float>(0.1f, 5.0f, 0.05f), 0.8f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"chorusDepth", 1}, "Chorus Depth",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.45f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"chorusMix", 1}, "Chorus Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.35f));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"delayEnabled", 1}, "Delay Enabled", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"delayTimeMs", 1}, "Delay Time (ms)",
        juce::NormalisableRange<float>(10.0f, 1500.0f, 1.0f), 320.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"delayFeedback", 1}, "Delay Feedback",
        juce::NormalisableRange<float>(0.0f, 0.90f, 0.01f), 0.35f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"delayMix", 1}, "Delay Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.25f));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"reverbEnabled", 1}, "Reverb Enabled", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbRoomSize", 1}, "Reverb Room Size",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.65f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbDamping", 1}, "Reverb Damping",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.40f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbMix", 1}, "Reverb Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.22f));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"compEnabled", 1}, "Compressor Enabled", true));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"compThresholdDb", 1}, "Compressor Threshold (dB)",
        juce::NormalisableRange<float>(-40.0f, 0.0f, 0.1f), -12.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"compRatio", 1}, "Compressor Ratio",
        juce::NormalisableRange<float>(1.0f, 20.0f, 0.1f), 3.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"compMakeupDb", 1}, "Compressor Makeup (dB)",
        juce::NormalisableRange<float>(0.0f, 18.0f, 0.1f), 0.0f));

    return {params.begin(), params.end()};
}

void KonosPluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    engine_.prepare(sampleRate, samplesPerBlock);
    keyboardState_.reset();
}

void KonosPluginProcessor::releaseResources() {
    engine_.reset();
}

bool KonosPluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void KonosPluginProcessor::syncApvtsToEngineParameters() noexcept {
    auto& p = engine_.getParameters();
    p.masterGainDb.store(*apvts_.getRawParameterValue("masterGainDb"), std::memory_order_relaxed);
    p.masterPan.store(*apvts_.getRawParameterValue("masterPan"), std::memory_order_relaxed);
    p.masterTuneSemitones.store(*apvts_.getRawParameterValue("masterTuneSemitones"), std::memory_order_relaxed);
    p.attackMs.store(*apvts_.getRawParameterValue("attackMs"), std::memory_order_relaxed);
    p.decayMs.store(*apvts_.getRawParameterValue("decayMs"), std::memory_order_relaxed);
    p.sustainLevel.store(*apvts_.getRawParameterValue("sustainLevel"), std::memory_order_relaxed);
    p.releaseMs.store(*apvts_.getRawParameterValue("releaseMs"), std::memory_order_relaxed);
    p.filterCutoffHz.store(*apvts_.getRawParameterValue("filterCutoffHz"), std::memory_order_relaxed);
    p.filterResonance.store(*apvts_.getRawParameterValue("filterResonance"), std::memory_order_relaxed);
    p.filterMode.store(static_cast<int>(*apvts_.getRawParameterValue("filterMode")), std::memory_order_relaxed);
    p.sampleStartNorm.store(*apvts_.getRawParameterValue("sampleStartNorm"), std::memory_order_relaxed);
    p.sampleEndNorm.store(*apvts_.getRawParameterValue("sampleEndNorm"), std::memory_order_relaxed);
    p.loopEnabled.store(*apvts_.getRawParameterValue("loopEnabled") > 0.5f, std::memory_order_relaxed);
    p.loopStartNorm.store(*apvts_.getRawParameterValue("loopStartNorm"), std::memory_order_relaxed);
    p.loopEndNorm.store(*apvts_.getRawParameterValue("loopEndNorm"), std::memory_order_relaxed);
    p.maxPolyphony.store(static_cast<int>(*apvts_.getRawParameterValue("maxPolyphony")), std::memory_order_relaxed);

    engine_.getModulationMatrix().lfo1RateHz.store(
        *apvts_.getRawParameterValue("lfo1RateHz"), std::memory_order_relaxed);

    auto& fx = engine_.getEffectsChain().getParameters();
    fx.eqEnabled.store(*apvts_.getRawParameterValue("eqEnabled") > 0.5f, std::memory_order_relaxed);
    fx.eqLowGainDb.store(*apvts_.getRawParameterValue("eqLowGainDb"), std::memory_order_relaxed);
    fx.eqMidGainDb.store(*apvts_.getRawParameterValue("eqMidGainDb"), std::memory_order_relaxed);
    fx.eqHighGainDb.store(*apvts_.getRawParameterValue("eqHighGainDb"), std::memory_order_relaxed);

    fx.chorusEnabled.store(*apvts_.getRawParameterValue("chorusEnabled") > 0.5f, std::memory_order_relaxed);
    fx.chorusRateHz.store(*apvts_.getRawParameterValue("chorusRateHz"), std::memory_order_relaxed);
    fx.chorusDepth.store(*apvts_.getRawParameterValue("chorusDepth"), std::memory_order_relaxed);
    fx.chorusMix.store(*apvts_.getRawParameterValue("chorusMix"), std::memory_order_relaxed);

    fx.delayEnabled.store(*apvts_.getRawParameterValue("delayEnabled") > 0.5f, std::memory_order_relaxed);
    fx.delayTimeMs.store(*apvts_.getRawParameterValue("delayTimeMs"), std::memory_order_relaxed);
    fx.delayFeedback.store(*apvts_.getRawParameterValue("delayFeedback"), std::memory_order_relaxed);
    fx.delayMix.store(*apvts_.getRawParameterValue("delayMix"), std::memory_order_relaxed);

    fx.reverbEnabled.store(*apvts_.getRawParameterValue("reverbEnabled") > 0.5f, std::memory_order_relaxed);
    fx.reverbRoomSize.store(*apvts_.getRawParameterValue("reverbRoomSize"), std::memory_order_relaxed);
    fx.reverbDamping.store(*apvts_.getRawParameterValue("reverbDamping"), std::memory_order_relaxed);
    fx.reverbMix.store(*apvts_.getRawParameterValue("reverbMix"), std::memory_order_relaxed);

    fx.compEnabled.store(*apvts_.getRawParameterValue("compEnabled") > 0.5f, std::memory_order_relaxed);
    fx.compThresholdDb.store(*apvts_.getRawParameterValue("compThresholdDb"), std::memory_order_relaxed);
    fx.compRatio.store(*apvts_.getRawParameterValue("compRatio"), std::memory_order_relaxed);
    fx.compMakeupDb.store(*apvts_.getRawParameterValue("compMakeupDb"), std::memory_order_relaxed);
}

void KonosPluginProcessor::syncEngineParametersToApvts() noexcept {
    auto setParam = [this](const char* id, float plainVal) {
        if (auto* param = apvts_.getParameter(id)) {
            param->setValueNotifyingHost(param->convertTo0to1(plainVal));
        }
    };

    const auto& p  = engine_.getParameters();
    const auto& fx = engine_.getEffectsChain().getParameters();

    setParam("masterGainDb",        p.masterGainDb.load());
    setParam("masterPan",           p.masterPan.load());
    setParam("masterTuneSemitones", p.masterTuneSemitones.load());
    setParam("attackMs",            p.attackMs.load());
    setParam("decayMs",             p.decayMs.load());
    setParam("sustainLevel",        p.sustainLevel.load());
    setParam("releaseMs",           p.releaseMs.load());
    setParam("filterCutoffHz",      p.filterCutoffHz.load());
    setParam("filterResonance",     p.filterResonance.load());
    setParam("filterMode",          static_cast<float>(p.filterMode.load()));
    setParam("lfo1RateHz",          engine_.getModulationMatrix().lfo1RateHz.load());
    setParam("sampleStartNorm",     p.sampleStartNorm.load());
    setParam("sampleEndNorm",       p.sampleEndNorm.load());
    setParam("loopEnabled",         p.loopEnabled.load() ? 1.0f : 0.0f);
    setParam("loopStartNorm",       p.loopStartNorm.load());
    setParam("loopEndNorm",         p.loopEndNorm.load());
    setParam("maxPolyphony",        static_cast<float>(p.maxPolyphony.load()));

    setParam("eqEnabled",           fx.eqEnabled.load() ? 1.0f : 0.0f);
    setParam("eqLowGainDb",         fx.eqLowGainDb.load());
    setParam("eqMidGainDb",         fx.eqMidGainDb.load());
    setParam("eqHighGainDb",        fx.eqHighGainDb.load());
    setParam("chorusEnabled",       fx.chorusEnabled.load() ? 1.0f : 0.0f);
    setParam("chorusRateHz",        fx.chorusRateHz.load());
    setParam("chorusDepth",         fx.chorusDepth.load());
    setParam("chorusMix",           fx.chorusMix.load());
    setParam("delayEnabled",        fx.delayEnabled.load() ? 1.0f : 0.0f);
    setParam("delayTimeMs",         fx.delayTimeMs.load());
    setParam("delayFeedback",       fx.delayFeedback.load());
    setParam("delayMix",            fx.delayMix.load());
    setParam("reverbEnabled",       fx.reverbEnabled.load() ? 1.0f : 0.0f);
    setParam("reverbRoomSize",      fx.reverbRoomSize.load());
    setParam("reverbDamping",       fx.reverbDamping.load());
    setParam("reverbMix",           fx.reverbMix.load());
    setParam("compEnabled",         fx.compEnabled.load() ? 1.0f : 0.0f);
    setParam("compThresholdDb",     fx.compThresholdDb.load());
    setParam("compRatio",           fx.compRatio.load());
    setParam("compMakeupDb",        fx.compMakeupDb.load());
}

void KonosPluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) {
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0) return;

    keyboardState_.processNextMidiBuffer(midiMessages, 0, numSamples, true);
    syncApvtsToEngineParameters();

    blockMidiEvents_.clear();
    for (const auto metadata : midiMessages) {
        const auto msg = metadata.getMessage();
        const uint8_t* raw = msg.getRawData();
        if (msg.getRawDataSize() >= 1) {
            MidiEvent ev;
            ev.sampleOffset = std::clamp(metadata.samplePosition, 0, numSamples - 1);
            ev.status       = raw[0];
            ev.data1        = msg.getRawDataSize() >= 2 ? raw[1] : 0;
            ev.data2        = msg.getRawDataSize() >= 3 ? raw[2] : 0;
            blockMidiEvents_.push_back(ev);
        }
    }

    // Guarantee chronological ordering of merged host + virtual keyboard MIDI events
    std::stable_sort(blockMidiEvents_.begin(), blockMidiEvents_.end(),
        [](const MidiEvent& a, const MidiEvent& b) noexcept {
            return a.sampleOffset < b.sampleOffset;
        });

    float* outLeft  = buffer.getWritePointer(0);
    float* outRight = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : outLeft;

    engine_.renderBlock(outLeft, outRight, numSamples, blockMidiEvents_.data(), blockMidiEvents_.size());
}

void KonosPluginProcessor::loadInstrumentFileAsync(const juce::File& file) {
    loadInstrumentFileToSlotAsync(file, engine_.getSelectedRackSlot());
}

void KonosPluginProcessor::loadInstrumentFileToSlotAsync(const juce::File& file, std::size_t targetSlot) {
    const std::string path = file.getFullPathName().toStdString();
    const std::string ext  = file.getFileExtension().toLowerCase().toStdString();

    {
        const juce::ScopedLock sl(statusLock_);
        lastLoadStatusText_ = "Loading into Slot " + juce::String(static_cast<int>(targetSlot + 1)) +
                              ": " + file.getFileName() + "...";
    }

    loaderThreadPool_.addJob([this, path, ext, targetSlot]() {
        FormatLoadResult res;
        WavAiffFormatLoader wavLoader;
        SfzFormatLoader     sfzLoader;
        Sf2FormatLoader     sf2Loader;
        KontaktFormatInspector kontaktInspector;

        if (wavLoader.canHandleExtension(ext)) {
            res = wavLoader.loadFromFile(path);
        } else if (sfzLoader.canHandleExtension(ext)) {
            res = sfzLoader.loadFromFile(path);
        } else if (sf2Loader.canHandleExtension(ext)) {
            res = sf2Loader.loadFromFile(path);
        } else if (kontaktInspector.canHandleExtension(ext)) {
            res = kontaktInspector.loadFromFile(path);
        } else {
            res.succeeded = false;
            res.diagnosticMessage = "Unsupported file extension: " + ext;
        }

        if (res.succeeded && res.patch) {
            engine_.setRackSlotPatchAsync(targetSlot, res.patch);
            libraryBrowser_.recordRecentInstrument(path);
            const juce::ScopedLock sl(statusLock_);
            if (targetSlot < SamplerEngine::kMaxRackSlots) {
                slotLoadedPaths_[targetSlot] = path;
            }
            lastLoadStatusText_ = "Slot " + juce::String(static_cast<int>(targetSlot + 1)) + ": " +
                                  juce::String(res.diagnosticMessage);
        } else {
            const juce::ScopedLock sl(statusLock_);
            lastLoadStatusText_ = "Load rejected: " + juce::String(res.diagnosticMessage);
        }
    });
}

void KonosPluginProcessor::loadLibraryItemToSlotAsync(const LibraryItem& item, std::size_t slotIndex) {
    if (item.isFactory) {
        auto patch = SamplerEngine::createFactoryPatchById(item.fullPath, 44100.0);
        engine_.setRackSlotPatchAsync(slotIndex, patch);
        libraryBrowser_.recordRecentInstrument(item.fullPath);
        const juce::ScopedLock sl(statusLock_);
        if (slotIndex < SamplerEngine::kMaxRackSlots) {
            slotLoadedPaths_[slotIndex] = item.fullPath;
        }
        lastLoadStatusText_ = "Loaded '" + juce::String(item.name) + "' into Slot " +
                              juce::String(static_cast<int>(slotIndex + 1));
    } else {
        loadInstrumentFileToSlotAsync(juce::File{juce::String(item.fullPath)}, slotIndex);
    }
}

juce::String KonosPluginProcessor::getLastLoadStatusText() const {
    const juce::ScopedLock sl(statusLock_);
    return lastLoadStatusText_;
}

void KonosPluginProcessor::getStateInformation(juce::MemoryBlock& destData) {
    syncApvtsToEngineParameters();
    std::array<std::string, SamplerEngine::kMaxRackSlots> pathsCopy;
    {
        const juce::ScopedLock sl(statusLock_);
        pathsCopy = slotLoadedPaths_;
    }
    const std::string stateStr = PresetStateManager::serializeEngineState(engine_, &pathsCopy);
    destData.replaceAll(stateStr.data(), stateStr.size());
}

void KonosPluginProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (!data || sizeInBytes <= 0) return;
    const std::string stateStr(static_cast<const char*>(data), static_cast<std::size_t>(sizeInBytes));
    std::array<std::string, SamplerEngine::kMaxRackSlots> restoredPaths{};

    if (PresetStateManager::deserializeEngineState(stateStr, engine_, restoredPaths)) {
        syncEngineParametersToApvts();

        for (std::size_t s = 0; s < SamplerEngine::kMaxRackSlots; ++s) {
            const std::string& p = restoredPaths[s];
            if (p.empty()) continue;

            if (p.rfind("Factory://", 0) == 0) {
                auto patch = SamplerEngine::createFactoryPatchById(p, 44100.0);
                engine_.setRackSlotPatchAsync(s, patch);
                const juce::ScopedLock sl(statusLock_);
                slotLoadedPaths_[s] = p;
            } else {
                const juce::File f{juce::String(p)};
                if (f.existsAsFile()) {
                    loadInstrumentFileToSlotAsync(f, s);
                }
            }
        }
    }
}

juce::AudioProcessorEditor* KonosPluginProcessor::createEditor() {
    return new KonosPluginEditor(*this);
}

} // namespace konos

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new konos::KonosPluginProcessor();
}
