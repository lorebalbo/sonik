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
    g.drawRect (getLocalBounds(), 2);

    g.setColour (active && suspended ? LibraryPalette::primary().withAlpha (0.5f) : text);
    g.setFont (LibraryPalette::bodyFont (13.0f));
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

    // BPM Vision: "±" chip (inverted, filled #2d2d2d, text #fdfdfd), no gap to editor.
    bpmPrefixLabel.setText (juce::CharPointer_UTF8 ("\xc2\xb1"), juce::dontSendNotification);
    bpmPrefixLabel.setFont (juce::Font (LibraryPalette::bodyFont (13.0f)));
    bpmPrefixLabel.setColour (juce::Label::backgroundColourId, LibraryPalette::primary());
    bpmPrefixLabel.setColour (juce::Label::textColourId,       LibraryPalette::surface());
    bpmPrefixLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (bpmPrefixLabel);

    // BPM Vision editor (float 0.0–30.0, 1 decimal)
    bpmVisionEditor.setMultiLine (false);
    bpmVisionEditor.setInputRestrictions (5, "0123456789.");
    bpmVisionEditor.setText (juce::String (bpmVision, 1), false);
    bpmVisionEditor.setFont (juce::Font (LibraryPalette::bodyFont (13.0f)));
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

    // BPM Vision: "BPM" chip (inverted, filled #2d2d2d, text #fdfdfd), no gap to editor.
    bpmSuffixLabel.setText ("BPM", juce::dontSendNotification);
    bpmSuffixLabel.setFont (juce::Font (LibraryPalette::bodyFont (13.0f)));
    bpmSuffixLabel.setColour (juce::Label::backgroundColourId, LibraryPalette::primary());
    bpmSuffixLabel.setColour (juce::Label::textColourId,       LibraryPalette::surface());
    bpmSuffixLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (bpmSuffixLabel);

    updateBpmVisionOpacity();
}

void FilterBarMolecule::paint (juce::Graphics& g)
{
    // Transparent gaps between children must read as the outer chassis colour
    // (#e5e5e5), so the filter bar paints the chassis fill rather than #fdfdfd.
    g.fillAll (LibraryPalette::chassis());
}

void FilterBarMolecule::paintOverChildren (juce::Graphics& g)
{
    // 2px #2d2d2d border around the BPM Vision editor, dimmed when BPM MATCH
    // is inactive (same alpha used by the editor/chip children).
    const float alpha = bpmMatchActive ? 1.0f : 0.5f;
    g.setColour (LibraryPalette::primary().withAlpha (alpha));
    g.drawRect (bpmVisionEditor.getBounds(), 2);
}

void FilterBarMolecule::resized()
{
    // No outer frame padding: children fill the bar edge-to-edge.
    auto b = getLocalBounds();
    constexpr int kGap     = 12;
    constexpr int kPrefixW = 22;
    constexpr int kEditorW = 48;
    constexpr int kSuffixW = 36;

    // ---- BPM Vision group (right side) -------------------------------------
    // [ ± ][ editor ][ BPM ]   — no gap between elements within the group.
    auto visionGroup = b.removeFromRight (kPrefixW + kEditorW + kSuffixW);
    bpmSuffixLabel .setBounds (visionGroup.removeFromRight  (kSuffixW));
    bpmVisionEditor.setBounds (visionGroup.removeFromRight  (kEditorW));
    bpmPrefixLabel .setBounds (visionGroup.removeFromRight  (kPrefixW));

    // ---- Toggles ------------------------------------------------------------
    b.removeFromRight (kGap);
    bpmMatchBtn.setBounds (b.removeFromRight (88));
    b.removeFromRight (kGap);
    keyMatchBtn.setBounds (b.removeFromRight (88));
    b.removeFromRight (kGap);

    // ---- Search bar fills remaining space ----------------------------------
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
    repaint(); // refresh the paintOverChildren border alpha
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
