#pragma once
//==============================================================================
// PRD-0052: MixerStateBridge — mirrors ValueTree mixer parameters into
// MixerAtomicSnapshot on the message thread.
//
// Architecture:
//   1. Registers as a juce::ValueTree::Listener on the top-level "Mixer"
//      sub-tree.  JUCE listeners are recursive: changes to any descendant
//      property also fire valueTreePropertyChanged on this listener.
//   2. On each property change, converts dB values to linear amplitude and
//      stores the result in the matching std::atomic<float/bool> slot inside
//      MixerAtomicSnapshot.
//   3. Runs exclusively on the message thread.  The audio thread never
//      touches MixerAtomicSnapshot except via relaxed atomic loads.
//
// Invariant: this class MUST NOT allocate memory, acquire locks, or perform
// I/O inside valueTreePropertyChanged (it runs on the message thread, not
// the audio thread, so allocation is technically allowed, but we avoid it
// anyway to keep the listener hot path clean).
//==============================================================================

#include "MixerStateSchema.h"
#include "MixerAtomicSnapshot.h"
#include "MixerParam.h"
#include <juce_data_structures/juce_data_structures.h>

class MixerStateBridge final : private juce::ValueTree::Listener
{
public:
    //--------------------------------------------------------------------------
    // Constructor registers the listener on the mixer tree.
    // Both `schema` and `atomics` must outlive this object.
    //--------------------------------------------------------------------------
    MixerStateBridge (MixerStateSchema& schema, MixerAtomicSnapshot& atomics);

    ~MixerStateBridge() override;

    // Non-copyable / non-movable.
    MixerStateBridge (const MixerStateBridge&) = delete;
    MixerStateBridge& operator= (const MixerStateBridge&) = delete;

    //--------------------------------------------------------------------------
    // Synchronises the entire ValueTree state into the atomic snapshot.
    // Call once after construction to populate the atomics with whatever
    // values are already present in the schema (e.g. after session restore).
    //--------------------------------------------------------------------------
    void syncAll();

private:
    //--------------------------------------------------------------------------
    // juce::ValueTree::Listener overrides.
    //--------------------------------------------------------------------------
    void valueTreePropertyChanged (juce::ValueTree& treeWhosePropertyHasChanged,
                                   const juce::Identifier& property) override;

    // Unused listener overrides (must be declared to suppress warnings).
    void valueTreeChildAdded    (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved  (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    //--------------------------------------------------------------------------
    // Per-channel sync helpers.
    //--------------------------------------------------------------------------
    void syncChannelProperty (int channelIndex,
                               const juce::ValueTree& channelTree,
                               const juce::Identifier& property);
    void syncChannelEqProperty (int channelIndex,
                                 const juce::ValueTree& eqTree,
                                 const juce::Identifier& property);
    void syncAllChannel (int channelIndex);
    void syncMasterProperty (const juce::ValueTree& masterTree,
                              const juce::Identifier& property);
    void syncMixerRootProperty (const juce::ValueTree& mixerTree,
                                 const juce::Identifier& property);

    /** PRD-0057: closed-enum validator for `mixer.crossfader.curve`.
        Returns the integer mirror written to the atomic snapshot, or -1
        when the string is not in the accepted set ("smooth", "sharp"). */
    static int encodeCrossfaderCurve (const juce::String& curveString) noexcept;

    MixerStateSchema&   schema;
    MixerAtomicSnapshot& atomics;
    juce::ValueTree      mixerTree;   // held for removeListener in destructor
};
