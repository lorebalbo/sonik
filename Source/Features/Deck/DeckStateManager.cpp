#include "DeckStateManager.h"
#include "../KeyDetection/KeyUtils.h"

DeckStateManager::DeckStateManager (TrackDatabase& database)
    : db (database),
      rootState (IDs::SonikState)
{
    rootState.setProperty (IDs::activeDeckId,    "A", nullptr);
    rootState.setProperty (IDs::deckCount,       0,   nullptr);
    rootState.setProperty (IDs::masterDeckIndex, -1,  nullptr);  // PRD-0026: no master initially

    auto decksNode = juce::ValueTree (IDs::Decks);
    rootState.addChild (decksNode, -1, nullptr);
}

// --- Deck management ---

juce::String DeckStateManager::addDeck()
{
    if (getDeckCount() >= maxDecks)
        return {};

    auto letter = findNextAvailableLetter();
    if (letter.isEmpty())
        return {};

    auto deckTree = createDeckTree (letter);
    rootState.getChildWithName (IDs::Decks).addChild (deckTree, -1, nullptr);

    usedLetters.insert (letter);

    auto runtime = std::make_unique<DeckRuntime>();
    runtime->sync = std::make_unique<AudioStateSync> (deckTree, runtime->audioState);
    deckRuntimes[letter] = std::move (runtime);

    auto newCount = getDeckCount();
    rootState.setProperty (IDs::deckCount, newCount, nullptr);

    if (newCount == 1)
        rootState.setProperty (IDs::activeDeckId, letter, nullptr);

    return letter;
}

bool DeckStateManager::removeDeck (const juce::String& deckId)
{
    if (! canRemoveDeck (deckId))
        return false;

    auto decks = rootState.getChildWithName (IDs::Decks);
    auto deckTree = getDeckState (deckId);
    if (! deckTree.isValid())
        return false;

    deckRuntimes.erase (deckId);
    usedLetters.erase (deckId);

    decks.removeChild (deckTree, nullptr);
    rootState.setProperty (IDs::deckCount, getDeckCount(), nullptr);

    if (getActiveDeckId() == deckId && getDeckCount() > 0)
    {
        auto firstDeck = decks.getChild (0);
        rootState.setProperty (IDs::activeDeckId, firstDeck.getProperty (IDs::id), nullptr);
    }

    return true;
}

bool DeckStateManager::canRemoveDeck (const juce::String& deckId) const
{
    if (getDeckCount() <= 1)
        return false;

    auto deckTree = getDeckState (deckId);
    if (! deckTree.isValid())
        return false;

    auto status = getPlaybackStatus (deckTree);
    return status != "playing";
}

// --- Accessors ---

juce::ValueTree DeckStateManager::getDeckState (const juce::String& deckId) const
{
    auto decks = rootState.getChildWithName (IDs::Decks);
    for (int i = 0; i < decks.getNumChildren(); ++i)
    {
        auto child = decks.getChild (i);
        if (child.getProperty (IDs::id).toString() == deckId)
            return child;
    }
    return {};
}

juce::String DeckStateManager::getActiveDeckId() const
{
    return rootState.getProperty (IDs::activeDeckId).toString();
}

void DeckStateManager::setActiveDeck (const juce::String& deckId)
{
    if (getDeckState (deckId).isValid())
        rootState.setProperty (IDs::activeDeckId, deckId, nullptr);
}

int DeckStateManager::getDeckCount() const
{
    return rootState.getChildWithName (IDs::Decks).getNumChildren();
}

juce::ValueTree DeckStateManager::getStateTree() const
{
    return rootState;
}

juce::StringArray DeckStateManager::getDeckIds() const
{
    juce::StringArray ids;
    auto decks = rootState.getChildWithName (IDs::Decks);
    for (int i = 0; i < decks.getNumChildren(); ++i)
        ids.add (decks.getChild (i).getProperty (IDs::id).toString());
    return ids;
}

// --- Track loading ---

void DeckStateManager::loadTrack (const juce::String& deckId, const TrackMetadata& metadata)
{
    auto deckTree = getDeckState (deckId);
    if (! deckTree.isValid())
        return;

    auto status = getPlaybackStatus (deckTree);
    if (status == "playing")
        return;

    resetTrackSpecificState (deckTree);

    auto trackMeta = deckTree.getChildWithName (IDs::TrackMetadata);
    trackMeta.setProperty (IDs::filePath,     metadata.filePath,     nullptr);
    trackMeta.setProperty (IDs::contentHash,  metadata.contentHash,  nullptr);
    trackMeta.setProperty (IDs::title,        metadata.title,        nullptr);
    trackMeta.setProperty (IDs::artist,       metadata.artist,       nullptr);
    trackMeta.setProperty (IDs::album,        metadata.album,        nullptr);
    trackMeta.setProperty (IDs::duration,     metadata.duration,     nullptr);
    trackMeta.setProperty (IDs::sampleRate,   metadata.sampleRate,   nullptr);
    trackMeta.setProperty (IDs::bitDepth,     metadata.bitDepth,     nullptr);
    trackMeta.setProperty (IDs::totalSamples, metadata.totalSamples, nullptr);
    trackMeta.setProperty (IDs::hasAlbumArt,  metadata.hasAlbumArt,  nullptr);
    trackMeta.setProperty (IDs::channelCount, metadata.channelCount, nullptr);

    // Restore persisted data from database
    auto persisted = db.loadTrackData (metadata.filePath, metadata.contentHash);
    if (persisted.has_value())
    {
        auto keyInfo = deckTree.getChildWithName (IDs::KeyInfo);
        keyInfo.setProperty (IDs::keyIndex,          persisted->keyIndex,           nullptr);
        keyInfo.setProperty (IDs::confidence,        persisted->keyConfidence,      nullptr);
        keyInfo.setProperty (IDs::manuallyAdjusted,  persisted->keyManuallyAdjusted, nullptr);
        if (persisted->keyIndex >= 0)
            keyInfo.setProperty (IDs::analysisStatus, "done", nullptr);

        // Cue points and beatgrid JSON stored for future features to parse
    }

    // Fallback: use embedded key tag if no key was persisted or detected
    {
        auto keyInfo = deckTree.getChildWithName (IDs::KeyInfo);
        int currentKey = static_cast<int> (keyInfo.getProperty (IDs::keyIndex, -1));
        if (currentKey < 0 && metadata.initialKeyString.isNotEmpty())
        {
            int parsedKey = KeyUtils::parseKeyString (metadata.initialKeyString);
            if (parsedKey >= 0)
            {
                keyInfo.setProperty (IDs::keyIndex,        parsedKey, nullptr);
                keyInfo.setProperty (IDs::confidence,      0.5,       nullptr);
                keyInfo.setProperty (IDs::analysisStatus,  "done",    nullptr);
            }
        }
    }

    deckTree.setProperty (IDs::playbackStatus, "stopped", nullptr);
}

bool DeckStateManager::ejectTrack (const juce::String& deckId)
{
    if (! canEjectTrack (deckId))
        return false;

    auto deckTree = getDeckState (deckId);
    resetTrackSpecificState (deckTree);

    // Reset loading state on eject
    deckTree.setProperty (IDs::loadingStatus,   "idle", nullptr);
    deckTree.setProperty (IDs::loadingProgress, 0.0f,   nullptr);
    deckTree.setProperty (IDs::loadingError,    "",     nullptr);

    deckTree.setProperty (IDs::playbackStatus, "empty", nullptr);
    return true;
}

bool DeckStateManager::canEjectTrack (const juce::String& deckId) const
{
    auto deckTree = getDeckState (deckId);
    if (! deckTree.isValid())
        return false;

    return getPlaybackStatus (deckTree) != "playing";
}

// --- Playback ---

bool DeckStateManager::setPlaybackStatus (const juce::String& deckId, const juce::String& newStatus)
{
    auto deckTree = getDeckState (deckId);
    if (! deckTree.isValid())
        return false;

    auto currentStatus = getPlaybackStatus (deckTree);
    if (! isValidTransition (currentStatus, newStatus))
        return false;

    deckTree.setProperty (IDs::playbackStatus, newStatus, nullptr);
    return true;
}

void DeckStateManager::setMasterTempo (const juce::String& deckId)
{
    auto decks = rootState.getChildWithName (IDs::Decks);
    for (int i = 0; i < decks.getNumChildren(); ++i)
    {
        auto child = decks.getChild (i);
        bool isMaster = (child.getProperty (IDs::id).toString() == deckId);
        child.setProperty (IDs::isMasterTempo, isMaster, nullptr);
    }
}

// --- Audio thread state ---

DeckAudioState* DeckStateManager::getAudioState (const juce::String& deckId)
{
    auto it = deckRuntimes.find (deckId);
    if (it != deckRuntimes.end())
        return &it->second->audioState;
    return nullptr;
}

// --- Session persistence ---

void DeckStateManager::saveSession()
{
    auto decks = rootState.getChildWithName (IDs::Decks);

    juce::Array<juce::var> tracksArray;
    for (int i = 0; i < decks.getNumChildren(); ++i)
    {
        auto child = decks.getChild (i);
        auto trackMeta = child.getChildWithName (IDs::TrackMetadata);
        auto fp = trackMeta.getProperty (IDs::filePath).toString();

        if (fp.isNotEmpty())
        {
            auto obj = std::make_unique<juce::DynamicObject>();
            obj->setProperty ("deckId", child.getProperty (IDs::id));
            obj->setProperty ("filePath", fp);
            tracksArray.add (juce::var (obj.release()));
        }
    }

    auto jsonStr = juce::JSON::toString (juce::var (tracksArray));
    db.saveSessionState (getDeckCount(), getActiveDeckId(), jsonStr);
}

void DeckStateManager::restoreSession()
{
    auto session = db.loadSessionState();

    int targetCount = juce::jlimit (1, maxDecks, session.deckCount);
    while (getDeckCount() < targetCount)
        addDeck();

    if (getDeckState (session.activeDeckId).isValid())
        setActiveDeck (session.activeDeckId);
}

// --- Private helpers ---

juce::ValueTree DeckStateManager::createDeckTree (const juce::String& deckId) const
{
    juce::ValueTree deck (IDs::Deck);
    deck.setProperty (IDs::id,               deckId,   nullptr);
    deck.setProperty (IDs::playbackStatus,   "empty",  nullptr);
    deck.setProperty (IDs::isMasterTempo,    false,    nullptr);

    // Master Clock state (PRD-0026) — deck-level, persists across track loads
    deck.setProperty (IDs::isMaster,         false,    nullptr);
    deck.setProperty (IDs::isSynced,         false,    nullptr);

    // Deck-level state (persists across track loads)
    deck.setProperty (IDs::gain,             1.0f,     nullptr);
    deck.setProperty (IDs::quantizeEnabled,  false,    nullptr);
    deck.setProperty (IDs::slipEnabled,      false,    nullptr);
    deck.setProperty (IDs::keyLockEnabled,   false,    nullptr);
    deck.setProperty (IDs::pitchRange,       8,        nullptr);
    deck.setProperty (IDs::beatJumpSize,     4.0,      nullptr);

    // Loading state (PRD-0003)
    deck.setProperty (IDs::loadingStatus,    "idle",   nullptr);
    deck.setProperty (IDs::loadingProgress,  0.0f,     nullptr);
    deck.setProperty (IDs::loadingError,     "",       nullptr);

    // Track-specific state
    deck.setProperty (IDs::pitch,            0.0f,     nullptr);
    deck.setProperty (IDs::speedMultiplier,  1.0f,     nullptr);

    // TrackMetadata child
    juce::ValueTree trackMeta (IDs::TrackMetadata);
    trackMeta.setProperty (IDs::filePath,     "",    nullptr);
    trackMeta.setProperty (IDs::contentHash,  "",    nullptr);
    trackMeta.setProperty (IDs::title,        "",    nullptr);
    trackMeta.setProperty (IDs::artist,       "",    nullptr);
    trackMeta.setProperty (IDs::album,        "",    nullptr);
    trackMeta.setProperty (IDs::duration,     0.0,   nullptr);
    trackMeta.setProperty (IDs::sampleRate,   0.0,   nullptr);
    trackMeta.setProperty (IDs::bitDepth,     0,     nullptr);
    trackMeta.setProperty (IDs::totalSamples, (int64_t) 0, nullptr);
    trackMeta.setProperty (IDs::hasAlbumArt,  false, nullptr);
    trackMeta.setProperty (IDs::channelCount, 0,     nullptr);
    deck.addChild (trackMeta, -1, nullptr);

    // Playhead
    juce::ValueTree playhead (IDs::Playhead);
    playhead.setProperty (IDs::position, (int64_t) 0, nullptr);
    deck.addChild (playhead, -1, nullptr);

    // TempCue
    juce::ValueTree tempCue (IDs::TempCue);
    tempCue.setProperty (IDs::position, (int64_t) -1, nullptr);
    deck.addChild (tempCue, -1, nullptr);

    // CuePoints (8 slots)
    juce::ValueTree cuePoints (IDs::CuePoints);
    for (int i = 0; i < 8; ++i)
    {
        juce::ValueTree cp (IDs::CuePoint);
        cp.setProperty (IDs::index,      i,          nullptr);
        cp.setProperty (IDs::position,   (int64_t) -1, nullptr);
        cp.setProperty (IDs::colorIndex, 0,          nullptr);
        cp.setProperty (IDs::label,      "",         nullptr);
        cp.setProperty (IDs::isValid,    false,      nullptr);
        cuePoints.addChild (cp, -1, nullptr);
    }
    deck.addChild (cuePoints, -1, nullptr);

    // BeatGrid
    juce::ValueTree beatGrid (IDs::BeatGrid);
    beatGrid.setProperty (IDs::bpm,                  0.0,         nullptr);
    beatGrid.setProperty (IDs::anchorSample,         (int64_t) 0, nullptr);
    beatGrid.setProperty (IDs::beatIntervalSamples,  0.0,         nullptr);
    beatGrid.setProperty (IDs::confidence,           0.0f,        nullptr);
    beatGrid.setProperty (IDs::manuallyAdjusted,     false,       nullptr);
    beatGrid.setProperty (IDs::analysisStatus,       "idle",      nullptr);
    beatGrid.setProperty (IDs::analysisProgress,     0.0f,        nullptr);
    deck.addChild (beatGrid, -1, nullptr);

    // KeyInfo
    juce::ValueTree keyInfo (IDs::KeyInfo);
    keyInfo.setProperty (IDs::keyIndex,          -1,    nullptr);
    keyInfo.setProperty (IDs::confidence,        0.0f,  nullptr);
    keyInfo.setProperty (IDs::manuallyAdjusted,  false, nullptr);
    keyInfo.setProperty (IDs::analysisStatus,    "idle", nullptr);
    keyInfo.setProperty (IDs::analysisProgress,  0.0f,  nullptr);
    deck.addChild (keyInfo, -1, nullptr);

    // Loop
    juce::ValueTree loop (IDs::Loop);
    loop.setProperty (IDs::loopIn,   (int64_t) -1, nullptr);
    loop.setProperty (IDs::loopOut,  (int64_t) -1, nullptr);
    loop.setProperty (IDs::active,   false,         nullptr);
    loop.setProperty (IDs::loopMode, 0,             nullptr);
    deck.addChild (loop, -1, nullptr);

    // Stems
    juce::ValueTree stems (IDs::Stems);
    stems.setProperty (IDs::status,       "none", nullptr);
    stems.setProperty (IDs::progress,     0.0f,   nullptr);
    stems.setProperty (IDs::stemError,    "",     nullptr);
    stems.setProperty (IDs::modelVersion, "",     nullptr);
    stems.setProperty (IDs::vocalsMuted,  false,  nullptr);
    stems.setProperty (IDs::drumsMuted,   false,  nullptr);
    stems.setProperty (IDs::bassMuted,    false,  nullptr);
    stems.setProperty (IDs::otherMuted,   false,  nullptr);
    stems.setProperty (IDs::vocalsSolo,   false,  nullptr);
    stems.setProperty (IDs::drumsSolo,    false,  nullptr);
    stems.setProperty (IDs::bassSolo,     false,  nullptr);
    stems.setProperty (IDs::otherSolo,    false,  nullptr);
    deck.addChild (stems, -1, nullptr);

    // Waveform
    juce::ValueTree waveform (IDs::Waveform);
    waveform.setProperty (IDs::analysisStatus,   "none", nullptr);
    waveform.setProperty (IDs::analysisProgress, 0.0f,   nullptr);
    deck.addChild (waveform, -1, nullptr);

    return deck;
}

void DeckStateManager::resetTrackSpecificState (juce::ValueTree& deckTree)
{
    deckTree.setProperty (IDs::pitch,           0.0f, nullptr);
    deckTree.setProperty (IDs::speedMultiplier, 1.0f, nullptr);

    // NOTE: loadingStatus/loadingProgress/loadingError are NOT reset here.
    // They are managed exclusively by AudioFileLoader (loadFile/cancelLoad/deliverBuffer).

    // Reset TrackMetadata
    auto trackMeta = deckTree.getChildWithName (IDs::TrackMetadata);
    trackMeta.setProperty (IDs::filePath,     "", nullptr);
    trackMeta.setProperty (IDs::contentHash,  "", nullptr);
    trackMeta.setProperty (IDs::title,        "", nullptr);
    trackMeta.setProperty (IDs::artist,       "", nullptr);
    trackMeta.setProperty (IDs::album,        "", nullptr);
    trackMeta.setProperty (IDs::duration,     0.0, nullptr);
    trackMeta.setProperty (IDs::sampleRate,   0.0, nullptr);
    trackMeta.setProperty (IDs::bitDepth,     0, nullptr);
    trackMeta.setProperty (IDs::totalSamples, (int64_t) 0, nullptr);
    trackMeta.setProperty (IDs::hasAlbumArt,  false, nullptr);
    trackMeta.setProperty (IDs::channelCount, 0, nullptr);

    // Reset Playhead
    auto playhead = deckTree.getChildWithName (IDs::Playhead);
    playhead.setProperty (IDs::position, (int64_t) 0, nullptr);

    // Reset TempCue
    auto tempCue = deckTree.getChildWithName (IDs::TempCue);
    tempCue.setProperty (IDs::position, (int64_t) -1, nullptr);

    // Reset CuePoints
    auto cuePoints = deckTree.getChildWithName (IDs::CuePoints);
    for (int i = 0; i < cuePoints.getNumChildren(); ++i)
    {
        auto cp = cuePoints.getChild (i);
        cp.setProperty (IDs::position,   (int64_t) -1, nullptr);
        cp.setProperty (IDs::colorIndex, 0, nullptr);
        cp.setProperty (IDs::label,      "", nullptr);
        cp.setProperty (IDs::isValid,    false, nullptr);
    }

    // Reset BeatGrid
    auto beatGrid = deckTree.getChildWithName (IDs::BeatGrid);
    beatGrid.setProperty (IDs::bpm,                  0.0, nullptr);
    beatGrid.setProperty (IDs::anchorSample,         (int64_t) 0, nullptr);
    beatGrid.setProperty (IDs::beatIntervalSamples,  0.0, nullptr);
    beatGrid.setProperty (IDs::confidence,           0.0f, nullptr);
    beatGrid.setProperty (IDs::manuallyAdjusted,     false, nullptr);
    beatGrid.setProperty (IDs::analysisStatus,       "idle", nullptr);
    beatGrid.setProperty (IDs::analysisProgress,     0.0f, nullptr);

    // Reset KeyInfo
    auto keyInfo = deckTree.getChildWithName (IDs::KeyInfo);
    keyInfo.setProperty (IDs::keyIndex,          -1, nullptr);
    keyInfo.setProperty (IDs::confidence,        0.0f, nullptr);
    keyInfo.setProperty (IDs::manuallyAdjusted,  false, nullptr);
    keyInfo.setProperty (IDs::analysisStatus,    "idle", nullptr);
    keyInfo.setProperty (IDs::analysisProgress,  0.0f, nullptr);

    // Reset Loop
    auto loop = deckTree.getChildWithName (IDs::Loop);
    loop.setProperty (IDs::loopIn,   (int64_t) -1, nullptr);
    loop.setProperty (IDs::loopOut,  (int64_t) -1, nullptr);
    loop.setProperty (IDs::active,   false, nullptr);
    loop.setProperty (IDs::loopMode, 0,     nullptr);

    // Reset Stems
    auto stems = deckTree.getChildWithName (IDs::Stems);
    stems.setProperty (IDs::status,       "none", nullptr);
    stems.setProperty (IDs::progress,     0.0f, nullptr);
    stems.setProperty (IDs::stemError,    "",   nullptr);
    stems.setProperty (IDs::modelVersion, "",   nullptr);
    stems.setProperty (IDs::vocalsMuted,  false, nullptr);
    stems.setProperty (IDs::drumsMuted,   false, nullptr);
    stems.setProperty (IDs::bassMuted,    false, nullptr);
    stems.setProperty (IDs::otherMuted,   false, nullptr);
    stems.setProperty (IDs::vocalsSolo,   false, nullptr);
    stems.setProperty (IDs::drumsSolo,    false, nullptr);
    stems.setProperty (IDs::bassSolo,     false, nullptr);
    stems.setProperty (IDs::otherSolo,    false, nullptr);

    // Reset Waveform
    auto waveform = deckTree.getChildWithName (IDs::Waveform);
    waveform.setProperty (IDs::analysisStatus,   "none", nullptr);
    waveform.setProperty (IDs::analysisProgress, 0.0f, nullptr);
}

juce::String DeckStateManager::getPlaybackStatus (const juce::ValueTree& deckTree) const
{
    return deckTree.getProperty (IDs::playbackStatus).toString();
}

bool DeckStateManager::isValidTransition (const juce::String& from, const juce::String& to) const
{
    if (from == "empty"   && to == "stopped") return true;
    if (from == "stopped" && to == "playing") return true;
    if (from == "playing" && to == "paused")  return true;
    if (from == "playing" && to == "stopped") return true;
    if (from == "paused"  && to == "playing") return true;
    if (from == "paused"  && to == "stopped") return true;
    if (to   == "empty"   && from != "playing") return true;
    return false;
}

juce::String DeckStateManager::findNextAvailableLetter() const
{
    for (const auto& letter : letterPool)
    {
        if (usedLetters.find (letter) == usedLetters.end())
            return letter;
    }
    return {};
}
