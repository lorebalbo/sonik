#pragma once
//==============================================================================
// PRD-0048 Molecule: BindingTable (Phases 4–6)
//
// Phase 4: read-only 7-column table.
// Phase 5: per-row MIDI Learn capture (transient MidiInputSubscriber + Timer).
// Phase 6: inline editors (TRANSFORM, SOFT-TAKEOVER ComboBoxes) and a per-row
//          DELETE button.  Editing emits callbacks; the owning DeviceHeader
//          mutates the active mapping and schedules a debounced save.
//
// Columns: TARGET | MIDI KEY | MODIFIER | TRANSFORM | SOFT-TAKEOVER |
//          FEEDBACK | ACTIONS.
//
// When `readOnly == true` (bundled profile) every cell is paint-only.
// When `readOnly == false` the TRANSFORM and SOFT-TAKEOVER columns are
// rendered as JUCE `ComboBox` children, and the ACTIONS column contains
// `[LEARN] [DEL]` buttons.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../MappingTypes.h"
#include "../../MidiDeviceManager.h"
#include "../../MidiInputSubscriber.h"
#include "../Atoms/LearnButton.h"

#include <atomic>
#include <memory>

namespace sonik::midi::ui
{
    class BindingTable final : public juce::Component,
                               private juce::Timer,
                               private MidiInputSubscriber
    {
    public:
        BindingTable (std::uint64_t      deviceId,
                      MidiDeviceManager& deviceManager);
        ~BindingTable() override;

        void setMapping (std::shared_ptr<const Mapping> mapping, bool readOnly);

        int  getPreferredHeight() const noexcept;

        void paint   (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

        // ---- Edit callbacks (fire on the Message thread) -------------------
        std::function<void (size_t bindingIndex,
                            std::uint32_t newMidiKey)>          onMidiLearned;
        std::function<void (size_t bindingIndex)>               onDeleteRow;
        std::function<void (size_t bindingIndex, Transform)>    onTransformChanged;
        std::function<void (size_t bindingIndex,
                            SoftTakeoverPolicy)>                onSoftTakeoverChanged;
        std::function<void (size_t bindingIndex,
                            TargetIndex)>                       onTargetChanged;

    private:
        // ---- juce::Timer / MidiInputSubscriber -----------------------------
        void timerCallback() override;
        void onMidiInbound (const MidiInboundEvent&) noexcept override;

        // ---- Internals -----------------------------------------------------
        void rebuildInteractiveControls();
        void startLearningRow  (int rowIdx);
        void cancelLearning();
        void handleLearnedOnMessageThread (std::uint32_t midiKey);

        std::uint64_t      deviceId;
        MidiDeviceManager& deviceManager;

        std::shared_ptr<const Mapping> mapping;
        bool                           readOnly { true };

        // Per-row children (only populated when editable).
        juce::OwnedArray<LearnButton>      learnButtons;
        juce::OwnedArray<juce::TextButton> deleteButtons;
        juce::OwnedArray<juce::ComboBox>   transformCombos;
        juce::OwnedArray<juce::ComboBox>   softTakeoverCombos;

        // Learning state (Message thread).
        int learningRow      { -1 };
        int secondsRemaining { 0 };

        // RT→Message handshake.
        std::atomic<std::uint32_t> capturedMidiKey { 0 };
        std::atomic<bool>          subscribed      { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BindingTable)
    };
}
