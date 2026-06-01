#include "HotCueManager.h"
#include "../Deck/AudioThreadState.h"
#include "../Daw/Recording/PerformanceCaptureSink.h"

HotCueManager::HotCueManager (juce::ValueTree deckTree,
                               AudioEngine& engine,
                               const juce::String& id,
                               TrackDatabase& db)
    : tree (deckTree),
      cuePointsNode (deckTree.getChildWithName (IDs::CuePoints)),
      trackMetaNode (deckTree.getChildWithName (IDs::TrackMetadata)),
      audioEngine (engine),
      deckId (id),
      database (db)
{
    tree.addListener (this);

    // If a track is already loaded, grab its content hash
    currentContentHash = trackMetaNode.getProperty (IDs::contentHash).toString();
    if (currentContentHash.isNotEmpty())
        loadCuesFromDB (currentContentHash);
}

HotCueManager::~HotCueManager()
{
    stopTimer();
    tree.removeListener (this);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void HotCueManager::setCue (int padIndex)
{
    if (padIndex < 0 || padIndex >= 8)
        return;

    auto status = tree.getProperty (IDs::playbackStatus).toString();
    if (status == "empty")
        return;

    // Read current playhead position from audio thread atomic (live value)
    int64_t pos = 0;
    if (audioState != nullptr)
        pos = audioState->playheadPosition.load (std::memory_order_relaxed);
    else
        pos = static_cast<int64_t> (tree.getChildWithName (IDs::Playhead).getProperty (IDs::position, 0));

    // Quantize snap if enabled and beatgrid is available
    bool quantize = static_cast<bool> (tree.getProperty (IDs::quantizeEnabled, false));
    if (quantize && beatGridData != nullptr && beatGridData->beatIntervalSamples > 0.0)
        pos = beatGridData->getNearestBeat (pos);

    cancelPendingUndo();

    // Write to ValueTree
    auto cp = cuePointsNode.getChild (padIndex);
    cp.setProperty (IDs::position,   pos,  nullptr);
    cp.setProperty (IDs::isValid,    true, nullptr);

    // Set default color if this was previously unassigned (colorIndex was 0 from reset)
    if (static_cast<int> (cp.getProperty (IDs::colorIndex, 0)) == 0
        && HotCueColors::defaultColorForPad[padIndex] != 0)
    {
        cp.setProperty (IDs::colorIndex, HotCueColors::defaultColorForPad[padIndex], nullptr);
    }

    saveCuesToDB();
    notifyListeners();
}

void HotCueManager::triggerCue (int padIndex)
{
    if (padIndex < 0 || padIndex >= 8)
        return;

    auto cp = cuePointsNode.getChild (padIndex);
    if (! static_cast<bool> (cp.getProperty (IDs::isValid, false)))
        return;

    int64_t position = static_cast<int64_t> (cp.getProperty (IDs::position, -1));
    if (position < 0)
        return;

    // Read live playback status from audio thread atomic
    bool isPlaying = false;
    if (audioState != nullptr)
    {
        auto st = static_cast<PlaybackStatusCode> (
            audioState->playbackStatus.load (std::memory_order_relaxed));
        isPlaying = (st == PlaybackStatusCode::playing);
    }

    // Skip if already within 64 samples of current position
    int64_t currentPos = 0;
    if (audioState != nullptr)
        currentPos = audioState->playheadPosition.load (std::memory_order_relaxed);
    else
        currentPos = static_cast<int64_t> (tree.getChildWithName (IDs::Playhead).getProperty (IDs::position, 0));
    if (std::abs (currentPos - position) < 64)
        return;

    // PRD-0075: surface the source discontinuity for recording capture before
    // the seek. currentPos is the jump-out, the stored cue is the jump-in.
    if (capture_ != nullptr)
        capture_->captureJump (captureDeckIndex_, Daw::PerformanceEventType::HotCueJump,
                               currentPos, position);

    // PRD-0017: Use slip-aware seek when slip is enabled
    bool slipOn = static_cast<bool> (tree.getProperty (IDs::slipEnabled, false));

    // Deactivate any active loop before jumping to the cue point.
    // The loop wrap-back would prevent reaching a cue outside the loop region,
    // and should be cleared regardless of cue position for consistent behaviour.
    {
        auto loopNode = tree.getChildWithName (IDs::Loop);
        if (loopNode.isValid() && static_cast<bool> (loopNode.getProperty (IDs::active, false)))
        {
            loopNode.setProperty (IDs::active,   false, nullptr);
            loopNode.setProperty (IDs::loopMode, 0,     nullptr);
        }
    }

    if (isPlaying)
    {
        if (slipOn)
            audioEngine.slipSeekDeck (deckId, position);
        else
            audioEngine.seekDeck (deckId, position);
    }
    else
    {
        if (slipOn)
            audioEngine.slipSeekAndPlayDeck (deckId, position);
        else
            audioEngine.seekAndPlayDeck (deckId, position);
    }
}

void HotCueManager::deleteCue (int padIndex)
{
    if (padIndex < 0 || padIndex >= 8)
        return;

    auto cp = cuePointsNode.getChild (padIndex);
    if (! static_cast<bool> (cp.getProperty (IDs::isValid, false)))
        return;

    cancelPendingUndo();

    // Store undo state
    UndoState undo;
    undo.padIndex   = padIndex;
    undo.position   = static_cast<int64_t> (cp.getProperty (IDs::position, -1));
    undo.colorIndex = static_cast<int> (cp.getProperty (IDs::colorIndex, 0));
    undo.label      = cp.getProperty (IDs::label).toString();
    pendingUndo     = undo;

    // Soft-delete: clear in ValueTree
    cp.setProperty (IDs::position,   static_cast<int64_t> (-1), nullptr);
    cp.setProperty (IDs::colorIndex, HotCueColors::defaultColorForPad[padIndex], nullptr);
    cp.setProperty (IDs::label,      "",    nullptr);
    cp.setProperty (IDs::isValid,    false, nullptr);

    // Start 10-second undo window
    startTimer (10000);

    saveCuesToDB();
    notifyListeners();
}

void HotCueManager::undoDelete()
{
    if (! pendingUndo.has_value())
        return;

    auto& undo = *pendingUndo;
    auto cp = cuePointsNode.getChild (undo.padIndex);

    cp.setProperty (IDs::position,   undo.position,   nullptr);
    cp.setProperty (IDs::colorIndex, undo.colorIndex,  nullptr);
    cp.setProperty (IDs::label,      undo.label,       nullptr);
    cp.setProperty (IDs::isValid,    true,             nullptr);

    pendingUndo.reset();
    stopTimer();

    saveCuesToDB();
    notifyListeners();
}

void HotCueManager::setColor (int padIndex, int colorIndex)
{
    if (padIndex < 0 || padIndex >= 8 || colorIndex < 0 || colorIndex >= 16)
        return;

    auto cp = cuePointsNode.getChild (padIndex);
    if (! static_cast<bool> (cp.getProperty (IDs::isValid, false)))
        return;

    cp.setProperty (IDs::colorIndex, colorIndex, nullptr);

    saveCuesToDB();
    notifyListeners();
}

void HotCueManager::setLabel (int padIndex, const juce::String& label)
{
    if (padIndex < 0 || padIndex >= 8)
        return;

    auto cp = cuePointsNode.getChild (padIndex);
    if (! static_cast<bool> (cp.getProperty (IDs::isValid, false)))
        return;

    cp.setProperty (IDs::label, label.substring (0, 12), nullptr);

    saveCuesToDB();
    notifyListeners();
}

// ---------------------------------------------------------------------------
// State access
// ---------------------------------------------------------------------------

std::array<HotCueInfo, 9> HotCueManager::getHotCues() const
{
    std::array<HotCueInfo, 9> cues;

    for (int i = 0; i < 9; ++i)
    {
        auto cp = cuePointsNode.getChild (i);
        cues[static_cast<size_t> (i)].padIndex        = i;
        cues[static_cast<size_t> (i)].positionSamples = static_cast<int64_t> (cp.getProperty (IDs::position, -1));
        cues[static_cast<size_t> (i)].colorIndex      = static_cast<int> (cp.getProperty (IDs::colorIndex, 0));
        cues[static_cast<size_t> (i)].label            = cp.getProperty (IDs::label).toString();
        cues[static_cast<size_t> (i)].active           = static_cast<bool> (cp.getProperty (IDs::isValid, false));
    }

    return cues;
}

void HotCueManager::setBeatGridData (BeatGridData::Ptr data)
{
    beatGridData = std::move (data);
}

void HotCueManager::setAudioState (DeckAudioState* state)
{
    audioState = state;
}

void HotCueManager::setPerformanceCapture (Daw::PerformanceCaptureSink* sink, int deckIndex)
{
    capture_          = sink;
    captureDeckIndex_ = deckIndex;
}

// ---------------------------------------------------------------------------
// Listener management
// ---------------------------------------------------------------------------

void HotCueManager::addListener (Listener* l)    { listeners.add (l); }
void HotCueManager::removeListener (Listener* l) { listeners.remove (l); }

void HotCueManager::notifyListeners()
{
    listeners.call (&Listener::hotCuesChanged);
}

// ---------------------------------------------------------------------------
// ValueTree::Listener
// ---------------------------------------------------------------------------

void HotCueManager::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                               const juce::Identifier& property)
{
    // Detect track load via contentHash change
    if (changedTree == trackMetaNode && property == IDs::contentHash)
    {
        auto newHash = changedTree.getProperty (IDs::contentHash).toString();

        if (newHash.isNotEmpty() && newHash != currentContentHash)
        {
            currentContentHash = newHash;
            loadCuesFromDB (newHash);
        }
        else if (newHash.isEmpty())
        {
            currentContentHash = {};
            cancelPendingUndo();
            notifyListeners();
        }
    }

    // Notify on any CuePoint property change (for waveform updates)
    if (changedTree.hasType (IDs::CuePoint) && changedTree.getParent() == cuePointsNode)
        notifyListeners();
}

// ---------------------------------------------------------------------------
// Timer (undo expiry)
// ---------------------------------------------------------------------------

void HotCueManager::timerCallback()
{
    pendingUndo.reset();
    stopTimer();
}

// ---------------------------------------------------------------------------
// DB persistence
// ---------------------------------------------------------------------------

void HotCueManager::loadCuesFromDB (const juce::String& contentHash)
{
    // Read cue points JSON from database (message thread, fast < 1ms)
    auto json = database.loadCuePointsJson (contentHash);

    auto totalSamples = static_cast<int64_t> (
        trackMetaNode.getProperty (IDs::totalSamples, 0));

    parseCuesFromJson (json, totalSamples);
    notifyListeners();
}

void HotCueManager::saveCuesToDB()
{
    auto filePath    = trackMetaNode.getProperty (IDs::filePath).toString();
    auto contentHash = trackMetaNode.getProperty (IDs::contentHash).toString();

    if (filePath.isEmpty() || contentHash.isEmpty())
        return;

    auto json = serializeCuesToJson();

    // Background DB write
    dbWritePool.addJob ([&db = database, fp = filePath, ch = contentHash, j = json]()
    {
        db.saveCuePointsJson (fp, ch, j);
    });
}

juce::String HotCueManager::serializeCuesToJson() const
{
    juce::Array<juce::var> arr;

    for (int i = 0; i < 9; ++i)
    {
        auto cp = cuePointsNode.getChild (i);
        if (! static_cast<bool> (cp.getProperty (IDs::isValid, false)))
            continue;

        auto obj = std::make_unique<juce::DynamicObject>();
        obj->setProperty ("pad",   i);
        obj->setProperty ("pos",   static_cast<double> (static_cast<int64_t> (
                              cp.getProperty (IDs::position, -1))));
        obj->setProperty ("color", static_cast<int> (cp.getProperty (IDs::colorIndex, 0)));
        obj->setProperty ("label", cp.getProperty (IDs::label).toString());
        arr.add (juce::var (obj.release()));
    }

    return juce::JSON::toString (juce::var (arr), true);
}

void HotCueManager::parseCuesFromJson (const juce::String& json, int64_t totalSamples)
{
    // First reset all cue points to defaults
    for (int i = 0; i < 9; ++i)
    {
        auto cp = cuePointsNode.getChild (i);
        cp.setProperty (IDs::position,   static_cast<int64_t> (-1), nullptr);
        cp.setProperty (IDs::colorIndex, HotCueColors::defaultColorForPad[i], nullptr);
        cp.setProperty (IDs::label,      "",    nullptr);
        cp.setProperty (IDs::isValid,    false, nullptr);
    }

    if (json.isEmpty())
        return;

    auto parsed = juce::JSON::parse (json);
    if (! parsed.isArray())
        return;

    auto* arr = parsed.getArray();
    if (arr == nullptr)
        return;

    for (const auto& item : *arr)
    {
        auto* obj = item.getDynamicObject();
        if (obj == nullptr)
            continue;

        int padIdx = static_cast<int> (obj->getProperty ("pad"));
        if (padIdx < 0 || padIdx >= 9)
            continue;

        int64_t pos = static_cast<int64_t> (static_cast<double> (obj->getProperty ("pos")));
        int color   = static_cast<int> (obj->getProperty ("color"));
        auto label  = obj->getProperty ("label").toString();

        // Validate position against track length
        bool valid = (pos >= 0) && (totalSamples <= 0 || pos < totalSamples);

        auto cp = cuePointsNode.getChild (padIdx);
        cp.setProperty (IDs::position,   pos,                       nullptr);
        cp.setProperty (IDs::colorIndex, juce::jlimit (0, 15, color), nullptr);
        cp.setProperty (IDs::label,      label.substring (0, 12),   nullptr);
        cp.setProperty (IDs::isValid,    valid,                     nullptr);
    }
}

void HotCueManager::cancelPendingUndo()
{
    pendingUndo.reset();
    stopTimer();
}
