#pragma once
//==============================================================================
// PRD-0045: SoftTakeoverManager
//
// Tracks per-(device, target) takeover state for Continuous controls so a
// hardware fader/knob that does not currently match the on-screen value
// cannot snap the parameter on first move. Three policies (from PRD-0044's
// MappingTypes.h SoftTakeoverPolicy):
//   - Pickup  : suppress writes until hardware crosses the current software
//               value, then engage and pass through subsequent moves.
//   - Always  : pass-through always (legacy hardware-snap behaviour).
//   - Never   : pass-through always (alias for Always: the binding owner has
//               decided this control should not soft-take-over).
//
// THREAD MODEL: Message thread only. JUCE_ASSERT_MESSAGE_THREAD on every
// public method. No heap allocations on the hot path.
//==============================================================================

#include "ControlTargetRegistry.h"
#include "MappingStore.h"
#include "MappingTypes.h"
#include "MidiTargetCategory.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace sonik::midi
{
    enum class TakeoverState : std::uint8_t
    {
        Disengaged, // Hardware has not yet crossed the software value.
        Engaged     // Hardware is in sync; subsequent moves pass through.
    };

    struct TakeoverEntry
    {
        TakeoverState state                 { TakeoverState::Disengaged };
        float         lastHardwareValue     { -1.0f }; // -1 == "never seen"
        float         lastSoftwareValueAtCheck { 0.0f };
    };

    //--------------------------------------------------------------------------
    /** PRD-0047: observers receive state transitions Disengaged↔Engaged so
        downstream subsystems (notably MidiFeedbackEngine) can drive a
        "disengaged" LED indicator. All callbacks fire on the Message thread. */
    class SoftTakeoverManagerListener
    {
    public:
        virtual ~SoftTakeoverManagerListener() = default;

        virtual void takeoverStateChanged (std::uint64_t /*deviceId*/,
                                           TargetIndex   /*target*/,
                                           TakeoverState /*newState*/) {}
    };

    //--------------------------------------------------------------------------
    /** RAII scope marker that DeckMidiHandler (and any other writer) wraps
        around a ValueTree property mutation it makes in response to MIDI.
        While the scope is alive on the current thread, the
        SoftTakeoverManager's ValueTree::Listener callback ignores the change
        (otherwise the manager would reset its own bindings every time a MIDI
        knob succeeded in writing). */
    class MidiOriginatedWriteScope
    {
    public:
        MidiOriginatedWriteScope() noexcept;
        ~MidiOriginatedWriteScope() noexcept;

        MidiOriginatedWriteScope (const MidiOriginatedWriteScope&)            = delete;
        MidiOriginatedWriteScope& operator= (const MidiOriginatedWriteScope&) = delete;

        static bool isActive() noexcept;
    };

    //--------------------------------------------------------------------------
    class SoftTakeoverManager final
        : public MappingStoreListener,
          public juce::ValueTree::Listener
    {
    public:
        explicit SoftTakeoverManager (juce::ValueTree rootStateTree,
                                      MappingStore&   mappingStore);
        ~SoftTakeoverManager() override;

        SoftTakeoverManager (const SoftTakeoverManager&)            = delete;
        SoftTakeoverManager& operator= (const SoftTakeoverManager&) = delete;

        /** Decide whether a MIDI-sourced write should reach the parameter.
            On Pickup, the first sample after a reset only records the
            hardware value and returns false; subsequent samples return true
            once the hardware crosses (sign-flips against) the current
            software value. */
        bool shouldPassThrough (std::uint64_t      deviceId,
                                TargetIndex        target,
                                float              hardwareValue,
                                float              softwareValue,
                                SoftTakeoverPolicy policy);

        void resetForDevice  (std::uint64_t deviceId);
        void resetForBinding (std::uint64_t deviceId, TargetIndex target);
        void resetForTarget  (TargetIndex target); // every device, given target.

        /** Force the (device, target) pair into Engaged state immediately;
            useful for tests and for an eventual "match-now" UI affordance. */
        void forceEngage (std::uint64_t deviceId,
                          TargetIndex   target,
                          float         hardwareValue,
                          float         softwareValue);

        TakeoverState getState (std::uint64_t deviceId, TargetIndex target) const;

        //-- Listeners (Message thread only) -------------------------------
        void addListener    (SoftTakeoverManagerListener* l);
        void removeListener (SoftTakeoverManagerListener* l);

        //-- MappingStoreListener ------------------------------------------
        void activeMappingChanged (std::uint64_t deviceId) override;

        //-- juce::ValueTree::Listener -------------------------------------
        void valueTreePropertyChanged (juce::ValueTree& treeWhosePropertyHasChanged,
                                       const juce::Identifier& property) override;

    private:
        struct Key
        {
            std::uint64_t deviceId;
            TargetIndex   target;
            bool operator== (const Key& o) const noexcept
            {
                return deviceId == o.deviceId && target == o.target;
            }
        };
        struct KeyHash
        {
            std::size_t operator() (const Key& k) const noexcept
            {
                return std::hash<std::uint64_t>{} (k.deviceId)
                       ^ (static_cast<std::size_t> (k.target) << 1);
            }
        };

        /** Maps a Continuous deck property identifier to its target category
            (per-deck). Returns nullopt for non-continuous properties. */
        static std::optional<MidiTargetCategory> categoryForDeckProperty (const juce::Identifier& p);

        /** Find the deck child index for a ValueTree node sitting under the
            "Decks" container of the root state tree. Returns nullopt if the
            tree does not belong to a deck. */
        std::optional<std::uint8_t> deckIndexFor (const juce::ValueTree& deckTree) const;

        void notifyStateChanged (std::uint64_t deviceId, TargetIndex target, TakeoverState s);

        juce::ValueTree rootTree;
        MappingStore&   mappingStoreRef;
        std::unordered_map<Key, TakeoverEntry, KeyHash> entries;
        std::vector<SoftTakeoverManagerListener*> listeners;
    };
} // namespace sonik::midi
