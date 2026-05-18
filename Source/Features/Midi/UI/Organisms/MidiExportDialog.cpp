#include "MidiExportDialog.h"

namespace sonik::midi::ui
{
    namespace
    {
        constexpr juce::uint32 kLight = 0xFFFDFDFD;
        constexpr juce::uint32 kInk   = 0xFF2D2D2D;

        void styleButton (juce::TextButton& b)
        {
            b.setColour (juce::TextButton::buttonColourId,   juce::Colour (kLight));
            b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (kInk));
            b.setColour (juce::TextButton::textColourOffId,  juce::Colour (kInk));
            b.setColour (juce::TextButton::textColourOnId,   juce::Colour (kLight));
        }
    }

    MidiExportDialog::MidiExportDialog (const std::vector<MappingProfileSummary>& summaries,
                                        const juce::String& defaultMappingId)
        : entries (summaries)
    {
        title.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 16.0f, juce::Font::plain));
        title.setColour (juce::Label::textColourId, juce::Colour (kInk));
        title.setText ("Export MIDI Mapping", juce::dontSendNotification);
        addAndMakeVisible (title);

        chooser.setColour (juce::ComboBox::backgroundColourId, juce::Colour (kLight));
        chooser.setColour (juce::ComboBox::textColourId,       juce::Colour (kInk));
        chooser.setColour (juce::ComboBox::outlineColourId,    juce::Colour (kInk));
        chooser.setColour (juce::ComboBox::arrowColourId,      juce::Colour (kInk));
        chooser.setColour (juce::ComboBox::buttonColourId,     juce::Colour (kLight));
        addAndMakeVisible (chooser);

        for (size_t i = 0; i < entries.size(); ++i)
        {
            juce::String label = entries[i].displayName.isNotEmpty()
                                     ? entries[i].displayName : entries[i].id;
            if (entries[i].origin == MappingOrigin::Bundled)
                label << "  (bundled)";
            chooser.addItem (label, static_cast<int> (i) + 1);
        }

        if (defaultMappingId.isNotEmpty())
        {
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].id == defaultMappingId)
                {
                    chooser.setSelectedId (static_cast<int> (i) + 1, juce::dontSendNotification);
                    break;
                }
        }
        if (chooser.getSelectedId() == 0 && ! entries.empty())
            chooser.setSelectedId (1, juce::dontSendNotification);

        styleButton (exportButton);
        styleButton (cancelButton);

        exportButton.onClick = [this]()
        {
            const int sel = chooser.getSelectedId();
            if (sel <= 0 || static_cast<size_t> (sel - 1) >= entries.size())
                return;
            if (onExportClicked)
                onExportClicked (entries[static_cast<size_t> (sel - 1)].id);
        };
        cancelButton.onClick = [this]() { if (onCancelClicked) onCancelClicked(); };

        addAndMakeVisible (exportButton);
        addAndMakeVisible (cancelButton);
    }

    void MidiExportDialog::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour (kLight));
        g.setColour (juce::Colour (kInk));
        g.drawRect (getLocalBounds(), 2);
    }

    void MidiExportDialog::resized()
    {
        auto bounds = getLocalBounds().reduced (16);
        title.setBounds (bounds.removeFromTop (28));
        bounds.removeFromTop (12);
        chooser.setBounds (bounds.removeFromTop (32));
        bounds.removeFromTop (16);

        auto buttons = bounds.removeFromBottom (36);
        cancelButton.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (8);
        exportButton.setBounds (buttons.removeFromRight (110));
    }
}
