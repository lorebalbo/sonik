#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/AudioEngine/DeckAudioSource.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/Deck/SourceModeReader.h"

// =============================================================================
// PRD-0062 — Deck Stem-Source Selection (Original vs Separated Stems)
//
// Verifies: the sourceMode ValueTree property (default + reset), the derived
// stemsActive flag (buffer presence no longer activates stems), locked-to-
// original behaviour when no stems exist, mode-switch independence from mute
// state, a click-free crossfade on switch, clearDeckStemBuffers forcing the
// mode back to original, and the published SourceModeReader lane mapping for
// every row of the §1.3 mapping contract.
// =============================================================================
class DeckSourceModeTests : public juce::UnitTest
{
public:
    DeckSourceModeTests() : juce::UnitTest ("Deck Source Mode", "Sonik") {}

    void runTest() override
    {
        // ValueTree property contract
        testDefaultSourceModeIsOriginal();
        testTrackLoadResetsSourceMode();

        // Derived stemsActive flag (observed via audio output)
        testBufferPresenceDoesNotActivateStems();
        testSetSourceModeStemsActivatesStems();
        testLockedToOriginalWhenNoStems();
        testModeSwitchIndependentOfMuteState();
        testClickFreeCrossfadeOnSwitch();
        testClearStemBuffersForcesOriginal();

        // Published lane mapping (SourceModeReader)
        testLaneMappingOriginal();
        testLaneMappingStemsBothAudible();
        testLaneMappingStemsVocalMuted();
        testLaneMappingStemsInstMuted();
    }

private:
    // -----------------------------------------------------------------------
    // Engine harness (mirrors StemPlaybackTests)
    // -----------------------------------------------------------------------
    struct EngineContext
    {
        juce::ValueTree rootState { IDs::SonikState };
        std::unique_ptr<AudioEngine> engine;
        EngineContext() { engine = std::make_unique<AudioEngine> (rootState); }
        ~EngineContext() { engine.reset(); }
    };

    AudioBufferHolder::Ptr makeConstBuffer (float l, float r, int frames, double sr = 44100.0)
    {
        juce::AudioBuffer<float> buf (2, frames);
        for (int i = 0; i < frames; ++i)
        {
            buf.setSample (0, i, l);
            buf.setSample (1, i, r);
        }
        return new AudioBufferHolder (std::move (buf), sr, static_cast<int64_t> (frames));
    }

    void runBlock (AudioEngine& engine, float* outL, float* outR, int n)
    {
        float* outputs[2] = { outL, outR };
        engine.audioDeviceIOCallbackWithContext (nullptr, 0, outputs, 2, n, {});
    }

    // Set up a playing deck with a main buffer and (optionally) stem buffers.
    // Returns nothing; caller drives source mode and reads output.
    void setupPlayingDeck (EngineContext& ctx, DeckAudioState& audioState,
                           float mainL, float mainR, bool withStems,
                           float vL = 0.1f, float dL = 0.2f, float bL = 0.15f, float oL = 0.05f)
    {
        ctx.engine->registerDeck ("A", &audioState);
        ctx.engine->setDeckBuffer ("A", makeConstBuffer (mainL, mainR, 8192));

        if (withStems)
        {
            ctx.engine->setDeckStemBuffers ("A",
                makeConstBuffer (vL, vL, 8192),
                makeConstBuffer (dL, dL, 8192),
                makeConstBuffer (bL, bL, 8192),
                makeConstBuffer (oL, oL, 8192));
        }

        audioState.playbackStatus.store (static_cast<int> (PlaybackStatusCode::playing),
                                         std::memory_order_relaxed);
    }

    // Run several blocks to settle past any fade-in / crossfade, return the
    // last left-channel sample.
    float settledLeft (AudioEngine& engine)
    {
        constexpr int n = 256;
        float l[n] = {}, r[n] = {};
        for (int i = 0; i < 5; ++i)
        {
            std::fill (l, l + n, 0.0f);
            std::fill (r, r + n, 0.0f);
            runBlock (engine, l, r, n);
        }
        return l[n - 1];
    }

    // -----------------------------------------------------------------------
    // ValueTree property contract
    // -----------------------------------------------------------------------
    void testDefaultSourceModeIsOriginal()
    {
        beginTest ("sourceMode defaults to \"original\" on deck creation");

        auto dbFile = juce::File::createTempFile ("sonik_srcmode_default.db");
        TrackDatabase db (dbFile);
        DeckStateManager mgr (db);
        auto deckId = mgr.addDeck();
        auto deck   = mgr.getDeckState (deckId);

        expect (deck.isValid(), "deck tree should be valid");
        expectEquals (deck.getProperty (IDs::sourceMode).toString(), juce::String ("original"));

        dbFile.deleteFile();
    }

    void testTrackLoadResetsSourceMode()
    {
        beginTest ("loading a new track forces sourceMode back to \"original\"");

        auto dbFile = juce::File::createTempFile ("sonik_srcmode_reset.db");
        TrackDatabase db (dbFile);
        DeckStateManager mgr (db);
        auto deckId = mgr.addDeck();
        auto deck   = mgr.getDeckState (deckId);

        // Pretend the DJ had switched this deck to stems.
        deck.setProperty (IDs::sourceMode, "stems", nullptr);

        TrackMetadata meta;
        meta.filePath    = "/tmp/whatever.wav";
        meta.contentHash = "deadbeef";
        meta.duration    = 200.0;
        mgr.loadTrack (deckId, meta);

        expectEquals (deck.getProperty (IDs::sourceMode).toString(), juce::String ("original"));

        dbFile.deleteFile();
    }

    // -----------------------------------------------------------------------
    // Derived stemsActive
    // -----------------------------------------------------------------------
    void testBufferPresenceDoesNotActivateStems()
    {
        beginTest ("stem buffers present but sourceMode=original -> output is the original buffer");
        EngineContext ctx;
        DeckAudioState audioState;
        // main L = 0.7, stems sum L = 0.5 — distinct.
        setupPlayingDeck (ctx, audioState, 0.7f, 0.7f, /*withStems*/ true);

        const float out = settledLeft (*ctx.engine);
        expect (std::abs (out - 0.7f) < 0.02f,
                "Expected original ~0.7 (stems must NOT auto-activate), got " + juce::String (out));
    }

    void testSetSourceModeStemsActivatesStems()
    {
        beginTest ("setDeckSourceMode(true) activates stems -> output is the summed stems");
        EngineContext ctx;
        DeckAudioState audioState;
        setupPlayingDeck (ctx, audioState, 0.7f, 0.7f, /*withStems*/ true);

        // Settle on original first.
        (void) settledLeft (*ctx.engine);

        ctx.engine->setDeckSourceMode ("A", true);
        const float out = settledLeft (*ctx.engine);

        // stems sum L = 0.1 + 0.2 + 0.15 + 0.05 = 0.5
        expect (std::abs (out - 0.5f) < 0.02f,
                "Expected summed stems ~0.5 after switching to stems, got " + juce::String (out));
    }

    void testLockedToOriginalWhenNoStems()
    {
        beginTest ("setDeckSourceMode(true) is a no-op when no stem buffers exist");
        EngineContext ctx;
        DeckAudioState audioState;
        setupPlayingDeck (ctx, audioState, 0.6f, 0.6f, /*withStems*/ false);

        ctx.engine->setDeckSourceMode ("A", true);   // should be ignored
        const float out = settledLeft (*ctx.engine);

        expect (std::abs (out - 0.6f) < 0.02f,
                "Expected original ~0.6 (locked to original with no stems), got " + juce::String (out));
    }

    void testModeSwitchIndependentOfMuteState()
    {
        beginTest ("source mode is independent of VOC/INST mute state");
        EngineContext ctx;
        DeckAudioState audioState;
        setupPlayingDeck (ctx, audioState, 0.7f, 0.7f, /*withStems*/ true);

        // Both stems unmuted but original mode → original, NOT the summed stems.
        const float orig = settledLeft (*ctx.engine);
        expect (std::abs (orig - 0.7f) < 0.02f,
                "Original mode ignores unmuted stems, got " + juce::String (orig));

        // Now mute vocals, switch to stems → sum minus vocals = 0.2+0.15+0.05 = 0.4
        audioState.stemVocalsMuted.store (true, std::memory_order_relaxed);
        ctx.engine->setDeckSourceMode ("A", true);
        const float stems = settledLeft (*ctx.engine);
        expect (std::abs (stems - 0.4f) < 0.02f,
                "Stems mode applies the vocal mute (expected ~0.4), got " + juce::String (stems));
    }

    void testClickFreeCrossfadeOnSwitch()
    {
        beginTest ("switching source mode crossfades click-free (no hard sample jump)");
        EngineContext ctx;
        DeckAudioState audioState;
        setupPlayingDeck (ctx, audioState, 0.7f, 0.7f, /*withStems*/ true);

        // Settle fully on the original buffer.
        (void) settledLeft (*ctx.engine);

        // Flip to stems, then capture the very next block which contains the
        // 64-sample activation crossfade from 0.7 → 0.5.
        ctx.engine->setDeckSourceMode ("A", true);

        constexpr int n = 256;
        float l[n] = {}, r[n] = {};
        runBlock (*ctx.engine, l, r, n);

        // No single-sample discontinuity larger than a small threshold — a hard
        // splice (no crossfade) would jump 0.2 in one sample.
        float maxStep = 0.0f;
        for (int i = 1; i < n; ++i)
            maxStep = std::max (maxStep, std::abs (l[i] - l[i - 1]));

        expect (maxStep < 0.05f,
                "Crossfade should avoid a hard splice; max per-sample step = " + juce::String (maxStep));

        // And by end of the block it has reached the stem sum.
        expect (std::abs (l[n - 1] - 0.5f) < 0.02f,
                "End of crossfade block should be at the stem sum ~0.5, got " + juce::String (l[n - 1]));
    }

    void testClearStemBuffersForcesOriginal()
    {
        beginTest ("clearDeckStemBuffers forces sourceMode back to \"original\"");
        EngineContext ctx;

        // Build a deck node under the engine's root so clearDeckStemBuffers can
        // find and reset it.
        auto decks = ctx.rootState.getOrCreateChildWithName (IDs::Decks, nullptr);
        juce::ValueTree deck (IDs::Deck);
        deck.setProperty (IDs::id, "A", nullptr);
        deck.setProperty (IDs::sourceMode, "stems", nullptr);
        decks.addChild (deck, -1, nullptr);

        DeckAudioState audioState;
        ctx.engine->registerDeck ("A", &audioState);
        ctx.engine->setDeckBuffer ("A", makeConstBuffer (0.5f, 0.5f, 4096));
        ctx.engine->setDeckStemBuffers ("A",
            makeConstBuffer (0.1f, 0.1f, 4096), makeConstBuffer (0.2f, 0.2f, 4096),
            makeConstBuffer (0.1f, 0.1f, 4096), makeConstBuffer (0.1f, 0.1f, 4096));
        ctx.engine->setDeckSourceMode ("A", true);

        ctx.engine->clearDeckStemBuffers ("A");

        expectEquals (deck.getProperty (IDs::sourceMode).toString(), juce::String ("original"));
    }

    // -----------------------------------------------------------------------
    // Published lane mapping (SourceModeReader)
    // -----------------------------------------------------------------------
    juce::ValueTree makeReaderDeck (const juce::String& mode,
                                    bool vocalsMuted, bool drumsMuted,
                                    bool bassMuted, bool otherMuted)
    {
        juce::ValueTree deck (IDs::Deck);
        deck.setProperty (IDs::id, "A", nullptr);
        deck.setProperty (IDs::sourceMode, mode, nullptr);

        juce::ValueTree stems (IDs::Stems);
        stems.setProperty (IDs::vocalsMuted, vocalsMuted, nullptr);
        stems.setProperty (IDs::drumsMuted,  drumsMuted,  nullptr);
        stems.setProperty (IDs::bassMuted,   bassMuted,   nullptr);
        stems.setProperty (IDs::otherMuted,  otherMuted,  nullptr);
        deck.addChild (stems, -1, nullptr);
        return deck;
    }

    void testLaneMappingOriginal()
    {
        beginTest ("lane mapping: original -> { Original }");
        auto deck = makeReaderDeck ("original", false, false, false, false);
        SourceModeReader reader (deck);
        auto lanes = reader.getPublishedLanes();
        expect (lanes.size() == 1, "expected a single lane");
        expect (lanes.size() == 1 && lanes[0] == SourceModeReader::Lane::Original,
                "expected Original lane");
    }

    void testLaneMappingStemsBothAudible()
    {
        beginTest ("lane mapping: stems, both audible -> { Instrumental, Vocal }");
        auto deck = makeReaderDeck ("stems", false, false, false, false);
        SourceModeReader reader (deck);
        auto lanes = reader.getPublishedLanes();
        expect (lanes.size() == 2, "expected two lanes");
        const bool hasInst  = std::find (lanes.begin(), lanes.end(), SourceModeReader::Lane::Instrumental) != lanes.end();
        const bool hasVocal = std::find (lanes.begin(), lanes.end(), SourceModeReader::Lane::Vocal) != lanes.end();
        expect (hasInst && hasVocal, "expected both Instrumental and Vocal lanes");
    }

    void testLaneMappingStemsVocalMuted()
    {
        beginTest ("lane mapping: stems, VOC muted -> { Instrumental }");
        auto deck = makeReaderDeck ("stems", true, false, false, false);
        SourceModeReader reader (deck);
        auto lanes = reader.getPublishedLanes();
        expect (lanes.size() == 1, "expected a single lane");
        expect (lanes.size() == 1 && lanes[0] == SourceModeReader::Lane::Instrumental,
                "expected only the Instrumental lane");
    }

    void testLaneMappingStemsInstMuted()
    {
        beginTest ("lane mapping: stems, INST muted -> { Vocal }");
        // INST is the summed instrumental — all three members muted.
        auto deck = makeReaderDeck ("stems", false, true, true, true);
        SourceModeReader reader (deck);
        auto lanes = reader.getPublishedLanes();
        expect (lanes.size() == 1, "expected a single lane");
        expect (lanes.size() == 1 && lanes[0] == SourceModeReader::Lane::Vocal,
                "expected only the Vocal lane");
    }
};

static DeckSourceModeTests deckSourceModeTests;
