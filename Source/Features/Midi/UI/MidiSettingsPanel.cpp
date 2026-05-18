#include "MidiSettingsPanel.h"

#include "Organisms/MidiExportDialog.h"
#include "Organisms/MidiImportDialog.h"

namespace sonik::midi::ui
{
    namespace
    {
        constexpr juce::uint32 kBg     = 0xFFFDFDFD;
        constexpr juce::uint32 kBorder = 0xFF2D2D2D;

        constexpr int kRowGap       = 8;
        constexpr int kOuterPad     = 12;
        constexpr int kEmptyHeight  = 80;
        constexpr int kToolbarH     = 44;

        void styleToolbarButton (juce::TextButton& b)
        {
            // DESIGN.md: 2px ink border, fill inversion on press (same palette
            // as LearnButton).
            b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFFFDFDFD));
            b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF2D2D2D));
            b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFF2D2D2D));
            b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xFFFDFDFD));
        }
    }

    //==========================================================================
    // Content
    //==========================================================================
    void MidiSettingsPanel::Content::resized()
    {
        auto bounds = getLocalBounds().reduced (kOuterPad);
        int y = bounds.getY();
        int totalHeight = kOuterPad;
        for (int i = 0; i < getNumChildComponents(); ++i)
        {
            if (auto* c = getChildComponent (i))
            {
                const int h = juce::jmax (60, ((DeviceHeader*) c)->getPreferredHeight());
                c->setBounds (bounds.getX(), y, bounds.getWidth(), h);
                y += h + kRowGap;
                totalHeight += h + kRowGap;
            }
        }
        totalHeight += kOuterPad - kRowGap;

        // If our height assumption is off (e.g. mapping size changed), update.
        if (getNumChildComponents() > 0 && getHeight() != totalHeight)
            setSize (getWidth(), juce::jmax (kEmptyHeight, totalHeight));
    }

    void MidiSettingsPanel::Content::setHeaders (std::vector<std::unique_ptr<DeviceHeader>>& src)
    {
        removeAllChildren();
        for (auto& h : src)
            if (h != nullptr)
                addAndMakeVisible (*h);

        int total = kOuterPad * 2;
        for (auto& h : src)
            if (h != nullptr)
                total += juce::jmax (60, h->getPreferredHeight()) + kRowGap;
        if (! src.empty())
            total -= kRowGap;

        setSize (getWidth() > 0 ? getWidth() : 800,
                 juce::jmax (kEmptyHeight, total));
        resized();
    }

    //==========================================================================
    // MidiSettingsPanel
    //==========================================================================
    MidiSettingsPanel::MidiSettingsPanel (MappingStore&        s,
                                          MidiDeviceManager&   dm,
                                          MidiInboundRouter&   r,
                                          SoftTakeoverManager& st,
                                          juce::String         appVersion)
        : store               (s),
          deviceManager       (dm),
          inboundRouter       (r),
          softTakeoverManager (st)
    {
        juce::ignoreUnused (inboundRouter, softTakeoverManager);

        exportService = std::make_unique<MappingExportService> (store, ioPool, appVersion);
        importService = std::make_unique<MappingImportService> (store,
                                                                store.getMigrationRegistry(),
                                                                ioPool,
                                                                store.getSchemaVersionTarget());

        styleToolbarButton (importButton);
        styleToolbarButton (exportButton);
        importButton.onClick = [this]() { onImportClicked(); };
        exportButton.onClick = [this]() { onExportClicked(); };
        toolbar.addAndMakeVisible (importButton);
        toolbar.addAndMakeVisible (exportButton);
        addAndMakeVisible (toolbar);

        loadErrorBanner.setVisible (false);
        loadErrorBanner.onReload = [this]() { store.reloadUserMappings(); };
        addAndMakeVisible (loadErrorBanner);

        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);

        deviceManager.addDeviceListChangeListener (this);
        store        .addListener                 (this);

        refreshLoadErrorBanner();
        rebuildDeviceList();
    }

    MidiSettingsPanel::~MidiSettingsPanel()
    {
        store        .removeListener                 (this);
        deviceManager.removeDeviceListChangeListener (this);
        ioPool.removeAllJobs (true, 4000);
    }

    //--------------------------------------------------------------------------
    void MidiSettingsPanel::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour (kBg));

        g.setColour (juce::Colour (kBorder));
        g.drawRect (getLocalBounds(), 2);
    }

    void MidiSettingsPanel::resized()
    {
        auto inner = getLocalBounds().reduced (2);

        auto toolbarBounds = inner.removeFromTop (kToolbarH);
        toolbar.setBounds (toolbarBounds);
        {
            auto t = toolbarBounds.reduced (8, 6);
            importButton.setBounds (t.removeFromLeft (110));
            t.removeFromLeft (8);
            exportButton.setBounds (t.removeFromLeft (110));
        }

        const int bannerH = loadErrorBanner.isVisible() ? loadErrorBanner.getPreferredHeight() : 0;
        if (bannerH > 0)
            loadErrorBanner.setBounds (inner.removeFromTop (bannerH));
        else
            loadErrorBanner.setBounds ({});

        viewport.setBounds (inner);
        // Re-fit the content's width to the viewport's interior so child
        // headers stretch horizontally.
        content.setSize (viewport.getMaximumVisibleWidth(), content.getHeight());
        content.resized();
    }

    void MidiSettingsPanel::refreshLoadErrorBanner()
    {
        loadErrorBanner.setErrors (store.getLoadErrors());
        resized();
    }

    //--------------------------------------------------------------------------
    void MidiSettingsPanel::rebuildDeviceList()
    {
        // Collect input devices only — output-only devices have no inbound
        // bindings to configure in Phase 3.
        const auto records = deviceManager.getDevices();

        headers.clear();
        for (const auto& rec : records)
        {
            if (! rec.isInput)
                continue;
            headers.push_back (std::make_unique<DeviceHeader> (rec, store, deviceManager, inboundRouter, softTakeoverManager));
        }

        content.setHeaders (headers);
        resized();
    }

    void MidiSettingsPanel::rebuildOnMessageThread()
    {
        juce::Component::SafePointer<MidiSettingsPanel> safe { this };
        juce::MessageManager::callAsync ([safe]()
        {
            if (safe != nullptr)
                safe->rebuildDeviceList();
        });
    }

    //--------------------------------------------------------------------------
    // DeviceListChangeListener
    //--------------------------------------------------------------------------
    void MidiSettingsPanel::midiDeviceAdded   (std::uint64_t) { rebuildOnMessageThread(); }
    void MidiSettingsPanel::midiDeviceRemoved (std::uint64_t) { rebuildOnMessageThread(); }
    void MidiSettingsPanel::midiDeviceOpened  (std::uint64_t) { rebuildOnMessageThread(); }
    void MidiSettingsPanel::midiDeviceClosed  (std::uint64_t) { rebuildOnMessageThread(); }

    //--------------------------------------------------------------------------
    // MappingStoreListener
    //--------------------------------------------------------------------------
    void MidiSettingsPanel::userProfilesLoaded()
    {
        refreshLoadErrorBanner();
        for (auto& h : headers)
            if (h != nullptr)
                h->refreshFromStore();
    }

    void MidiSettingsPanel::activeMappingChanged (std::uint64_t deviceId)
    {
        for (auto& h : headers)
            if (h != nullptr && h->getDeviceId() == deviceId)
                h->refreshFromStore();
    }

    void MidiSettingsPanel::mappingAdded (juce::String)
    {
        for (auto& h : headers)
            if (h != nullptr)
                h->refreshFromStore();
    }

    void MidiSettingsPanel::mappingRemoved (juce::String)
    {
        for (auto& h : headers)
            if (h != nullptr)
                h->refreshFromStore();
    }

    //--------------------------------------------------------------------------
    // PRD-0050: Import / Export
    //--------------------------------------------------------------------------
    std::uint64_t MidiSettingsPanel::firstActiveInputDeviceId() const
    {
        for (const auto& rec : deviceManager.getDevices())
            if (rec.isInput)
                return rec.deviceId;
        return 0;
    }

    void MidiSettingsPanel::onImportClicked()
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        activeFileChooser = std::make_unique<juce::FileChooser> (
            "Import MIDI Mapping",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
            "*.sonikmidi.json");

        const auto flags = juce::FileBrowserComponent::openMode
                         | juce::FileBrowserComponent::canSelectFiles;

        activeFileChooser->launchAsync (flags,
            [this] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file == juce::File())
                    return;
                launchImportFlow (file);
            });
    }

    void MidiSettingsPanel::onExportClicked()
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        const auto deviceId = firstActiveInputDeviceId();
        if (deviceId == 0)
        {
            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("No device")
                    .withMessage ("Connect a MIDI input device before exporting a mapping.")
                    .withButton ("OK"),
                nullptr);
            return;
        }

        const auto summaries = store.listAvailableMappings (deviceId);
        const auto activeId  = store.getActiveMappingIdForDevice (deviceId);

        auto* dialog = new MidiExportDialog (summaries, activeId);
        dialog->setSize (440, 160);
        dialog->onCancelClicked = [this]()
        {
            if (activeDialogWindow != nullptr)
                activeDialogWindow->exitModalState (0);
        };
        dialog->onExportClicked = [this] (juce::String mappingId)
        {
            if (activeDialogWindow != nullptr)
                activeDialogWindow->exitModalState (0);
            launchExportFlow (mappingId);
        };

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (dialog);
        opts.dialogTitle             = "Export MIDI Mapping";
        opts.dialogBackgroundColour  = juce::Colour (0xFFFDFDFD);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar       = true;
        opts.resizable               = false;
        activeDialogWindow.reset (opts.launchAsync());
    }

    void MidiSettingsPanel::launchExportFlow (const juce::String& mappingId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        const auto mapping = store.getMappingById (mappingId);
        if (mapping == nullptr)
            return;

        const auto stemFromName = MappingStore::sanitiseFilenameStem (
            mapping->displayName.isNotEmpty() ? mapping->displayName : mappingId);
        const auto defaultName  = (stemFromName.isEmpty() ? mappingId : stemFromName)
                                + ".sonikmidi.json";

        activeFileChooser = std::make_unique<juce::FileChooser> (
            "Export MIDI Mapping",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getChildFile (defaultName),
            "*.sonikmidi.json");

        const auto flags = juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectFiles
                         | juce::FileBrowserComponent::warnAboutOverwriting;

        activeFileChooser->launchAsync (flags,
            [this, mappingId] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file == juce::File())
                    return;

                exportService->exportMappingAsync (mappingId, file,
                    [this] (const ExportResult& res)
                    {
                        JUCE_ASSERT_MESSAGE_THREAD;
                        if (res.status == ExportResult::Status::Ok)
                        {
                            juce::AlertWindow::showAsync (
                                juce::MessageBoxOptions()
                                    .withIconType (juce::MessageBoxIconType::InfoIcon)
                                    .withTitle ("Export complete")
                                    .withMessage ("Saved to " + res.destination.getFullPathName())
                                    .withButton ("OK"),
                                nullptr);
                        }
                        else
                        {
                            juce::AlertWindow::showAsync (
                                juce::MessageBoxOptions()
                                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                                    .withTitle ("Export failed")
                                    .withMessage (res.errorDetail)
                                    .withButton ("OK"),
                                nullptr);
                        }
                    });
            });
    }

    void MidiSettingsPanel::launchImportFlow (const juce::File& source)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        importService->prepareImportAsync (source,
            [this] (ImportPrepared prepared)
            {
                JUCE_ASSERT_MESSAGE_THREAD;

                auto* dlg = new MidiImportDialog();
                dlg->setSize (560, 380);

                if (! prepared.ok)
                    dlg->setError (prepared.error);
                else
                    dlg->setPreview (prepared.preview);

                auto previewCopy = std::make_shared<ImportPrepared> (std::move (prepared));

                dlg->onCancelClicked = [this]()
                {
                    if (activeDialogWindow != nullptr)
                        activeDialogWindow->exitModalState (0);
                };

                dlg->onImportClicked = [this, previewCopy]()
                {
                    if (! previewCopy->ok)
                        return;

                    if (activeDialogWindow != nullptr)
                        activeDialogWindow->exitModalState (0);

                    auto finalise = [this, previewCopy] (ConflictResolution resolution,
                                                         juce::String renameStem)
                    {
                        const auto commit = importService->commitImport (*previewCopy,
                                                                         resolution,
                                                                         renameStem);
                        if (commit.status != ImportCommitResult::Status::Ok)
                        {
                            juce::AlertWindow::showAsync (
                                juce::MessageBoxOptions()
                                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                                    .withTitle ("Import failed")
                                    .withMessage (commit.errorDetail)
                                    .withButton ("OK"),
                                nullptr);
                            return;
                        }

                        const auto devId = firstActiveInputDeviceId();
                        if (devId == 0)
                            return;

                        juce::AlertWindow::showAsync (
                            juce::MessageBoxOptions()
                                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                .withTitle ("Activate now?")
                                .withMessage ("Imported '" + commit.finalMappingId
                                               + "'. Activate for the connected MIDI device?")
                                .withButton ("Yes")
                                .withButton ("No"),
                            [this, devId, id = commit.finalMappingId] (int result)
                            {
                                if (result == 1)
                                    store.setActiveMapping (devId, id);
                            });
                    };

                    if (! previewCopy->preview.conflictDetected)
                    {
                        finalise (ConflictResolution::None, {});
                        return;
                    }

                    auto* alert = new juce::AlertWindow ("Mapping conflict",
                        "A mapping named '" + previewCopy->preview.conflictExistingMappingId
                        + "' already exists. Choose Rename, Replace, or Cancel.",
                        juce::MessageBoxIconType::QuestionIcon);
                    alert->addTextEditor ("renameTo", previewCopy->preview.mappingId, "New stem:");
                    alert->addButton ("Rename",  1);
                    alert->addButton ("Replace", 2);
                    alert->addButton ("Cancel",  0);
                    alert->enterModalState (true,
                        juce::ModalCallbackFunction::create (
                            [alert, finalise] (int chosen)
                            {
                                ConflictResolution resolution = ConflictResolution::None;
                                juce::String       renameStem;
                                switch (chosen)
                                {
                                    case 1:
                                        resolution = ConflictResolution::RenameTo;
                                        renameStem = alert->getTextEditorContents ("renameTo");
                                        break;
                                    case 2:
                                        resolution = ConflictResolution::Replace;
                                        break;
                                    default:
                                        return;
                                }
                                finalise (resolution, renameStem);
                            }),
                        /*deleteWhenDismissed*/ true);
                };

                juce::DialogWindow::LaunchOptions opts;
                opts.content.setOwned (dlg);
                opts.dialogTitle             = "Import MIDI Mapping";
                opts.dialogBackgroundColour  = juce::Colour (0xFFFDFDFD);
                opts.escapeKeyTriggersCloseButton = true;
                opts.useNativeTitleBar       = true;
                opts.resizable               = false;
                activeDialogWindow.reset (opts.launchAsync());
            });
    }
}
