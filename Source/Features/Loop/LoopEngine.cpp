#include "LoopEngine.h"
#include "../Deck/AudioThreadState.h"

LoopEngine::LoopEngine (juce::ValueTree deckTree,
                         AudioEngine& engine,
                         const juce::String& id,
                         TrackDatabase& db)
    : tree (deckTree),
      loopNode (deckTree.getChildWithName (IDs::Loop)),
      trackMetaNode (deckTree.getChildWithName (IDs::TrackMetadata)),
      audioEngine (engine),
      deckId (id),
      database (db)
{
    tree.addListener (this);

    currentContentHash = trackMetaNode.getProperty (IDs::contentHash).toString();
    if (currentContentHash.isNotEmpty())
        loadLoopsFromDB (currentContentHash);
}

LoopEngine::~LoopEngine()
{
    autoSaveCurrentLoop();
    tree.removeListener (this);
}

void LoopEngine::setAudioState (DeckAudioState* state)
{
    audioState = state;
}

void LoopEngine::setBeatGridData (BeatGridData::Ptr data)
{
    beatGridData = std::move (data);
}

// ---------------------------------------------------------------------------
// Auto-loop
// ---------------------------------------------------------------------------

void LoopEngine::autoLoop (float beatCount)
{
    auto status = tree.getProperty (IDs::playbackStatus).toString();
    if (status == "empty")
        return;

    bool loopActive = static_cast<bool> (loopNode.getProperty (IDs::active, false));

    // Toggle off if same size is already active
    if (loopActive && activeAutoBeats == beatCount)
    {
        loopNode.setProperty (IDs::active, false, nullptr);
        activeAutoBeats = 0.0f;
        notifyListeners();
        return;
    }

    double interval = getBeatInterval();
    if (interval <= 0.0)
        return;

    int64_t loopLength = static_cast<int64_t> (static_cast<double> (beatCount) * interval);
    if (loopLength < 128)
        loopLength = 128;

    auto totalSamples = static_cast<int64_t> (trackMetaNode.getProperty (IDs::totalSamples, 0));

    // If loop is already active with different size, keep loop-in and update loop-out
    if (loopActive)
    {
        int64_t existingIn = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
        if (existingIn >= 0)
        {
            int64_t newOut = existingIn + loopLength;
            if (totalSamples > 0 && newOut > totalSamples)
                newOut = totalSamples;
            if (newOut - existingIn < 128)
                return;

            loopNode.setProperty (IDs::loopOut, newOut, nullptr);
            loopNode.setProperty (IDs::loopMode, 1, nullptr);
            activeAutoBeats = beatCount;
            hasPendingLoopIn = false;
            notifyListeners();
            return;
        }
    }

    // Compute loop-in from current playhead
    int64_t playhead = readPlayhead();
    bool quantize = static_cast<bool> (tree.getProperty (IDs::quantizeEnabled, false));

    int64_t loopIn;
    if (quantize && beatGridData != nullptr && beatGridData->beatIntervalSamples > 0.0)
    {
        loopIn = QuantizeService::snapToNearestBeat (
            playhead, beatGridData->anchorSample, beatGridData->beatIntervalSamples);
    }
    else if (quantize && audioState != nullptr)
    {
        auto anchor   = audioState->beatgridAnchor.load (std::memory_order_relaxed);
        auto bgInt    = audioState->beatgridInterval.load (std::memory_order_relaxed);
        if (bgInt > 0.0)
            loopIn = QuantizeService::snapToNearestBeat (playhead, anchor, bgInt);
        else
            loopIn = playhead;
    }
    else
    {
        loopIn = playhead;
    }

    int64_t loopOut = loopIn + loopLength;
    if (totalSamples > 0 && loopOut > totalSamples)
        loopOut = totalSamples;
    if (loopOut - loopIn < 128)
        return;

    loopNode.setProperty (IDs::loopIn,   loopIn,  nullptr);
    loopNode.setProperty (IDs::loopOut,  loopOut,  nullptr);
    loopNode.setProperty (IDs::active,   true,     nullptr);
    loopNode.setProperty (IDs::loopMode, 1,        nullptr);

    activeAutoBeats  = beatCount;
    hasPendingLoopIn = false;
    notifyListeners();
}

// ---------------------------------------------------------------------------
// Manual loop
// ---------------------------------------------------------------------------

void LoopEngine::setLoopIn()
{
    auto status = tree.getProperty (IDs::playbackStatus).toString();
    if (status == "empty")
        return;

    int64_t playhead = readPlayhead();
    bool quantize = static_cast<bool> (tree.getProperty (IDs::quantizeEnabled, false));
    if (quantize && beatGridData != nullptr && beatGridData->beatIntervalSamples > 0.0)
        playhead = beatGridData->getNearestBeat (playhead);

    bool loopActive = static_cast<bool> (loopNode.getProperty (IDs::active, false));

    if (loopActive)
    {
        int64_t currentOut = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));
        if (playhead >= currentOut)
        {
            loopNode.setProperty (IDs::active,   false, nullptr);
            loopNode.setProperty (IDs::loopMode, 0,     nullptr);
            activeAutoBeats = 0.0f;
        }
        else if (currentOut - playhead < 128)
        {
            return;
        }
        else
        {
            loopNode.setProperty (IDs::loopIn,   playhead, nullptr);
            loopNode.setProperty (IDs::loopMode, 2,        nullptr);
            activeAutoBeats = 0.0f;
        }
        notifyListeners();
        return;
    }

    hasPendingLoopIn     = true;
    pendingLoopInSamples = playhead;
    notifyListeners();
}

void LoopEngine::setLoopOut()
{
    auto status = tree.getProperty (IDs::playbackStatus).toString();
    if (status == "empty")
        return;

    bool loopActive = static_cast<bool> (loopNode.getProperty (IDs::active, false));

    if (! hasPendingLoopIn)
    {
        if (loopActive)
        {
            int64_t playhead = readPlayhead();
            bool quantize = static_cast<bool> (tree.getProperty (IDs::quantizeEnabled, false));
            if (quantize && beatGridData != nullptr && beatGridData->beatIntervalSamples > 0.0)
                playhead = beatGridData->getNearestBeat (playhead);

            int64_t currentIn = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
            if (playhead <= currentIn)
            {
                loopNode.setProperty (IDs::active,   false, nullptr);
                loopNode.setProperty (IDs::loopMode, 0,     nullptr);
                activeAutoBeats = 0.0f;
            }
            else if (playhead - currentIn < 128)
            {
                return;
            }
            else
            {
                loopNode.setProperty (IDs::loopOut,  playhead, nullptr);
                loopNode.setProperty (IDs::loopMode, 2,        nullptr);
                activeAutoBeats = 0.0f;
            }
            notifyListeners();
        }
        return;
    }

    int64_t playhead = readPlayhead();
    bool quantize = static_cast<bool> (tree.getProperty (IDs::quantizeEnabled, false));
    if (quantize && beatGridData != nullptr && beatGridData->beatIntervalSamples > 0.0)
        playhead = beatGridData->getNearestBeat (playhead);

    int64_t inPos  = pendingLoopInSamples;
    int64_t outPos = playhead;

    if (outPos < inPos)
        std::swap (inPos, outPos);

    if (outPos - inPos < 128)
        return;

    loopNode.setProperty (IDs::loopIn,   inPos,  nullptr);
    loopNode.setProperty (IDs::loopOut,  outPos,  nullptr);
    loopNode.setProperty (IDs::active,   true,    nullptr);
    loopNode.setProperty (IDs::loopMode, 2,       nullptr);

    activeAutoBeats  = 0.0f;
    hasPendingLoopIn = false;
    notifyListeners();
}

// ---------------------------------------------------------------------------
// Toggle / Re-loop
// ---------------------------------------------------------------------------

void LoopEngine::toggleLoop()
{
    bool active  = static_cast<bool> (loopNode.getProperty (IDs::active, false));
    int64_t lIn  = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
    int64_t lOut = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));

    if (active)
    {
        loopNode.setProperty (IDs::active, false, nullptr);
        activeAutoBeats = 0.0f;
        notifyListeners();
    }
    else if (lIn >= 0 && lOut > lIn)
    {
        reLoop();
    }
}

void LoopEngine::reLoop()
{
    int64_t lIn  = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
    int64_t lOut = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));

    if (lIn < 0 || lOut <= lIn)
        return;

    loopNode.setProperty (IDs::active, true, nullptr);
    audioEngine.seekDeck (deckId, lIn);
    notifyListeners();
}

// ---------------------------------------------------------------------------
// Halve / Double
// ---------------------------------------------------------------------------

void LoopEngine::loopHalve()
{
    int64_t lIn  = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
    int64_t lOut = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));

    if (lIn < 0 || lOut <= lIn)
        return;

    int64_t currentLen = lOut - lIn;
    int64_t newLen     = currentLen / 2;

    double interval = getBeatInterval();
    int64_t minLen  = 128;
    if (interval > 0.0)
    {
        int64_t beatMin = static_cast<int64_t> (interval / 32.0);
        minLen = std::max (minLen, beatMin);
    }

    if (newLen < minLen)
        return;

    loopNode.setProperty (IDs::loopOut, lIn + newLen, nullptr);

    if (activeAutoBeats > 0.0f)
        activeAutoBeats *= 0.5f;

    notifyListeners();
}

void LoopEngine::loopDouble()
{
    int64_t lIn  = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
    int64_t lOut = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));

    if (lIn < 0 || lOut <= lIn)
        return;

    int64_t currentLen = lOut - lIn;
    int64_t newLen     = currentLen * 2;

    auto totalSamples = static_cast<int64_t> (trackMetaNode.getProperty (IDs::totalSamples, 0));
    double interval   = getBeatInterval();
    int64_t maxLen    = (totalSamples > 0) ? (totalSamples - lIn) : newLen;

    if (interval > 0.0)
    {
        int64_t beatMax = static_cast<int64_t> (interval * 64.0);
        maxLen = std::min (maxLen, beatMax);
    }

    if (newLen > maxLen)
        return;

    loopNode.setProperty (IDs::loopOut, lIn + newLen, nullptr);

    if (activeAutoBeats > 0.0f)
        activeAutoBeats *= 2.0f;

    notifyListeners();
}

// ---------------------------------------------------------------------------
// Loop shift (PRD-0015 Beat Jump)
// ---------------------------------------------------------------------------

bool LoopEngine::shiftLoop (int64_t offset, bool snap, int64_t anchor, double beatInterval)
{
    bool active = static_cast<bool> (loopNode.getProperty (IDs::active, false));
    if (! active)
        return false;

    int64_t lIn  = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
    int64_t lOut = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));

    if (lIn < 0 || lOut <= lIn)
        return false;

    int64_t loopLen = lOut - lIn;
    int64_t newIn   = lIn + offset;
    int64_t newOut  = lOut + offset;

    // Snap shifted boundaries to beat grid when quantize is active
    if (snap && beatInterval > 0.0)
    {
        newIn  = QuantizeService::snapToNearestBeat (newIn, anchor, beatInterval);
        newOut = newIn + loopLen;
    }

    auto totalSamples = static_cast<int64_t> (trackMetaNode.getProperty (IDs::totalSamples, 0));

    // Clamp boundaries, preserving loop length
    if (newIn < 0)
    {
        newIn  = 0;
        newOut = loopLen;
    }

    if (totalSamples > 0 && newOut > totalSamples - 1)
    {
        newOut = totalSamples - 1;
        newIn  = newOut - loopLen;
    }

    // If after clamping loop length cannot be preserved, reject
    if (newIn < 0 || (totalSamples > 0 && newOut > totalSamples - 1))
        return false;

    if (newOut - newIn != loopLen)
        return false;

    loopNode.setProperty (IDs::loopIn,  newIn,  nullptr);
    loopNode.setProperty (IDs::loopOut, newOut,  nullptr);
    notifyListeners();
    return true;
}

// ---------------------------------------------------------------------------
// State access
// ---------------------------------------------------------------------------

LoopEngine::LoopInfo LoopEngine::getLoopInfo() const
{
    LoopInfo info;
    info.inSamples      = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
    info.outSamples     = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));
    info.active         = static_cast<bool> (loopNode.getProperty (IDs::active, false));
    info.mode           = static_cast<int> (loopNode.getProperty (IDs::loopMode, 0));
    info.activeAutoBeats = activeAutoBeats;
    info.pendingIn      = hasPendingLoopIn;
    info.pendingInPos   = pendingLoopInSamples;
    return info;
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

void LoopEngine::addListener (Listener* l)    { listeners.add (l); }
void LoopEngine::removeListener (Listener* l) { listeners.remove (l); }
void LoopEngine::notifyListeners()            { listeners.call (&Listener::loopStateChanged); }

// ---------------------------------------------------------------------------
// ValueTree::Listener
// ---------------------------------------------------------------------------

void LoopEngine::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                            const juce::Identifier& property)
{
    if (changedTree == trackMetaNode && property == IDs::contentHash)
    {
        auto newHash = changedTree.getProperty (IDs::contentHash).toString();

        if (newHash != currentContentHash)
        {
            if (currentContentHash.isNotEmpty())
                autoSaveCurrentLoop();

            activeAutoBeats      = 0.0f;
            hasPendingLoopIn     = false;
            pendingLoopInSamples = -1;

            currentContentHash = newHash;
            if (newHash.isNotEmpty())
                loadLoopsFromDB (newHash);

            notifyListeners();
        }
    }

    if (changedTree.hasType (IDs::Loop))
        notifyListeners();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

double LoopEngine::getBeatInterval() const
{
    if (beatGridData != nullptr && beatGridData->beatIntervalSamples > 0.0)
        return beatGridData->beatIntervalSamples;

    if (audioState != nullptr)
    {
        double interval = audioState->beatgridInterval.load (std::memory_order_relaxed);
        if (interval > 0.0)
            return interval;
    }

    // Fallback: 120 BPM
    double sr = getSampleRate();
    return sr * 60.0 / 120.0;
}

double LoopEngine::getSampleRate() const
{
    double sr = static_cast<double> (trackMetaNode.getProperty (IDs::sampleRate, 0.0));
    if (sr > 0.0)
        return sr;
    return 44100.0;
}

int64_t LoopEngine::readPlayhead() const
{
    if (audioState != nullptr)
        return audioState->playheadPosition.load (std::memory_order_relaxed);
    return static_cast<int64_t> (tree.getChildWithName (IDs::Playhead)
                                     .getProperty (IDs::position, 0));
}

// ---------------------------------------------------------------------------
// DB persistence
// ---------------------------------------------------------------------------

void LoopEngine::loadLoopsFromDB (const juce::String& contentHash)
{
    auto json = database.loadLoopsJson (contentHash);
    if (json.isEmpty())
        return;

    auto parsed = juce::JSON::parse (json);
    if (! parsed.isArray())
        return;

    auto* arr = parsed.getArray();
    if (arr == nullptr || arr->isEmpty())
        return;

    auto totalSamples = static_cast<int64_t> (
        trackMetaNode.getProperty (IDs::totalSamples, 0));

    for (const auto& item : *arr)
    {
        if (auto* obj = item.getDynamicObject())
        {
            auto inPos  = static_cast<int64_t> (static_cast<double> (obj->getProperty ("loopIn")));
            auto outPos = static_cast<int64_t> (static_cast<double> (obj->getProperty ("loopOut")));

            if (inPos >= 0 && outPos > inPos)
            {
                if (totalSamples > 0 && (inPos >= totalSamples || outPos > totalSamples))
                    continue;

                loopNode.setProperty (IDs::loopIn,   inPos,  nullptr);
                loopNode.setProperty (IDs::loopOut,  outPos,  nullptr);
                loopNode.setProperty (IDs::active,   false,   nullptr);
                loopNode.setProperty (IDs::loopMode, 0,       nullptr);
                break;
            }
        }
    }
}

void LoopEngine::autoSaveCurrentLoop()
{
    int64_t lIn  = static_cast<int64_t> (loopNode.getProperty (IDs::loopIn, -1));
    int64_t lOut = static_cast<int64_t> (loopNode.getProperty (IDs::loopOut, -1));

    if (lIn < 0 || lOut <= lIn || currentContentHash.isEmpty())
        return;

    auto hash = currentContentHash;
    auto inPos  = lIn;
    auto outPos = lOut;

    dbWritePool.addJob ([this, hash, inPos, outPos]()
    {
        auto obj = std::make_unique<juce::DynamicObject>();
        obj->setProperty ("loopIn",  static_cast<double> (inPos));
        obj->setProperty ("loopOut", static_cast<double> (outPos));
        obj->setProperty ("slot",    0);

        juce::Array<juce::var> arr;
        arr.add (juce::var (obj.release()));

        auto jsonStr = juce::JSON::toString (juce::var (arr));
        database.saveLoopsJson (hash, jsonStr);
    });
}
