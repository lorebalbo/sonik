#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../DeckIdentifiers.h"

class GainKnobComponent final : public juce::Component,
                                 private juce::ValueTree::Listener
{
public:
    explicit GainKnobComponent (juce::ValueTree deckTree);
    ~GainKnobComponent() override;

    float getNormalizedValue() const;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override;

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void updateFromState();
    void setGainDb (float newDb);
    void commitToState();

    float dbToLinear (float db) const;
    float linearToDb (float linear) const;
    float dbToNormalized (float db) const;
    float normalizedToDb (float norm) const;
    float dbToAngle (float db) const;

    juce::ValueTree tree;

    float gainDb     = 0.0f;
    bool  isDragging = false;
    float dragStartY = 0.0f;
    float dragStartDb = 0.0f;

    static constexpr float minDb          = -60.0f;
    static constexpr float maxDb          =  12.0f;
    static constexpr float defaultDb      =   0.0f;
    static constexpr float wheelIncrement =   0.5f;
    static constexpr float labelHeight    =  14.0f;
    static constexpr float displayHeight  =  16.0f;

    // Arc angles (radians, screen-space: 0 = 3 o'clock, increasing = clockwise)
    // 7 o'clock = 120° CW from 3 = 2π/3;  5 o'clock via 12 = 420° CW = 7π/3
    static constexpr float startAngle = juce::MathConstants<float>::pi * (2.0f / 3.0f);   // 7 o'clock
    static constexpr float endAngle   = juce::MathConstants<float>::pi * (7.0f / 3.0f);   // 5 o'clock (CW through 12)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainKnobComponent)
};
