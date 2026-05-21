#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "WaveformData.h"
#include "OverviewWaveform.h"
#include "DetailWaveform.h"
#include "../Deck/AudioThreadState.h"
#include "../BeatGrid/BeatGridData.h"
#include "../Cue/HotCueData.h"
#include <array>

/// Small monochrome glyph button used for the +/- zoom controls beside the
/// detail waveform. Local to the waveform feature; renders per DESIGN.md
/// (2px ink border, no radius, Space Mono glyph, full inversion on press).
class WaveformZoomButton final : public juce::Component
{
public:
    explicit WaveformZoomButton (juce::String glyphText)
        : glyph (std::move (glyphText))
    {
        setOpaque (false);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        const juce::Colour ink     { 0xFF2D2D2D };
        const juce::Colour surface { 0xFFFDFDFD };

        const auto bounds = getLocalBounds();
        g.setColour (pressed ? ink : surface);
        g.fillRect (bounds);
        g.setColour (ink);
        g.drawRect (bounds, 2);

        g.setColour (pressed ? surface : ink);
        const float fontHeight = juce::jlimit (8.0f, 14.0f,
                                                static_cast<float> (bounds.getHeight()) * 0.6f);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      fontHeight, juce::Font::plain));
        g.drawText (glyph, bounds, juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        pressed = true;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        pressed = false;
        repaint();
        if (getLocalBounds().contains (e.getPosition()) && onClick)
            onClick();
    }

private:
    juce::String glyph;
    bool pressed { false };
};

class WaveformComponent final : public juce::Component,
                                 private juce::Timer
{
public:
    WaveformComponent()
    {
        addAndMakeVisible (overview);
        addAndMakeVisible (detail);
        addAndMakeVisible (zoomInButton);
        addAndMakeVisible (zoomOutButton);

        // Forward seek from overview to parent
        overview.onSeek = [this] (int64_t pos)
        {
            if (onSeek)
                onSeek (pos);
        };

        // Forward seek from detail to parent (PRD-0016)
        detail.onSeek = [this] (int64_t pos)
        {
            if (onSeek)
                onSeek (pos);
        };

        // Forward scratch lifecycle to parent (PRD-0016 vinyl-style gesture)
        detail.onScratchBegin = [this]
        {
            if (onScratchBegin)
                onScratchBegin();
        };
        detail.onScratchEnd = [this]
        {
            if (onScratchEnd)
                onScratchEnd();
        };

        zoomInButton.onClick  = [this] { detail.zoomIn(); };
        zoomOutButton.onClick = [this] { detail.zoomOut(); };

        startTimerHz (30);
    }

    ~WaveformComponent() override
    {
        stopTimer();
    }

    void setWaveformData (WaveformData::Ptr data)
    {
        overview.setWaveformData (data);
        detail.setWaveformData (data);
    }

    void setAudioState (DeckAudioState* state)
    {
        overview.setAudioState (state);
        detail.setAudioState (state);
    }

    void setBeatGridData (BeatGridData::Ptr data)
    {
        detail.setBeatGridData (std::move (data));
    }

    void setHotCues (const std::array<HotCueInfo, 9>& cues)
    {
        overview.setHotCues (cues);
        detail.setHotCues (cues);
    }

    void setDeckAccentColour (juce::Colour colour)
    {
        overview.setDeckAccentColour (colour);
        detail.setDeckAccentColour (colour);
    }

    std::function<void (int64_t samplePosition)> onSeek;

    /// PRD-0016: Vinyl-style scratch gesture on the detail waveform.
    /// Host wires these to capture/restore transport state across the hold:
    /// touching while playing stops until release; touching while paused does
    /// not start playback.
    std::function<void()> onScratchBegin;
    std::function<void()> onScratchEnd;

    void resized() override
    {
        auto bounds = getLocalBounds();

        overview.setBounds (bounds.removeFromTop (overviewHeight));
        bounds.removeFromTop (gap);

        auto detailArea = bounds;
        detail.setBounds (detailArea);

        // Overlay +/- controls directly on top of the detail waveform (no
        // reserved side column). '+' stays near the top-right corner while
        // '-' is anchored near the bottom-right corner.
        auto overlayArea = detailArea.reduced (2);
        const int x = overlayArea.getRight() - zoomButtonSize - zoomButtonInset;
        zoomInButton.setBounds (x, overlayArea.getY() + zoomButtonInset,
                                zoomButtonSize, zoomButtonSize);
        zoomOutButton.setBounds (x,
                                 overlayArea.getBottom() - zoomButtonSize - zoomButtonInset,
                                 zoomButtonSize, zoomButtonSize);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        const auto b = getLocalBounds();

        g.setColour (juce::Colour (0xFF2D2D2D));

        // Outer border — 2px, matching the empty-state border
        g.drawRect (b, 2);
    }

private:
    void timerCallback() override
    {
        // Update overview's viewport rectangle from detail's visible range
        int64_t start = 0, end = 0;
        detail.getVisibleRange (start, end);
        overview.setVisibleRange (start, end);
    }

    OverviewWaveform overview;
    DetailWaveform   detail;
    WaveformZoomButton zoomInButton  { "+" };
    WaveformZoomButton zoomOutButton { "-" };

    static constexpr int overviewHeight  = 36;  // reduced by 40% (was 60)
    static constexpr int gap             = 2;
    static constexpr int zoomButtonSize  = 18;  // 20% smaller than 22px
    static constexpr int zoomButtonInset = 6;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformComponent)
};
