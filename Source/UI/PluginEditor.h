#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "../Plugin/PluginProcessor.h"
#include "WorkstationLookAndFeel.h"

namespace konos {

/**
 * Full Resizable Sampler Workstation UI in JUCE.
 * Every control is directly bound to the real-time SamplerEngine, Multi-Instrument
 * Rack, Modulation Matrix, or DSP Effects Chain — zero decorative dummy controls:
 *
 * 1. Top Navigation & Telemetry Header (Load File, Scan Folder, Audition, Panic, CPU/RAM/Voice/Peak meters)
 * 2. Left Dock: Library Browser (Search filter, Category filter, Favorites toggle, Load to Slot, Item List)
 * 3. Upper Center: 4-Slot Multi-Instrument Rack (Edit select, MIDI Ch, Mute, Solo, Volume, Pan, Duplicate, Clear)
 * 4. Middle Center: Interactive Waveform & 2D Key/Velocity Zone Map Editor (drag markers for Sample Start/End & Loop)
 * 5. Lower Workstation Panels:
 *    - ADSR Envelope & TPT Filter + LFO 1 Controls
 *    - 4-Slot Modulation Matrix (Source -> Destination -> Bipolar Depth)
 *    - 5-Stage Effects Rack (3-Band EQ, Chorus, Ping-Pong Delay, Reverb, Master Compressor) & Stereo Mixer
 * 6. Bottom Dock: Clickable Polyphonic Virtual Piano Keyboard with live MIDI activity
 */
class KonosPluginEditor final : public juce::AudioProcessorEditor,
                                private juce::Timer {
public:
    explicit KonosPluginEditor(KonosPluginProcessor& processor);
    ~KonosPluginEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

private:
    void timerCallback() override;
    void openSampleFileChooser();
    void openLibraryFolderScanner();
    void refreshBrowserListText();
    void syncRackControlsFromEngine();
    void handleWaveformMouseInteraction(int mouseX, bool isLoopMarker);

    void drawTopHeaderBar(juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawBrowserPanel(juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawRackPanel(juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawWaveformAndZoneMapPanel(juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawSynthesisAndModMatrixPanel(juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawEffectsAndMixerPanel(juce::Graphics& g, juce::Rectangle<int> bounds);

    struct RotaryControl {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    struct RackSlotWidgets {
        juce::TextButton selectBtn{"Slot"};
        juce::ComboBox   midiChBox;
        juce::ToggleButton muteBtn{"M"};
        juce::ToggleButton soloBtn{"S"};
        juce::Slider     volSlider;
        juce::Slider     panSlider;
        juce::TextButton dupBtn{"Dup"};
        juce::TextButton clearBtn{"X"};
    };

    struct ModSlotWidgets {
        juce::ComboBox sourceBox;
        juce::ComboBox destBox;
        juce::Slider   amountSlider;
    };

    void setupRotary(RotaryControl& rc, const juce::String& paramId, const juce::String& title);
    void setupRackSlotRow(std::size_t idx);
    void setupModMatrixRow(std::size_t idx);

    KonosPluginProcessor& processor_;
    WorkstationLookAndFeel workstationLaf_;

    // Top Navigation Bar controls
    juce::TextButton loadFileButton_{"Load File..."};
    juce::TextButton scanFolderButton_{"Scan Folder..."};
    juce::TextButton previewNoteButton_{"Audition C4"};
    juce::TextButton panicButton_{"Panic"};

    // Left Panel: Library Browser controls
    juce::TextEditor searchBox_;
    juce::ComboBox   categoryBox_;
    juce::ComboBox   browserItemSelector_;
    juce::TextButton loadBrowserItemBtn_{"Load Selected -> Slot"};
    juce::TextButton toggleFavBtn_{"Favorite *"};

    // Multi-Instrument Rack (4 Slots)
    std::array<RackSlotWidgets, SamplerEngine::kMaxRackSlots> rackWidgets_;

    // Waveform & Loop Controls
    juce::ToggleButton loopToggle_{"Loop"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttachment_;
    juce::ComboBox filterModeBox_;
    juce::Rectangle<int> lastWaveformRect_{};

    // ADSR, Filter, Sample Bounds & LFO1 Rotary Controls
    RotaryControl attackKnob_;
    RotaryControl decayKnob_;
    RotaryControl sustainKnob_;
    RotaryControl releaseKnob_;
    RotaryControl cutoffKnob_;
    RotaryControl resonanceKnob_;
    RotaryControl lfo1RateKnob_;
    RotaryControl sampleStartKnob_;
    RotaryControl sampleEndKnob_;
    RotaryControl loopStartKnob_;
    RotaryControl loopEndKnob_;

    // 4-Slot Modulation Matrix Controls
    std::array<ModSlotWidgets, ModulationMatrix::kNumSlots> modWidgets_;

    // Effects Rack & Master Mixer Controls
    juce::ToggleButton eqToggle_{"EQ"};
    juce::ToggleButton chorusToggle_{"Chorus"};
    juce::ToggleButton delayToggle_{"Delay"};
    juce::ToggleButton reverbToggle_{"Reverb"};
    juce::ToggleButton compToggle_{"Comp"};

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> eqBtnAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chorusBtnAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> delayBtnAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> reverbBtnAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> compBtnAtt_;

    RotaryControl eqLowKnob_;
    RotaryControl eqMidKnob_;
    RotaryControl eqHighKnob_;
    RotaryControl chorusMixKnob_;
    RotaryControl delayTimeKnob_;
    RotaryControl delayMixKnob_;
    RotaryControl reverbRoomKnob_;
    RotaryControl reverbMixKnob_;
    RotaryControl compThreshKnob_;
    RotaryControl compRatioKnob_;
    RotaryControl masterGainKnob_;
    RotaryControl masterPanKnob_;
    RotaryControl masterTuneKnob_;

    // Bottom Dock: Virtual Piano Keyboard
    juce::MidiKeyboardComponent keyboardComponent_;
    std::unique_ptr<juce::FileChooser> fileChooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KonosPluginEditor)
};

} // namespace konos
