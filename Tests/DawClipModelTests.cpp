//==============================================================================
// PRD-0063: DawClipModelTests — DawClip value object round-trip fidelity,
// length identity, uncrop/extend purity, and id stability across serialize /
// deserialize and a simulated source-path relocation. Category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/State/DawClipModel.h"
#include "Features/Daw/State/DawState.h"

#include <cstdint>

class DawClipModelTests final : public juce::UnitTest
{
public:
    DawClipModelTests() : juce::UnitTest ("DAW Clip Model", "Sonik") {}

    void runTest() override
    {
        testDefaults();
        testTimelineLength();
        testRoundTripBitExact();
        testNoPathStored();
        testUncropExtendIsPureMutation();
        testIdStableAcrossSerializeAndRelocation();
    }

private:
    static DawClip makeSampleClip()
    {
        DawClip c;
        c.laneId              = juce::Uuid();
        c.sourceFileId        = "stemcache:7f3a-vocal";
        c.sourceStartSample   = 123456789012LL;       // > 2^31, proves int64 fidelity
        c.sourceEndSample     = 123456999999LL;
        c.timelineStartSample = 9876543210LL;
        c.sourceLengthSamples = 500000000000LL;
        c.gainDb              = -3.5f;
        return c;
    }

    void testDefaults()
    {
        beginTest ("gainDb defaults to 0.0 (unity); sample indices default 0");
        DawClip c;
        expectEquals (c.gainDb, 0.0f);
        expectEquals ((long long) c.sourceStartSample,   0LL);
        expectEquals ((long long) c.sourceEndSample,     0LL);
        expectEquals ((long long) c.timelineStartSample, 0LL);
        expectEquals ((long long) c.sourceLengthSamples, 0LL);
        expect (c.clipId != juce::Uuid::null(), "clipId minted by default");
    }

    void testTimelineLength()
    {
        beginTest ("timelineLengthSamples == sourceEndSample - sourceStartSample");
        auto c = makeSampleClip();
        expectEquals ((long long) c.timelineLengthSamples(),
                      (long long) (c.sourceEndSample - c.sourceStartSample));

        DawClip zero;
        zero.sourceStartSample = 1000;
        zero.sourceEndSample   = 1000;
        expectEquals ((long long) zero.timelineLengthSamples(), 0LL);
    }

    void testRoundTripBitExact()
    {
        beginTest ("toValueTree -> fromValueTree round-trips all 8 fields bit-exact");
        auto in = makeSampleClip();
        auto tree = DawClip::toValueTree (in);
        expect (tree.hasType (DawIDs::clip));

        auto out = DawClip::fromValueTree (tree);

        expect (out.clipId == in.clipId, "clipId preserved");
        expect (out.laneId == in.laneId, "laneId preserved");
        expectEquals (out.sourceFileId, in.sourceFileId);
        expectEquals ((long long) out.sourceStartSample,   (long long) in.sourceStartSample);
        expectEquals ((long long) out.sourceEndSample,     (long long) in.sourceEndSample);
        expectEquals ((long long) out.timelineStartSample, (long long) in.timelineStartSample);
        expectEquals ((long long) out.sourceLengthSamples, (long long) in.sourceLengthSamples);
        expectEquals (out.gainDb, in.gainDb);
    }

    void testNoPathStored()
    {
        beginTest ("Clip node stores only ids and indices, never a filesystem path");
        auto c = makeSampleClip();
        c.sourceFileId = "library:track-99";   // opaque stable id, not a path
        auto tree = DawClip::toValueTree (c);

        // sourceFileId is stored verbatim and uninterpreted.
        expectEquals (tree.getProperty (DawClipIDs::sourceFileId).toString(),
                      juce::String ("library:track-99"));

        // No property named anything path-like exists.
        expect (! tree.hasProperty (juce::Identifier ("filePath")));
        expect (! tree.hasProperty (juce::Identifier ("path")));
    }

    void testUncropExtendIsPureMutation()
    {
        beginTest ("Mutating source start/end within [0, sourceLengthSamples] keeps schema valid");
        auto c = makeSampleClip();
        auto tree = DawClip::toValueTree (c);

        // Simulate EPIC-0010 uncrop: extend start to 0 and end to full length.
        tree.setProperty (DawClipIDs::sourceStartSample, (std::int64_t) 0, nullptr);
        tree.setProperty (DawClipIDs::sourceEndSample, c.sourceLengthSamples, nullptr);

        auto mutated = DawClip::fromValueTree (tree);
        expectEquals ((long long) mutated.sourceStartSample, 0LL);
        expectEquals ((long long) mutated.sourceEndSample, (long long) c.sourceLengthSamples);
        expectEquals ((long long) mutated.timelineLengthSamples(),
                      (long long) c.sourceLengthSamples);
        // Identity (id) and source reference unchanged by the edit.
        expect (mutated.clipId == c.clipId);
        expectEquals (mutated.sourceFileId, c.sourceFileId);
    }

    void testIdStableAcrossSerializeAndRelocation()
    {
        beginTest ("Clip resolvable by id after serialize/deserialize + path relocation");

        // Build a lane with a clips container holding our clip.
        juce::ValueTree clipsContainer (DawIDs::clips);
        auto c = makeSampleClip();
        const auto originalId = c.clipId;
        clipsContainer.addChild (DawClip::toValueTree (c), -1, nullptr);

        // Round-trip the whole container through XML (EPIC-0012 serialization sim).
        auto xml = clipsContainer.toXmlString();
        auto restored = juce::ValueTree::fromXml (xml);
        expect (restored.isValid());

        // PRD-0039 relocation: the underlying file moved, but sourceFileId is a
        // STABLE id, not a path — so it does not change. The clip stays resolvable.
        auto found = DawClipModel::findClipNodeById (restored, originalId);
        expect (found.isValid(), "clip resolvable by id after serialize cycle");

        auto roundTripped = DawClip::fromValueTree (found);
        expect (roundTripped.clipId == originalId, "id stable across serialization");
        expectEquals (roundTripped.sourceFileId, c.sourceFileId,
                      "stable id unaffected by file relocation");
    }
};

static DawClipModelTests dawClipModelTests;
