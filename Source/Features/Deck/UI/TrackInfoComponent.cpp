#include "TrackInfoComponent.h"
#include "../../KeyDetection/KeyUtils.h"
#include <cmath>

//==============================================================================
// BpmEditPopup — small callout for editing the analysis BPM of a track.
// Displayed via CallOutBox::launchAsynchronously (no modal loop required).
//==============================================================================
class BpmEditPopup final : public juce::Component
{
public:
    std::function<void (double)> onConfirm;

    explicit BpmEditPopup (double currentBpm)
    {
        setSize (160, 66);

        label.setText ("BPM:", juce::dontSendNotification);
        label.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
        label.setColour (juce::Label::textColourId, juce::Colour (0xFF2D2D2D));
        addAndMakeVisible (label);

        editor.setText (currentBpm > 0.0 ? juce::String (currentBpm, 2) : juce::String());
        editor.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        editor.setInputRestrictions (7, "0123456789.");
        editor.setSelectAllWhenFocused (true);
        editor.onReturnKey = [this] { confirm(); };
        addAndMakeVisible (editor);

        setBtn.setButtonText ("SET");
        setBtn.onClick = [this] { confirm(); };
        addAndMakeVisible (setBtn);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8);
        auto topRow = b.removeFromTop (28);
        label.setBounds (topRow.removeFromLeft (36));
        editor.setBounds (topRow);
        b.removeFromTop (4);
        setBtn.setBounds (b);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFFFDFDFD));
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawRect (getLocalBounds(), 2);
    }

private:
    void confirm()
    {
        const double bpm = editor.getText().getDoubleValue();
        if (bpm >= 20.0 && bpm <= 300.0 && onConfirm)
            onConfirm (bpm);
        if (auto* cb = findParentComponentOfClass<juce::CallOutBox>())
            cb->dismiss();
    }

    juce::Label      label;
    juce::TextEditor editor;
    juce::TextButton setBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BpmEditPopup)
};

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

    // Extend the hit area slightly to the left to include the pencil zone.
    const bool newBpmHovering = baseBpm > 0.0
                                && computeOrigBpmArea().expanded (12, 2).contains (e.getPosition());
    if (newBpmHovering != bpmHovering)
    {
        bpmHovering = newBpmHovering;
        setMouseCursor (bpmHovering ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void TrackInfoComponent::mouseExit (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    isHovering   = false;
    badgeHovered = false;
    bpmHovering  = false;
    titleScrollOffset  = 0.0f;
    artistScrollOffset = 0.0f;
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

void TrackInfoComponent::mouseDown (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    // Activate the deck on any click (same behaviour as DeckShellComponent::mouseDown)
    deckStateManager.setActiveDeck (deckId);

    // If clicking on the orig-BPM row (or pencil zone), open the BPM editor popup.
    if (bpmHovering)
    {
        showBpmEditPopup();
        return;
    }

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

// --- BPM area helpers ---

juce::Rectangle<int> TrackInfoComponent::computeOrigBpmArea() const
{
    auto bounds = getLocalBounds();
    bounds.removeFromLeft  (artWidth);
    bounds.removeFromRight (badgeWidth);
    bounds.removeFromRight (colGap);
    auto bpmKeyArea = bounds.removeFromRight (bpmColWidth);

    auto content = bpmKeyArea.withTrimmedTop (4).withTrimmedBottom (4);
    const int rowH = content.getHeight() / 3;
    content.removeFromTop (rowH);   // currentBpmArea
    content.removeFromTop (rowH);   // keyArea
    return content;                 // origBpmArea
}

void TrackInfoComponent::showBpmEditPopup()
{
    if (baseBpm <= 0.0) return;

    auto popup = std::make_unique<BpmEditPopup> (baseBpm);
    popup->onConfirm = [this] (double newBpm)
    {
        if (onBpmEditRequested)
            onBpmEditRequested (newBpm);
    };

    auto targetInScreen = localAreaToGlobal (computeOrigBpmArea());
    juce::CallOutBox::launchAsynchronously (std::move (popup), targetInScreen, nullptr);
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

    // 3b. Time column (remaining / elapsed) — Figma's left readout block,
    // sitting immediately left of the BPM/key column.
    area.removeFromRight (colGap);
    auto timeArea = area.removeFromRight (timeColWidth);
    paintTimeInfo (g, timeArea);

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

void TrackInfoComponent::paintTimeInfo (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Two stacked, right-aligned readouts matching Figma: remaining time
    // (big, top) and elapsed time (small, below).
    auto content = area.withTrimmedTop (4).withTrimmedBottom (4);
    const int rowH = content.getHeight() / 3;
    auto remainArea  = content.removeFromTop (rowH);
    auto elapsedArea = content.removeFromTop (rowH);

    auto formatTime = [] (int64_t samples, double sr, bool negative) -> juce::String
    {
        if (sr <= 0.0) sr = 44100.0;
        const int totalSecs = (int) std::floor (std::abs ((double) samples) / sr);
        const int mins = totalSecs / 60;
        const int secs = totalSecs % 60;
        juce::String s = juce::String (mins).paddedLeft ('0', 2)
                       + ":" + juce::String (secs).paddedLeft ('0', 2);
        return negative ? "-" + s : s;
    };

    int64_t playhead = 0;
    if (auto* audioState = deckStateManager.getAudioState (deckId))
        playhead = audioState->playheadPosition.load (std::memory_order_relaxed);

    const bool loaded = totalSamples > 0;
    const int64_t remaining = loaded ? juce::jmax<int64_t> (0, totalSamples - playhead) : 0;

    auto remainStr  = loaded ? formatTime (remaining, sampleRate, true)  : juce::String ("-00:00");
    auto elapsedStr = loaded ? formatTime (playhead,  sampleRate, false) : juce::String ("00:00");

    g.setColour (juce::Colour (0xFF2D2D2D));
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    g.drawText (remainStr, remainArea, juce::Justification::centredRight, false);

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::plain));
    g.drawText (elapsedStr, elapsedArea, juce::Justification::centredRight, false);
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

    // Original BPM — 13px; when hovering, show pencil icon on the left.
    if (bpmHovering && baseBpm > 0.0)
    {
        // Pencil icon occupies the left 12px, BPM text fills the remaining width.
        constexpr int kPencilW = 12;
        auto pencilRect = origBpmArea.removeFromLeft (kPencilW);
        paintPencilIcon (g, pencilRect, juce::Colour (0xFF2D2D2D));
        g.setFont (smallFont);
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawText (origBpmStr, origBpmArea, juce::Justification::centredRight, false);
    }
    else
    {
        g.drawText (origBpmStr, origBpmArea, juce::Justification::centredRight, false);
    }
}

void TrackInfoComponent::paintPencilIcon (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour col)
{
    // Pixel-art pencil (8 wide x 14 tall, pointing upward).
    constexpr int totalW = 8;
    constexpr int totalH = 14;
    const int ox = area.getX() + (area.getWidth()  - totalW) / 2;
    const int oy = area.getY() + (area.getHeight() - totalH) / 2;

    g.setColour (col);

    // Tip (narrowing top)
    g.fillRect (ox + 3, oy,     2, 2);
    g.fillRect (ox + 2, oy + 2, 4, 2);

    // Body
    g.fillRect (ox + 1, oy + 4, 6, 6);

    // Eraser band
    g.fillRect (ox,     oy + 10, 8, 2);

    // Eraser
    g.fillRect (ox + 1, oy + 12, 6, 2);
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
