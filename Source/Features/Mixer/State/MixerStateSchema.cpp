#include "MixerStateSchema.h"

MixerStateSchema::MixerStateSchema (juce::ValueTree rootState)
{
    jassert (rootState.isValid());

    // Obtain or create the top-level "Mixer" sub-tree.
    mixerTree = rootState.getChildWithName (MixerIDs::Mixer);
    if (! mixerTree.isValid())
    {
        mixerTree = juce::ValueTree (MixerIDs::Mixer);
        rootState.addChild (mixerTree, -1, nullptr);
    }

    // crossfader: direct property on Mixer tree.
    setDefaultIfMissing (mixerTree, MixerIDs::crossfader, kDefaultCrossfader);

    // PRD-0057: crossfader curve string enum, direct property on Mixer tree.
    setDefaultIfMissing (mixerTree, MixerIDs::crossfaderCurve,
                          juce::String (kDefaultCrossfaderCurve));

    // Channel container sub-tree.
    channelContainerTree = mixerTree.getChildWithName (MixerIDs::channel);
    if (! channelContainerTree.isValid())
    {
        channelContainerTree = juce::ValueTree (MixerIDs::channel);
        mixerTree.addChild (channelContainerTree, -1, nullptr);
    }

    // Per-channel sub-trees (A, B, C, D).
    const juce::Identifier letters[] = {
        MixerIDs::A, MixerIDs::B, MixerIDs::C, MixerIDs::D
    };
    for (int i = 0; i < 4; ++i)
    {
        channelTrees[i] = channelContainerTree.getChildWithName (letters[i]);
        if (! channelTrees[i].isValid())
        {
            channelTrees[i] = juce::ValueTree (letters[i]);
            channelContainerTree.addChild (channelTrees[i], -1, nullptr);
        }
        initialiseChannelTree (channelTrees[i], i);
    }

    // Master sub-tree.
    masterTree = mixerTree.getChildWithName (MixerIDs::master);
    if (! masterTree.isValid())
    {
        masterTree = juce::ValueTree (MixerIDs::master);
        mixerTree.addChild (masterTree, -1, nullptr);
    }
    initialiseMasterTree (masterTree);
}

//==============================================================================

juce::ValueTree MixerStateSchema::getChannelTree (int channelIndex) const noexcept
{
    jassert (channelIndex >= 0 && channelIndex < 4);
    return channelTrees[channelIndex];
}

juce::ValueTree MixerStateSchema::getChannelEqTree (int channelIndex) const noexcept
{
    jassert (channelIndex >= 0 && channelIndex < 4);
    return channelTrees[channelIndex].getChildWithName (MixerIDs::eq);
}

//==============================================================================

void MixerStateSchema::resetChannel (int channelIndex, MixerMeterSnapshot* meters)
{
    jassert (channelIndex >= 0 && channelIndex < 4);

    auto& ch = channelTrees[channelIndex];

    ch.setProperty (MixerIDs::gain,    kDefaultGainDb,                 nullptr);
    ch.setProperty (MixerIDs::filter,  kDefaultFilter,                 nullptr);
    ch.setProperty (MixerIDs::fader,   kDefaultFader,                  nullptr);
    ch.setProperty (MixerIDs::assignA, defaultAssignA (channelIndex),  nullptr);
    ch.setProperty (MixerIDs::assignB, defaultAssignB (channelIndex),  nullptr);
    ch.setProperty (MixerIDs::cue,     false,                          nullptr);

    auto eq = ch.getChildWithName (MixerIDs::eq);
    jassert (eq.isValid());
    eq.setProperty (MixerIDs::high,     kDefaultEqDb, nullptr);
    eq.setProperty (MixerIDs::mid,      kDefaultEqDb, nullptr);
    eq.setProperty (MixerIDs::low,      kDefaultEqDb, nullptr);
    eq.setProperty (MixerIDs::killHigh, kDefaultKill, nullptr);
    eq.setProperty (MixerIDs::killMid,  kDefaultKill, nullptr);
    eq.setProperty (MixerIDs::killLow,  kDefaultKill, nullptr);

    if (meters != nullptr)
        meters->resetChannel (channelIndex);
}

//==============================================================================
// Private helpers
//==============================================================================

void MixerStateSchema::initialiseChannelTree (juce::ValueTree& ch, int channelIndex)
{
    setDefaultIfMissing (ch, MixerIDs::gain,    kDefaultGainDb);
    setDefaultIfMissing (ch, MixerIDs::filter,  kDefaultFilter);
    setDefaultIfMissing (ch, MixerIDs::fader,   kDefaultFader);
    setDefaultIfMissing (ch, MixerIDs::assignA, defaultAssignA (channelIndex));
    setDefaultIfMissing (ch, MixerIDs::assignB, defaultAssignB (channelIndex));
    setDefaultIfMissing (ch, MixerIDs::cue,     false);

    // EQ sub-tree (nested under channel).
    auto eq = ch.getChildWithName (MixerIDs::eq);
    if (! eq.isValid())
    {
        eq = juce::ValueTree (MixerIDs::eq);
        ch.addChild (eq, -1, nullptr);
    }
    setDefaultIfMissing (eq, MixerIDs::high,     kDefaultEqDb);
    setDefaultIfMissing (eq, MixerIDs::mid,      kDefaultEqDb);
    setDefaultIfMissing (eq, MixerIDs::low,      kDefaultEqDb);
    setDefaultIfMissing (eq, MixerIDs::killHigh, kDefaultKill);
    setDefaultIfMissing (eq, MixerIDs::killMid,  kDefaultKill);
    setDefaultIfMissing (eq, MixerIDs::killLow,  kDefaultKill);
}

void MixerStateSchema::initialiseMasterTree (juce::ValueTree& tree)
{
    setDefaultIfMissing (tree, MixerIDs::gain, kDefaultGainDb);
}

void MixerStateSchema::setDefaultIfMissing (juce::ValueTree& tree,
                                             const juce::Identifier& id,
                                             const juce::var& defaultValue)
{
    if (! tree.hasProperty (id))
        tree.setProperty (id, defaultValue, nullptr);
}
