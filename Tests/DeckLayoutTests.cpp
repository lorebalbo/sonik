#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/Deck/Database/TrackDatabase.h"

class DeckLayoutTests : public juce::UnitTest
{
public:
    DeckLayoutTests() : juce::UnitTest ("Deck Layout & getDeckIds", "Sonik") {}

    void runTest() override
    {
        testGetDeckIdsInitial();
        testGetDeckIdsAfterAdd();
        testGetDeckIdsAfterRemove();
        testAddDeckRespectsMax();
        testRemoveDeckRespectsMin();
        testSetActiveDeck();
        testSetActiveDeckInvalid();
        testNewDeckBecomesActive();
    }

private:
    struct TestContext
    {
        juce::File dbFile;
        std::unique_ptr<TrackDatabase> db;
        std::unique_ptr<DeckStateManager> mgr;

        TestContext()
        {
            dbFile = juce::File::createTempFile ("sonik_layout_test.db");
            db = std::make_unique<TrackDatabase> (dbFile);
            mgr = std::make_unique<DeckStateManager> (*db);
        }

        ~TestContext()
        {
            mgr.reset();
            db.reset();
            dbFile.deleteFile();
        }
    };

    // -----------------------------------------------------------------------
    void testGetDeckIdsInitial()
    {
        beginTest ("getDeckIds returns initial decks (2 default)");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B

        auto ids = ctx.mgr->getDeckIds();
        expectEquals (ids.size(), 2);
        expectEquals (ids[0], juce::String ("A"));
        expectEquals (ids[1], juce::String ("B"));
    }

    // -----------------------------------------------------------------------
    void testGetDeckIdsAfterAdd()
    {
        beginTest ("getDeckIds after addDeck returns 3 IDs");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B
        ctx.mgr->addDeck(); // C

        auto ids = ctx.mgr->getDeckIds();
        expectEquals (ids.size(), 3);
        expectEquals (ids[0], juce::String ("A"));
        expectEquals (ids[1], juce::String ("B"));
        expectEquals (ids[2], juce::String ("C"));
    }

    // -----------------------------------------------------------------------
    void testGetDeckIdsAfterRemove()
    {
        beginTest ("getDeckIds after removeDeck returns count back to 2");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B
        ctx.mgr->addDeck(); // C

        expect (ctx.mgr->removeDeck ("C"));

        auto ids = ctx.mgr->getDeckIds();
        expectEquals (ids.size(), 2);
        expectEquals (ids[0], juce::String ("A"));
        expectEquals (ids[1], juce::String ("B"));
    }

    // -----------------------------------------------------------------------
    void testAddDeckRespectsMax()
    {
        beginTest ("addDeck respects max 4 decks");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B
        ctx.mgr->addDeck(); // C
        ctx.mgr->addDeck(); // D

        expectEquals (ctx.mgr->getDeckCount(), 4);

        auto fifth = ctx.mgr->addDeck();
        expect (fifth.isEmpty(), "5th addDeck should return empty string");
        expectEquals (ctx.mgr->getDeckCount(), 4);
    }

    // -----------------------------------------------------------------------
    void testRemoveDeckRespectsMin()
    {
        beginTest ("removeDeck respects min 1 deck");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B

        expect (ctx.mgr->removeDeck ("B"));
        expectEquals (ctx.mgr->getDeckCount(), 1);

        expect (! ctx.mgr->canRemoveDeck ("A"), "Should not be able to remove last deck");
        expect (! ctx.mgr->removeDeck ("A"), "removeDeck should fail on last deck");
        expectEquals (ctx.mgr->getDeckCount(), 1);
    }

    // -----------------------------------------------------------------------
    void testSetActiveDeck()
    {
        beginTest ("setActiveDeck changes activeDeckId");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B

        expectEquals (ctx.mgr->getActiveDeckId(), juce::String ("A"));

        ctx.mgr->setActiveDeck ("B");
        expectEquals (ctx.mgr->getActiveDeckId(), juce::String ("B"));
    }

    // -----------------------------------------------------------------------
    void testSetActiveDeckInvalid()
    {
        beginTest ("setActiveDeck to invalid ID is no-op");
        TestContext ctx;
        ctx.mgr->addDeck(); // A
        ctx.mgr->addDeck(); // B

        expectEquals (ctx.mgr->getActiveDeckId(), juce::String ("A"));

        ctx.mgr->setActiveDeck ("Z");
        expectEquals (ctx.mgr->getActiveDeckId(), juce::String ("A"));
    }

    // -----------------------------------------------------------------------
    void testNewDeckBecomesActive()
    {
        beginTest ("New deck becomes active when it is the first deck");
        TestContext ctx;

        auto firstId = ctx.mgr->addDeck(); // A
        expectEquals (ctx.mgr->getActiveDeckId(), firstId);

        // Adding a second deck should NOT change the active deck
        // (only the first deck auto-activates)
        ctx.mgr->addDeck(); // B
        expectEquals (ctx.mgr->getActiveDeckId(), firstId);
    }
};

static DeckLayoutTests deckLayoutTests;
