#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../DeckIdentifiers.h"
#include "../DeckStateManager.h"
#include "../../AudioEngine/AudioFileLoader.h"

class TrackInfoComponent final : public juce::Component,
                                  private juce::ValueTree::Listener,
                                  private juce::Timer
{
public:
    TrackInfoComponent (juce::ValueTree deckTree,
                        DeckStateManager& deckState,
                        AudioFileLoader& loader,
                        const juce::String& deckId);
    ~TrackInfoComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

private:
    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;
    void valueTreeChildAdded (juce::ValueTree& parent,
                              juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent,
                                juce::ValueTree& child,
                                int index) override;

    // Timer
    void timerCallback() override;

    // Helpers
    void refreshMetadata();
    void refreshAlbumArt();
    juce::String getCamelotKey (int keyIdx) const;
    float getScrollOffset (float textWidth, float areaWidth, float& offset, bool hovering) const;

    void paintAlbumArt    (juce::Graphics& g, juce::Rectangle<int> area);
    void paintTextInfo    (juce::Graphics& g, juce::Rectangle<int> area);
    void paintBpmKeyInfo  (juce::Graphics& g, juce::Rectangle<int> area);
    void paintDeckBadge   (juce::Graphics& g, juce::Rectangle<int> area);

    juce::ValueTree   deckTree;
    DeckStateManager& deckStateManager;
    AudioFileLoader&  audioFileLoader;
    juce::String      deckId;

    // Cached metadata
    juce::String cachedTitle;
    juce::String cachedArtist;
    juce::String cachedAlbum;
    juce::String cachedContentHash;
    juce::Image  albumArt;
    double       sampleRate   = 44100.0;
    int64_t      totalSamples = 0;
    double       baseBpm      = 0.0;
    int          keyIndex     = -1;

    // Scrolling state (title)
    bool  isHovering         = false;
    float titleScrollOffset  = 0.0f;
    float artistScrollOffset = 0.0f;
    float scrollPauseTimer   = 0.0f;
    bool  scrollPaused       = false;

    static constexpr float scrollSpeed   = 50.0f;  // px/sec
    static constexpr float pauseDuration = 1.0f;   // seconds

    // Layout constants matching Figma Deck Header
    static constexpr int artWidth    = 70;   // art column width (matches Figma 70x59)
    static constexpr int badgeWidth  = 70;   // deck identifier badge width
    static constexpr int bpmColWidth = 48;   // BPM/key numbers column
    static constexpr int colGap      = 8;    // gap between columns

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackInfoComponent)
};
