//==============================================================================
// PRD-0097: SourceIdResolverTests — distinct-source dedup + clip counting,
// the ordered resolution strategy (library id -> stored path -> content hash),
// StemCache re-derivation, source-keyed batch relocation, and play/export
// gating. JUCE UnitTest, category "Sonik". Headless; no disk, no audio thread.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Session/SessionSchema.h"
#include "Features/Daw/Session/SourceIdResolver.h"

using namespace Daw::Session;

class SourceIdResolverTests final : public juce::UnitTest
{
public:
    SourceIdResolverTests() : juce::UnitTest ("Source Id Resolver", "Sonik") {}

    void runTest() override
    {
        testDistinctDedupAndCounts();
        testOrderedStrategy();
        testStemCacheReDerive();
        testRelocationAndGating();
    }

private:
    static void addClip (juce::ValueTree lane, const juce::String& sourceId, std::int64_t tStart)
    {
        DawClip c;
        c.clipId              = juce::Uuid();
        c.laneId              = juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
        c.sourceFileId        = sourceId;
        c.sourceStartSample   = 0;
        c.sourceEndSample     = 44100;
        c.timelineStartSample = tStart;
        c.sourceLengthSamples = 44100;
        lane.getChildWithName (DawIDs::clips).addChild (DawClip::toValueTree (c), -1, nullptr);
    }

    static juce::ValueTree laneOf (juce::ValueTree daw, int deck, ChannelGroup::LaneKind k)
    {
        auto t = DawState::ensureTrackForDeck (daw, deck);
        return ChannelGroup::findLane (t.getChildWithName (DawIDs::lanes), k);
    }

    static juce::ValueTree makeSourceRef (const juce::String& id, const juce::String& kind,
                                          const juce::String& path, const juce::String& name,
                                          const juce::String& parent = {})
    {
        juce::ValueTree ref (IDs::SOURCE_REF);
        ref.setProperty (IDs::sourceFileId,  id,   nullptr);
        ref.setProperty (IDs::sourceKind,    kind, nullptr);
        ref.setProperty (IDs::lastKnownPath, path, nullptr);
        ref.setProperty (IDs::displayName,   name, nullptr);
        if (parent.isNotEmpty())
            ref.setProperty ("parentTrackId", parent, nullptr);
        return ref;
    }

    //==========================================================================
    void testDistinctDedupAndCounts()
    {
        beginTest ("distinct sources are resolved once; clip counts are tallied");

        auto daw = DawState::createDawBranch();
        auto orig = laneOf (daw, 0, ChannelGroup::LaneKind::Original);
        addClip (orig, "lib:track-1", 0);
        addClip (orig, "lib:track-1", 100000);   // same source -> one entry, count 2
        addClip (orig, "lib:track-1", 200000);   // count 3
        auto inst = laneOf (daw, 0, ChannelGroup::LaneKind::Instrumental);
        addClip (inst, "ext:other", 0);          // distinct source

        SourceIdResolver::Strategies strat;
        strat.libraryById = [] (const juce::String&) { return std::optional<juce::String>{}; };
        strat.pathExists  = [] (const juce::String&) { return false; };

        SourceIdResolver resolver (strat);
        auto sources = resolver.resolve (daw, {});

        expectEquals ((int) sources.size(), 2, "two distinct sources");
        expectEquals (sources[0].sourceFileId, juce::String ("lib:track-1"));
        expectEquals (sources[0].clipCount, 3, "shared source counted across all clips");
        expectEquals (sources[1].sourceFileId, juce::String ("ext:other"));
        expectEquals (sources[1].clipCount, 1);
    }

    //==========================================================================
    void testOrderedStrategy()
    {
        beginTest ("ordered strategy: library id -> stored path -> content hash");

        auto daw = DawState::createDawBranch();
        auto lane = laneOf (daw, 0, ChannelGroup::LaneKind::Original);
        addClip (lane, "lib:byId",   0);   // resolves via library
        addClip (lane, "ext:byPath", 1);   // resolves via stored path
        addClip (lane, "lib:byHash", 2);   // resolves via content hash
        addClip (lane, "lib:missing",3);   // resolves through nothing -> Missing

        juce::ValueTree refs (IDs::SOURCE_REFS);
        refs.addChild (makeSourceRef ("ext:byPath", SourceKindStrings::kExternal,
                                      "/music/found.wav", "found.wav"), -1, nullptr);

        // A set of paths that "exist".
        juce::StringArray onDisk { "/library/byId.flac", "/music/found.wav", "/library/byHash.flac" };

        SourceIdResolver::Strategies strat;
        strat.pathExists  = [onDisk] (const juce::String& p) { return onDisk.contains (p); };
        strat.libraryById = [] (const juce::String& id) -> std::optional<juce::String>
        {
            if (id == "lib:byId") return juce::String ("/library/byId.flac");
            return {};   // library does not know byHash directly
        };
        strat.hashMatch = [] (const juce::String& id) -> std::optional<juce::String>
        {
            if (id == "lib:byHash") return juce::String ("/library/byHash.flac");
            return {};
        };

        SourceIdResolver resolver (strat);
        auto sources = resolver.resolve (daw, refs);
        expectEquals ((int) sources.size(), 4);

        auto byId = [&] (const juce::String& id) -> ResolvedSource
        {
            for (auto& s : sources) if (s.sourceFileId == id) return s;
            return {};
        };

        auto a = byId ("lib:byId");
        expect (a.state == ResolutionState::Resolved, "byId resolved");
        expectEquals (a.resolvedPath, juce::String ("/library/byId.flac"), "via library");

        auto b = byId ("ext:byPath");
        expect (b.state == ResolutionState::Resolved, "byPath resolved");
        expectEquals (b.resolvedPath, juce::String ("/music/found.wav"), "via stored path");

        auto c = byId ("lib:byHash");
        expect (c.state == ResolutionState::Resolved, "byHash resolved");
        expectEquals (c.resolvedPath, juce::String ("/library/byHash.flac"), "via content hash");

        auto d = byId ("lib:missing");
        expect (d.state == ResolutionState::Missing, "missing source flagged");
        expect (d.resolvedPath.isEmpty());
    }

    //==========================================================================
    void testStemCacheReDerive()
    {
        beginTest ("StemCache resolves via parent id; otherwise missing");

        auto daw = DawState::createDawBranch();
        auto lane = laneOf (daw, 0, ChannelGroup::LaneKind::Vocal);
        addClip (lane, "stem:parent-7:vocal", 0);

        juce::ValueTree refs (IDs::SOURCE_REFS);
        refs.addChild (makeSourceRef ("stem:parent-7:vocal", SourceKindStrings::kStemCache,
                                      {}, "Vocal stem", "lib:parent-7"), -1, nullptr);

        SourceIdResolver::Strategies strat;
        strat.pathExists     = [] (const juce::String& p) { return p.isNotEmpty(); };
        strat.stemByParentId = [] (const juce::String& parent) -> std::optional<juce::String>
        {
            if (parent == "lib:parent-7") return juce::String ("/cache/parent-7/vocal.wav");
            return {};
        };

        SourceIdResolver resolver (strat);
        auto sources = resolver.resolve (daw, refs);
        expectEquals ((int) sources.size(), 1);
        expect (sources[0].kind == SourceKind::StemCache, "classified as stem cache");
        expect (sources[0].state == ResolutionState::Resolved, "resolved from parent");
        expectEquals (sources[0].resolvedPath, juce::String ("/cache/parent-7/vocal.wav"));
    }

    //==========================================================================
    void testRelocationAndGating()
    {
        beginTest ("relocation flips a source to Resolved; gating tracks missing count");

        auto daw = DawState::createDawBranch();
        auto lane = laneOf (daw, 0, ChannelGroup::LaneKind::Original);
        addClip (lane, "ext:gone", 0);
        addClip (lane, "ext:gone", 50000);

        juce::StringArray onDisk { "/new/location.wav" };
        SourceIdResolver::Strategies strat;
        strat.pathExists = [&onDisk] (const juce::String& p) { return onDisk.contains (p); };

        SourceIdResolver resolver (strat);
        auto sources = resolver.resolve (daw, {});

        expect (! SourceIdResolver::allResolved (sources), "starts unresolved");
        expectEquals (SourceIdResolver::missingCount (sources), 1);
        expectEquals ((int) SourceIdResolver::missingOnly (sources).size(), 1);
        expectEquals (SourceIdResolver::resolvedSourceIds (sources).size(), 0);

        // Relocate the single shared source -> applies to all its clips at once.
        expect (resolver.applyRelocation (sources[0], "/new/location.wav"), "relocation succeeds");
        expect (sources[0].state == ResolutionState::Resolved);
        expectEquals (sources[0].resolvedPath, juce::String ("/new/location.wav"));
        expectEquals (sources[0].lastKnownPath, juce::String ("/new/location.wav"), "new hint persisted");

        expect (SourceIdResolver::allResolved (sources), "all resolved after one relocate");
        expectEquals (SourceIdResolver::missingCount (sources), 0);
        expectEquals (SourceIdResolver::resolvedSourceIds (sources).size(), 1);

        // A relocation to a non-existent path is rejected.
        ResolvedSource bad; bad.sourceFileId = "x";
        expect (! resolver.applyRelocation (bad, "/nope.wav"));
    }
};

static SourceIdResolverTests sourceIdResolverTests;
