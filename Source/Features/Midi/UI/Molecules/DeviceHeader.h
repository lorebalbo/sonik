#pragma once
//==============================================================================
// PRD-0048 Molecule: DeviceHeader
//
// Represents one MIDI device row at the top of the MIDI Settings panel.
// Shows the device's manufacturer/product, its deviceId (monospace), a
// connection-state pill, and a profile dropdown listing every bundled and
// user mapping available for it.  Phase 3 wiring: switching the dropdown
// pins the active mapping (`MappingStore::setActiveMapping`).  Duplicate /
// Delete / Reset buttons are also functional in Phase 3.
//
// The actual binding table (Phase 4) is rendered separately under the
// header by the parent organism.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../MappingStore.h"
#include "../../MidiDeviceManager.h"
#include "../../MidiDeviceRecord.h"
#include "../../MidiInboundRouter.h"
#include "../../SoftTakeoverManager.h"
#include "../Atoms/DevicePill.h"
#include "../Atoms/DisengagedList.h"
#include "../Atoms/ModifierBanner.h"
#include "../Atoms/ProfileDropdown.h"
#include "BindingTable.h"
#include "PortBindingControls.h"

#include <memory>

namespace sonik::midi::ui
{
    class DeviceHeader final : public juce::Component,
                               private juce::Timer
    {
    public:
        DeviceHeader (MidiDeviceRecord recordIn,
                      MappingStore&    storeIn,
                      MidiDeviceManager& deviceManagerIn,
                      MidiInboundRouter& routerIn,
                      SoftTakeoverManager& softTakeoverIn);
        ~DeviceHeader() override = default;

        void paint   (juce::Graphics&) override;
        void resized() override;

        /** Returns the device id this header represents. */
        std::uint64_t getDeviceId() const noexcept { return record.deviceId; }

        /** Update the cached device record (e.g. on disconnect) and refresh
            the pill / labels. */
        void setRecord (const MidiDeviceRecord& newRecord);

        /** Re-pull the available profile list and active mapping id from the
            store and refresh the dropdown.  Called by the parent organism
            in response to `MappingStoreListener` events. */
        void refreshFromStore();

        /** Height needed to display the header bar + the binding table.
            Read by the parent `MidiSettingsPanel::Content` layout. */
        int getPreferredHeight() const noexcept;

        /** Pure helper (exposed for unit tests). Given a freshly-learned
            midiKey and the current Transform of the row, returns the
            Transform that should be stored. If the row is still at the
            placeholder default (Momentary), this promotes CC keys to
            Linear and Pitch-Bend keys to Linear14, so continuous controls
            (knobs, faders, wheels) move smoothly instead of pinning to
            min/max. If the user has already chosen a transform explicitly,
            it is preserved.
            See PRD note "MIDI learn binds but on-screen knobs don't move". */
        static Transform inferDefaultTransform (std::uint32_t midiKey,
                                                Transform current) noexcept;

    private:
        void rebuildProfileList();
        void refreshPortBindingControls();
        void handleBindToPortToggled (bool shouldBind);
        void handleMidiLearned       (size_t bindingIndex, std::uint32_t newMidiKey);
        void resolveConflictByReplace (bool currentIsPlaceholder,
                                       size_t currentPhIdx,
                                       size_t currentBindingIndex,
                                       std::uint32_t newMidiKey,
                                       bool conflictIsPlaceholder,
                                       size_t conflictIdx);
        void handleDeleteRow         (size_t bindingIndex);
        void handleTransformChanged  (size_t bindingIndex, Transform);
        void handleSoftTakeoverChanged (size_t bindingIndex, SoftTakeoverPolicy);
        void handleTargetChanged     (size_t bindingIndex, TargetIndex);
        void handleAddBindingClicked();
        void handleProfileSelected();
        void handleDuplicateClicked();
        void handleDeleteClicked();
        void handleResetClicked();
        void updateActionButtonsEnablement();

        // ---- Debounced save -------------------------------------------
        void beginEdit();                           // copies active mapping → pendingEdit if empty
        void scheduleDebouncedSave();               // (re)arms the 500ms timer
        void timerCallback() override;              // flushes pendingEdit → MappingStore
        void pushPendingToTable();                  // async refresh from pendingEdit
        void pushSnapshotToTable();                 // store snapshot + placeholders → table

        MidiDeviceRecord   record;
        MappingStore&      store;
        MidiDeviceManager& deviceManager;
        MidiInboundRouter& inboundRouter;
        SoftTakeoverManager& softTakeoverManager;

        juce::Label      titleLabel;       // "Manufacturer - Product"
        juce::Label      deviceIdLabel;    // monospace hex id
        DevicePill       pill;
        ProfileDropdown  profileDropdown;

        juce::TextButton duplicateButton { "DUPLICATE" };
        juce::TextButton deleteButton    { "DELETE" };
        juce::TextButton resetButton     { "RESET" };
        juce::TextButton addBindingButton { "+ ADD BINDING" };

        BindingTable     bindingTable;
        std::unique_ptr<ModifierBanner> modifierBanner;
        std::unique_ptr<DisengagedList> disengagedList;
        PortBindingControls portBindingControls;
        bool             activeIsBundled { true };

        // Debounced save state -----------------------------------------
        std::unique_ptr<Mapping> pendingEdit;
        // Locally added rows whose target is still InvalidTargetIndex.  These
        // never round-trip through `MappingStore` (serializer skips them),
        // so we preserve them across save+refresh cycles by re-appending
        // them to the table snapshot after every `refreshFromStore`.
        std::vector<Binding> pendingPlaceholders;
        // Number of real (persisted) rows currently in the table, used to
        // split row indices into "real" (`< baseBindingCount`) vs
        // "placeholder" (`>= baseBindingCount`).
        size_t baseBindingCount { 0 };
        static constexpr int kDebounceMs = 500;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceHeader)
    };
}
