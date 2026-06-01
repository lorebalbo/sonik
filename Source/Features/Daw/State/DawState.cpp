#include "DawState.h"
#include "../Model/ChannelGroup.h"

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
}
