// =============================================================================
// StemSeparationUiTests
//
// Automated coverage for the stem-separation UI surface (PRD-0023):
//   - StemToggleComponent: ValueTree mute contract, group (INSTRUMENTAL)
//     semantics per §1.5.1, click-gating on readiness, paint smoke in every
//     visual state.
//   - StemSeparateButton: lifecycle-driven paint smoke across every status
//     (none / queued / separating / loading_cached / ready / error /
//     model_unavailable + persistent-failure), tooltip contract, and
//     disabled-state click gating.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "Features/StemSeparation/UI/StemToggleComponent.h"
#include "Features/StemSeparation/UI/StemSeparateButton.h"
#include "Features/StemSeparation/UI/SourceModeToggleComponent.h"
#include "Features/StemSeparation/StemSeparationManager.h"
#include "Features/StemSeparation/ModelManager.h"
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/Deck/DeckIdentifiers.h"

// =============================================================================
namespace
{
    juce::MouseEvent makeMouseEventAt (juce::Component& target, float x, float y)
    {
        auto source = juce::Desktop::getInstance().getMainMouseSource();
        return juce::MouseEvent (source,
                                 juce::Point<float> (x, y),
                                 juce::ModifierKeys(),
                                 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &target, &target,
                                 juce::Time::getCurrentTime(),
                                 juce::Point<float> (x, y),
                                 juce::Time::getCurrentTime(),
                                 1, false);
    }

    juce::MouseEvent makeDummyMouseEvent (juce::Component& target)
    {
        return makeMouseEventAt (target, 8.0f, 8.0f);
    }

    // Paint the component into an off-screen image. Returns true if it did not
    // throw / assert (smoke test for every visual branch).
    void paintSmoke (juce::Component& c, int w = 40, int h = 120)
    {
        c.setBounds (0, 0, w, h);
        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        c.paint (g);
    }

    // Pump the message loop so callAsync'd refreshState lambdas run, letting
    // the component observe external ValueTree writes synchronously.
    void pump (int ms = 30)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
    }
}

// =============================================================================
// StemToggleComponent
// =============================================================================
class StemToggleComponentTests : public juce::UnitTest
{
public:
    StemToggleComponentTests()
        : juce::UnitTest ("StemToggleComponent", "Sonik") {}

    void runTest() override
    {
        testIgnoresClicksWhenNotReady();
        testVocalToggleFlipsSingleProperty();
        testInstrumentalTogglesAllThreeTogether();
        testInstrumentalMixedStateClickMutesAll();
        testInstrumentalAllMutedClickUnmutesAll();
        testPaintSmokeAcrossStates();
    }

private:
    static juce::ValueTree makeStemsNode (const juce::String& status)
    {
        juce::ValueTree stems (IDs::Stems);
        stems.setProperty (IDs::status, status, nullptr);
        stems.setProperty (IDs::vocalsMuted, false, nullptr);
        stems.setProperty (IDs::drumsMuted,  false, nullptr);
        stems.setProperty (IDs::bassMuted,   false, nullptr);
        stems.setProperty (IDs::otherMuted,  false, nullptr);
        return stems;
    }

    void testIgnoresClicksWhenNotReady()
    {
        beginTest ("Click is ignored while stems are not ready");

        for (auto status : { "none", "queued", "separating", "error" })
        {
            auto stems = makeStemsNode (status);
            StemToggleComponent toggle (stems, "VOCALS", { IDs::vocalsMuted });

            auto ev = makeDummyMouseEvent (toggle);
            toggle.mouseDown (ev);

            expect (! static_cast<bool> (stems.getProperty (IDs::vocalsMuted)),
                    juce::String ("vocalsMuted must stay false when status=") + status);
        }
    }

    void testVocalToggleFlipsSingleProperty()
    {
        beginTest ("VOCALS toggle flips vocalsMuted on each click");

        auto stems = makeStemsNode ("ready");
        StemToggleComponent toggle (stems, "VOCALS", { IDs::vocalsMuted });

        auto ev = makeDummyMouseEvent (toggle);

        toggle.mouseDown (ev);
        expect (static_cast<bool> (stems.getProperty (IDs::vocalsMuted)),
                "first click mutes vocals");

        toggle.mouseDown (ev);
        expect (! static_cast<bool> (stems.getProperty (IDs::vocalsMuted)),
                "second click unmutes vocals");

        // Other lanes are never touched by the VOCALS toggle.
        expect (! static_cast<bool> (stems.getProperty (IDs::drumsMuted)));
        expect (! static_cast<bool> (stems.getProperty (IDs::bassMuted)));
        expect (! static_cast<bool> (stems.getProperty (IDs::otherMuted)));
    }

    void testInstrumentalTogglesAllThreeTogether()
    {
        beginTest ("INSTRUMENTAL toggle mutes/unmutes drums+bass+other together");

        auto stems = makeStemsNode ("ready");
        StemToggleComponent toggle (stems, "INSTRUMENTAL",
                                    { IDs::drumsMuted, IDs::bassMuted, IDs::otherMuted });

        auto ev = makeDummyMouseEvent (toggle);

        toggle.mouseDown (ev);
        expect (static_cast<bool> (stems.getProperty (IDs::drumsMuted)), "drums muted");
        expect (static_cast<bool> (stems.getProperty (IDs::bassMuted)),  "bass muted");
        expect (static_cast<bool> (stems.getProperty (IDs::otherMuted)), "other muted");
        // Vocals untouched.
        expect (! static_cast<bool> (stems.getProperty (IDs::vocalsMuted)));

        toggle.mouseDown (ev);
        expect (! static_cast<bool> (stems.getProperty (IDs::drumsMuted)), "drums unmuted");
        expect (! static_cast<bool> (stems.getProperty (IDs::bassMuted)),  "bass unmuted");
        expect (! static_cast<bool> (stems.getProperty (IDs::otherMuted)), "other unmuted");
    }

    void testInstrumentalMixedStateClickMutesAll()
    {
        beginTest ("INSTRUMENTAL: a mixed group reads as unmuted; next click mutes all (§1.5.1)");

        auto stems = makeStemsNode ("ready");
        // External agent puts the group in a mixed state.
        stems.setProperty (IDs::drumsMuted, true, nullptr);

        StemToggleComponent toggle (stems, "INSTRUMENTAL",
                                    { IDs::drumsMuted, IDs::bassMuted, IDs::otherMuted });

        auto ev = makeDummyMouseEvent (toggle);
        toggle.mouseDown (ev);

        expect (static_cast<bool> (stems.getProperty (IDs::drumsMuted)), "drums muted");
        expect (static_cast<bool> (stems.getProperty (IDs::bassMuted)),  "bass muted");
        expect (static_cast<bool> (stems.getProperty (IDs::otherMuted)), "other muted");
    }

    void testInstrumentalAllMutedClickUnmutesAll()
    {
        beginTest ("INSTRUMENTAL: when all three muted, click unmutes all");

        auto stems = makeStemsNode ("ready");
        stems.setProperty (IDs::drumsMuted, true, nullptr);
        stems.setProperty (IDs::bassMuted,  true, nullptr);
        stems.setProperty (IDs::otherMuted, true, nullptr);

        StemToggleComponent toggle (stems, "INSTRUMENTAL",
                                    { IDs::drumsMuted, IDs::bassMuted, IDs::otherMuted });

        auto ev = makeDummyMouseEvent (toggle);
        toggle.mouseDown (ev);

        expect (! static_cast<bool> (stems.getProperty (IDs::drumsMuted)), "drums unmuted");
        expect (! static_cast<bool> (stems.getProperty (IDs::bassMuted)),  "bass unmuted");
        expect (! static_cast<bool> (stems.getProperty (IDs::otherMuted)), "other unmuted");
    }

    void testPaintSmokeAcrossStates()
    {
        beginTest ("Paints without crashing in disabled / unmuted / muted states");

        // Disabled (not ready).
        {
            auto stems = makeStemsNode ("none");
            StemToggleComponent toggle (stems, "VOCALS", { IDs::vocalsMuted });
            paintSmoke (toggle);
        }
        // Ready + unmuted.
        {
            auto stems = makeStemsNode ("ready");
            StemToggleComponent toggle (stems, "VOCALS", { IDs::vocalsMuted });
            paintSmoke (toggle);
        }
        // Ready + fully muted group.
        {
            auto stems = makeStemsNode ("ready");
            stems.setProperty (IDs::drumsMuted, true, nullptr);
            stems.setProperty (IDs::bassMuted,  true, nullptr);
            stems.setProperty (IDs::otherMuted, true, nullptr);
            StemToggleComponent toggle (stems, "INSTRUMENTAL",
                                        { IDs::drumsMuted, IDs::bassMuted, IDs::otherMuted });
            paintSmoke (toggle);
        }

        expect (true, "all paint passes completed");
    }
};

static StemToggleComponentTests stemToggleComponentTests;

// =============================================================================
// StemSeparateButton
// =============================================================================
class StemSeparateButtonTests : public juce::UnitTest
{
public:
    StemSeparateButtonTests()
        : juce::UnitTest ("StemSeparateButton", "Sonik") {}

    void runTest() override
    {
        testPaintSmokeAcrossStatuses();
        testTooltipContract();
        testDisabledStatesIgnoreClick();
        testPersistentFailurePresentation();
    }

private:
    // Owns the full real dependency stack the button needs.
    struct Fixture
    {
        juce::ValueTree root { IDs::SonikState };
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;
        std::unique_ptr<DeckStateManager> deckState;
        std::unique_ptr<ModelManager> modelManager;
        std::unique_ptr<AudioEngine> engine;
        std::unique_ptr<StemSeparationManager> manager;
        juce::ValueTree deckTree;

        juce::String deckId;

        Fixture()
        {
            dbFile = juce::File::createTempFile ("sonik_stemui_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
            deckState = std::make_unique<DeckStateManager> (*db);
            modelManager = std::make_unique<ModelManager> (root);
            engine = std::make_unique<AudioEngine> (root);
            manager = std::make_unique<StemSeparationManager> (*deckState, *db, *modelManager, *engine);
            // DeckStateManager starts with zero decks; create one so the
            // deck tree (and its Stems child) is valid.
            deckId = deckState->addDeck();
            deckTree = deckState->getDeckState (deckId);
        }

        ~Fixture()
        {
            manager.reset();
            engine.reset();
            modelManager.reset();
            deckState.reset();
            db.reset();
            dbFile.deleteFile();
        }

        juce::ValueTree stems() { return deckTree.getChildWithName (IDs::Stems); }

        // Pretend a long track is loaded so the deck is not "empty"/short.
        void loadLongTrack()
        {
            deckTree.setProperty (IDs::playbackStatus, "stopped", nullptr);
            auto meta = deckTree.getChildWithName (IDs::TrackMetadata);
            if (! meta.isValid())
            {
                meta = juce::ValueTree (IDs::TrackMetadata);
                deckTree.addChild (meta, -1, nullptr);
            }
            meta.setProperty (IDs::duration, 240.0, nullptr);
            meta.setProperty (IDs::contentHash, "deadbeef", nullptr);
        }
    };

    void testPaintSmokeAcrossStatuses()
    {
        beginTest ("Paints without crashing across every status");

        Fixture fx;
        fx.loadLongTrack();

        StemSeparateButton button (fx.deckTree, *fx.manager, *fx.engine, fx.deckId);

        for (auto status : { "none", "queued", "separating", "loading_cached",
                             "ready", "error", "model_unavailable" })
        {
            fx.stems().setProperty (IDs::stemError, "synthetic failure", nullptr);
            fx.stems().setProperty (IDs::progress, 0.5f, nullptr);
            fx.stems().setProperty (IDs::status, status, nullptr);
            pump();
            paintSmoke (button);
        }

        expect (true, "all status paint passes completed");
    }

    void testTooltipContract()
    {
        beginTest ("Tooltip reflects error / short-track / default contexts");

        // Error tooltip surfaces the stemError message.
        {
            Fixture fx;
            fx.loadLongTrack();
            StemSeparateButton button (fx.deckTree, *fx.manager, *fx.engine, fx.deckId);
            fx.stems().setProperty (IDs::stemError, "Worker crashed", nullptr);
            fx.stems().setProperty (IDs::status, "error", nullptr);
            pump();
            // The error tooltip is deterministic: the model-readiness synthesis
            // only applies to status=="none", so it never overrides "error".
            expectEquals (button.getTooltip(), juce::String ("Worker crashed"));
        }

        // Short-track tooltip. A short loaded track is a "disabled" reason. The
        // exact message depends on whether the ONNX model file is present on the
        // host: with a model present the short-track reason wins; without it,
        // status=="none" synthesises model_unavailable (also a valid disabled
        // reason). Accept either so the test is host-independent.
        {
            Fixture fx;
            fx.deckTree.setProperty (IDs::playbackStatus, "stopped", nullptr);
            auto meta = fx.deckTree.getChildWithName (IDs::TrackMetadata);
            if (! meta.isValid())
            {
                meta = juce::ValueTree (IDs::TrackMetadata);
                fx.deckTree.addChild (meta, -1, nullptr);
            }
            meta.setProperty (IDs::duration, 2.0, nullptr);
            StemSeparateButton button (fx.deckTree, *fx.manager, *fx.engine, fx.deckId);
            fx.stems().setProperty (IDs::status, "none", nullptr);
            pump();
            const auto tip = button.getTooltip();
            expect (tip == "Track too short for stem separation"
                        || tip.startsWith ("Place htdemucs.onnx"),
                    "short loaded track surfaces a disabled-reason tooltip, got: " + tip);
        }
    }

    void testDisabledStatesIgnoreClick()
    {
        beginTest ("Empty deck click never starts a separation");

        Fixture fx; // deck stays "empty"
        StemSeparateButton button (fx.deckTree, *fx.manager, *fx.engine, fx.deckId);

        auto ev = makeDummyMouseEvent (button);
        button.mouseDown (ev);
        pump();

        expectEquals (fx.stems().getProperty (IDs::status).toString(), juce::String ("none"),
                      "status must remain 'none' for an empty deck");
    }

    void testPersistentFailurePresentation()
    {
        beginTest ("Repeated error transitions paint the persistent-failure state");

        Fixture fx;
        fx.loadLongTrack();
        StemSeparateButton button (fx.deckTree, *fx.manager, *fx.engine, fx.deckId);

        // Drive several error<->none transitions to accumulate the counter.
        for (int i = 0; i < 4; ++i)
        {
            fx.stems().setProperty (IDs::stemError, "fail", nullptr);
            fx.stems().setProperty (IDs::status, "error", nullptr);
            pump();
            paintSmoke (button);
            fx.stems().setProperty (IDs::status, "none", nullptr);
            pump();
        }

        // Final error paint exercises the persistent-failure branch.
        fx.stems().setProperty (IDs::status, "error", nullptr);
        pump();
        paintSmoke (button);

        expect (true, "persistent-failure presentation painted without crashing");
    }
};

static StemSeparateButtonTests stemSeparateButtonTests;

// =============================================================================
// SourceModeToggleComponent (PRD-0062)
// =============================================================================
class SourceModeToggleComponentTests : public juce::UnitTest
{
public:
    SourceModeToggleComponentTests()
        : juce::UnitTest ("SourceModeToggleComponent", "Sonik") {}

    void runTest() override
    {
        testPaintSmokeAcrossStates();
        testStemsClickIgnoredUntilReady();
        testReadyClicksFlipSourceMode();
        testExternalSourceModeWriteRefreshes();
    }

private:
    // Reuses the same real dependency stack as the button tests.
    struct Fixture
    {
        juce::ValueTree root { IDs::SonikState };
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;
        std::unique_ptr<DeckStateManager> deckState;
        std::unique_ptr<ModelManager> modelManager;
        std::unique_ptr<AudioEngine> engine;
        juce::ValueTree deckTree;
        juce::String deckId;

        Fixture()
        {
            dbFile = juce::File::createTempFile ("sonik_srcmode_ui_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
            deckState = std::make_unique<DeckStateManager> (*db);
            modelManager = std::make_unique<ModelManager> (root);
            engine = std::make_unique<AudioEngine> (root);
            deckId = deckState->addDeck();
            deckTree = deckState->getDeckState (deckId);
        }

        ~Fixture()
        {
            engine.reset();
            modelManager.reset();
            deckState.reset();
            db.reset();
            dbFile.deleteFile();
        }

        juce::ValueTree stems() { return deckTree.getChildWithName (IDs::Stems); }
        void setStemsReady (bool ready)
        {
            stems().setProperty (IDs::status, ready ? "ready" : "none", nullptr);
        }
    };

    void testPaintSmokeAcrossStates()
    {
        beginTest ("Paints in both orientations across ready/source states");

        Fixture fx;
        SourceModeToggleComponent toggle (fx.deckTree, *fx.engine, fx.deckId);

        // not ready (STEMS greyed), ready+original, ready+stems — vertical & horizontal.
        for (auto* dims : { "vertical", "horizontal" })
        {
            const bool vertical = juce::String (dims) == "vertical";
            const int w = vertical ? 24 : 120;
            const int h = vertical ? 120 : 24;

            fx.setStemsReady (false);
            pump();
            paintSmoke (toggle, w, h);

            fx.setStemsReady (true);
            fx.deckTree.setProperty (IDs::sourceMode, "original", nullptr);
            pump();
            paintSmoke (toggle, w, h);

            fx.deckTree.setProperty (IDs::sourceMode, "stems", nullptr);
            pump();
            paintSmoke (toggle, w, h);
        }

        expect (true, "all source-mode paint states completed");
    }

    void testStemsClickIgnoredUntilReady()
    {
        beginTest ("Clicking STEMS is a no-op until a ready set exists");

        Fixture fx;
        fx.setStemsReady (false);
        SourceModeToggleComponent toggle (fx.deckTree, *fx.engine, fx.deckId);
        toggle.setBounds (0, 0, 120, 24);   // horizontal: x selects segment
        pump();

        // Click the right (STEMS) half while not ready.
        auto ev = makeMouseEventAt (toggle, 100.0f, 12.0f);
        toggle.mouseDown (ev);
        pump();

        expectEquals (fx.deckTree.getProperty (IDs::sourceMode).toString(),
                      juce::String ("original"),
                      "sourceMode must stay 'original' when STEMS is locked");
    }

    void testReadyClicksFlipSourceMode()
    {
        beginTest ("Ready STEMS/ORIG clicks flip the canonical sourceMode");

        Fixture fx;
        fx.setStemsReady (true);
        SourceModeToggleComponent toggle (fx.deckTree, *fx.engine, fx.deckId);
        toggle.setBounds (0, 0, 120, 24);   // horizontal: left=ORIG, right=STEMS
        pump();

        // Click STEMS (right half) -> sourceMode becomes "stems".
        toggle.mouseDown (makeMouseEventAt (toggle, 100.0f, 12.0f));
        pump();
        expectEquals (fx.deckTree.getProperty (IDs::sourceMode).toString(),
                      juce::String ("stems"),
                      "clicking STEMS when ready selects the stem source");

        // Click ORIG (left half) -> sourceMode returns to "original".
        toggle.mouseDown (makeMouseEventAt (toggle, 20.0f, 12.0f));
        pump();
        expectEquals (fx.deckTree.getProperty (IDs::sourceMode).toString(),
                      juce::String ("original"),
                      "clicking ORIG returns the deck to the original source");
    }

    void testExternalSourceModeWriteRefreshes()
    {
        beginTest ("External sourceMode write repaints without clobbering state");

        Fixture fx;
        fx.setStemsReady (true);
        SourceModeToggleComponent toggle (fx.deckTree, *fx.engine, fx.deckId);
        toggle.setBounds (0, 0, 120, 24);
        pump();

        // Some other surface writes the canonical property directly.
        fx.deckTree.setProperty (IDs::sourceMode, "stems", nullptr);
        pump();
        paintSmoke (toggle, 120, 24);

        expectEquals (fx.deckTree.getProperty (IDs::sourceMode).toString(),
                      juce::String ("stems"),
                      "external write is preserved and observed by the toggle");
    }
};

static SourceModeToggleComponentTests sourceModeToggleComponentTests;