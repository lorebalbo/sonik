#pragma once
//==============================================================================
// PRD-0060: MixFader — generalised linear fader atom for the mixer.
//
// Used for:
//   • Per-channel volume fader (vertical, range [0, 1], default 1, no detent).
//   • Crossfader (horizontal, range [0, 1], default 0.5, detent at 0.5).
//
// PRD-0010's PitchFaderComponent is intentionally not reused here: that
// component is hard-coded to deck pitch semantics (CDJ "down = faster"
// inversion, percent range, pitch-range cycling button). Generalising it
// would invert its meaning at every call site. MixFader is the mixer-feature
// counterpart with explicit orientation and detent contracts (PRD-0060
// §1.5.6).
//
// DESIGN.md compliance:
//   • Monochrome ink / surface palette.
//   • Track painted from #2D2D2D fill on #E2E2E2 surface-container-highest
//     chassis (vertical) or vice-versa for the cap.
//   • The fader cap is a massive, solid #2D2D2D block with a 1-px white
//     centre stripe and a +3 px drop shadow (matches PitchFader style).
//   • 2-px ink border on the chassis. Zero border-radius. Pixel-art ticks.
//
// Binding:
//   • One ValueTree property (float in [minValue, maxValue]).
//   • juce::ValueTree::Listener for external writes (MIDI, other UIs).
//   • No audio-thread interaction.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <optional>

class MixFader final : public juce::Component,
                        private juce::ValueTree::Listener
{
public:
    enum class Orientation { Vertical, Horizontal };

    struct Config
    {
        Orientation orientation { Orientation::Vertical };
        float       minValue    { 0.0f };
        float       maxValue    { 1.0f };
        float       defaultValue { 1.0f };
        // Snap-to value (e.g. 0.5 for crossfader centre). std::nullopt = no detent.
        std::optional<float> detentValue {};
        float       detentDeadzone { 0.015f }; // ±1.5% of full range
        // For vertical faders, set true so dragging DOWN decreases value (default).
        // PRD-0010's deck PitchFader inverts this — but MixFader is for the
        // channel volume fader where "up = louder" is the convention.
        bool        invertVertical { false };
    };

    MixFader (juce::ValueTree boundTree,
              juce::Identifier propertyId,
              Config           cfg);

    ~MixFader() override;

    float getValue() const noexcept { return currentValue; }
    void  setValue (float newValue);
    void  resetToDefault();

    // Component overrides
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override;

    // Visual constants
    static constexpr int kCapThickness = 16;   // along the fader axis
    static constexpr int kTrackPad     = 4;    // padding from chassis edges

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void  commitToTree();
    void  readFromTree();
    float clampAndDetent (float v) const noexcept;
    float normalise (float v) const noexcept;
    float denormalise (float n) const noexcept;
    juce::Rectangle<int> getTrackArea() const;
    juce::Rectangle<int> getCapArea() const;

    juce::ValueTree  tree;
    juce::Identifier propertyId;
    Config           config;
    float            currentValue { 0.0f };

    bool  isDragging   = false;
    float dragStartPos = 0.0f;
    float dragStartVal = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixFader)
};
