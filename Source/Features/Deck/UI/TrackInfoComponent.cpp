#include "TrackInfoComponent.h"
#include "../../KeyDetection/KeyUtils.h"
#include <cmath>

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
        cachedTitle       = metaTree.getProperty (IDs::title).toString();
        cachedArtist      = metaTree.getProperty (IDs::artist).toString();
        cachedAlbum       = metaTree.getProperty (IDs::album).toString();
        cachedContentHash = metaTree.getProperty (IDs::contentHash).toString();
        sampleRate        = static_cast<double> (metaTree.getProperty (IDs::sampleRate, 44100.0));
        totalSamples      = static_cast<int64_t> (metaTree.getProperty (IDs::totalSamples, 0));
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
    // Keep pitch UI state aligned with the real synced speed.
    if (static_cast<bool> (deckTree.getProperty (IDs::isSynced, false)))
    {
        if (auto* audioState = deckStateManager.getAudioState (deckId))
        {
            const float syncedSpeed = audioState->speedMultiplier.load (std::memory_order_relaxed);
            const float syncedPitch = (syncedSpeed - 1.0f) * 100.0f;

            const float treeSpeed = static_cast<float> (deckTree.getProperty (IDs::speedMultiplier, 1.0f));
            const float treePitch = static_cast<float> (deckTree.getProperty (IDs::pitch, 0.0f));

            constexpr float speedEpsilon = 0.0001f;
            constexpr float pitchEpsilon = 0.001f;

            if (std::abs (treeSpeed - syncedSpeed) > speedEpsilon)
                deckTree.setProperty (IDs::speedMultiplier, syncedSpeed, nullptr);

            if (std::abs (treePitch - syncedPitch) > pitchEpsilon)
                deckTree.setProperty (IDs::pitch, syncedPitch, nullptr);
        }
    }

    repaint();
}

// --- Mouse hover for scrolling ---

void TrackInfoComponent::mouseEnter (const juce::MouseEvent& e)
{
    isHovering = true;
    titleScrollOffset  = 0.0f;
    artistScrollOffset = 0.0f;
    scrollPauseTimer   = 0.0f;
    scrollPaused       = false;

    // Check if already hovering over badge
    auto bounds = getLocalBounds();
    auto badgeArea = bounds.removeFromRight (badgeWidth);
    bool newBadgeHovered = canRemoveDeck() && badgeArea.contains (e.getPosition());
    if (newBadgeHovered != badgeHovered)
    {
        badgeHovered = newBadgeHovered;
        repaint();
    }
}

void TrackInfoComponent::mouseMove (const juce::MouseEvent& e)
{
    auto bounds = getLocalBounds();
    auto badgeArea = bounds.removeFromRight (badgeWidth);
    bool newBadgeHovered = canRemoveDeck() && badgeArea.contains (e.getPosition());
    if (newBadgeHovered != badgeHovered)
    {
        badgeHovered = newBadgeHovered;
        repaint();
    }
}

void TrackInfoComponent::mouseExit (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    isHovering   = false;
    badgeHovered = false;
    titleScrollOffset  = 0.0f;
    artistScrollOffset = 0.0f;
    repaint();
}

void TrackInfoComponent::mouseDown (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    // Activate the deck on any click (same behaviour as DeckShellComponent::mouseDown)
    deckStateManager.setActiveDeck (deckId);

    // If hovering over the badge trash icon, request deck removal
    if (badgeHovered)
    {
        if (onRemoveRequested)
            onRemoveRequested();
    }
}

bool TrackInfoComponent::canRemoveDeck() const
{
    return deckStateManager.canRemoveDeck (deckId);
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

    // Background — matches Figma Deck background #e5e5e5
    g.setColour (juce::Colour (0xFFE5E5E5));
    g.fillRect (bounds);

    // Borders on art and badge blocks only — omitting the full-width horizontal
    // top/bottom lines that would visually connect the two dark blocks across the
    // text area in the middle.
    g.setColour (juce::Colour (0xFF2D2D2D));
    g.drawRect (bounds.withWidth (artWidth), 2);
    g.drawRect (bounds.withTrimmedLeft (bounds.getWidth() - badgeWidth), 2);

    auto area = bounds;

    // 1. Album art (left, fixed width)
    auto artArea = area.removeFromLeft (artWidth);
    paintAlbumArt (g, artArea);

    // 2. Deck identifier badge (right, fixed width)
    auto badgeArea = area.removeFromRight (badgeWidth);
    paintDeckBadge (g, badgeArea);

    // 3. BPM / Key / OrigBPM column (right of text, left of badge)
    area.removeFromRight (colGap);
    auto bpmKeyArea = area.removeFromRight (bpmColWidth);
    paintBpmKeyInfo (g, bpmKeyArea);

    // 4. Text info (title / artist / album) — remaining space
    area.removeFromLeft (colGap);
    paintTextInfo (g, area);
}

void TrackInfoComponent::paintAlbumArt (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (albumArt.isValid())
    {
        g.drawImage (albumArt, area.toFloat(), juce::RectanglePlacement::stretchToFit);
    }
    else
    {
        // Black placeholder matching Figma "Song Cover Image"
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.fillRect (area);

        g.setColour (juce::Colour (0xFFF9F9F9));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        g.drawText (juce::String::charToString (0x266A), area, juce::Justification::centred);
    }
}

void TrackInfoComponent::paintTextInfo (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Three equal rows: title (13px), artist (10px), album (10px)
    // with 4px top padding to align with Figma "py-[4px]"
    auto content = area.withTrimmedTop (4).withTrimmedBottom (4);
    int rowH = content.getHeight() / 3;

    auto titleArea  = content.removeFromTop (rowH);
    auto artistArea = content.removeFromTop (rowH);
    auto albumArea  = content; // remaining

    auto displayTitle  = cachedTitle.isEmpty()  ? juce::String ("--") : cachedTitle;
    auto displayArtist = cachedArtist.isEmpty() ? juce::String ("--") : cachedArtist;
    auto displayAlbum  = cachedAlbum.isEmpty()  ? juce::String ("--") : cachedAlbum;

    // Title — 13px Space Mono bold
    auto monoFont = juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain);
    g.setFont (monoFont);
    g.setColour (juce::Colour (0xFF2D2D2D));

    juce::GlyphArrangement titleGlyphs;
    titleGlyphs.addLineOfText (g.getCurrentFont(), displayTitle, 0.0f, 0.0f);
    float titleTextWidth = titleGlyphs.getBoundingBox (0, -1, false).getWidth();
    float titleAreaWidth = static_cast<float> (titleArea.getWidth());

    if (isHovering && titleTextWidth > titleAreaWidth)
    {
        float scrollOff = getScrollOffset (titleTextWidth, titleAreaWidth, titleScrollOffset, isHovering);
        g.saveState();
        g.reduceClipRegion (titleArea);
        g.drawText (displayTitle,
                    titleArea.toFloat().withWidth (titleTextWidth).translated (-scrollOff, 0.0f).toNearestInt(),
                    juce::Justification::centredLeft, false);
        g.restoreState();
    }
    else
    {
        g.drawText (displayTitle, titleArea, juce::Justification::centredLeft, true);
    }

    // Artist — 13px
    auto smallFont = juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain);
    g.setFont (smallFont);

    juce::GlyphArrangement artistGlyphs;
    artistGlyphs.addLineOfText (g.getCurrentFont(), displayArtist, 0.0f, 0.0f);
    float artistTextWidth = artistGlyphs.getBoundingBox (0, -1, false).getWidth();
    float artistAreaWidth = static_cast<float> (artistArea.getWidth());

    if (isHovering && artistTextWidth > artistAreaWidth)
    {
        float scrollOff = getScrollOffset (artistTextWidth, artistAreaWidth, artistScrollOffset, isHovering);
        g.saveState();
        g.reduceClipRegion (artistArea);
        g.drawText (displayArtist,
                    artistArea.toFloat().withWidth (artistTextWidth).translated (-scrollOff, 0.0f).toNearestInt(),
                    juce::Justification::centredLeft, false);
        g.restoreState();
    }
    else
    {
        g.drawText (displayArtist, artistArea, juce::Justification::centredLeft, true);
    }

    // Album — 10px
    g.drawText (displayAlbum, albumArea, juce::Justification::centredLeft, true);
}

void TrackInfoComponent::paintBpmKeyInfo (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Three rows: currentBPM (13px), key (10px), origBPM (10px)
    // matches Figma Frame 4: right-aligned numbers
    auto content = area.withTrimmedTop (4).withTrimmedBottom (4);
    int rowH = content.getHeight() / 3;

    auto currentBpmArea = content.removeFromTop (rowH);
    auto keyArea        = content.removeFromTop (rowH);
    auto origBpmArea    = content;

    auto* audioState = deckStateManager.getAudioState (deckId);
    float speedMul = 1.0f;
    if (audioState != nullptr)
        speedMul = audioState->speedMultiplier.load (std::memory_order_relaxed);

    double displayBpm = baseBpm * static_cast<double> (speedMul);
    auto currentBpmStr = (baseBpm <= 0.0) ? juce::String ("--") : juce::String (displayBpm, 2);
    auto origBpmStr    = (baseBpm <= 0.0) ? juce::String ("--") : juce::String (baseBpm, 2);
    auto keyStr        = getCamelotKey (keyIndex);

    // Current BPM — 13px
    auto monoFont = juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain);
    g.setFont (monoFont);
    g.setColour (juce::Colour (0xFF2D2D2D));
    g.drawText (currentBpmStr, currentBpmArea, juce::Justification::centredRight, false);

    // Key — 13px
    auto smallFont = juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain);
    g.setFont (smallFont);
    g.drawText (keyStr, keyArea, juce::Justification::centredRight, false);

    // Original BPM — 13px
    g.drawText (origBpmStr, origBpmArea, juce::Justification::centredRight, false);
}

void TrackInfoComponent::paintDeckBadge (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Filled black rect (matching Figma "Deck Identifier" node: bg-[#2d2d2d])
    g.setColour (juce::Colour (0xFF2D2D2D));
    g.fillRect (area);

    if (badgeHovered)
    {
        // Show pixel-art trash icon instead of deck letter
        paintTrashIcon (g, area, juce::Colour (0xFFF9F9F9));
    }
    else
    {
        // Large deck letter centred — monospace to match SYNC/QUANTIZE/SLIP style
        g.setColour (juce::Colour (0xFFF9F9F9));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 28.0f, juce::Font::bold));
        g.drawText (deckId, area, juce::Justification::centred);
    }
}

void TrackInfoComponent::paintTrashIcon (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour col)
{
    // Pixel-art monochrome trash can
    // Total icon: 18 wide x 20 tall
    const int canW  = 16;
    const int canH  = 12;
    const int lidW  = 18;
    const int lidH  = 3;
    const int hdlW  = 6;
    const int hdlH  = 2;
    const int totalH = hdlH + 1 + lidH + 2 + canH; // 2+1+3+2+12 = 20
    const int totalW = lidW;                         // 18

    const int ox = area.getX() + (area.getWidth()  - totalW) / 2;
    const int oy = area.getY() + (area.getHeight() - totalH) / 2;

    g.setColour (col);

    // Handle
    g.fillRect (ox + (totalW - hdlW) / 2, oy, hdlW, hdlH);

    // Lid
    g.fillRect (ox, oy + hdlH + 1, lidW, lidH);

    // Body
    const int bodyX = ox + 1;
    const int bodyY = oy + hdlH + 1 + lidH + 2;

    g.fillRect (bodyX,            bodyY, 2,    canH); // left edge
    g.fillRect (bodyX + canW - 2, bodyY, 2,    canH); // right edge
    g.fillRect (bodyX,  bodyY + canH - 2, canW, 2);   // bottom edge
    // top edge  (optional – lid already implies the top)
    g.fillRect (bodyX, bodyY, canW, 2);               // top edge

    // Two vertical dividers inside the body
    const int divY = bodyY + 3;
    const int divH = canH - 5;
    g.fillRect (bodyX + 5, divY, 1, divH);
    g.fillRect (bodyX + 9, divY, 1, divH);
}

void TrackInfoComponent::resized()
{
    // Painting is done entirely in paint(), no child components to layout.
}
