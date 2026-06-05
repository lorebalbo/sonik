#include "DawState.h"
#include "../Model/ChannelGroup.h"
#include "DawClipModel.h"

namespace DawState
{
    juce::ValueTree createDawBranch()
    {
        juce::ValueTree daw (DawIDs::Daw);
        daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);
        return daw;
    }

    juce::ValueTree getOrCreateDawBranch (juce::ValueTree rootState)
    {
        jassert (rootState.isValid());

        auto daw = rootState.getChildWithName (DawIDs::Daw);
        if (! daw.isValid())
        {
            daw = createDawBranch();
            rootState.addChild (daw, -1, nullptr);
            return daw;
        }

        // Existing branch (e.g. restored session): ensure the tracks container.
        if (! daw.getChildWithName (DawIDs::tracks).isValid())
            daw.addChild (juce::ValueTree (DawIDs::tracks), -1, nullptr);

        return daw;
    }

    juce::ValueTree findTrackForDeck (juce::ValueTree dawBranch, int deckIndex)
    {
        if (! dawBranch.isValid())
            return {};

        auto tracks = dawBranch.getChildWithName (DawIDs::tracks);
        if (! tracks.isValid())
            return {};

        for (int i = 0; i < tracks.getNumChildren(); ++i)
        {
            auto t = tracks.getChild (i);
            if (t.hasType (DawIDs::track)
                && static_cast<int> (t.getProperty (DawIDs::deckIndex)) == deckIndex)
                return t;
        }
        return {};
    }

    juce::ValueTree ensureTrackForDeck (juce::ValueTree dawBranch, int deckIndex)
    {
        jassert (dawBranch.isValid());

        // Idempotency: return the existing track untouched if present.
        if (auto existing = findTrackForDeck (dawBranch, deckIndex); existing.isValid())
            return existing;

        auto tracks = dawBranch.getChildWithName (DawIDs::tracks);
        if (! tracks.isValid())
        {
            tracks = juce::ValueTree (DawIDs::tracks);
            dawBranch.addChild (tracks, -1, nullptr);
        }

        juce::ValueTree track (DawIDs::track);
        track.setProperty (DawIDs::deckIndex, deckIndex, nullptr);

        // Lanes are pre-created eagerly (§1.5.5): exactly three, each with a
        // freshly minted laneId Uuid and a laneKind, each carrying an empty
        // clips container.
        track.addChild (ChannelGroup::createLanesContainer(), -1, nullptr);

        tracks.addChild (track, -1, nullptr);
        return track;
    }

    std::int64_t earliestClipStartSample (const juce::ValueTree& dawBranch)
    {
        if (! dawBranch.isValid())
            return 0;

        auto tracks = dawBranch.getChildWithName (DawIDs::tracks);
        if (! tracks.isValid())
            return 0;

        bool         found    = false;
        std::int64_t earliest = 0;

        for (int t = 0; t < tracks.getNumChildren(); ++t)
        {
            auto track = tracks.getChild (t);
            if (! track.hasType (DawIDs::track))
                continue;

            auto lanes = track.getChildWithName (DawIDs::lanes);
            if (! lanes.isValid())
                continue;

            for (int l = 0; l < lanes.getNumChildren(); ++l)
            {
                auto lane = lanes.getChild (l);
                if (! lane.hasType (DawIDs::lane))
                    continue;

                auto clips = lane.getChildWithName (DawIDs::clips);
                if (! clips.isValid())
                    continue;

                for (int c = 0; c < clips.getNumChildren(); ++c)
                {
                    auto clip = clips.getChild (c);
                    if (! clip.hasType (DawIDs::clip))
                        continue;

                    // A missing-source clip is silent and excluded from the
                    // engine snapshot (PRD-0097); it must not define the start.
                    if (static_cast<bool> (clip.getProperty (DawClipIDs::missingSource)))
                        continue;

                    const std::int64_t start = static_cast<std::int64_t> (
                        static_cast<double> (clip.getProperty (DawClipIDs::timelineStartSample)));

                    if (! found || start < earliest)
                    {
                        earliest = start;
                        found    = true;
                    }
                }
            }
        }

        return found ? juce::jmax<std::int64_t> (0, earliest) : 0;
    }
}
