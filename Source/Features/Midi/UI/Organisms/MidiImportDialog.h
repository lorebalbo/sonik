#pragma once
//==============================================================================
// PRD-0050 Organism: MidiImportDialog
//
// Modal preview pane shown after `MappingImportService::prepareImportAsync`
// returns successfully (stages 1-7). Renders the preview metadata, the
// stage-7 unknown-targets warning (if any), and Import / Cancel actions.
// Conflict resolution (stage 6) is handled by the parent panel through a
// JUCE AlertWindow before this dialog is dismissed.
//
// DESIGN.md compliance:
//   * monochrome palette: light fill 0xFFFDFDFD, ink 0xFF2D2D2D.
//   * 2px borders, zero rounded corners.
//   * Space Mono font (set via LookAndFeel for the embedded labels).
//   * Tonal layering for depth (mid-tone for the metadata band).
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../Bundle/ImportPipeline.h"

namespace sonik::midi::ui
{
    class MidiImportDialog final : public juce::Component
    {
    public:
        std::function<void()> onImportClicked;
        std::function<void()> onCancelClicked;

        MidiImportDialog();

        void setPreview (const ImportPreview& preview);
        void setError   (const ImportError& error);

        void paint   (juce::Graphics&) override;
        void resized() override;

    private:
        void layoutRows();

        juce::Label       title;
        juce::Label       errorBanner;
        juce::Label       metadata;
        juce::Label       warningLabel;
        juce::TextButton  importButton  { "Import" };
        juce::TextButton  cancelButton  { "Cancel" };

        bool              hasError { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiImportDialog)
    };
}
