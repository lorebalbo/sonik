#pragma once
//==============================================================================
// Grouped-tracks mute/solo: MuteSoloButton atom.
//
// A small square toggle carrying a single letter ("M" / "S") that flips one
// boolean property (DawIDs::muted / DawIDs::solo) on its target ValueTree node
// — a track node for the group header, a lane node for a lane header. The
// ValueTree stays the single source of truth: the button writes the property
// and repaints reactively via a listener, so MIDI / session-load / undo paths
// that touch the same property keep the button honest.
//
// DESIGN.md: 2-px ink frame, instant active/inactive fill inversion, zero
// radius, monospace letter, no colour.
//
// Message/UI thread only; no audio-thread code.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace Daw
{

class MuteSoloButton final : public juce::Component,
                             private juce::ValueTree::Listener
{
public:
    MuteSoloButton (juce::String letter, juce::Identifier property)
        : letter_ (std::move (letter)), property_ (std::move (property))
    {
    }

    ~MuteSoloButton() override
    {
        if (target_.isValid())
            target_.removeListener (this);
    }

    void setTargetTree (juce::ValueTree target)
    {
        if (target_ == target)
            return;
        if (target_.isValid())
            target_.removeListener (this);
        target_ = std::move (target);
        if (target_.isValid())
            target_.addListener (this);
        repaint();
    }

    bool isOn() const
    {
        return target_.isValid()
            && static_cast<bool> (target_.getProperty (property_, false));
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();
        const bool on = isOn();

        g.setColour (on ? kInk : kSurface);
        g.fillRect (bounds);
        g.setColour (kInk);
        g.drawRect (bounds, 2);

        g.setColour (on ? kSurface : kInk);
        g.setFont (juce::Font (juce::FontOptions (
            juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::bold)));
        g.drawText (letter_, bounds, juce::Justification::centred, false);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! target_.isValid() || ! getLocalBounds().contains (event.getPosition()))
            return;
        target_.setProperty (property_, ! isOn(), nullptr);
    }

private:
    // The listener receives events for the whole subtree of the target node
    // (a track node hosts lanes/clips), so gate on the exact node + property.
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        if (tree == target_ && property == property_)
            repaint();
    }
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    static inline const juce::Colour kInk     { 0xFF2D2D2D };
    static inline const juce::Colour kSurface { 0xFFFDFDFD };

    juce::String     letter_;
    juce::Identifier property_;
    juce::ValueTree  target_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MuteSoloButton)
};

} // namespace Daw
