#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../DeckIdentifiers.h"

class PitchFaderComponent final : public juce::Component,
                                   private juce::ValueTree::Listener
{
public:
    explicit PitchFaderComponent (juce::ValueTree deckTree);
    ~PitchFaderComponent() override;

    float getNormalizedValue() const;
    int   getPitchRange() const { return pitchRange; }
    void  cyclePitchRange();

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
    void setPitchPercent (float newPitch, bool animate = false);
    void commitToState();

    float pitchPercentToNormalized (float pitchPct) const;
    float normalizedToPitchPercent (float norm) const;
    float yToNormalized (float y) const;
    float normalizedToY (float norm) const;
    float getMouseWheelIncrement() const;

    juce::Rectangle<int> getTrackArea() const;
    juce::Rectangle<int> getHandleArea() const;

    juce::ValueTree tree;

    float pitchPercent   = 0.0f;
    int   pitchRange     = 8;
    bool  isDragging     = false;
    float dragStartY     = 0.0f;
    float dragStartPitch = 0.0f;

    bool  isAnimating    = false;
    float animTarget     = 0.0f;

    static constexpr float deadZone        = 0.10f;
    static constexpr int   handleHeight    = 16;   // req 5: shorter handle
    static constexpr int   trackMarginTop  = handleHeight / 2; // half handle height so top isn't clipped
    static constexpr int   trackMarginBot  = 31;   // 8px gap + 23px range button

    static const std::array<int, 4> ranges;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchFaderComponent)
};
