#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "WaveformData.h"
#include "../Deck/AudioThreadState.h"
#include "../Cue/HotCueData.h"
#include "../Quantize/QuantizeService.h"
#include <array>
#include <atomic>

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

    /// Converts a local pixel X coordinate to an absolute sample position.
    int64_t pixelXToSamplePosition (float pixelX) const;

    std::function<void (int64_t samplePosition)> onSeek;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseEnter (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override;

private:
    void timerCallback() override;
    void rebuildImage();
    void handleSeekAt (const juce::MouseEvent& e);
    void paintTooltip (juce::Graphics& g);
    juce::String formatTime (int64_t samplePos) const;

    WaveformData::Ptr waveformData;
    DeckAudioState*   audioState    = nullptr;
    int64_t           totalSamples  = 0;
    int64_t           visibleStart  = 0;
    int64_t           visibleEnd    = 0;

    // Hot cue markers (PRD-0012)
    std::array<HotCueInfo, 8> hotCues;

    // Loop overlay colour (PRD-0014)
    juce::Colour deckAccentColour { juce::Colours::black };

    juce::Image cachedImage;
    int         cachedWidth  = 0;
    int         cachedHeight = 0;

    // Tooltip state (PRD-0016)
    bool    showTooltip     = false;
    float   tooltipPixelX   = 0.0f;
    int64_t tooltipSample   = 0;

    // Drag state (PRD-0016)
    bool    isDragging      = false;

    static constexpr int preferredHeight = 60;
    static constexpr int timerHz         = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OverviewWaveform)
};
