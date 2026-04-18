#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "WaveformData.h"
#include "../Deck/AudioThreadState.h"
#include "../BeatGrid/BeatGridData.h"
#include "../Cue/HotCueData.h"
#include <array>

class DetailWaveform final : public juce::Component,
                              private juce::Timer
{
public:
    DetailWaveform();
    ~DetailWaveform() override;

    void setWaveformData (WaveformData::Ptr data);
    void setAudioState (DeckAudioState* state);
    void setTotalSamples (int64_t total);
    void setBeatGridData (BeatGridData::Ptr data);
    void setHotCues (const std::array<HotCueInfo, 8>& cues);

    // Returns the currently visible sample range (for overview viewport)
    void getVisibleRange (int64_t& startSample, int64_t& endSample) const;

    std::function<void (int64_t samplePosition)> onSeek;

    void paint (juce::Graphics& g) override;
    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override;

private:
    void timerCallback() override;
    void rebuildImage();

    WaveformData::Ptr waveformData;
    BeatGridData::Ptr beatGridData;
    DeckAudioState*   audioState   = nullptr;
    int64_t           totalSamples = 0;

    // Hot cue markers (PRD-0012)
    std::array<HotCueInfo, 8> hotCues;

    // Zoom: visible duration in seconds
    int     zoomLevelIndex    = 2; // default: 16s
    float   visibleSeconds    = 16.0f;
    double  sampleRate        = 44100.0;

    // Cached rendering
    juce::Image cachedImage;
    int         cachedWidth       = 0;
    int         cachedHeight      = 0;
    int         cachedZoomIndex   = -1;
    int64_t     cachedCenterSample = -1;

    // Zoom levels in seconds
    static constexpr float zoomLevels[] = { 4.0f, 8.0f, 16.0f, 32.0f, 48.0f, 64.0f };
    static constexpr int   numZoomLevels = 6;
    static constexpr int   timerHz = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DetailWaveform)
};
