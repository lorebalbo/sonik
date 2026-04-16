#include "TrackInfoComponent.h"
#include "../../KeyDetection/KeyUtils.h"

TrackInfoComponent::TrackInfoComponent (juce::ValueTree tree,
                                        DeckStateManager& deckState,
                                        AudioFileLoader& loader,
                                        const juce::String& id)
    : deckTree (tree),
      deckStateManager (deckState),
      audioFileLoader (loader),
      deckId (id)
{
    setOpaque (false);

    deckTree.addListener (this);

    refreshMetadata();
    refreshAlbumArt();

    // Start timer for time display updates
    startTimerHz (60);
}

TrackInfoComponent::~TrackInfoComponent()
{
    stopTimer();
    deckTree.removeListener (this);
}

// --- Metadata refresh ---

void TrackInfoComponent::refreshMetadata()
{
    auto metaTree = deckTree.getChildWithName (IDs::TrackMetadata);
    if (metaTree.isValid())
    {
        cachedTitle      = metaTree.getProperty (IDs::title).toString();
        cachedArtist     = metaTree.getProperty (IDs::artist).toString();
        cachedContentHash = metaTree.getProperty (IDs::contentHash).toString();
        sampleRate       = static_cast<double> (metaTree.getProperty (IDs::sampleRate, 44100.0));
        totalSamples     = static_cast<int64_t> (metaTree.getProperty (IDs::totalSamples, 0));
    }

    auto beatTree = deckTree.getChildWithName (IDs::BeatGrid);
    if (beatTree.isValid())
        baseBpm = static_cast<double> (beatTree.getProperty (IDs::bpm, 0.0));

    auto keyTree = deckTree.getChildWithName (IDs::KeyInfo);
    if (keyTree.isValid())
        keyIndex = static_cast<int> (keyTree.getProperty (IDs::keyIndex, -1));
}

void TrackInfoComponent::refreshAlbumArt()
{
    if (cachedContentHash.isNotEmpty())
        albumArt = audioFileLoader.getAlbumArt (cachedContentHash);
    else
        albumArt = {};
}

// --- ValueTree::Listener ---

void TrackInfoComponent::valueTreePropertyChanged (juce::ValueTree& tree,
                                                    const juce::Identifier& property)
{
    juce::ignoreUnused (property);

    auto name = tree.getType();
    if (name == IDs::TrackMetadata || name == IDs::BeatGrid || name == IDs::KeyInfo)
    {
        refreshMetadata();
        refreshAlbumArt();
        repaint();
    }
}

void TrackInfoComponent::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    juce::ignoreUnused (parent);
    auto name = child.getType();
    if (name == IDs::TrackMetadata || name == IDs::BeatGrid || name == IDs::KeyInfo)
    {
        child.addListener (this);
        refreshMetadata();
        refreshAlbumArt();
        repaint();
    }
}

void TrackInfoComponent::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int index)
{
    juce::ignoreUnused (parent, index);
    auto name = child.getType();
    if (name == IDs::TrackMetadata || name == IDs::BeatGrid || name == IDs::KeyInfo)
    {
        child.removeListener (this);
        refreshMetadata();
        repaint();
    }
}

// --- Timer ---

void TrackInfoComponent::timerCallback()
{
    repaint();
}

// --- Mouse hover for scrolling ---

void TrackInfoComponent::mouseEnter (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    isHovering = true;
    titleScrollOffset  = 0.0f;
    artistScrollOffset = 0.0f;
    scrollPauseTimer   = 0.0f;
    scrollPaused       = false;
}

void TrackInfoComponent::mouseExit (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    isHovering = false;
    titleScrollOffset  = 0.0f;
    artistScrollOffset = 0.0f;
}

// --- Time formatting ---

juce::String TrackInfoComponent::formatTime (double seconds, bool negative) const
{
    if (seconds < 0.0)
        seconds = 0.0;

    int totalSecs = static_cast<int> (seconds);
    int mins = totalSecs / 60;
    int secs = totalSecs % 60;

    auto timeStr = juce::String (mins) + ":" + juce::String (secs).paddedLeft ('0', 2);
    return negative ? ("-" + timeStr) : timeStr;
}

// --- Camelot key ---

juce::String TrackInfoComponent::getCamelotKey (int keyIdx) const
{
    return KeyUtils::toCamelot (keyIdx);
}

// --- Text scrolling ---

float TrackInfoComponent::getScrollOffset (float textWidth, float areaWidth,
                                           float& offset, bool hovering) const
{
    if (! hovering || textWidth <= areaWidth)
        return 0.0f;

    float maxScroll = textWidth - areaWidth;
    float dt = 1.0f / 60.0f; // timer runs at 60Hz

    if (scrollPaused)
        return offset;

    offset += scrollSpeed * dt;

    if (offset >= maxScroll)
    {
        offset = 0.0f; // reset to start after reaching end
    }

    return offset;
}

// --- Paint ---

void TrackInfoComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background
    g.setColour (juce::Colour (0xFFF3F3F4)); // surface-container-low
    g.fillRect (bounds);

    auto contentArea = bounds.reduced (padding);

    // Album art area
    auto artArea = contentArea.removeFromLeft (artSize);
    paintAlbumArt (g, artArea);

    contentArea.removeFromLeft (padding); // gap after art

    // BPM/Key column on the right
    auto rightCol = contentArea.removeFromRight (70);
    paintBpmKeyTime (g, rightCol);

    // Text info in the middle
    paintTextInfo (g, contentArea);
}

void TrackInfoComponent::paintAlbumArt (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto artRect = area.withSizeKeepingCentre (artSize, artSize);

    if (albumArt.isValid())
    {
        g.drawImage (albumArt, artRect.toFloat(),
                     juce::RectanglePlacement::stretchToFit);
    }
    else
    {
        // Placeholder: dark rect with music note symbol
        g.setColour (juce::Colour (0xFF000000));
        g.fillRect (artRect);

        g.setColour (juce::Colour (0xFFF9F9F9));
        g.setFont (juce::FontOptions (28.0f));
        g.drawText (juce::String::charToString (0x266A), artRect, // ♪
                    juce::Justification::centred);
    }
}

void TrackInfoComponent::paintTextInfo (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto titleArea  = area.removeFromTop (area.getHeight() / 3);
    auto artistArea = area.removeFromTop (area.getHeight() / 2);
    auto timeArea   = area;

    // Title
    g.setColour (juce::Colours::black);
    auto titleFont = juce::FontOptions (13.0f).withStyle ("Bold");
    g.setFont (titleFont);

    auto displayTitle = cachedTitle.isEmpty() ? juce::String ("--") : cachedTitle;
    juce::GlyphArrangement titleGlyphs;
    titleGlyphs.addLineOfText (g.getCurrentFont(), displayTitle, 0.0f, 0.0f);
    float titleTextWidth = titleGlyphs.getBoundingBox (0, -1, false).getWidth();
    float titleAreaWidth = static_cast<float> (titleArea.getWidth());

    if (isHovering && titleTextWidth > titleAreaWidth)
    {
        float scrollOff = getScrollOffset (titleTextWidth, titleAreaWidth,
                                           titleScrollOffset, isHovering);
        auto wideRect = titleArea.toFloat().withWidth (titleTextWidth).translated (-scrollOff, 0.0f);
        g.saveState();
        g.reduceClipRegion (titleArea);
        g.drawText (displayTitle, wideRect.toNearestInt(),
                    juce::Justification::centredLeft, false);
        g.restoreState();
    }
    else
    {
        g.drawText (displayTitle, titleArea, juce::Justification::centredLeft, true);
    }

    // Artist
    auto artistFont = juce::FontOptions (11.0f);
    g.setFont (artistFont);
    g.setColour (juce::Colour (0xCC000000));

    auto displayArtist = cachedArtist.isEmpty() ? juce::String ("--") : cachedArtist;
    juce::GlyphArrangement artistGlyphs;
    artistGlyphs.addLineOfText (g.getCurrentFont(), displayArtist, 0.0f, 0.0f);
    float artistTextWidth = artistGlyphs.getBoundingBox (0, -1, false).getWidth();
    float artistAreaWidth = static_cast<float> (artistArea.getWidth());

    if (isHovering && artistTextWidth > artistAreaWidth)
    {
        float scrollOff = getScrollOffset (artistTextWidth, artistAreaWidth,
                                           artistScrollOffset, isHovering);
        auto wideRect = artistArea.toFloat().withWidth (artistTextWidth).translated (-scrollOff, 0.0f);
        g.saveState();
        g.reduceClipRegion (artistArea);
        g.drawText (displayArtist, wideRect.toNearestInt(),
                    juce::Justification::centredLeft, false);
        g.restoreState();
    }
    else
    {
        g.drawText (displayArtist, artistArea, juce::Justification::centredLeft, true);
    }

    // Elapsed / Remaining time
    auto monoFont = juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain);
    g.setFont (monoFont);
    g.setColour (juce::Colours::black);

    auto* audioState = deckStateManager.getAudioState (deckId);
    int64_t playhead = 0;
    if (audioState != nullptr)
        playhead = audioState->playheadPosition.load (std::memory_order_relaxed);

    double elapsed   = (sampleRate > 0.0) ? static_cast<double> (playhead) / sampleRate : 0.0;
    double remaining = (sampleRate > 0.0) ? static_cast<double> (totalSamples - playhead) / sampleRate : 0.0;

    auto elapsedStr   = formatTime (elapsed, false);
    auto remainingStr = formatTime (remaining, true);

    auto elapsedArea  = timeArea.removeFromLeft (timeArea.getWidth() / 2);
    auto remainArea   = timeArea;

    g.drawText (elapsedStr, elapsedArea, juce::Justification::centredLeft, false);
    g.drawText (remainingStr, remainArea, juce::Justification::centredRight, false);
}

void TrackInfoComponent::paintBpmKeyTime (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto monoFont = juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain);
    g.setFont (monoFont);
    g.setColour (juce::Colours::black);

    auto bpmArea = area.removeFromTop (area.getHeight() / 2);
    auto keyArea = area;

    // BPM: originalBPM * speedMultiplier
    auto* audioState = deckStateManager.getAudioState (deckId);
    float speedMul = 1.0f;
    if (audioState != nullptr)
        speedMul = audioState->speedMultiplier.load (std::memory_order_relaxed);

    double displayBpm = baseBpm * static_cast<double> (speedMul);
    juce::String bpmStr;
    if (baseBpm <= 0.0)
        bpmStr = "--";
    else
        bpmStr = juce::String (displayBpm, 1);

    g.drawText (bpmStr, bpmArea, juce::Justification::centredRight, false);

    // Key in Camelot with color indicator
    auto keyStr = getCamelotKey (keyIndex);
    auto keyColour = KeyUtils::getCamelotColour (keyIndex);

    if (keyColour != juce::Colours::transparentBlack)
    {
        // Draw a small color swatch before the key text
        auto swatchSize = 8;
        auto swatchArea = keyArea.removeFromRight (swatchSize + 4);
        auto swatchRect = swatchArea.withSizeKeepingCentre (swatchSize, swatchSize);
        g.setColour (keyColour);
        g.fillRect (swatchRect);
    }

    g.setColour (juce::Colours::black);
    g.drawText (keyStr, keyArea, juce::Justification::centredRight, false);
}

void TrackInfoComponent::resized()
{
    // Painting is done entirely in paint(), no child components to layout.
}
