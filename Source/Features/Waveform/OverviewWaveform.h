#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "WaveformData.h"
#include "../Deck/AudioThreadState.h"
#include "../Cue/HotCueData.h"
#include <array>

class OverviewWaveform final : public juce::Component,
                                private juce::Timer
{
public:
    OverviewWaveform();
    ~OverviewWaveform() override;

    void setWaveformData (WaveformData::Ptr data);
    void setAudioState (DeckAudioState* state);
    void setTotalSamples (int64_t total);
    void setHotCues (const std::array<HotCueInfo, 8>& cues);
    void setDeckAccentColour (juce::Colour colour) { deckAccentColour = colour; }

    // Viewport rectangle showing detail waveform's visible region
    void setVisibleRange (int64_t startSample, int64_t endSample);

    std::function<void (int64_t samplePosition)> onSeek;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    void timerCallback() override;
    void rebuildImage();

    WaveformData::Ptr waveformData;
    DeckAudioState*   audioState    = nullptr;
    int64_t           totalSamples  = 0;
    int64_t           visibleStart  = 0;
    int64_t           visibleEnd    = 0;

    // Hot cue markers (PRD-0012)
    std::array<HotCueInfo, 8> hotCues;

    // Loop overlay colour (PRD-0014)
    juce::Colour deckAccentColour { juce::Colours::white };

    juce::Image cachedImage;
    int         cachedWidth  = 0;
    int         cachedHeight = 0;

    static constexpr int preferredHeight = 60;
    static constexpr int timerHz         = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OverviewWaveform)
};
