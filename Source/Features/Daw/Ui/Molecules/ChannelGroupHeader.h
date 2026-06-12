#pragma once
//==============================================================================
// PRD-0067 (revised): ChannelGroupHeader molecule — the Logic-style track header.
//
// Two stacked rows inside the left gutter:
//   row 1 — all-caps Space Mono deck label (e.g. "DECK 1") + the group
//           collapse/expand toggle.
//   row 2 — a horizontal channel VOLUME fader writing the authoritative mixer
//           channel `fader` property (the same property the mixer strip and
//           MIDI faders drive, observed by the engine bridge), plus the
//           automation-parameter dropdown that picks which automation lane is
//           shown beneath the track (Logic's track-header automation selector).
//
// DESIGN.md: fader track on surface-container-highest with a solid ink block
// cap; every button is a 2-px ink frame with instant active/inactive fill
// inversion; zero radius; no colour.
//
// Message/UI thread only; observes the mixer channel node via a
// juce::ValueTree::Listener (no polling); no audio-thread code.
//==============================================================================

#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../DawLayoutMetrics.h"
#include "../Atoms/PixelIcons.h"
#include "../Atoms/MuteSoloButton.h"
#include "../../State/DawState.h"
#include "../../../Mixer/State/MixerIdentifiers.h"

namespace Daw
{

class ChannelGroupHeader final : public juce::Component,
                                 private juce::ValueTree::Listener,
                                 private juce::Timer
{
public:
    explicit ChannelGroupHeader (int deckIndex) : deckIndex_ (deckIndex)
    {
        addAndMakeVisible (muteButton_);
        addAndMakeVisible (soloButton_);
    }

    ~ChannelGroupHeader() override
    {
        if (channelTree_.isValid())
            channelTree_.removeListener (this);
    }

    // Fired when the collapse toggle is clicked. The owner flips collapse state.
    std::function<void()> onToggleCollapsed;

    // Fired when the automation-parameter dropdown is clicked. The owner (the
    // ChannelGroupView) shows the parameter menu and applies the selection.
    std::function<void()> onAutomationDropdown;

    // Grouped-tracks mute/solo: the M / S toggles write the group-level flags
    // directly onto the track node (the daw.tracks[i] single source of truth).
    void setTrackTree (juce::ValueTree trackTree)
    {
        muteButton_.setTargetTree (trackTree);
        soloButton_.setTargetTree (std::move (trackTree));
    }

    //--------------------------------------------------------------------------
    // Fader level meter. The provider returns this deck/channel's current
    // linear peak amplitude (live deck and/or arrangement playback, resolved by
    // the host); the header polls it at 30 Hz — the PRD-0058 atomics pattern,
    // never the ValueTree — and draws a dithered bar inside the fader track.
    //--------------------------------------------------------------------------
    void setLevelProvider (std::function<float()> provider)
    {
        levelProvider_ = std::move (provider);
        if (levelProvider_)
            startTimerHz (30);
        else
        {
            stopTimer();
            levelFraction_ = 0.0f;
            repaint (faderBounds_.expanded (2, 4));
        }
    }

    // Test hook: the currently displayed level fraction [0,1].
    float getLevelFractionForTests() const noexcept { return levelFraction_; }

    //--------------------------------------------------------------------------
    // Mixer channel wiring (volume fader). The header reads/writes the channel
    // node's `fader` property [0,1] — the same single source of truth the mixer
    // strip drives — and repaints reactively when anything else moves it.
    //--------------------------------------------------------------------------
    void setMixerChannelTree (juce::ValueTree channelTree)
    {
        if (channelTree_ == channelTree)
            return;
        if (channelTree_.isValid())
            channelTree_.removeListener (this);
        channelTree_ = std::move (channelTree);
        if (channelTree_.isValid())
            channelTree_.addListener (this);
        repaint();
    }

    float getFaderValue() const
    {
        if (! channelTree_.isValid())
            return 1.0f;
        return juce::jlimit (0.0f, 1.0f,
                             static_cast<float> (channelTree_.getProperty (MixerIDs::fader, 1.0f)));
    }

    void setCollapsed (bool isCollapsed)
    {
        if (collapsed_ == isCollapsed)
            return;
        collapsed_ = isCollapsed;
        repaint();
    }

    bool isCollapsed() const noexcept { return collapsed_; }

    // The label shown inside the automation dropdown ("OFF" or the selected
    // parameter, e.g. "GAIN"), plus the revealed state for fill inversion.
    void setAutomationDisplay (const juce::String& label, bool revealed)
    {
        if (autoLabel_ == label && automationRevealed_ == revealed)
            return;
        autoLabel_ = label;
        automationRevealed_ = revealed;
        repaint();
    }

    bool isAutomationRevealed() const noexcept { return automationRevealed_; }

    static juce::String labelForDeck (int deckIndex)
    {
        return "DECK " + juce::String (deckIndex + 1);
    }

    void resized() override
    {
        const int gutterW = DawLayout::kTrackHeaderWidth;
        const int rowH    = DawLayout::kGroupHeaderHeight / 2;

        // Row 1: collapse toggle flush with the gutter's right padding, with the
        // group M / S toggles stacked to its left.
        toggleBounds_ = juce::Rectangle<int> (gutterW - 30, 5, 20, 18);
        soloButton_.setBounds (gutterW - 30 - 22, 5, 18, 18);
        muteButton_.setBounds (gutterW - 30 - 44, 5, 18, 18);

        // Row 2: volume fader left, automation dropdown filling the remainder.
        const int row2Y = rowH;
        faderBounds_ = juce::Rectangle<int> (12, row2Y + (rowH - 10) / 2 - 1, 84, 10);
        autoBounds_  = juce::Rectangle<int> (faderBounds_.getRight() + 8,
                                             row2Y + (rowH - 20) / 2 - 1,
                                             gutterW - faderBounds_.getRight() - 8 - 10,
                                             20);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        const int gutterW = DawLayout::kTrackHeaderWidth;

        // Flat track-header band (Logic): header tone across the full row, a
        // 2-px ink top edge marking the track boundary, and the track-header
        // column's 2-px right edge — no boxed cells.
        g.setColour (kHeaderFill);
        g.fillRect (bounds);
        g.setColour (kInk);
        g.fillRect (bounds.getX(), bounds.getY(), bounds.getWidth(), 2);
        g.fillRect (gutterW - 2, bounds.getY(), 2, bounds.getHeight());

        // Row 1 — track name (trimmed clear of the M / S / collapse cluster).
        g.setColour (kInk);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::bold)));
        g.drawText (labelForDeck (deckIndex_),
                    juce::Rectangle<int> (12, 2, gutterW - 90,
                                          DawLayout::kGroupHeaderHeight / 2 - 2),
                    juce::Justification::centredLeft, false);

        // Collapse / expand toggle (chevron, DESIGN.md frame).
        g.setColour (kSurface);
        g.fillRect (toggleBounds_);
        g.setColour (kInk);
        g.drawRect (toggleBounds_, 2);
        if (collapsed_)
            PixelIcons::drawChevronDown (g, toggleBounds_);
        else
            PixelIcons::drawChevronUp (g, toggleBounds_);

        // Row 2 — volume fader (thin track, solid ink block cap).
        paintFader (g);

        // Automation parameter dropdown — fill inversion when a lane is shown.
        g.setColour (automationRevealed_ ? kInk : kSurface);
        g.fillRect (autoBounds_);
        g.setColour (kInk);
        g.drawRect (autoBounds_, 2);
        g.setColour (automationRevealed_ ? kSurface : kInk);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 8.0f, juce::Font::bold)));
        g.drawText (autoLabel_, autoBounds_.withTrimmedLeft (5).withTrimmedRight (12),
                    juce::Justification::centredLeft, false);
        g.setColour (automationRevealed_ ? kSurface : kInk);
        PixelIcons::drawDropdownCaret (g, autoBounds_.withTrimmedLeft (autoBounds_.getWidth() - 14));
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (faderBounds_.contains (event.getPosition()))
        {
            draggingFader_ = true;
            applyFaderFromX (event.x);
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (draggingFader_)
            applyFaderFromX (event.x);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (draggingFader_)
        {
            draggingFader_ = false;
            return;
        }

        if (toggleBounds_.contains (event.getPosition()) && onToggleCollapsed)
            onToggleCollapsed();
        else if (autoBounds_.contains (event.getPosition()) && onAutomationDropdown)
            onAutomationDropdown();
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        // Double-click the fader resets the channel volume to full open (the
        // schema default), matching the mixer strip's reset gesture.
        if (faderBounds_.contains (event.getPosition()) && channelTree_.isValid())
            channelTree_.setProperty (MixerIDs::fader, 1.0f, nullptr);
    }

    // Exposed for tests: the fader hit area inside the header.
    juce::Rectangle<int> getFaderBounds() const noexcept { return faderBounds_; }

private:
    void applyFaderFromX (int x)
    {
        if (! channelTree_.isValid())
            return;
        const float norm = juce::jlimit (0.0f, 1.0f,
            static_cast<float> (x - faderBounds_.getX())
                / static_cast<float> (juce::jmax (1, faderBounds_.getWidth())));
        channelTree_.setProperty (MixerIDs::fader, norm, nullptr);
    }

    void paintFader (juce::Graphics& g)
    {
        // Thin recessed track with a 1-px ink outline (a fader is not a button,
        // so it carries the slim instrument-panel frame instead of the 2-px
        // button border), and a massive solid ink block cap (DESIGN.md fader).
        const auto track = faderBounds_.reduced (0, 3);
        g.setColour (kSurface);
        g.fillRect (track);
        g.setColour (kInk);
        g.drawRect (track, 1);

        // Level meter: a dithered checkerboard fill rising from the left to
        // the polled channel level (DESIGN.md dithered pattern — no gradient,
        // no colour), inside the track so the cap still rides on top.
        const auto inner  = track.reduced (1);
        const int  levelW = juce::roundToInt (levelFraction_
                                              * static_cast<float> (inner.getWidth()));
        if (levelW > 0)
        {
            g.setColour (kInk);
            const auto fill = inner.withWidth (juce::jmin (levelW, inner.getWidth()));
            for (int y = fill.getY(); y < fill.getBottom(); ++y)
                for (int x = fill.getX() + (y & 1); x < fill.getRight(); x += 2)
                    g.fillRect (x, y, 1, 1);
        }

        const int capW = 8;
        const int maxX = faderBounds_.getRight() - capW;
        const int capX = faderBounds_.getX()
                       + juce::roundToInt (getFaderValue()
                                           * static_cast<float> (maxX - faderBounds_.getX()));
        g.fillRect (juce::jlimit (faderBounds_.getX(), maxX, capX),
                    faderBounds_.getY() - 3, capW, faderBounds_.getHeight() + 6);
    }

    // 30 Hz meter poll: linear peak → dB → bar fraction over [-60, 0] dBFS
    // (the MixLevelMeter window). Repaints only the fader cell, only on change.
    void timerCallback() override
    {
        if (! levelProvider_)
            return;
        const float fraction = fractionForLevel (levelProvider_());
        if (std::abs (fraction - levelFraction_) < 0.004f)
            return;
        levelFraction_ = fraction;
        repaint (faderBounds_.expanded (2, 4));
    }

    static float fractionForLevel (float linear)
    {
        if (linear <= 0.0f)
            return 0.0f;
        const float db = 20.0f * std::log10 (linear);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    }

    // ValueTree::Listener — any channel change may have moved the fader.
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { repaint(); }
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    static inline const juce::Colour kInk        { 0xFF2D2D2D };
    static inline const juce::Colour kSurface    { 0xFFFDFDFD };
    static inline const juce::Colour kHeaderFill { 0xFFE5E5E5 };
    static inline const juce::Colour kTrackFill  { 0xFFE2E2E2 }; // container-highest

    int                  deckIndex_;
    bool                 collapsed_ { false };
    bool                 automationRevealed_ { false };
    bool                 draggingFader_ { false };
    juce::String         autoLabel_ { "OFF" };
    MuteSoloButton       muteButton_ { "M", DawIDs::muted };
    MuteSoloButton       soloButton_ { "S", DawIDs::solo };
    std::function<float()> levelProvider_;
    float                levelFraction_ { 0.0f };
    juce::ValueTree      channelTree_;
    juce::Rectangle<int> toggleBounds_;
    juce::Rectangle<int> faderBounds_;
    juce::Rectangle<int> autoBounds_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelGroupHeader)
};

} // namespace Daw
