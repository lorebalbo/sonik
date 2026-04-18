#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "WaveformData.h"
#include "OverviewWaveform.h"
#include "DetailWaveform.h"
#include "../Deck/AudioThreadState.h"
#include "../BeatGrid/BeatGridData.h"
#include "../Cue/HotCueData.h"
#include <array>

class WaveformComponent final : public juce::Component,
                                 private juce::Timer
{
public:
    WaveformComponent()
    {
        addAndMakeVisible (overview);
        addAndMakeVisible (detail);

        // Forward seek from overview to parent
        overview.onSeek = [this] (int64_t pos)
        {
            if (onSeek)
                onSeek (pos);
        };

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

    void setHotCues (const std::array<HotCueInfo, 8>& cues)
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

    void resized() override
    {
        auto bounds = getLocalBounds();

        overview.setBounds (bounds.removeFromTop (overviewHeight));
        bounds.removeFromTop (gap);
        detail.setBounds (bounds);
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

    static constexpr int overviewHeight = 60;
    static constexpr int gap            = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformComponent)
};
