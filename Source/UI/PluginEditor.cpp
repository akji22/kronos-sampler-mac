#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace konos {

KonosPluginEditor::KonosPluginEditor(KonosPluginProcessor& p)
    : AudioProcessorEditor(&p),
      processor_(p),
      keyboardComponent_(p.getKeyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard) {
    setLookAndFeel(&workstationLaf_);
    setSize(1240, 780);
    setResizable(true, true);
    setResizeLimits(1024, 680, 1920, 1200);

    // 1. Top Header Buttons
    addAndMakeVisible(loadFileButton_);
    loadFileButton_.onClick = [this]() { openSampleFileChooser(); };

    addAndMakeVisible(scanFolderButton_);
    scanFolderButton_.onClick = [this]() { openLibraryFolderScanner(); };

    addAndMakeVisible(previewNoteButton_);
    previewNoteButton_.onClick = [this]() {
        processor_.getSamplerEngine().triggerPreviewNoteAsync(60, 108);
    };

    addAndMakeVisible(panicButton_);
    panicButton_.onClick = [this]() {
        processor_.getSamplerEngine().reset();
    };

    // 2. Left Library Browser Controls
    addAndMakeVisible(searchBox_);
    searchBox_.setTextToShowWhenEmpty("Search instruments...", juce::Colour(0xFF6E7681));
    searchBox_.onTextChange = [this]() {
        processor_.getLibraryBrowser().setSearchQuery(searchBox_.getText().toStdString());
        refreshBrowserListText();
    };

    addAndMakeVisible(categoryBox_);
    categoryBox_.addItem("All Categories", 1);
    categoryBox_.addItem("Factory",        2);
    categoryBox_.addItem("WAV/AIFF",       3);
    categoryBox_.addItem("SFZ",            4);
    categoryBox_.addItem("SF2",            5);
    categoryBox_.addItem("Favorites",      6);
    categoryBox_.setSelectedId(1, juce::dontSendNotification);
    categoryBox_.onChange = [this]() {
        static const char* kCats[6] = {"All", "Factory", "WAV/AIFF", "SFZ", "SF2", "Favorites"};
        const int idx = std::clamp(categoryBox_.getSelectedId() - 1, 0, 5);
        processor_.getLibraryBrowser().setCategoryFilter(kCats[idx]);
        refreshBrowserListText();
    };

    addAndMakeVisible(browserItemSelector_);
    addAndMakeVisible(loadBrowserItemBtn_);
    loadBrowserItemBtn_.onClick = [this]() {
        const auto& items = processor_.getLibraryBrowser().getFilteredItems();
        const int sel = browserItemSelector_.getSelectedId() - 1;
        if (sel >= 0 && static_cast<std::size_t>(sel) < items.size()) {
            processor_.loadLibraryItemToSlotAsync(items[static_cast<std::size_t>(sel)],
                                                  processor_.getSamplerEngine().getSelectedRackSlot());
        }
    };

    addAndMakeVisible(toggleFavBtn_);
    toggleFavBtn_.onClick = [this]() {
        const auto& items = processor_.getLibraryBrowser().getFilteredItems();
        const int sel = browserItemSelector_.getSelectedId() - 1;
        if (sel >= 0 && static_cast<std::size_t>(sel) < items.size()) {
            const std::string path = items[static_cast<std::size_t>(sel)].fullPath;
            processor_.getLibraryBrowser().toggleFavoriteByPath(path);
            refreshBrowserListText();
        }
    };
    refreshBrowserListText();

    // 3. Multi-Instrument Rack Slots (4 Slots)
    for (std::size_t s = 0; s < SamplerEngine::kMaxRackSlots; ++s) {
        setupRackSlotRow(s);
    }

    // 4. Waveform, Loop & Filter Mode Controls
    addAndMakeVisible(loopToggle_);
    loopAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor_.getApvts(), "loopEnabled", loopToggle_);

    addAndMakeVisible(filterModeBox_);
    filterModeBox_.addItem("Low-Pass SVF",  1);
    filterModeBox_.addItem("High-Pass SVF", 2);
    filterModeBox_.addItem("Band-Pass SVF", 3);
    filterModeBox_.setSelectedId(1, juce::dontSendNotification);
    filterModeBox_.onChange = [this]() {
        const int mode = std::clamp(filterModeBox_.getSelectedId() - 1, 0, 2);
        processor_.getSamplerEngine().getParameters().filterMode.store(mode, std::memory_order_relaxed);
        if (auto* param = processor_.getApvts().getParameter("filterMode")) {
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(mode)));
        }
    };

    // 5. Synthesis, ADSR, Filter & Sample Boundary Knobs
    setupRotary(attackKnob_,      "attackMs",        "Attack");
    setupRotary(decayKnob_,       "decayMs",         "Decay");
    setupRotary(sustainKnob_,     "sustainLevel",    "Sustain");
    setupRotary(releaseKnob_,     "releaseMs",       "Release");
    setupRotary(cutoffKnob_,      "filterCutoffHz",  "Cutoff");
    setupRotary(resonanceKnob_,   "filterResonance", "Res Q");
    setupRotary(lfo1RateKnob_,    "lfo1RateHz",      "LFO1 Hz");
    setupRotary(sampleStartKnob_, "sampleStartNorm", "Start");
    setupRotary(sampleEndKnob_,   "sampleEndNorm",   "End");
    setupRotary(loopStartKnob_,   "loopStartNorm",   "LpStart");
    setupRotary(loopEndKnob_,     "loopEndNorm",     "LpEnd");

    // 6. 4-Slot Modulation Matrix Rows
    for (std::size_t m = 0; m < ModulationMatrix::kNumSlots; ++m) {
        setupModMatrixRow(m);
    }

    // 7. DSP Effects Rack & Master Mixer Controls
    addAndMakeVisible(eqToggle_);
    addAndMakeVisible(chorusToggle_);
    addAndMakeVisible(delayToggle_);
    addAndMakeVisible(reverbToggle_);
    addAndMakeVisible(compToggle_);

    eqBtnAtt_     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor_.getApvts(), "eqEnabled",     eqToggle_);
    chorusBtnAtt_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor_.getApvts(), "chorusEnabled", chorusToggle_);
    delayBtnAtt_  = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor_.getApvts(), "delayEnabled",  delayToggle_);
    reverbBtnAtt_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor_.getApvts(), "reverbEnabled", reverbToggle_);
    compBtnAtt_   = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor_.getApvts(), "compEnabled",   compToggle_);

    setupRotary(eqLowKnob_,       "eqLowGainDb",         "EQ Low");
    setupRotary(eqMidKnob_,       "eqMidGainDb",         "EQ Mid");
    setupRotary(eqHighKnob_,      "eqHighGainDb",        "EQ High");
    setupRotary(chorusMixKnob_,   "chorusMix",           "Chr Mix");
    setupRotary(delayTimeKnob_,   "delayTimeMs",         "Dly ms");
    setupRotary(delayMixKnob_,    "delayMix",            "Dly Mix");
    setupRotary(reverbRoomKnob_,  "reverbRoomSize",      "Rev Rm");
    setupRotary(reverbMixKnob_,   "reverbMix",           "Rev Mix");
    setupRotary(compThreshKnob_,  "compThresholdDb",     "Cmp Thr");
    setupRotary(compRatioKnob_,   "compRatio",           "Cmp Rat");
    setupRotary(masterGainKnob_,  "masterGainDb",        "Master");
    setupRotary(masterPanKnob_,   "masterPan",           "Mst Pan");
    setupRotary(masterTuneKnob_,  "masterTuneSemitones", "Mst Tune");

    // 8. Virtual Piano Keyboard
    addAndMakeVisible(keyboardComponent_);
    keyboardComponent_.setAvailableRange(24, 108);
    keyboardComponent_.setKeyWidth(17.5f);

    syncRackControlsFromEngine();
    startTimerHz(30);
}

KonosPluginEditor::~KonosPluginEditor() {
    stopTimer();
    setLookAndFeel(nullptr);
}

void KonosPluginEditor::setupRotary(RotaryControl& rc, const juce::String& paramId, const juce::String& title) {
    rc.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    rc.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 15);
    rc.slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xFF3A86FF));
    rc.slider.setColour(juce::Slider::thumbColourId, juce::Colour(0xFF00F5D4));
    rc.slider.setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xFFD0D7DE));
    rc.slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(rc.slider);

    rc.label.setText(title, juce::dontSendNotification);
    rc.label.setJustificationType(juce::Justification::centred);
    rc.label.setColour(juce::Label::textColourId, juce::Colour(0xFF8B949E));
    addAndMakeVisible(rc.label);

    rc.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor_.getApvts(), paramId, rc.slider);
}

void KonosPluginEditor::setupRackSlotRow(std::size_t idx) {
    auto& rw = rackWidgets_[idx];
    rw.selectBtn.setButtonText("Slot " + juce::String(static_cast<int>(idx + 1)));
    addAndMakeVisible(rw.selectBtn);
    rw.selectBtn.onClick = [this, idx]() {
        processor_.getSamplerEngine().setSelectedRackSlot(idx);
        repaint();
    };

    addAndMakeVisible(rw.midiChBox);
    rw.midiChBox.addItem("Omni", 1);
    for (int ch = 1; ch <= 16; ++ch) {
        rw.midiChBox.addItem("Ch " + juce::String(ch), ch + 1);
    }
    rw.midiChBox.onChange = [this, idx]() {
        const uint8_t ch = static_cast<uint8_t>(std::clamp(rackWidgets_[idx].midiChBox.getSelectedId() - 1, 0, 16));
        processor_.getSamplerEngine().getRackSlotState(idx).midiChannel.store(ch, std::memory_order_relaxed);
    };

    addAndMakeVisible(rw.muteBtn);
    rw.muteBtn.onClick = [this, idx]() {
        processor_.getSamplerEngine().getRackSlotState(idx).mute.store(
            rackWidgets_[idx].muteBtn.getToggleState(), std::memory_order_relaxed);
    };

    addAndMakeVisible(rw.soloBtn);
    rw.soloBtn.onClick = [this, idx]() {
        processor_.getSamplerEngine().getRackSlotState(idx).solo.store(
            rackWidgets_[idx].soloBtn.getToggleState(), std::memory_order_relaxed);
    };

    addAndMakeVisible(rw.volSlider);
    rw.volSlider.setRange(-60.0, 12.0, 0.1);
    rw.volSlider.setValue(0.0, juce::dontSendNotification);
    rw.volSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    rw.volSlider.onValueChange = [this, idx]() {
        processor_.getSamplerEngine().getRackSlotState(idx).volumeDb.store(
            static_cast<float>(rackWidgets_[idx].volSlider.getValue()), std::memory_order_relaxed);
    };

    addAndMakeVisible(rw.panSlider);
    rw.panSlider.setRange(-1.0, 1.0, 0.01);
    rw.panSlider.setValue(0.0, juce::dontSendNotification);
    rw.panSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    rw.panSlider.onValueChange = [this, idx]() {
        processor_.getSamplerEngine().getRackSlotState(idx).pan.store(
            static_cast<float>(rackWidgets_[idx].panSlider.getValue()), std::memory_order_relaxed);
    };

    addAndMakeVisible(rw.dupBtn);
    rw.dupBtn.onClick = [this, idx]() {
        const std::size_t nextSlot = (idx + 1) % SamplerEngine::kMaxRackSlots;
        processor_.getSamplerEngine().duplicateRackSlotAsync(idx, nextSlot);
    };

    addAndMakeVisible(rw.clearBtn);
    rw.clearBtn.onClick = [this, idx]() {
        processor_.getSamplerEngine().clearRackSlotAsync(idx);
    };
}

void KonosPluginEditor::setupModMatrixRow(std::size_t idx) {
    auto& mw = modWidgets_[idx];
    addAndMakeVisible(mw.sourceBox);
    mw.sourceBox.addItem("Src: None",     1);
    mw.sourceBox.addItem("Src: LFO 1",    2);
    mw.sourceBox.addItem("Src: ModWheel", 3);
    mw.sourceBox.addItem("Src: Velocity", 4);
    mw.sourceBox.addItem("Src: KeyTrack", 5);
    mw.sourceBox.addItem("Src: PitchBnd", 6);
    mw.sourceBox.addItem("Src: Amp Env",  7);

    addAndMakeVisible(mw.destBox);
    mw.destBox.addItem("Dst: None",       1);
    mw.destBox.addItem("Dst: Pitch",      2);
    mw.destBox.addItem("Dst: Cutoff",     3);
    mw.destBox.addItem("Dst: Volume",     4);
    mw.destBox.addItem("Dst: Pan",        5);
    mw.destBox.addItem("Dst: LFO1 Rate",  6);

    addAndMakeVisible(mw.amountSlider);
    mw.amountSlider.setRange(-1.0, 1.0, 0.01);
    mw.amountSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 16);

    const auto& slot = processor_.getSamplerEngine().getModulationMatrix().getSlot(idx);
    mw.sourceBox.setSelectedId(slot.source.load() + 1, juce::dontSendNotification);
    mw.destBox.setSelectedId(slot.destination.load() + 1, juce::dontSendNotification);
    mw.amountSlider.setValue(slot.amount.load(), juce::dontSendNotification);

    auto updateEngineModSlot = [this, idx]() {
        const auto src = static_cast<ModSource>(std::clamp(modWidgets_[idx].sourceBox.getSelectedId() - 1, 0, 6));
        const auto dst = static_cast<ModDestination>(std::clamp(modWidgets_[idx].destBox.getSelectedId() - 1, 0, 5));
        const float amt = static_cast<float>(modWidgets_[idx].amountSlider.getValue());
        processor_.getSamplerEngine().getModulationMatrix().setSlot(idx, src, dst, amt);
    };

    mw.sourceBox.onChange = updateEngineModSlot;
    mw.destBox.onChange   = updateEngineModSlot;
    mw.amountSlider.onValueChange = updateEngineModSlot;
}

void KonosPluginEditor::refreshBrowserListText() {
    browserItemSelector_.clear(juce::dontSendNotification);
    const auto& items = processor_.getLibraryBrowser().getFilteredItems();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::string star = items[i].isFavorite ? "[*] " : "";
        const std::string label = star + "[" + items[i].category + "] " + items[i].name;
        browserItemSelector_.addItem(juce::String(label), static_cast<int>(i + 1));
    }
    if (!items.empty()) {
        browserItemSelector_.setSelectedId(1, juce::dontSendNotification);
    }
}

void KonosPluginEditor::syncRackControlsFromEngine() {
    for (std::size_t s = 0; s < SamplerEngine::kMaxRackSlots; ++s) {
        const auto& st = processor_.getSamplerEngine().getRackSlotState(s);
        rackWidgets_[s].midiChBox.setSelectedId(st.midiChannel.load() + 1, juce::dontSendNotification);
        rackWidgets_[s].muteBtn.setToggleState(st.mute.load(), juce::dontSendNotification);
        rackWidgets_[s].soloBtn.setToggleState(st.solo.load(), juce::dontSendNotification);
        rackWidgets_[s].volSlider.setValue(st.volumeDb.load(), juce::dontSendNotification);
        rackWidgets_[s].panSlider.setValue(st.pan.load(), juce::dontSendNotification);
    }
}

void KonosPluginEditor::openSampleFileChooser() {
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Select WAV, AIFF, SFZ, or SF2 Instrument",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.wav;*.wave;*.aif;*.aiff;*.sfz;*.sf2;*.nki;*.nkm;*.nkx;*.ncw");

    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    fileChooser_->launchAsync(flags, [this](const juce::FileChooser& chooser) {
        const juce::File selected = chooser.getResult();
        if (selected.existsAsFile()) {
            processor_.loadInstrumentFileAsync(selected);
        }
    });
}

void KonosPluginEditor::openLibraryFolderScanner() {
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Select Sample Library Folder to Scan",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory));

    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    fileChooser_->launchAsync(flags, [this](const juce::FileChooser& chooser) {
        const juce::File dir = chooser.getResult();
        if (dir.exists()) {
            processor_.getLibraryBrowser().scanDirectory(dir.getFullPathName().toStdString());
            refreshBrowserListText();
        }
    });
}

void KonosPluginEditor::mouseDown(const juce::MouseEvent& e) {
    if (lastWaveformRect_.contains(e.getPosition())) {
        handleWaveformMouseInteraction(e.x, e.mods.isShiftDown());
    }
}

void KonosPluginEditor::mouseDrag(const juce::MouseEvent& e) {
    if (lastWaveformRect_.contains(e.getPosition())) {
        handleWaveformMouseInteraction(e.x, e.mods.isShiftDown());
    }
}

void KonosPluginEditor::handleWaveformMouseInteraction(int mouseX, bool isLoopMarker) {
    if (lastWaveformRect_.getWidth() <= 16) return;
    const float norm = std::clamp(
        static_cast<float>(mouseX - lastWaveformRect_.getX()) / static_cast<float>(lastWaveformRect_.getWidth()),
        0.0f, 1.0f);

    auto& p = processor_.getSamplerEngine().getParameters();
    if (isLoopMarker) {
        if (norm < 0.5f) {
            p.loopStartNorm.store(norm, std::memory_order_relaxed);
            loopStartKnob_.slider.setValue(norm, juce::sendNotificationSync);
        } else {
            p.loopEndNorm.store(norm, std::memory_order_relaxed);
            loopEndKnob_.slider.setValue(norm, juce::sendNotificationSync);
        }
    } else {
        if (norm < 0.5f) {
            p.sampleStartNorm.store(norm, std::memory_order_relaxed);
            sampleStartKnob_.slider.setValue(norm, juce::sendNotificationSync);
        } else {
            p.sampleEndNorm.store(norm, std::memory_order_relaxed);
            sampleEndKnob_.slider.setValue(norm, juce::sendNotificationSync);
        }
    }
    repaint();
}

void KonosPluginEditor::timerCallback() {
    processor_.getSamplerEngine().reclaimRetiredPatches();
    repaint();
}

void KonosPluginEditor::drawTopHeaderBar(juce::Graphics& g, juce::Rectangle<int> bounds) {
    g.setColour(juce::Colour(0xFF1A1E24));
    g.fillRect(bounds);
    g.setColour(juce::Colour(0xFF30363D));
    g.drawRect(bounds, 1);

    g.setColour(juce::Colour(0xFF00F5D4));
#if JUCE_MAJOR_VERSION >= 8
    g.setFont(juce::Font(juce::FontOptions(17.0f).withStyle("Bold")));
#else
    g.setFont(juce::Font(17.0f, juce::Font::bold));
#endif
    g.drawText("KONOS SAMPLER WORKSTATION", 14, 6, 260, 32, juce::Justification::centredLeft);

    g.setColour(juce::Colour(0xFFC9D1D9));
#if JUCE_MAJOR_VERSION >= 8
    g.setFont(juce::Font(juce::FontOptions(12.5f)));
#else
    g.setFont(juce::Font(12.5f));
#endif
    g.drawText(processor_.getLastLoadStatusText(), 595, 6, bounds.getWidth() - 880, 32, juce::Justification::centredLeft);

    const auto& eng = processor_.getSamplerEngine();
    const int voices = eng.getActiveVoiceCount();
    const float cpuPct = eng.getCpuLoadPercent();
    const double memMb = static_cast<double>(eng.getSampleMemoryBytes()) / (1024.0 * 1024.0);
    const float grDb   = eng.getEffectsChain().getCompressorGainReductionDb();

    const juce::String stats = "Voices: " + juce::String(voices) + "/64" +
                               " | CPU: " + juce::String(cpuPct, 1) + "%" +
                               " | RAM: " + juce::String(memMb, 2) + " MB" +
                               " | Comp GR: -" + juce::String(grDb, 1) + " dB";
    g.setColour(juce::Colour(0xFF58A6FF));
    g.drawText(stats, bounds.getWidth() - 310, 6, 298, 32, juce::Justification::centredRight);
}

void KonosPluginEditor::drawBrowserPanel(juce::Graphics& g, juce::Rectangle<int> bounds) {
    g.setColour(juce::Colour(0xFF161B22));
    g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    g.setColour(juce::Colour(0xFF30363D));
    g.drawRoundedRectangle(bounds.toFloat(), 5.0f, 1.0f);

    g.setColour(juce::Colour(0xFFE6EDF3));
    g.drawText("LIBRARY BROWSER", bounds.getX() + 10, bounds.getY() + 6, bounds.getWidth() - 20, 20,
               juce::Justification::centredLeft);

    // Draw list of filtered instruments & recent files preview
    const auto& items = processor_.getLibraryBrowser().getFilteredItems();
    const int listTop = bounds.getY() + 148;
    const int rowH    = 22;
    const int maxRows = (bounds.getBottom() - listTop - 10) / rowH;

    for (int i = 0; i < static_cast<int>(items.size()) && i < maxRows; ++i) {
        const auto& item = items[static_cast<std::size_t>(i)];
        const bool isSelected = (browserItemSelector_.getSelectedId() == i + 1);
        juce::Rectangle<int> rowRect(bounds.getX() + 8, listTop + i * rowH, bounds.getWidth() - 16, rowH - 2);
        g.setColour(isSelected ? juce::Colour(0xFF1F6FEB) : juce::Colour(0xFF21262D));
        g.fillRoundedRectangle(rowRect.toFloat(), 3.0f);

        g.setColour(item.isFavorite ? juce::Colour(0xFFFFD166) : juce::Colour(0xFFC9D1D9));
        const std::string tag = (item.isFavorite ? "* " : "  ") + item.name + " [" + item.category + "]";
        g.drawText(juce::String(tag), rowRect.getX() + 6, rowRect.getY(), rowRect.getWidth() - 12, rowRect.getHeight(),
                   juce::Justification::centredLeft);
    }
}

void KonosPluginEditor::drawRackPanel(juce::Graphics& g, juce::Rectangle<int> bounds) {
    g.setColour(juce::Colour(0xFF161B22));
    g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    g.setColour(juce::Colour(0xFF30363D));
    g.drawRoundedRectangle(bounds.toFloat(), 5.0f, 1.0f);

    g.setColour(juce::Colour(0xFFE6EDF3));
    g.drawText("MULTI-INSTRUMENT RACK (4 CHANNELS)", bounds.getX() + 12, bounds.getY() + 6, 340, 20,
               juce::Justification::centredLeft);

    const std::size_t selectedSlot = processor_.getSamplerEngine().getSelectedRackSlot();
    const int rowStartY = bounds.getY() + 30;
    const int rowH      = 32;

    for (std::size_t s = 0; s < SamplerEngine::kMaxRackSlots; ++s) {
        const int y = rowStartY + static_cast<int>(s) * rowH;
        juce::Rectangle<int> slotRect(bounds.getX() + 8, y, bounds.getWidth() - 16, rowH - 4);
        g.setColour(s == selectedSlot ? juce::Colour(0xFF1C2533) : juce::Colour(0xFF0D1117));
        g.fillRoundedRectangle(slotRect.toFloat(), 4.0f);
        g.setColour(s == selectedSlot ? juce::Colour(0xFF00F5D4) : juce::Colour(0xFF30363D));
        g.drawRoundedRectangle(slotRect.toFloat(), 4.0f, 1.0f);

        const auto patch = processor_.getSamplerEngine().getRackSlotPatchForUI(s);
        const auto& st   = processor_.getSamplerEngine().getRackSlotState(s);
        const juce::String patchName = patch ? juce::String(patch->name + " (" + patch->formatName + ")")
                                             : juce::String("[Empty Slot]");
        g.setColour(patch ? juce::Colour(0xFFE6EDF3) : juce::Colour(0xFF6E7681));
        g.drawText(patchName, bounds.getX() + 74, y + 2, 220, rowH - 8, juce::Justification::centredLeft);

        // Draw per-slot voice count & level meter bar
        const float peak = std::clamp(std::max(st.peakLeft.load(), st.peakRight.load()), 0.0f, 1.0f);
        juce::Rectangle<int> meterBg(bounds.getRight() - 105, y + 8, 90, 12);
        g.setColour(juce::Colour(0xFF21262D));
        g.fillRect(meterBg);
        g.setColour(juce::Colour(0xFF00F5D4));
        g.fillRect(meterBg.getX(), meterBg.getY(), static_cast<int>(peak * 90.0f), 12);
    }
}

void KonosPluginEditor::drawWaveformAndZoneMapPanel(juce::Graphics& g, juce::Rectangle<int> bounds) {
    lastWaveformRect_ = bounds;
    g.setColour(juce::Colour(0xFF0D1117));
    g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    g.setColour(juce::Colour(0xFF30363D));
    g.drawRoundedRectangle(bounds.toFloat(), 5.0f, 1.0f);

    const auto patch = processor_.getSamplerEngine().getCurrentPatchForUI();
    if (!patch || patch->zones.empty() || !patch->zones[0].sample) {
        g.setColour(juce::Colour(0xFF8B949E));
        g.drawText("Selected Rack Slot Empty — Load a WAV, AIFF, SFZ, or SF2 Instrument",
                   bounds, juce::Justification::centred);
        return;
    }

    // Upper 65%: Waveform & Draggable Sample/Loop Markers
    const int waveH = static_cast<int>(bounds.getHeight() * 0.64f);
    juce::Rectangle<int> waveArea(bounds.getX() + 8, bounds.getY() + 6, bounds.getWidth() - 16, waveH);

    const auto& sample = *patch->zones[0].sample;
    const uint64_t numFrames = sample.getNumFrames();
    if (numFrames > 0) {
        const float* data = sample.getReadPointer(0);
        const int width   = waveArea.getWidth();
        const int midY    = waveArea.getCentreY();

        juce::Path wavePath;
        wavePath.startNewSubPath(static_cast<float>(waveArea.getX()), static_cast<float>(midY));
        for (int px = 0; px < width; ++px) {
            const uint64_t fIdx = static_cast<uint64_t>(
                (static_cast<double>(px) / static_cast<double>(width)) * static_cast<double>(numFrames - 1));
            const float s = std::clamp(data[fIdx], -1.0f, 1.0f);
            const float y = static_cast<float>(midY) - s * static_cast<float>(waveH * 0.42f);
            wavePath.lineTo(static_cast<float>(waveArea.getX() + px), y);
        }
        g.setColour(juce::Colour(0xFF3A86FF));
        g.strokePath(wavePath, juce::PathStrokeType(1.3f));

        // Sample Start/End (cyan) and Loop Start/End (amber) vertical markers
        const auto& params = processor_.getSamplerEngine().getParameters();
        const int sStartX = waveArea.getX() + static_cast<int>(params.sampleStartNorm.load() * static_cast<float>(width));
        const int sEndX   = waveArea.getX() + static_cast<int>(params.sampleEndNorm.load()   * static_cast<float>(width));
        const int lStartX = waveArea.getX() + static_cast<int>(params.loopStartNorm.load()   * static_cast<float>(width));
        const int lEndX   = waveArea.getX() + static_cast<int>(params.loopEndNorm.load()     * static_cast<float>(width));

        g.setColour(juce::Colour(0xFF00F5D4));
        g.drawVerticalLine(sStartX, static_cast<float>(waveArea.getY()), static_cast<float>(waveArea.getBottom()));
        g.drawVerticalLine(sEndX,   static_cast<float>(waveArea.getY()), static_cast<float>(waveArea.getBottom()));

        if (params.loopEnabled.load()) {
            g.setColour(juce::Colour(0xFFFFB703));
            g.drawVerticalLine(lStartX, static_cast<float>(waveArea.getY()), static_cast<float>(waveArea.getBottom()));
            g.drawVerticalLine(lEndX,   static_cast<float>(waveArea.getY()), static_cast<float>(waveArea.getBottom()));
        }
    }

    // Lower 35%: 2D Key-Range & Velocity-Layer Zone Map Editor
    juce::Rectangle<int> zoneMapArea(bounds.getX() + 8, waveArea.getBottom() + 4,
                                     bounds.getWidth() - 16, bounds.getBottom() - waveArea.getBottom() - 8);
    g.setColour(juce::Colour(0xFF161B22));
    g.fillRect(zoneMapArea);

    for (const auto& z : patch->zones) {
        const float x0 = static_cast<float>(zoneMapArea.getX()) +
                         (static_cast<float>(z.loKey) / 127.0f) * static_cast<float>(zoneMapArea.getWidth());
        const float x1 = static_cast<float>(zoneMapArea.getX()) +
                         (static_cast<float>(z.hiKey + 1) / 128.0f) * static_cast<float>(zoneMapArea.getWidth());
        const float y0 = static_cast<float>(zoneMapArea.getBottom()) -
                         (static_cast<float>(z.hiVel) / 127.0f) * static_cast<float>(zoneMapArea.getHeight());
        const float y1 = static_cast<float>(zoneMapArea.getBottom()) -
                         (static_cast<float>(z.loVel - 1) / 127.0f) * static_cast<float>(zoneMapArea.getHeight());

        juce::Rectangle<float> zr(x0, y0, std::max(4.0f, x1 - x0), std::max(4.0f, y1 - y0));
        g.setColour(juce::Colour(0x553A86FF));
        g.fillRect(zr);
        g.setColour(juce::Colour(0xFF58A6FF));
        g.drawRect(zr, 1.0f);
    }
}

void KonosPluginEditor::drawSynthesisAndModMatrixPanel(juce::Graphics& g, juce::Rectangle<int> bounds) {
    g.setColour(juce::Colour(0xFF161B22));
    g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    g.setColour(juce::Colour(0xFF30363D));
    g.drawRoundedRectangle(bounds.toFloat(), 5.0f, 1.0f);

    g.setColour(juce::Colour(0xFFE6EDF3));
    g.drawText("ADSR ENVELOPE / TPT SVF FILTER / SAMPLE BOUNDS / MODULATION MATRIX",
               bounds.getX() + 12, bounds.getY() + 5, bounds.getWidth() - 24, 18,
               juce::Justification::centredLeft);
}

void KonosPluginEditor::drawEffectsAndMixerPanel(juce::Graphics& g, juce::Rectangle<int> bounds) {
    g.setColour(juce::Colour(0xFF161B22));
    g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    g.setColour(juce::Colour(0xFF30363D));
    g.drawRoundedRectangle(bounds.toFloat(), 5.0f, 1.0f);

    g.setColour(juce::Colour(0xFFE6EDF3));
    g.drawText("DSP EFFECTS RACK (3-BAND EQ / CHORUS / DELAY / REVERB / COMP) & MASTER MIXER",
               bounds.getX() + 12, bounds.getY() + 5, bounds.getWidth() - 24, 18,
               juce::Justification::centredLeft);
}

void KonosPluginEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xFF121418));

    const int w = getWidth();
    const int h = getHeight();
    const int leftBrowserW = 250;
    const int rightMainX   = leftBrowserW + 20;
    const int rightMainW   = w - rightMainX - 12;

    drawTopHeaderBar(g, juce::Rectangle<int>(0, 0, w, 44));
    drawBrowserPanel(g, juce::Rectangle<int>(12, 52, leftBrowserW, h - 148));
    drawRackPanel(g, juce::Rectangle<int>(rightMainX, 52, rightMainW, 162));
    drawWaveformAndZoneMapPanel(g, juce::Rectangle<int>(rightMainX, 220, rightMainW, 155));
    drawSynthesisAndModMatrixPanel(g, juce::Rectangle<int>(rightMainX, 381, rightMainW, 142));
    drawEffectsAndMixerPanel(g, juce::Rectangle<int>(rightMainX, 529, rightMainW, h - 625));
}

void KonosPluginEditor::resized() {
    const int w = getWidth();
    const int h = getHeight();
    const int leftBrowserW = 250;
    const int rightMainX   = leftBrowserW + 20;
    const int rightMainW   = w - rightMainX - 12;

    // 1. Top Navigation Bar
    loadFileButton_.setBounds(280, 8, 95, 28);
    scanFolderButton_.setBounds(380, 8, 100, 28);
    previewNoteButton_.setBounds(485, 8, 88, 28);
    panicButton_.setBounds(578, 8, 56, 28);

    // 2. Left Library Browser
    searchBox_.setBounds(20, 78, leftBrowserW - 16, 24);
    categoryBox_.setBounds(20, 106, leftBrowserW - 16, 24);
    browserItemSelector_.setBounds(20, 134, leftBrowserW - 16, 24);
    loadBrowserItemBtn_.setBounds(20, 162, leftBrowserW - 92, 24);
    toggleFavBtn_.setBounds(leftBrowserW - 68, 162, 64, 24);

    // 3. Multi-Instrument Rack (4 Slots)
    const int rackStartY = 82;
    const int rackRowH   = 32;
    for (std::size_t s = 0; s < SamplerEngine::kMaxRackSlots; ++s) {
        const int y = rackStartY + static_cast<int>(s) * rackRowH;
        auto& rw = rackWidgets_[s];
        rw.selectBtn.setBounds(rightMainX + 12, y, 56, 24);
        rw.midiChBox.setBounds(rightMainX + 300, y, 74, 24);
        rw.muteBtn.setBounds(rightMainX + 380, y, 32, 24);
        rw.soloBtn.setBounds(rightMainX + 414, y, 32, 24);
        rw.volSlider.setBounds(rightMainX + 452, y, 120, 24);
        rw.panSlider.setBounds(rightMainX + 578, y, 80, 24);
        rw.dupBtn.setBounds(rightMainX + 664, y, 42, 24);
        rw.clearBtn.setBounds(rightMainX + 710, y, 28, 24);
    }

    // 4. Synthesis / ADSR / Filter / Bounds + 4-Slot Modulation Matrix
    const int synthY = 404;
    loopToggle_.setBounds(rightMainX + 12, synthY - 20, 58, 18);
    filterModeBox_.setBounds(rightMainX + 74, synthY - 20, 118, 18);

    RotaryControl* synthKnobs[11] = {
        &attackKnob_, &decayKnob_, &sustainKnob_, &releaseKnob_,
        &cutoffKnob_, &resonanceKnob_, &lfo1RateKnob_,
        &sampleStartKnob_, &sampleEndKnob_, &loopStartKnob_, &loopEndKnob_
    };
    const int synthKnobW = 54;
    for (int i = 0; i < 11; ++i) {
        const int kx = rightMainX + 10 + i * synthKnobW;
        synthKnobs[i]->label.setBounds(kx, synthY, synthKnobW - 2, 14);
        synthKnobs[i]->slider.setBounds(kx, synthY + 14, synthKnobW - 2, 74);
    }

    // Modulation Matrix on the right side of the Synthesis panel
    const int modStartX = rightMainX + 610;
    const int modW      = std::max(240, rightMainW - 620);
    for (std::size_t m = 0; m < ModulationMatrix::kNumSlots; ++m) {
        const int my = synthY - 2 + static_cast<int>(m) * 24;
        modWidgets_[m].sourceBox.setBounds(modStartX, my, modW / 3 - 4, 21);
        modWidgets_[m].destBox.setBounds(modStartX + modW / 3, my, modW / 3 - 4, 21);
        modWidgets_[m].amountSlider.setBounds(modStartX + (2 * modW) / 3, my, modW / 3, 21);
    }

    // 5. DSP Effects Rack & Master Mixer
    const int fxY = 552;
    eqToggle_.setBounds(rightMainX + 12,     fxY - 20, 48, 18);
    chorusToggle_.setBounds(rightMainX + 190, fxY - 20, 64, 18);
    delayToggle_.setBounds(rightMainX + 260,  fxY - 20, 58, 18);
    reverbToggle_.setBounds(rightMainX + 380, fxY - 20, 64, 18);
    compToggle_.setBounds(rightMainX + 500,   fxY - 20, 60, 18);

    RotaryControl* fxKnobs[13] = {
        &eqLowKnob_, &eqMidKnob_, &eqHighKnob_,
        &chorusMixKnob_, &delayTimeKnob_, &delayMixKnob_,
        &reverbRoomKnob_, &reverbMixKnob_,
        &compThreshKnob_, &compRatioKnob_,
        &masterGainKnob_, &masterPanKnob_, &masterTuneKnob_
    };
    const int fxKnobW = std::max(56, (rightMainW - 20) / 13);
    for (int i = 0; i < 13; ++i) {
        const int kx = rightMainX + 10 + i * fxKnobW;
        fxKnobs[i]->label.setBounds(kx, fxY, fxKnobW - 2, 14);
        fxKnobs[i]->slider.setBounds(kx, fxY + 14, fxKnobW - 2, 74);
    }

    // 6. Bottom Dock: Virtual Piano Keyboard
    keyboardComponent_.setBounds(12, h - 88, w - 24, 78);
}

} // namespace konos
