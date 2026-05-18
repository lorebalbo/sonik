#pragma once
//==============================================================================
// PRD-0050 Organism: MidiExportDialog
//
// Small modal showing a dropdown of mappings exportable for a given device
// (defaulting to the currently-active mapping) and Export / Cancel buttons.
//
// Justification for keeping this as a separate dialog rather than an inline
// toolbar control: the export dropdown is only relevant during an active
// export action; embedding it in the always-visible toolbar would compete
// with the per-device ProfileDropdown that already lives inside each
// DeviceHeader. A short-lived modal keeps the device-list panel uncluttered.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../MappingStore.h"

namespace sonik::midi::ui
{
    class MidiExportDialog final : public juce::Component
    {
    public:
        // Invoked with the selected mappingId on Export click.
        std::function<void (juce::String /*mappingId*/)> onExportClicked;
        std::function<void()>                            onCancelClicked;

        MidiExportDialog (const std::vector<MappingProfileSummary>& summaries,
                          const juce::String& defaultMappingId);

        void paint   (juce::Graphics&) override;
        void resized() override;

    private:
        juce::Label      title;
        juce::ComboBox   chooser;
        juce::TextButton exportButton { "Export" };
        juce::TextButton cancelButton { "Cancel" };

        std::vector<MappingProfileSummary> entries;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiExportDialog)
    };
}
