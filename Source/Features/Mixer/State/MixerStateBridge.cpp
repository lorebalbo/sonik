#include "MixerStateBridge.h"

MixerStateBridge::MixerStateBridge (MixerStateSchema& schemaIn,
                                     MixerAtomicSnapshot& atomicsIn)
    : schema  (schemaIn),
      atomics (atomicsIn),
      mixerTree (schemaIn.getMixerTree())
{
    mixerTree.addListener (this);
    syncAll();
}

MixerStateBridge::~MixerStateBridge()
{
    mixerTree.removeListener (this);
}

//==============================================================================
// Full sync — populates atomics from current ValueTree state.
//==============================================================================

void MixerStateBridge::syncAll()
{
    for (int i = 0; i < 4; ++i)
        syncAllChannel (i);

    // Crossfader
    const float xf = static_cast<float> (
        schema.getMixerTree().getProperty (MixerIDs::crossfader, 0.5f));
    atomics.crossfader.store (xf, std::memory_order_relaxed);

    // PRD-0057: crossfader curve string → int enum. Default "smooth" (0).
    {
        const juce::String s = schema.getMixerTree()
                                     .getProperty (MixerIDs::crossfaderCurve,
                                                    juce::String ("smooth"))
                                     .toString();
        const int enc = encodeCrossfaderCurve (s);
        // syncAll always seeds the atomic from the current tree value; the
        // listener's validate-or-reject logic does not apply here because
        // syncAll is the only path that may overwrite the atomic with a
        // freshly-parsed default after session restore.
        atomics.crossfaderCurve.store (enc >= 0 ? enc : 0, std::memory_order_relaxed);
    }

    // Master gain (dB → linear, clamped to [kMinDb, kMaxGainDb] per PRD-0054 AC)
    const float masterDb = static_cast<float> (
        schema.getMasterTree().getProperty (MixerIDs::gain, 0.0f));
    atomics.masterGain.store (
        MixerParam::dbToLinear (MixerParam::clampGainDb (masterDb)),
        std::memory_order_relaxed);
}

//==============================================================================
// juce::ValueTree::Listener
//
// JUCE delivers property-change callbacks to listeners attached at any
// ancestor of the tree whose property changed.  We registered on the root
// "Mixer" tree, so this is invoked for every descendant — channel sub-tree,
// eq sub-tree, and master sub-tree — and we dispatch on tree type.
//==============================================================================

void MixerStateBridge::valueTreePropertyChanged (juce::ValueTree& tree,
                                                   const juce::Identifier& property)
{
    const auto treeType = tree.getType();

    // EQ sub-tree (parent type is a channel letter).
    if (treeType == MixerIDs::eq)
    {
        const auto parent = tree.getParent();
        const int  idx    = MixerIDs::letterToChannelIndex (parent.getType());
        if (idx >= 0)
            syncChannelEqProperty (idx, tree, property);
        return;
    }

    // Channel sub-tree directly (type A/B/C/D).
    const int channelIdx = MixerIDs::letterToChannelIndex (treeType);
    if (channelIdx >= 0)
    {
        syncChannelProperty (channelIdx, tree, property);
        return;
    }

    if (treeType == MixerIDs::master)
    {
        syncMasterProperty (tree, property);
        return;
    }

    if (treeType == MixerIDs::Mixer)
    {
        syncMixerRootProperty (tree, property);
        return;
    }
}

//==============================================================================
// Per-channel helpers
//==============================================================================

void MixerStateBridge::syncChannelProperty (int idx,
                                              const juce::ValueTree& ch,
                                              const juce::Identifier& prop)
{
    auto& params = atomics.getChannel (idx);

    if (prop == MixerIDs::gain)
    {
        // PRD-0054 AC: clamp dB to [kMinDb, kMaxGainDb] before dB→linear.
        const float db = static_cast<float> (ch.getProperty (prop, 0.0f));
        params.gain.store (
            MixerParam::dbToLinear (MixerParam::clampGainDb (db)),
            std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::filter)
    {
        // PRD-0056 §1.5.6: snap-to-zero inside the bipolar detent so the
        // audio-thread DSP can rely on the invariant
        //   filter == 0.0f  OR  |filter| >= kFilterDetentEpsilon.
        const float bipolar = static_cast<float> (ch.getProperty (prop, 0.0f));
        params.filter.store (MixerParam::snapFilterDetent (bipolar),
                              std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::fader)
    {
        // PRD-0054 AC: clamp fader to [0, 1] on the message thread so the
        // audio thread never sees out-of-range values.
        const float val = static_cast<float> (ch.getProperty (prop, 1.0f));
        params.fader.store (MixerParam::clampFader (val),
                            std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::assignA)
    {
        params.assignA.store (static_cast<bool> (ch.getProperty (prop, false)),
                              std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::assignB)
    {
        params.assignB.store (static_cast<bool> (ch.getProperty (prop, false)),
                              std::memory_order_relaxed);
    }
}

void MixerStateBridge::syncChannelEqProperty (int idx,
                                                const juce::ValueTree& eq,
                                                const juce::Identifier& prop)
{
    auto& params = atomics.getChannel (idx);

    if (prop == MixerIDs::high)
    {
        const float db = static_cast<float> (eq.getProperty (prop, 0.0f));
        params.eqHigh.store (MixerParam::dbToLinear (db), std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::mid)
    {
        const float db = static_cast<float> (eq.getProperty (prop, 0.0f));
        params.eqMid.store (MixerParam::dbToLinear (db), std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::low)
    {
        const float db = static_cast<float> (eq.getProperty (prop, 0.0f));
        params.eqLow.store (MixerParam::dbToLinear (db), std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::killHigh)
    {
        params.killHigh.store (static_cast<bool> (eq.getProperty (prop, false)),
                                std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::killMid)
    {
        params.killMid.store (static_cast<bool> (eq.getProperty (prop, false)),
                               std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::killLow)
    {
        params.killLow.store (static_cast<bool> (eq.getProperty (prop, false)),
                               std::memory_order_relaxed);
    }
}

void MixerStateBridge::syncAllChannel (int idx)
{
    auto ch = schema.getChannelTree (idx);
    auto eq = schema.getChannelEqTree (idx);

    auto& params = atomics.getChannel (idx);

    // PRD-0054 AC: clamp gain dB and fader to their valid ranges on the
    // message thread before they reach the audio-thread atomics.
    params.gain.store (MixerParam::dbToLinear (MixerParam::clampGainDb (
        static_cast<float> (ch.getProperty (MixerIDs::gain, 0.0f)))),
        std::memory_order_relaxed);

    params.filter.store (
        MixerParam::snapFilterDetent (
            static_cast<float> (ch.getProperty (MixerIDs::filter, 0.0f))),
        std::memory_order_relaxed);

    params.fader.store (MixerParam::clampFader (
        static_cast<float> (ch.getProperty (MixerIDs::fader, 1.0f))),
        std::memory_order_relaxed);

    params.assignA.store (
        static_cast<bool> (ch.getProperty (MixerIDs::assignA, false)),
        std::memory_order_relaxed);

    params.assignB.store (
        static_cast<bool> (ch.getProperty (MixerIDs::assignB, false)),
        std::memory_order_relaxed);

    params.eqHigh.store (MixerParam::dbToLinear (
        static_cast<float> (eq.getProperty (MixerIDs::high, 0.0f))),
        std::memory_order_relaxed);

    params.eqMid.store (MixerParam::dbToLinear (
        static_cast<float> (eq.getProperty (MixerIDs::mid, 0.0f))),
        std::memory_order_relaxed);

    params.eqLow.store (MixerParam::dbToLinear (
        static_cast<float> (eq.getProperty (MixerIDs::low, 0.0f))),
        std::memory_order_relaxed);

    params.killHigh.store (
        static_cast<bool> (eq.getProperty (MixerIDs::killHigh, false)),
        std::memory_order_relaxed);

    params.killMid.store (
        static_cast<bool> (eq.getProperty (MixerIDs::killMid, false)),
        std::memory_order_relaxed);

    params.killLow.store (
        static_cast<bool> (eq.getProperty (MixerIDs::killLow, false)),
        std::memory_order_relaxed);
}

void MixerStateBridge::syncMasterProperty (const juce::ValueTree& masterTree,
                                             const juce::Identifier& prop)
{
    if (prop == MixerIDs::gain)
    {
        // PRD-0054 AC: clamp master dB to [kMinDb, kMaxGainDb] before dB→linear.
        const float db = static_cast<float> (masterTree.getProperty (prop, 0.0f));
        atomics.masterGain.store (
            MixerParam::dbToLinear (MixerParam::clampGainDb (db)),
            std::memory_order_relaxed);
    }
}

void MixerStateBridge::syncMixerRootProperty (const juce::ValueTree& mixerRootTree,
                                               const juce::Identifier& prop)
{
    if (prop == MixerIDs::crossfader)
    {
        const float val = static_cast<float> (mixerRootTree.getProperty (prop, 0.5f));
        atomics.crossfader.store (val, std::memory_order_relaxed);
    }
    else if (prop == MixerIDs::crossfaderCurve)
    {
        // PRD-0057 §1.4 AC: validate the string against the closed enum
        // {"smooth", "sharp"}. Any other value is rejected and the previous
        // valid enum integer in the atomic is retained.
        const juce::String s = mixerRootTree.getProperty (prop).toString();
        const int enc = encodeCrossfaderCurve (s);
        if (enc >= 0)
            atomics.crossfaderCurve.store (enc, std::memory_order_relaxed);
    }
}

int MixerStateBridge::encodeCrossfaderCurve (const juce::String& s) noexcept
{
    if (s == "smooth") return 0;   // CrossfaderCurve::Smooth
    if (s == "sharp")  return 1;   // CrossfaderCurve::Sharp
    return -1;
}
