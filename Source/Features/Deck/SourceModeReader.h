#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <vector>
#include "DeckIdentifiers.h"

// =============================================================================
// SourceModeReader (PRD-0062)
//
// A thin, read-only mirror of a deck's active source mode, published for
// consumers (notably EPIC-0008's in-app DAW `LiveProjectionTimer`) that need to
// know which lane(s) the deck is currently producing audio on, without attaching
// their own ValueTree listener.
//
// The `sourceMode` ValueTree property remains the single source of truth
// (PRD-0062 §1.5.7); this reader derives the published lane set from that
// property plus the two stem-mute booleans. It is safe to construct and query
// on the message thread (the only thread that mutates the underlying tree).
// =============================================================================
class SourceModeReader
{
public:
    enum class Lane
    {
        Original,      // pristine original source file
        Instrumental,  // summed instrumental stem (drums + bass + other)
        Vocal          // vocal stem
    };

    explicit SourceModeReader (juce::ValueTree deckTree)
        : tree (std::move (deckTree))
    {
    }

    /// The canonical source mode string: "original" or "stems".
    juce::String getSourceMode() const
    {
        return tree.getProperty (IDs::sourceMode, "original").toString();
    }

    bool isOriginal() const { return getSourceMode() != "stems"; }
    bool isStems() const     { return getSourceMode() == "stems"; }

    /// The set of lanes the deck is currently audible on, per the PRD-0062
    /// mapping contract (§1.3):
    ///   original                         -> { Original }
    ///   stems, both audible              -> { Instrumental, Vocal }
    ///   stems, VOC muted                 -> { Instrumental }
    ///   stems, INST muted                -> { Vocal }
    ///   stems, both muted                -> { }
    std::vector<Lane> getPublishedLanes() const
    {
        if (isOriginal())
            return { Lane::Original };

        auto stems = tree.getChildWithName (IDs::Stems);

        const bool vocalsMuted = static_cast<bool> (stems.getProperty (IDs::vocalsMuted, false));

        // "INST" is the summed instrumental stem, governed by the INST toggle as
        // a unit. It is audible unless all three of its members are muted.
        const bool drumsMuted = static_cast<bool> (stems.getProperty (IDs::drumsMuted, false));
        const bool bassMuted  = static_cast<bool> (stems.getProperty (IDs::bassMuted,  false));
        const bool otherMuted = static_cast<bool> (stems.getProperty (IDs::otherMuted, false));
        const bool instMuted  = drumsMuted && bassMuted && otherMuted;

        std::vector<Lane> lanes;
        if (! instMuted)
            lanes.push_back (Lane::Instrumental);
        if (! vocalsMuted)
            lanes.push_back (Lane::Vocal);
        return lanes;
    }

private:
    juce::ValueTree tree;
};
