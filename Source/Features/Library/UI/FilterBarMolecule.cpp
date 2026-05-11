#include "FilterBarMolecule.h"
#include "LibraryPalette.h"

// =============================================================================
// SquareToggle
// =============================================================================

void FilterBarMolecule::SquareToggle::paint (juce::Graphics& g)
{
    const auto bg   = active ? LibraryPalette::primary() : LibraryPalette::surface();
    const auto text = active ? LibraryPalette::surface() : LibraryPalette::primary();

    g.fillAll (bg);

    // Suspended overlay: dim the button to indicate "stored but inactive"
    if (active && suspended)
        g.fillAll (LibraryPalette::surface().withAlpha (0.45f));

    g.setColour (LibraryPalette::primary());
    g.drawRect (getLocalBounds(), 1);

    g.setColour (active && suspended ? LibraryPalette::primary().withAlpha (0.5f) : text);
    g.setFont (LibraryPalette::boldLabelFont());
    g.drawText (label, getLocalBounds(), juce::Justification::centred, false);
}

void FilterBarMolecule::SquareToggle::mouseUp (const juce::MouseEvent& e)
{
    if (e.getNumberOfClicks() == 1 && contains (e.position.toInt()) && onClick)
        onClick();
}

// =============================================================================
// FilterBarMolecule
// =============================================================================

FilterBarMolecule::FilterBarMolecule()
{
    // Search bar
    searchBar.onTextChanged = [this] (const juce::String& t)
    {
        if (onSearchChanged) onSearchChanged (t);
    };
    addAndMakeVisible (searchBar);

    // KEY MATCH toggle
    keyMatchBtn.label   = "KEY MATCH";
    keyMatchBtn.active  = false;
    keyMatchBtn.onClick = [this] { toggleKeyMatch(); };
    addAndMakeVisible (keyMatchBtn);

    // BPM MATCH toggle
    bpmMatchBtn.label   = "BPM MATCH";
    bpmMatchBtn.active  = false;
    bpmMatchBtn.onClick = [this] { toggleBpmMatch(); };
    addAndMakeVisible (bpmMatchBtn);

    // HALF TIME toggle (shown only when width >= 900)
    halfTimeBtn.label   = "1/2 BPM";
    halfTimeBtn.active  = false;
    halfTimeBtn.onClick = [this] { toggleHalfTime(); };
    addAndMakeVisible (halfTimeBtn);

    // BPM Vision prefix
    bpmPrefixLabel.setText ("+/-", juce::dontSendNotification);
    bpmPrefixLabel.setFont (juce::Font (LibraryPalette::boldLabelFont()));
    bpmPrefixLabel.setColour (juce::Label::textColourId, LibraryPalette::primary());
    bpmPrefixLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (bpmPrefixLabel);

    // BPM Vision editor (float 0.0–30.0, 1 decimal)
    bpmVisionEditor.setMultiLine (false);
    bpmVisionEditor.setInputRestrictions (5, "0123456789.");
    bpmVisionEditor.setText (juce::String (bpmVision, 1), false);
    bpmVisionEditor.setFont (juce::Font (LibraryPalette::boldLabelFont()));
    bpmVisionEditor.setColour (juce::TextEditor::backgroundColourId,
                                LibraryPalette::surface());
    bpmVisionEditor.setColour (juce::TextEditor::textColourId,
                                LibraryPalette::primary());
    bpmVisionEditor.setColour (juce::TextEditor::outlineColourId,
                                LibraryPalette::primary());
    bpmVisionEditor.setColour (juce::TextEditor::focusedOutlineColourId,
                                LibraryPalette::primary());
    bpmVisionEditor.onReturnKey = [this] { commitBpmVision(); };
    bpmVisionEditor.onFocusLost = [this] { commitBpmVision(); };
    bpmVisionEditor.setJustification (juce::Justification::centred);
    addAndMakeVisible (bpmVisionEditor);

    // BPM Vision suffix
    bpmSuffixLabel.setText ("BPM", juce::dontSendNotification);
    bpmSuffixLabel.setFont (juce::Font (LibraryPalette::boldLabelFont()));
    bpmSuffixLabel.setColour (juce::Label::textColourId, LibraryPalette::primary());
    bpmSuffixLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (bpmSuffixLabel);

    updateBpmVisionOpacity();
}

void FilterBarMolecule::paint (juce::Graphics& g)
{
    g.fillAll (LibraryPalette::surface());
}

void FilterBarMolecule::resized()
{
    auto b = getLocalBounds().reduced (2, 2);
    const bool showExtended = (getWidth() >= 900);

    if (showExtended)
    {
        bpmSuffixLabel.setBounds  (b.removeFromRight (32));
        bpmVisionEditor.setBounds (b.removeFromRight (28));
        bpmPrefixLabel.setBounds  (b.removeFromRight (24));
        b.removeFromRight (4);

        halfTimeBtn.setVisible (true);
        halfTimeBtn.setBounds (b.removeFromRight (72));
        b.removeFromRight (2);
    }
    else
    {
        bpmSuffixLabel.setBounds  ({});
        bpmVisionEditor.setBounds ({});
        bpmPrefixLabel.setBounds  ({});
        halfTimeBtn.setVisible (false);
        halfTimeBtn.setBounds ({});
    }

    bpmMatchBtn.setBounds (b.removeFromRight (80));
    b.removeFromRight (2);
    keyMatchBtn.setBounds (b.removeFromRight (80));
    b.removeFromRight (4);

    searchBar.setBounds (b);
}

// ---- private helpers -------------------------------------------------------

void FilterBarMolecule::toggleKeyMatch()
{
    keyMatchActive     = !keyMatchActive;
    keyMatchBtn.active = keyMatchActive;
    keyMatchBtn.repaint();
    if (onKeyMatchToggled) onKeyMatchToggled (keyMatchActive);
}

void FilterBarMolecule::toggleBpmMatch()
{
    bpmMatchActive     = !bpmMatchActive;
    bpmMatchBtn.active = bpmMatchActive;
    bpmMatchBtn.repaint();
    updateBpmVisionOpacity();
    if (onBpmMatchToggled) onBpmMatchToggled (bpmMatchActive);
}

void FilterBarMolecule::toggleHalfTime()
{
    halfTimeEnabled    = !halfTimeEnabled;
    halfTimeBtn.active = halfTimeEnabled;
    halfTimeBtn.repaint();
    if (onHalfTimeToggled) onHalfTimeToggled (halfTimeEnabled);
}

void FilterBarMolecule::commitBpmVision()
{
    const double val   = juce::jlimit (0.0, 30.0,
                                        bpmVisionEditor.getText().getDoubleValue());
    bpmVision          = val;
    lastValidBpmVision = val;
    bpmVisionEditor.setText (juce::String (val, 1), false);

    if (bpmMatchActive && onBpmVisionChanged)
        onBpmVisionChanged (bpmVision);
}

void FilterBarMolecule::updateBpmVisionOpacity()
{
    const float alpha = bpmMatchActive ? 1.0f : 0.5f;
    bpmVisionEditor.setAlpha (alpha);
    bpmPrefixLabel .setAlpha (alpha);
    bpmSuffixLabel .setAlpha (alpha);
    bpmVisionEditor.setEnabled (bpmMatchActive);
}

// ---- Programmatic setters --------------------------------------------------

void FilterBarMolecule::setKeyMatchActive (bool a)
{
    keyMatchActive     = a;
    keyMatchBtn.active = a;
    keyMatchBtn.repaint();
}

void FilterBarMolecule::setBpmMatchActive (bool a)
{
    bpmMatchActive     = a;
    bpmMatchBtn.active = a;
    bpmMatchBtn.repaint();
    updateBpmVisionOpacity();
}

void FilterBarMolecule::setHalfTimeEnabled (bool e)
{
    halfTimeEnabled    = e;
    halfTimeBtn.active = e;
    halfTimeBtn.repaint();
}

void FilterBarMolecule::setBpmVisionValue (double v)
{
    bpmVision          = v;
    lastValidBpmVision = v;
    bpmVisionEditor.setText (juce::String (v, 1), false);
}

void FilterBarMolecule::setSuspended (bool s)
{
    keyMatchBtn.suspended = s;
    bpmMatchBtn.suspended = s;
    keyMatchBtn.repaint();
    bpmMatchBtn.repaint();
}
