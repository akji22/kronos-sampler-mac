#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

namespace konos {

/**
 * Custom dark charcoal workstation LookAndFeel inspired by modern studio samplers.
 * Renders vector rotary arc knobs, illuminated toggle buttons, and crisp dark panels.
 */
class WorkstationLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    WorkstationLookAndFeel() {
        setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(0xFF121418));
        setColour(juce::TextButton::buttonColourId,          juce::Colour(0xFF21262D));
        setColour(juce::TextButton::buttonOnColourId,        juce::Colour(0xFF1F6FEB));
        setColour(juce::TextButton::textColourOffId,         juce::Colour(0xFFE6EDF3));
        setColour(juce::TextButton::textColourOnId,          juce::Colour(0xFFFFFFFF));
        setColour(juce::ComboBox::backgroundColourId,        juce::Colour(0xFF161B22));
        setColour(juce::ComboBox::textColourId,              juce::Colour(0xFFE6EDF3));
        setColour(juce::ComboBox::outlineColourId,           juce::Colour(0xFF30363D));
        setColour(juce::ListBox::backgroundColourId,         juce::Colour(0xFF14171C));
        setColour(juce::TextEditor::backgroundColourId,      juce::Colour(0xFF0D1117));
        setColour(juce::TextEditor::textColourId,            juce::Colour(0xFFE6EDF3));
        setColour(juce::TextEditor::outlineColourId,         juce::Colour(0xFF30363D));
    }

    void drawRotarySlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPosProportional,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider& /*slider*/) override {
        const float radius  = static_cast<float>(std::min(width, height)) * 0.40f;
        const float centreX = static_cast<float>(x) + static_cast<float>(width)  * 0.5f;
        const float centreY = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
        const float rx      = centreX - radius;
        const float ry      = centreY - radius;
        const float rw      = radius * 2.0f;
        const float angle   = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        // Outer body circle
        g.setColour(juce::Colour(0xFF161B22));
        g.fillEllipse(rx, ry, rw, rw);
        g.setColour(juce::Colour(0xFF30363D));
        g.drawEllipse(rx, ry, rw, rw, 1.2f);

        // Background track arc
        juce::Path bgArc;
        bgArc.addCentredArc(centreX, centreY, radius + 3.0f, radius + 3.0f, 0.0f,
                            rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xFF282E37));
        g.strokePath(bgArc, juce::PathStrokeType(2.8f));

        // Active value arc
        juce::Path valArc;
        valArc.addCentredArc(centreX, centreY, radius + 3.0f, radius + 3.0f, 0.0f,
                             rotaryStartAngle, angle, true);
        g.setColour(juce::Colour(0xFF00F5D4));
        g.strokePath(valArc, juce::PathStrokeType(2.8f));

        // Pointer tick
        juce::Path pointer;
        const float pointerLength = radius * 0.72f;
        pointer.addRectangle(-1.2f, -radius, 2.4f, pointerLength);
        pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(centreX, centreY));
        g.setColour(juce::Colour(0xFFE6EDF3));
        g.fillPath(pointer);
    }
};

} // namespace konos
