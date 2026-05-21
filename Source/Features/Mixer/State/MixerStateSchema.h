#pragma once
//==============================================================================
// PRD-0052: MixerStateSchema — creates and owns the mixer ValueTree sub-tree.
//
// Responsibilities:
//   1. On construction, adds a "Mixer" child to the supplied root ValueTree
//      (SonikState) with all four channel sub-trees (A–D) and the master
//      sub-tree, populated with documented defaults.
//   2. Provides typed accessors for the mixer tree and its channel/master
//      sub-trees; all downstream components (MixerStateBridge, UI, tests)
//      obtain their trees through this class.
//   3. Implements resetChannel(idx) which resets a channel's writable
//      properties to defaults without destroying the sub-tree — called
//      whenever a deck is removed at runtime (PRD-0052 §1.5.5).
//
// File location: Source/Features/Mixer/State/
// No #includes from Source/Features/Deck, AudioEngine, or other features.
//==============================================================================

#include "MixerIdentifiers.h"
#include "MixerAtomicSnapshot.h"
#include "MixerMeterSnapshot.h"
#include <juce_data_structures/juce_data_structures.h>

class MixerStateSchema final
{
public:
    //--------------------------------------------------------------------------
    // Constructor: inserts the "Mixer" sub-tree into rootState.
    // If a "Mixer" child already exists (e.g. restored from a saved session),
    // it is used as-is; missing properties are initialised to their defaults.
    //--------------------------------------------------------------------------
    explicit MixerStateSchema (juce::ValueTree rootState);

    // Non-copyable / non-movable.
    MixerStateSchema (const MixerStateSchema&) = delete;
    MixerStateSchema& operator= (const MixerStateSchema&) = delete;

    //--------------------------------------------------------------------------
    // Accessors (all return valid ValueTree references; assert-fail on misuse).
    //--------------------------------------------------------------------------
    juce::ValueTree getMixerTree() const noexcept    { return mixerTree; }

    /** Returns the channel sub-tree for the given index (0=A, 1=B, 2=C, 3=D). */
    juce::ValueTree getChannelTree (int channelIndex) const noexcept;

    /** Returns the channel's nested "eq" sub-tree (carries high/mid/low and
        the three kill booleans). */
    juce::ValueTree getChannelEqTree (int channelIndex) const noexcept;

    juce::ValueTree getMasterTree() const noexcept   { return masterTree; }

    //--------------------------------------------------------------------------
    // Deck lifecycle — called by SonikApplication when a deck is removed.
    // Resets all writable properties in the channel sub-tree to defaults
    // without destroying the sub-tree (PRD-0052 §1.5.5).
    // Also resets the corresponding meter snapshot slots.
    //--------------------------------------------------------------------------
    void resetChannel (int channelIndex, MixerMeterSnapshot* meters = nullptr);

    //--------------------------------------------------------------------------
    // Default property values — exposed as constants for test access.
    //--------------------------------------------------------------------------
    static constexpr float kDefaultGainDb      =   0.0f;  // dB
    static constexpr float kDefaultEqDb        =   0.0f;  // dB (flat)
    static constexpr bool  kDefaultKill        = false;
    static constexpr float kDefaultFilter      =   0.0f;  // bipolar, bypass
    static constexpr float kDefaultFader       =   1.0f;  // full open
    static constexpr float kDefaultCrossfader  =   0.5f;  // centred

    //--------------------------------------------------------------------------
    // PRD-0057 §1.5.5: default crossfader curve (string enum on the Mixer
    // ValueTree). Valid values: "smooth", "sharp".
    //--------------------------------------------------------------------------
    static constexpr const char* kDefaultCrossfaderCurve = "smooth";

    //--------------------------------------------------------------------------
    // A/B crossfader assign defaults (PRD-0052 §1.5.1):
    //   A → assignA=true,  assignB=false
    //   B → assignA=false, assignB=true
    //   C → assignA=true,  assignB=false
    //   D → assignA=false, assignB=true
    //--------------------------------------------------------------------------
    static bool defaultAssignA (int channelIndex) noexcept
    {
        return (channelIndex == 0 || channelIndex == 2);
    }
    static bool defaultAssignB (int channelIndex) noexcept
    {
        return (channelIndex == 1 || channelIndex == 3);
    }

private:
    void initialiseChannelTree (juce::ValueTree& channelTree, int channelIndex);
    void initialiseMasterTree  (juce::ValueTree& tree);
    void setDefaultIfMissing   (juce::ValueTree& tree,
                                const juce::Identifier& id,
                                const juce::var& defaultValue);

    juce::ValueTree mixerTree;
    juce::ValueTree channelContainerTree;   // type "channel"
    juce::ValueTree channelTrees[4];        // A, B, C, D
    juce::ValueTree masterTree;
};
