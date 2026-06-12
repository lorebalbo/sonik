#include "MidiImportDialog.h"

namespace sonik::midi::ui
{
    namespace
    {
        constexpr juce::uint32 kLight     = 0xFFFDFDFD;
        constexpr juce::uint32 kInk       = 0xFF2D2D2D;
        constexpr juce::uint32 kMidTone   = 0xFFE6E6E6;
        constexpr juce::uint32 kErrorBg   = 0xFFFDFDFD;

        juce::String stageLabel (ImportStage s)
        {
            switch (s)
            {
                case ImportStage::JsonParse:        return "JSON parse";
                case ImportStage::ManifestExtract:  return "Manifest";
                case ImportStage::Sha256Verify:     return "Integrity (SHA-256)";
                case ImportStage::SchemaMigrate:    return "Schema migration";
                case ImportStage::MappingParse:     return "Mapping parse";
                case ImportStage::ConflictDetect:   return "Conflict";
                case ImportStage::TargetIdValidate: return "Target validation";
            }
            return "Unknown";
        }

        void styleLabel (juce::Label& l, float fontSize, juce::Justification just = juce::Justification::topLeft)
        {
            l.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), fontSize, juce::Font::plain)));
            l.setColour (juce::Label::textColourId,       juce::Colour (kInk));
            l.setColour (juce::Label::backgroundColourId, juce::Colour (0x00000000));
            l.setJustificationType (just);
        }

        void styleButton (juce::TextButton& b)
        {
            b.setColour (juce::TextButton::buttonColourId,   juce::Colour (kLight));
            b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (kInk));
            b.setColour (juce::TextButton::textColourOffId,  juce::Colour (kInk));
            b.setColour (juce::TextButton::textColourOnId,   juce::Colour (kLight));
        }
    }

    //--------------------------------------------------------------------------
    MidiImportDialog::MidiImportDialog()
    {
        styleLabel (title,        16.0f, juce::Justification::topLeft);
        styleLabel (errorBanner,  12.0f);
        styleLabel (metadata,     12.0f);
        styleLabel (warningLabel, 12.0f);

        title.setText ("Import MIDI Mapping", juce::dontSendNotification);
        errorBanner.setColour (juce::Label::textColourId, juce::Colour (kInk));
        errorBanner.setColour (juce::Label::backgroundColourId, juce::Colour (kErrorBg));

        styleButton (importButton);
        styleButton (cancelButton);

        importButton.onClick = [this]() { if (onImportClicked) onImportClicked(); };
        cancelButton.onClick = [this]() { if (onCancelClicked) onCancelClicked(); };

        addAndMakeVisible (title);
        addAndMakeVisible (metadata);
        addAndMakeVisible (warningLabel);
        addAndMakeVisible (errorBanner);
        addAndMakeVisible (importButton);
        addAndMakeVisible (cancelButton);

        errorBanner.setVisible (false);
        warningLabel.setVisible (false);
        importButton.setEnabled (false);
    }

    //--------------------------------------------------------------------------
    void MidiImportDialog::setPreview (const ImportPreview& p)
    {
        hasError = false;
        errorBanner.setVisible (false);

        juce::String md;
        md << "Device:        " << p.deviceMatchDisplay << juce::newLine
           << "Mapping name:  " << p.mappingName        << juce::newLine
           << "Schema:        v" << juce::String (p.schemaVersion) << juce::newLine
           << "Bindings:      " << juce::String (p.bindingCount)   << juce::newLine
           << "Modifiers:     " << juce::String (p.modifierCount)  << juce::newLine
           << "Exported at:   " << p.exportedAtIso8601 << juce::newLine
           << "App version:   " << p.exporterAppVersion;

        if (p.exporterDeviceName.isNotEmpty())
            md << juce::newLine << "From host:     " << p.exporterDeviceName;

        if (p.migrationStepsApplied > 0)
            md << juce::newLine
               << "Migration:     " << juce::String (p.migrationStepsApplied) << " step(s) applied";

        if (p.conflictDetected)
            md << juce::newLine
               << "Conflict:      a user mapping named '" << p.conflictExistingMappingId << "' already exists";

        metadata.setText (md, juce::dontSendNotification);

        if (! p.unknownTargetIds.isEmpty())
        {
            juce::String w;
            w << p.unknownTargetIds.size()
              << " binding(s) reference unknown control targets and will be skipped on load:"
              << juce::newLine << "  " << p.unknownTargetIds.joinIntoString (", ");
            warningLabel.setText (w, juce::dontSendNotification);
            warningLabel.setVisible (true);
        }
        else
        {
            warningLabel.setVisible (false);
        }

        importButton.setEnabled (true);
        resized();
        repaint();
    }

    //--------------------------------------------------------------------------
    void MidiImportDialog::setError (const ImportError& e)
    {
        hasError = true;
        importButton.setEnabled (false);
        warningLabel.setVisible (false);
        metadata.setText ({}, juce::dontSendNotification);

        juce::String msg;
        msg << "Import failed at stage: " << stageLabel (e.stage) << juce::newLine
            << "Reason: " << e.reason;

        if (e.parserErrorDetail.isNotEmpty())
            msg << juce::newLine << "Detail: " << e.parserErrorDetail;
        if (e.missingManifestField.isNotEmpty())
            msg << juce::newLine << "Missing field: " << e.missingManifestField;
        if (e.expectedSha256.isNotEmpty())
            msg << juce::newLine
                << "Expected: " << e.expectedSha256 << juce::newLine
                << "Computed: " << e.computedSha256;
        if (e.migrationError.has_value())
            msg << juce::newLine
                << "Migration error at v" << juce::String (e.migrationError->atVersion)
                << ": " << e.migrationError->reason;

        errorBanner.setText (msg, juce::dontSendNotification);
        errorBanner.setVisible (true);
        resized();
        repaint();
    }

    //--------------------------------------------------------------------------
    void MidiImportDialog::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour (kLight));
        g.setColour (juce::Colour (kInk));
        g.drawRect (getLocalBounds(), 2);

        if (metadata.isVisible() && metadata.getText().isNotEmpty())
        {
            g.setColour (juce::Colour (kMidTone));
            g.fillRect (metadata.getBounds().expanded (4, 4));
            g.setColour (juce::Colour (kInk));
            g.drawRect (metadata.getBounds().expanded (4, 4), 2);
        }
    }

    void MidiImportDialog::resized() { layoutRows(); }

    void MidiImportDialog::layoutRows()
    {
        auto bounds = getLocalBounds().reduced (16);

        title.setBounds (bounds.removeFromTop (28));
        bounds.removeFromTop (8);

        if (hasError)
        {
            errorBanner.setBounds (bounds.removeFromTop (110));
            bounds.removeFromTop (12);
        }
        else
        {
            metadata.setBounds (bounds.removeFromTop (180).reduced (4, 4));
            bounds.removeFromTop (12);

            if (warningLabel.isVisible())
            {
                warningLabel.setBounds (bounds.removeFromTop (50));
                bounds.removeFromTop (8);
            }
        }

        auto buttonsRow = bounds.removeFromBottom (36);
        cancelButton.setBounds (buttonsRow.removeFromRight (110));
        buttonsRow.removeFromRight (8);
        importButton.setBounds (buttonsRow.removeFromRight (110));
    }
}
