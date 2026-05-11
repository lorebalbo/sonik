#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "SearchBarAtom.h"

/// Molecule: fixed-height (40 px) bar containing SearchBarAtom, KEY MATCH
/// toggle, BPM MATCH toggle, BPM VISION float input, and half-time toggle.
/// All toggle state is binary-inversion only; no animations.
class FilterBarMolecule final : public juce::Component
{
public:
    std::function<void (const juce::String&)> onSearchChanged;
    std::function<void (bool)>                onKeyMatchToggled;
    std::function<void (bool)>                onBpmMatchToggled;
    std::function<void (double)>              onBpmVisionChanged;
    std::function<void (bool)>                onHalfTimeToggled;

    FilterBarMolecule();

    void paint   (juce::Graphics& g) override;
    void resized () override;

    void focusSearchBar () { searchBar.grabFocus(); }
    void clearSearchBar () { searchBar.clear(); }

    bool   isKeyMatchActive  () const { return keyMatchActive;  }
    bool   isBpmMatchActive  () const { return bpmMatchActive;  }
    bool   isHalfTimeEnabled () const { return halfTimeEnabled; }
    double getBpmVision      () const { return bpmVision;       }
    juce::String getText     () const { return searchBar.getText(); }

    // Programmatic state setters (do NOT fire callbacks)
    void setKeyMatchActive  (bool active);
    void setBpmMatchActive  (bool active);
    void setHalfTimeEnabled (bool enabled);
    void setBpmVisionValue  (double value);

    // Suspended: visually marks active toggles as stored-but-inactive
    void setSuspended (bool suspended);

private:
    // ---- Inner square-button atom (zero border-radius) ---------------------
    class SquareToggle final : public juce::Component
    {
    public:
        juce::String          label;
        bool                  active    = false;
        bool                  suspended = false;
        std::function<void()> onClick;

        void paint   (juce::Graphics& g) override;
        void mouseUp (const juce::MouseEvent& e) override;
    };

    void toggleKeyMatch         ();
    void toggleBpmMatch         ();
    void toggleHalfTime         ();
    void commitBpmVision        ();
    void updateBpmVisionOpacity ();

    SearchBarAtom    searchBar;
    SquareToggle     keyMatchBtn;
    SquareToggle     bpmMatchBtn;
    SquareToggle     halfTimeBtn;

    juce::Label      bpmPrefixLabel;
    juce::TextEditor bpmVisionEditor;
    juce::Label      bpmSuffixLabel;

    bool   keyMatchActive     = false;
    bool   bpmMatchActive     = false;
    bool   halfTimeEnabled    = false;
    double bpmVision          = 6.0;
    double lastValidBpmVision = 6.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterBarMolecule)
};
