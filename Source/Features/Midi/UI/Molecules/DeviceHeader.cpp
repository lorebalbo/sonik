#include "DeviceHeader.h"
#include "../../ControlTargetRegistry.h"

namespace sonik::midi::ui
{
    namespace
    {
        constexpr juce::uint32 kBg     = 0xFFFDFDFD;
        constexpr juce::uint32 kFg     = 0xFF2D2D2D;
        constexpr juce::uint32 kBorder = 0xFF2D2D2D;

        // Normalize a packed midiKey for conflict comparison.  Pitch-bend
        // keys collapse data1 to 0 (the parser already stores them this way;
        // BindingTable's learn capture preserves the raw data1 byte, so we
        // mask here to keep the comparison consistent).
        std::uint32_t normalizeMidiKey (std::uint32_t key) noexcept
        {
            const std::uint8_t status = (std::uint8_t) ((key >> 8) & 0xF0u);
            if (status == 0xE0u)
                return key & 0xFFFF00u; // keep channel + status, zero data1
            return key;
        }

        juce::String formatDeviceId (std::uint64_t id)
        {
            return juce::String::toHexString ((juce::int64) id).paddedLeft ('0', 16).toUpperCase();
        }

        void styleButton (juce::TextButton& b)
        {
            // DESIGN.md: 2px solid #2d2d2d border, monochrome fill inversion.
            b.setColour (juce::TextButton::buttonColourId,   juce::Colour (kBg));
            b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (kFg));
            b.setColour (juce::TextButton::textColourOnId,   juce::Colour (kBg));
            b.setColour (juce::TextButton::textColourOffId,  juce::Colour (kFg));
        }

        juce::String describeProfile (const MappingProfileSummary& p)
        {
            juce::String label = p.displayName.isNotEmpty() ? p.displayName : p.id;
            if (p.origin == MappingOrigin::Bundled)
                label << " (bundled)";
            return label;
        }
    }

    //--------------------------------------------------------------------------
    DeviceHeader::DeviceHeader (MidiDeviceRecord recordIn,
                                MappingStore&    storeIn,
                                MidiDeviceManager& deviceManagerIn,
                                MidiInboundRouter& routerIn,
                                SoftTakeoverManager& softTakeoverIn)
        : record (std::move (recordIn)),
          store  (storeIn),
          deviceManager (deviceManagerIn),
          inboundRouter (routerIn),
          softTakeoverManager (softTakeoverIn),
          bindingTable  (record.deviceId, deviceManagerIn)
    {
        // Title: "Manufacturer - Product"
        juce::String title = record.manufacturer.isNotEmpty()
                             ? record.manufacturer + " - " + record.productName
                             : record.productName;
        titleLabel.setText (title, juce::dontSendNotification);
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (kFg));
        titleLabel.setFont   (juce::FontOptions ("Space Mono", 14.0f, juce::Font::plain));
        addAndMakeVisible (titleLabel);

        deviceIdLabel.setText ("ID " + formatDeviceId (record.deviceId),
                               juce::dontSendNotification);
        deviceIdLabel.setColour (juce::Label::textColourId, juce::Colour (kFg));
        deviceIdLabel.setFont   (juce::FontOptions ("Space Mono", 10.0f, juce::Font::plain));
        addAndMakeVisible (deviceIdLabel);

        pill.setConnected (record.isConnected);
        addAndMakeVisible (pill);

        addAndMakeVisible (profileDropdown);
        profileDropdown.onChange = [this]() { handleProfileSelected(); };

        styleButton (duplicateButton);
        styleButton (deleteButton);
        styleButton (resetButton);
        styleButton (addBindingButton);
        addAndMakeVisible (duplicateButton);
        addAndMakeVisible (deleteButton);
        addAndMakeVisible (resetButton);
        addAndMakeVisible (addBindingButton);

        duplicateButton.onClick  = [this]() { handleDuplicateClicked();   };
        deleteButton   .onClick  = [this]() { handleDeleteClicked();      };
        resetButton    .onClick  = [this]() { handleResetClicked();       };
        addBindingButton.onClick = [this]() { handleAddBindingClicked();  };

        addAndMakeVisible (bindingTable);

        modifierBanner = std::make_unique<ModifierBanner> (record.deviceId,
                                                           inboundRouter,
                                                           store);
        addAndMakeVisible (*modifierBanner);

        disengagedList = std::make_unique<DisengagedList> (record.deviceId,
                                                           softTakeoverManager,
                                                           store);
        addAndMakeVisible (*disengagedList);
        bindingTable.onMidiLearned = [this] (size_t i, std::uint32_t k)
        {
            handleMidiLearned (i, k);
        };
        bindingTable.onDeleteRow = [this] (size_t i)
        {
            handleDeleteRow (i);
        };
        bindingTable.onTransformChanged = [this] (size_t i, Transform t)
        {
            handleTransformChanged (i, t);
        };
        bindingTable.onSoftTakeoverChanged = [this] (size_t i, SoftTakeoverPolicy p)
        {
            handleSoftTakeoverChanged (i, p);
        };
        bindingTable.onTargetChanged = [this] (size_t i, TargetIndex t)
        {
            handleTargetChanged (i, t);
        };

        rebuildProfileList();
    }

    //--------------------------------------------------------------------------
    void DeviceHeader::paint (juce::Graphics& g)
    {
        // Tonal layering: header sits on the panel surface with a 2px ink
        // border to make it a distinct organism row per DESIGN.md.
        g.fillAll (juce::Colour (kBg));

        g.setColour (juce::Colour (kBorder));
        g.drawRect (getLocalBounds(), 2);
    }

    void DeviceHeader::resized()
    {
        auto bounds = getLocalBounds().reduced (12, 8);

        // ---- Header bar (title row + id row + action row) -------------
        auto topRow = bounds.removeFromTop (24);
        pill.setBounds (topRow.removeFromRight (110).withSizeKeepingCentre (110, 18));
        topRow.removeFromRight (8);
        titleLabel.setBounds (topRow);

        bounds.removeFromTop (4);

        deviceIdLabel.setBounds (bounds.removeFromTop (16));

        bounds.removeFromTop (6);

        auto actionRow = bounds.removeFromTop (28);

        constexpr int kBtnW = 96;
        constexpr int kGap  = 6;

        resetButton    .setBounds (actionRow.removeFromRight (kBtnW));
        actionRow.removeFromRight (kGap);
        deleteButton   .setBounds (actionRow.removeFromRight (kBtnW));
        actionRow.removeFromRight (kGap);
        duplicateButton.setBounds (actionRow.removeFromRight (kBtnW));
        actionRow.removeFromRight (12);

        profileDropdown.setBounds (actionRow);

        // ---- Binding table (fills the remaining vertical space) -------
        bounds.removeFromTop (8);

        // Modifier banner (collapses to 0 when no modifiers held).
        if (modifierBanner != nullptr)
        {
            const int mbH = modifierBanner->getPreferredHeight();
            if (mbH > 0)
            {
                modifierBanner->setBounds (bounds.removeFromTop (mbH));
                bounds.removeFromTop (6);
            }
            else
            {
                modifierBanner->setBounds ({});
            }
        }

        // Disengaged soft-takeover list (collapses to 0 when empty).
        if (disengagedList != nullptr)
        {
            const int dlH = disengagedList->getPreferredHeight();
            if (dlH > 0)
            {
                disengagedList->setBounds (bounds.removeFromTop (dlH));
                bounds.removeFromTop (6);
            }
            else
            {
                disengagedList->setBounds ({});
            }
        }

        // Reserve space for the "+ ADD BINDING" footer when editable.
        constexpr int kAddBindingBarHeight = 28;
        constexpr int kAddBindingBarGap    = 6;
        if (addBindingButton.isVisible())
        {
            auto footer = bounds.removeFromBottom (kAddBindingBarHeight);
            bounds.removeFromBottom (kAddBindingBarGap);
            constexpr int kAddBtnW = 140;
            addBindingButton.setBounds (footer.removeFromLeft (kAddBtnW)
                                              .withHeight (kAddBindingBarHeight));
        }

        bindingTable.setBounds (bounds);
    }

    //--------------------------------------------------------------------------
    void DeviceHeader::setRecord (const MidiDeviceRecord& newRecord)
    {
        record = newRecord;
        pill.setConnected (record.isConnected);
        repaint();
    }

    void DeviceHeader::refreshFromStore()
    {
        rebuildProfileList();
    }

    int DeviceHeader::getPreferredHeight() const noexcept
    {
        // Header bar = 8 (top pad) + 24 (title) + 4 + 16 (id) + 6 + 28 (actions) + 8 (gap) + 8 (bottom pad)
        constexpr int kHeaderArea = 8 + 24 + 4 + 16 + 6 + 28 + 8 + 8;
        const int footer  = activeIsBundled ? 0 : (28 + 6); // "+ ADD BINDING" bar
        const int mbH     = modifierBanner != nullptr ? modifierBanner->getPreferredHeight() : 0;
        const int mbBlock = mbH > 0 ? (mbH + 6) : 0;
        const int dlH     = disengagedList != nullptr ? disengagedList->getPreferredHeight() : 0;
        const int dlBlock = dlH > 0 ? (dlH + 6) : 0;
        return kHeaderArea + mbBlock + dlBlock + bindingTable.getPreferredHeight() + footer;
    }

    //--------------------------------------------------------------------------
    void DeviceHeader::rebuildProfileList()
    {
        const auto entries  = store.listAvailableMappings (record.deviceId);
        const auto activeId = store.getActiveMappingIdForDevice (record.deviceId);
        const auto prevActiveId = profileDropdown.getSelectedProfile() != nullptr
                                  ? profileDropdown.getSelectedProfile()->id
                                  : juce::String();
        profileDropdown.setProfiles (entries, activeId);
        updateActionButtonsEnablement();

        bool isBundled = false;
        for (const auto& s : entries)
        {
            if (s.id == activeId)
            {
                isBundled = (s.origin == MappingOrigin::Bundled);
                break;
            }
        }
        const bool profileChanged = (prevActiveId.isNotEmpty() && prevActiveId != activeId)
                                    || (isBundled && ! pendingPlaceholders.empty());
        activeIsBundled = isBundled;

        // Profile switch invalidates any in-flight edit AND placeholders.
        if (profileChanged)
        {
            stopTimer();
            pendingEdit.reset();
            pendingPlaceholders.clear();
        }

        addBindingButton.setVisible (! activeIsBundled);
        pushSnapshotToTable();

        // PRD-0048 Phase 10: re-seed the disengaged list against the new
        // active mapping (state entries persist across mapping switches
        // until explicitly reset, but the list of softTakeover-eligible
        // targets changes per mapping).
        if (disengagedList != nullptr)
            disengagedList->refresh();

        // Notify ancestors that our preferred height may have changed so
        // they can re-stack the device sections.
        if (auto* parent = getParentComponent())
            parent->resized();
    }

    void DeviceHeader::updateActionButtonsEnablement()
    {
        const auto* selected = profileDropdown.getSelectedProfile();
        const bool isUserSelected = selected != nullptr
                                    && selected->origin == MappingOrigin::User;
        // Duplicate: always available when something is selected.
        duplicateButton.setEnabled (selected != nullptr);
        // Delete: only for user profiles.
        deleteButton.setEnabled (isUserSelected);
        // Reset: only meaningful if a user override is currently active.
        resetButton.setEnabled (isUserSelected);
    }

    //--------------------------------------------------------------------------
    void DeviceHeader::handleProfileSelected()
    {
        const auto* selected = profileDropdown.getSelectedProfile();
        if (selected == nullptr)
            return;

        const auto result = store.setActiveMapping (record.deviceId, selected->id);
        juce::ignoreUnused (result);
        updateActionButtonsEnablement();
    }

    void DeviceHeader::handleDuplicateClicked()
    {
        const auto* selected = profileDropdown.getSelectedProfile();
        if (selected == nullptr)
            return;

        auto aw = std::make_shared<juce::AlertWindow> ("Duplicate to User Profile",
                                                       "Enter a name for the new user profile:",
                                                       juce::MessageBoxIconType::QuestionIcon);
        aw->addTextEditor ("name", describeProfile (*selected) + " (copy)");
        aw->addButton ("Duplicate", 1);
        aw->addButton ("Cancel",    0);

        auto sourceId = selected->id;
        auto deviceId = record.deviceId;
        auto& storeRef = store;
        juce::Component::SafePointer<DeviceHeader> safe { this };

        aw->enterModalState (true,
            juce::ModalCallbackFunction::create (
                [aw, sourceId, deviceId, &storeRef, safe] (int result) mutable
                {
                    if (result != 1 || safe == nullptr)
                        return;
                    const auto newName = aw->getTextEditorContents ("name").trim();
                    if (newName.isEmpty())
                        return;

                    juce::String newId;
                    const auto rc = storeRef.createUserCopy (sourceId, newName, &newId);
                    if (rc == CreateUserCopyResult::Ok && newId.isNotEmpty())
                        storeRef.setActiveMapping (deviceId, newId);
                    if (safe != nullptr)
                        safe->refreshFromStore();
                }),
            false);
    }

    void DeviceHeader::handleDeleteClicked()
    {
        const auto* selected = profileDropdown.getSelectedProfile();
        if (selected == nullptr || selected->origin != MappingOrigin::User)
            return;

        auto options = juce::MessageBoxOptions()
            .withTitle ("Delete User Profile")
            .withMessage ("Permanently delete \"" + describeProfile (*selected) + "\"?")
            .withButton ("Delete")
            .withButton ("Cancel")
            .withIconType (juce::MessageBoxIconType::WarningIcon);

        const auto mappingId = selected->id;
        auto& storeRef = store;
        juce::Component::SafePointer<DeviceHeader> safe { this };

        juce::AlertWindow::showAsync (options, [mappingId, &storeRef, safe] (int result)
        {
            if (result != 1)
                return;
            storeRef.deleteUserMapping (mappingId);
            if (safe != nullptr)
                safe->refreshFromStore();
        });
    }

    void DeviceHeader::handleResetClicked()
    {
        auto options = juce::MessageBoxOptions()
            .withTitle ("Reset to Defaults")
            .withMessage ("Clear the user override and revert to the bundled default for this device?")
            .withButton ("Reset")
            .withButton ("Cancel")
            .withIconType (juce::MessageBoxIconType::QuestionIcon);

        const auto deviceId = record.deviceId;
        auto& storeRef = store;
        juce::Component::SafePointer<DeviceHeader> safe { this };

        juce::AlertWindow::showAsync (options, [deviceId, &storeRef, safe] (int result)
        {
            if (result != 1)
                return;
            // Empty mappingId clears the override; the store re-resolves the
            // device against its automatic matcher (bundled default).
            storeRef.setActiveMapping (deviceId, juce::String());
            if (safe != nullptr)
                safe->refreshFromStore();
        });
    }

    //--------------------------------------------------------------------------
    // Phase 6: debounced edit pipeline
    //--------------------------------------------------------------------------
    void DeviceHeader::beginEdit()
    {
        if (pendingEdit != nullptr)
            return;
        auto current = store.getActiveMappingForDevice (record.deviceId);
        if (current == nullptr)
            return;
        pendingEdit = std::make_unique<Mapping> (*current);
    }

    void DeviceHeader::scheduleDebouncedSave()
    {
        startTimer (kDebounceMs);
    }

    void DeviceHeader::pushPendingToTable()
    {
        if (pendingEdit == nullptr) return;
        // Deferred to avoid tearing down the very child component whose
        // onClick / onChange we are currently inside.
        juce::Component::SafePointer<DeviceHeader> safe { this };
        juce::MessageManager::callAsync ([safe]()
        {
            if (safe == nullptr) return;
            safe->pushSnapshotToTable();
        });
    }

    void DeviceHeader::pushSnapshotToTable()
    {
        // Build the snapshot from the most authoritative source available:
        //   1. pendingEdit (in-flight, not yet saved) when present
        //   2. otherwise the store's active mapping
        // Then append all pendingPlaceholders so locally-added blank rows
        // survive save/refresh round-trips.
        std::shared_ptr<Mapping> snapshot;
        if (pendingEdit != nullptr)
        {
            snapshot = std::make_shared<Mapping> (*pendingEdit);
        }
        else
        {
            auto active = store.getActiveMappingForDevice (record.deviceId);
            if (active != nullptr)
                snapshot = std::make_shared<Mapping> (*active);
            else
                snapshot = std::make_shared<Mapping>();
        }

        baseBindingCount = snapshot->bindings.size();
        for (const auto& ph : pendingPlaceholders)
            snapshot->bindings.push_back (ph);

        bindingTable.setMapping (snapshot, /*readOnly*/ activeIsBundled);

        if (auto* parent = getParentComponent())
            parent->resized();
    }

    void DeviceHeader::timerCallback()
    {
        stopTimer();
        if (pendingEdit == nullptr || activeIsBundled)
            return;

        const auto activeId = store.getActiveMappingIdForDevice (record.deviceId);
        if (activeId.isEmpty())
        {
            pendingEdit.reset();
            return;
        }

        Mapping toSave = std::move (*pendingEdit);
        pendingEdit.reset();
        const auto rc = store.saveUserMapping (toSave, activeId + ".json");
        juce::ignoreUnused (rc);
        // The store's listener will refresh us via MidiSettingsPanel \u2192
        // refreshFromStore \u2192 bindingTable.setMapping.
    }

    void DeviceHeader::handleMidiLearned (size_t bindingIndex,
                                         std::uint32_t newMidiKey)
    {
        if (activeIsBundled) return;

        const auto isPlaceholderRow = bindingIndex >= baseBindingCount;
        const auto phIdx            = isPlaceholderRow ? bindingIndex - baseBindingCount : 0u;

        if (isPlaceholderRow && phIdx >= pendingPlaceholders.size()) return;

        // Read the current row's required modifier mask (so the conflict
        // comparison considers it).  Placeholders carry their own; real rows
        // come from pendingEdit or the stored mapping.
        std::uint32_t currentModifierMask = 0u;
        if (isPlaceholderRow)
        {
            currentModifierMask = pendingPlaceholders[phIdx].requiredModifierMask;
        }
        else
        {
            if (pendingEdit != nullptr)
            {
                if (bindingIndex >= pendingEdit->bindings.size()) return;
                currentModifierMask = pendingEdit->bindings[bindingIndex].requiredModifierMask;
            }
            else
            {
                auto active = store.getActiveMappingForDevice (record.deviceId);
                if (active == nullptr) return;
                if (bindingIndex >= active->bindings.size()) return;
                currentModifierMask = active->bindings[bindingIndex].requiredModifierMask;
            }
        }

        // ---- Conflict detection -----------------------------------------
        // Scan real bindings (pendingEdit-or-stored) and placeholders for any
        // OTHER row with the same normalized MIDI key + modifier mask.
        const std::uint32_t learnedKey = normalizeMidiKey (newMidiKey);
        struct RowRef { bool isPlaceholder; size_t idx; std::uint16_t target; };
        std::optional<RowRef> conflict;

        const Mapping* realSrc = pendingEdit.get();
        std::shared_ptr<const Mapping> storedHolder;
        if (realSrc == nullptr)
        {
            storedHolder = store.getActiveMappingForDevice (record.deviceId);
            realSrc = storedHolder.get();
        }
        if (realSrc != nullptr)
        {
            for (size_t i = 0; i < realSrc->bindings.size(); ++i)
            {
                if (! isPlaceholderRow && i == bindingIndex) continue;
                const auto& b = realSrc->bindings[i];
                if (normalizeMidiKey (b.midiKey) == learnedKey
                    && b.requiredModifierMask == currentModifierMask)
                {
                    conflict = RowRef { false, i, b.target };
                    break;
                }
            }
        }
        if (! conflict.has_value())
        {
            for (size_t i = 0; i < pendingPlaceholders.size(); ++i)
            {
                if (isPlaceholderRow && i == phIdx) continue;
                const auto& b = pendingPlaceholders[i];
                if (b.midiKey == 0u) continue; // unassigned placeholder, no conflict
                if (normalizeMidiKey (b.midiKey) == learnedKey
                    && b.requiredModifierMask == currentModifierMask)
                {
                    conflict = RowRef { true, i, b.target };
                    break;
                }
            }
        }

        if (conflict.has_value())
        {
            juce::String conflictName ("(unassigned)");
            if (conflict->target != InvalidTargetIndex
                && conflict->target < ControlTargetRegistry::size())
            {
                const auto& t = ControlTargetRegistry::get (conflict->target);
                if (t.id != nullptr) conflictName = juce::String (t.id);
            }

            auto options = juce::MessageBoxOptions()
                .withTitle ("MIDI Key Already Bound")
                .withMessage ("This MIDI key is already bound to "
                              + conflictName + ". Replace, change modifier, or cancel?")
                .withButton ("Replace")
                .withButton ("Change Modifier")
                .withButton ("Cancel")
                .withIconType (juce::MessageBoxIconType::QuestionIcon);

            juce::Component::SafePointer<DeviceHeader> safe { this };
            const auto isPlaceholderRowCopy = isPlaceholderRow;
            const auto phIdxCopy            = phIdx;
            const auto bindingIndexCopy     = bindingIndex;
            const auto newMidiKeyCopy       = newMidiKey;
            const auto conflictCopy         = *conflict;
            juce::AlertWindow::showAsync (options,
                [safe, isPlaceholderRowCopy, phIdxCopy, bindingIndexCopy,
                 newMidiKeyCopy, conflictCopy] (int result)
            {
                if (safe == nullptr) return;
                if (result == 1) // Replace
                {
                    safe->resolveConflictByReplace (isPlaceholderRowCopy, phIdxCopy,
                                                    bindingIndexCopy, newMidiKeyCopy,
                                                    conflictCopy.isPlaceholder,
                                                    conflictCopy.idx);
                }
                else if (result == 2) // Change Modifier
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::InfoIcon,
                        "Change Modifier",
                        "Hold the desired modifier control on a controller, "
                        "then click LEARN on this row again.  The modifier "
                        "mask currently held will be combined with the new "
                        "MIDI key.");
                }
                // result == 0 → Cancel: do nothing.
            });
            return;
        }

        // No conflict — apply directly.
        if (isPlaceholderRow)
        {
            pendingPlaceholders[phIdx].midiKey  = learnedKey;
            pendingPlaceholders[phIdx].lsbData1 = 0xFFu;
            pushSnapshotToTable();
            return;
        }

        beginEdit();
        if (pendingEdit == nullptr) return;
        if (bindingIndex >= pendingEdit->bindings.size()) return;

        pendingEdit->bindings[bindingIndex].midiKey  = learnedKey;
        pendingEdit->bindings[bindingIndex].lsbData1 = 0xFFu;

        pushPendingToTable();
        scheduleDebouncedSave();
    }

    void DeviceHeader::resolveConflictByReplace (bool currentIsPlaceholder,
                                                 size_t currentPhIdx,
                                                 size_t currentBindingIndex,
                                                 std::uint32_t newMidiKey,
                                                 bool conflictIsPlaceholder,
                                                 size_t conflictIdx)
    {
        const std::uint32_t learnedKey = normalizeMidiKey (newMidiKey);

        // Step 1: erase the conflicting row.  We may have to adjust the
        // current row's index if both rows live in the same space and the
        // conflict precedes the current row.
        if (conflictIsPlaceholder)
        {
            if (conflictIdx >= pendingPlaceholders.size()) return;
            pendingPlaceholders.erase (pendingPlaceholders.begin()
                                       + (std::ptrdiff_t) conflictIdx);
            if (currentIsPlaceholder && conflictIdx < currentPhIdx)
                --currentPhIdx;
        }
        else
        {
            beginEdit();
            if (pendingEdit == nullptr) return;
            if (conflictIdx >= pendingEdit->bindings.size()) return;
            pendingEdit->bindings.erase (pendingEdit->bindings.begin()
                                         + (std::ptrdiff_t) conflictIdx);
            if (! currentIsPlaceholder && conflictIdx < currentBindingIndex)
                --currentBindingIndex;
        }

        // Step 2: apply the new key to the (possibly shifted) current row.
        if (currentIsPlaceholder)
        {
            if (currentPhIdx >= pendingPlaceholders.size()) return;
            pendingPlaceholders[currentPhIdx].midiKey  = learnedKey;
            pendingPlaceholders[currentPhIdx].lsbData1 = 0xFFu;
        }
        else
        {
            if (pendingEdit == nullptr)
            {
                beginEdit();
                if (pendingEdit == nullptr) return;
            }
            if (currentBindingIndex >= pendingEdit->bindings.size()) return;
            pendingEdit->bindings[currentBindingIndex].midiKey  = learnedKey;
            pendingEdit->bindings[currentBindingIndex].lsbData1 = 0xFFu;
        }

        // Step 3: push & save (only if we touched real bindings).
        if (pendingEdit != nullptr)
        {
            pushPendingToTable();
            scheduleDebouncedSave();
        }
        else
        {
            pushSnapshotToTable();
        }
    }

    void DeviceHeader::handleDeleteRow (size_t bindingIndex)
    {
        if (activeIsBundled) return;

        // Placeholder row?  Remove locally, no save / no confirmation prompt.
        if (bindingIndex >= baseBindingCount)
        {
            const size_t phIdx = bindingIndex - baseBindingCount;
            if (phIdx >= pendingPlaceholders.size()) return;
            juce::Component::SafePointer<DeviceHeader> safe { this };
            juce::MessageManager::callAsync ([safe, phIdx]()
            {
                if (safe == nullptr) return;
                if (phIdx >= safe->pendingPlaceholders.size()) return;
                safe->pendingPlaceholders.erase (safe->pendingPlaceholders.begin()
                                                 + (std::ptrdiff_t) phIdx);
                safe->pushSnapshotToTable();
            });
            return;
        }

        beginEdit();
        if (pendingEdit == nullptr) return;
        if (bindingIndex >= pendingEdit->bindings.size()) return;

        auto options = juce::MessageBoxOptions()
            .withTitle ("Delete Binding")
            .withMessage ("Permanently delete this binding from the user profile?")
            .withButton ("Delete")
            .withButton ("Cancel")
            .withIconType (juce::MessageBoxIconType::WarningIcon);

        juce::Component::SafePointer<DeviceHeader> safe { this };
        juce::AlertWindow::showAsync (options, [safe, bindingIndex] (int result)
        {
            if (safe == nullptr) return;
            if (result != 1) return;
            if (safe->pendingEdit == nullptr) return;
            if (bindingIndex >= safe->pendingEdit->bindings.size()) return;
            safe->pendingEdit->bindings.erase (safe->pendingEdit->bindings.begin()
                                              + (std::ptrdiff_t) bindingIndex);
            safe->pushPendingToTable();
            safe->scheduleDebouncedSave();
        });
    }

    void DeviceHeader::handleTransformChanged (size_t bindingIndex, Transform t)
    {
        if (activeIsBundled) return;

        if (bindingIndex >= baseBindingCount)
        {
            const size_t phIdx = bindingIndex - baseBindingCount;
            if (phIdx >= pendingPlaceholders.size()) return;
            pendingPlaceholders[phIdx].transform = t;
            return; // ComboBox already shows new value; no save.
        }

        beginEdit();
        if (pendingEdit == nullptr) return;
        if (bindingIndex >= pendingEdit->bindings.size()) return;

        pendingEdit->bindings[bindingIndex].transform = t;
        scheduleDebouncedSave();
    }

    void DeviceHeader::handleSoftTakeoverChanged (size_t bindingIndex, SoftTakeoverPolicy p)
    {
        if (activeIsBundled) return;

        if (bindingIndex >= baseBindingCount)
        {
            const size_t phIdx = bindingIndex - baseBindingCount;
            if (phIdx >= pendingPlaceholders.size()) return;
            pendingPlaceholders[phIdx].softTakeover = p;
            return;
        }

        beginEdit();
        if (pendingEdit == nullptr) return;
        if (bindingIndex >= pendingEdit->bindings.size()) return;

        pendingEdit->bindings[bindingIndex].softTakeover = p;
        scheduleDebouncedSave();
    }

    void DeviceHeader::handleTargetChanged (size_t bindingIndex, TargetIndex newTarget)
    {
        if (activeIsBundled) return;
        if (newTarget >= ControlTargetRegistry::size()) return;

        // ---- Placeholder path -------------------------------------------
        if (bindingIndex >= baseBindingCount)
        {
            const size_t phIdx = bindingIndex - baseBindingCount;
            if (phIdx >= pendingPlaceholders.size()) return;

            pendingPlaceholders[phIdx].target = newTarget;

            // Promote to a real binding once both a target and a MIDI key
            // have been assigned.  Otherwise keep it in the placeholder pool.
            if (pendingPlaceholders[phIdx].midiKey != 0u)
            {
                beginEdit();
                if (pendingEdit == nullptr) return;
                pendingEdit->bindings.push_back (pendingPlaceholders[phIdx]);
                pendingPlaceholders.erase (pendingPlaceholders.begin()
                                            + (std::ptrdiff_t) phIdx);
                pushPendingToTable();
                scheduleDebouncedSave();
            }
            else
            {
                pushSnapshotToTable();
            }
            return;
        }

        // ---- Real binding path ------------------------------------------
        beginEdit();
        if (pendingEdit == nullptr) return;
        if (bindingIndex >= pendingEdit->bindings.size()) return;

        pendingEdit->bindings[bindingIndex].target = newTarget;
        pushPendingToTable();
        scheduleDebouncedSave();
    }

    void DeviceHeader::handleAddBindingClicked()
    {
        if (activeIsBundled) return;

        // Append a placeholder row.  Stays in-memory until a future target
        // picker (Phase 11) assigns it a real target, at which point it will
        // be promoted into pendingEdit->bindings and persisted.
        Binding b {};
        b.target               = InvalidTargetIndex;
        b.midiKey              = 0;
        b.lsbData1             = 0xFFu;
        b.transform            = Transform::Momentary;
        b.requiredModifierMask = 0u;
        b.softTakeover         = SoftTakeoverPolicy::Pickup;
        b.feedback             = BindingFeedback {};
        b.disengagedFeedback   = BindingFeedback {};
        pendingPlaceholders.push_back (b);

        pushSnapshotToTable();
    }
}
