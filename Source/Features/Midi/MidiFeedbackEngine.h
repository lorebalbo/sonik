#pragma once
//==============================================================================
// PRD-0047: MidiFeedbackEngine
//
// Message-thread orchestrator that translates juce::ValueTree state changes
// into outbound MIDI for every connected MIDI controller, per the active
// mapping's `feedback` declarations. All actual MIDI sends happen on the
// dedicated MidiOutputThread; this engine only enqueues events.
//
// LIFECYCLE: owned by SonikApplication, constructed after the
// SoftTakeoverManager (depends on it) and destroyed before it. All public
// methods on the Message thread; JUCE_ASSERT_MESSAGE_THREAD throughout.
//
// FEATURES:
//   * Listens to root ValueTree → derives (deckIndex, sourceKind) from each
//     changed property → emits one outbound event per matching binding-with-
//     feedback across all connected devices.
//   * Boot dump on midiDeviceAdded / midiDeviceOpened / activeMappingChanged:
//     iterates every binding's feedback (computed from current VT state) with
//     a 5 ms inter-event stagger.
//   * Blackout dump on activeMappingChanged (preceding the boot dump):
//     emits the off-velocity for every binding in the outgoing mapping that
//     the incoming mapping does not address.
//   * Drops queued events on midiDeviceRemoved (via MidiOutputThread epoch).
//   * Soft-takeover integration: on takeoverStateChanged(Disengaged), starts a
//     Message-thread blink timer driving the binding's `disengagedFeedback`
//     at its configured `blinkHz`. On (Engaged), cancels the blink and emits
//     the regular feedback for the current source value.
//==============================================================================

#include "ControlTargetRegistry.h"
#include "DeviceListChangeListener.h"
#include "MappingStore.h"
#include "MappingTypes.h"
#include "MidiDeviceManager.h"
#include "MidiOutputThread.h"
#include "SoftTakeoverManager.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace sonik::midi
{
    class MidiFeedbackEngine final : private juce::ValueTree::Listener,
                                     private MappingStoreListener,
                                     private DeviceListChangeListener,
                                     private SoftTakeoverManagerListener,
                                     private juce::Timer
    {
    public:
        MidiFeedbackEngine (juce::ValueTree       rootState,
                            MidiDeviceManager&    midiDeviceManager,
                            MappingStore&         mappingStore,
                            SoftTakeoverManager&  softTakeoverManager,
                            bool                  testTapEnabled = false);
        ~MidiFeedbackEngine() override;

        MidiFeedbackEngine (const MidiFeedbackEngine&)            = delete;
        MidiFeedbackEngine& operator= (const MidiFeedbackEngine&) = delete;

        //----------------------------------------------------------------------
        // Test seams. These exercise the same code paths the production
        // listeners do; they never bypass the FIFO/throttle.

        /** Pulls one outbound event from the test tap (a side-channel buffer
            populated by `enqueueOutbound` when test mode is enabled at
            construction). Returns false if the tap is empty. Test-only. */
        bool drainOneForTest (OutboundMidiEvent& out) noexcept;

        int  pendingForTest() const noexcept;

        /** Test-only: query whether a disengaged-blink timer is currently
            active for a given (deviceId, target). */
        bool isBlinkingForTest (std::uint64_t deviceId, TargetIndex target) const noexcept;

        MidiOutputThread& outputThreadForTest() noexcept { return *outputThread; }

    private:
        //-- juce::ValueTree::Listener -------------------------------------
        void valueTreePropertyChanged (juce::ValueTree& treeWhosePropertyHasChanged,
                                       const juce::Identifier& property) override;

        //-- MappingStoreListener ------------------------------------------
        void activeMappingChanged (std::uint64_t deviceId) override;

        //-- DeviceListChangeListener --------------------------------------
        void midiDeviceAdded   (std::uint64_t deviceId) override;
        void midiDeviceOpened  (std::uint64_t deviceId) override;
        void midiDeviceRemoved (std::uint64_t deviceId) override;
        void midiDeviceClosed  (std::uint64_t deviceId) override;

        //-- SoftTakeoverManagerListener -----------------------------------
        void takeoverStateChanged (std::uint64_t deviceId,
                                   TargetIndex   target,
                                   TakeoverState newState) override;

        //-- juce::Timer (blink driver, 25 ms tick) ------------------------
        void timerCallback() override;

        //----------------------------------------------------------------------
        enum class SourceKind : std::uint8_t
        {
            None,
            DeckPlaying,    // bool
            DeckPaused,     // bool (transport.cue LED — lit when paused)
            DeckSynced,     // bool
            LoopActive,     // bool
            HotCueValid,    // bool
            HotCueColour,   // int 0..15
            Continuous,     // float [0,1]
            MixerChannelBool, // bool — see auxIndex enum below (PRD-0061)
        };

        /** auxIndex values for MixerChannelBool. Reuses `deckIndex` slot for
            the channel index (0..3). */
        enum class MixerBoolProp : std::uint8_t
        {
            KillHigh = 1,
            KillMid  = 2,
            KillLow  = 3,
            AssignA  = 4,
            AssignB  = 5,
            Cue      = 6,
        };

        struct SourceKey
        {
            SourceKind   kind      { SourceKind::None };
            std::uint8_t deckIndex { 255 };
            std::uint8_t auxIndex  { 0 };   // hot-cue 1..8; ignored otherwise.
        };

        struct SourceValue
        {
            bool  valid      { false };
            bool  boolValue  { false };
            int   intValue   { 0 };
            float floatValue { 0.0f };
        };

        static SourceKey sourceKeyForBinding (const Binding& binding);

        SourceValue readSourceValue (const SourceKey& key) const;

        static std::optional<std::uint8_t>
            computeFeedbackVelocity (const BindingFeedback& fb,
                                     const SourceValue&     source);

        //----------------------------------------------------------------------
        void enqueueOutbound (std::uint64_t deviceId,
                              const BindingFeedback& fb,
                              std::uint8_t value,
                              std::chrono::steady_clock::time_point earliestSendTime);

        void emitFeedbackForBinding (std::uint64_t deviceId,
                                     const Binding& binding,
                                     std::chrono::steady_clock::time_point earliestSendTime);

        void sendBootDumpForDevice (std::uint64_t deviceId);

        void sendBlackoutDumpForOldMapping (std::uint64_t deviceId,
                                            const Mapping& oldMapping,
                                            const Mapping* newMapping);

        void dispatchSourceChanged (const SourceKey& key);

        //----------------------------------------------------------------------
        struct BlinkEntry
        {
            std::uint64_t   deviceId;
            TargetIndex     target;
            BindingFeedback feedback;
            int             periodMs;
            std::chrono::steady_clock::time_point nextToggleAt;
            bool            currentlyOn;
        };

        std::vector<BlinkEntry>::iterator findBlink (std::uint64_t deviceId, TargetIndex target);

        void startBlink  (std::uint64_t deviceId, TargetIndex target, const BindingFeedback& fb);
        void cancelBlink (std::uint64_t deviceId, TargetIndex target);

        //----------------------------------------------------------------------
        juce::ValueTree deckTreeFor (std::uint8_t deckIndex) const;
        static std::optional<std::uint8_t> deckIndexFromTree (const juce::ValueTree& deckTree);
        static std::optional<std::uint8_t> parseHotCueIndexFromTargetId (const char* id);

        //----------------------------------------------------------------------
        juce::ValueTree        root;
        MidiDeviceManager&     devices;
        MappingStore&          mappings;
        SoftTakeoverManager&   takeover;

        std::unique_ptr<MidiOutputThread> outputThread;

        // Last mapping we boot-dumped for each device, retained so that on
        // activeMappingChanged we can compute the blackout diff.
        std::unordered_map<std::uint64_t, std::shared_ptr<const Mapping>> lastDumpedByDevice;

        std::vector<BlinkEntry> blinks;

        // Test tap: a side-channel buffer that mirrors every event passed
        // to `enqueueOutbound`. Production builds construct the engine with
        // testTapEnabled=false (default), which leaves the tap dormant.
        const bool                       testTapEnabled { false };
        mutable juce::CriticalSection    testTapLock;
        std::deque<OutboundMidiEvent>    testTap;
    };
} // namespace sonik::midi
