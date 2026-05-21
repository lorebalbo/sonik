#pragma once
//==============================================================================
// PRD-0059: MixRotaryKnob — a reusable mixer UI atom.
//
// A pixel-art rotary knob bound to a single juce::ValueTree property.
// Strict DESIGN.md compliance: monochrome #2d2d2d / #fdfdfd palette, Space
// Mono Regular typography, 2-px borders, zero border-radius, a 1-px white
// centre-detent tick mark, no gradients (a dithered "lifted" face suggests
// depth without breaking the 1-bit aesthetic).
//
// Interaction model (PRD-0059 §1.5.1 / §1.5.5):
//   • Vertical click-drag (default; arc-drag explicitly rejected by PRD)
//   • Mouse wheel = step adjustment
//   • Shift = 4× finer drag sensitivity
//   • Double-click = reset to declared default
//   • Bipolar normalisation snaps the value to centre when within deadzone
//
// Threading:
//   • Reads / writes happen exclusively on the message thread.
//   • A juce::ValueTree::Listener (added at construction, removed at
//     destruction) keeps the visual in sync with external writers (MIDI,
//     other UIs).
//   • No audio-thread interaction — see PRD-0059 §1.2 / AGENTS.md.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>

class MixRotaryKnob final : public juce::Component,
                             private juce::ValueTree::Listener
{
public:
    // ── Normalisation taper ─────────────────────────────────────────────
    enum class Normalisation
    {
        Linear,      // value ∈ [min, max]; default at "defaultValue"
        DbTapered,   // value is dB; piecewise: norm 0..0.5 → minDb..0 dB,
                     //                          norm 0.5..1 → 0..maxDb
        Bipolar      // value ∈ [-1, +1]; centre detent at 0, deadzone snap
    };

    // ── Configuration POD ───────────────────────────────────────────────
    struct Config
    {
        juce::String label          { "PARAM" };
        Normalisation taper         { Normalisation::Linear };
        float minValue              { 0.0f };
        float maxValue              { 1.0f };
        float defaultValue          { 0.0f };
        float wheelIncrement        { 0.05f };
        float bipolarDeadzone       { 0.05f };          // only used for Bipolar
        // When true, the knob skips the top label band and bottom value
        // display band so the rotary circle fills the entire bounds.
        // Intended for compact contexts like the global toolbar where
        // there is no vertical room for chrome.
        bool  compact               { false };
        // When true (and compact is false), skip the top label band but
        // reserve a bottom band showing config.label as plain text.
        // Used by mixer channel strips where the name sits below the knob.
        bool  showBottomLabel       { false };
        // Optional value formatter for the small label below the knob.
        // Defaults to a per-taper formatter set in the constructor.
        std::function<juce::String (float)> formatter;
    };

    MixRotaryKnob (juce::ValueTree boundTree,
                   juce::Identifier propertyId,
                   Config           cfg);

    ~MixRotaryKnob() override;

    // ── Programmatic API (testable / used by callers) ───────────────────
    float getValue() const noexcept              { return currentValue; }
    void  setValue (float newValue);
    void  resetToDefault();

    // Optional: bind a "kill" boolean. When set, the small bottom label is
    // replaced with "KILL" while the boolean is true. Pass a default
    // Identifier (i.e. {}) to disable.
    void  setKillIndicatorBinding (juce::Identifier killBoolId);

    // ── Component overrides ─────────────────────────────────────────────
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown        (const juce::MouseEvent& e) override;
    void mouseDrag        (const juce::MouseEvent& e) override;
    void mouseUp          (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove   (const juce::MouseEvent& e,
                           const juce::MouseWheelDetails& wheel) override;

    // ── Visual constants ────────────────────────────────────────────────
    static constexpr int kMinimumSize   = 24;   // PRD §1.4 minimum
    static constexpr int kLabelHeight   = 12;
    static constexpr int kDisplayHeight = 12;
    static constexpr int kLabelBandH    = 14;   // bottom band in showBottomLabel mode

private:
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;

    void updateFromTree();
    void commitToTree();

    float normalise (float value) const noexcept;
    float denormalise (float norm) const noexcept;
    float angleFor (float value) const noexcept;
    juce::String defaultFormat (float value) const;

    // Knob arc: 7 o'clock → 5 o'clock through 12 (240° sweep, CW).
    static constexpr float kStartAngle =
        juce::MathConstants<float>::pi * (2.0f / 3.0f);   // 7 o'clock
    static constexpr float kEndAngle   =
        juce::MathConstants<float>::pi * (7.0f / 3.0f);   // 5 o'clock CW

    juce::ValueTree   tree;
    juce::Identifier  propertyId;
    juce::Identifier  killBoolId;          // empty by default
    Config            config;

    float currentValue = 0.0f;
    bool  isDragging   = false;
    float dragStartY   = 0.0f;
    float dragStartVal = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixRotaryKnob)
};
