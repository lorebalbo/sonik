#pragma once
//==============================================================================
// PRD-0048 Atom: ProfileDropdown
//
// A juce::ComboBox styled per DESIGN.md (2px border, monochrome, Space Mono)
// populated with `MappingProfileSummary` entries for a single device.  Bundled
// profiles are flagged "(bundled, read-only)" in the displayed label; the
// underlying item id is the index into the summaries vector + 1 so the caller
// can map a selection back via `getSelectedProfile()`.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../MappingStore.h"

namespace sonik::midi::ui
{
    class ProfileDropdown final : public juce::ComboBox
    {
    public:
        ProfileDropdown()
        {
            setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xFFFDFDFD));
            setColour (juce::ComboBox::textColourId,       juce::Colour (0xFF2D2D2D));
            setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xFF2D2D2D));
            setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xFF2D2D2D));
            setColour (juce::ComboBox::buttonColourId,     juce::Colour (0xFFFDFDFD));
        }

        /** Populate the dropdown with the supplied summaries, attempting to
            preserve the current selection by `id`.  If `activeId` is non-empty
            and present in `entries`, it is preselected. */
        void setProfiles (const std::vector<MappingProfileSummary>& entries,
                          const juce::String&                       activeId)
        {
            summaries = entries;
            clear (juce::dontSendNotification);

            for (size_t i = 0; i < summaries.size(); ++i)
            {
                const auto& s = summaries[i];
                juce::String label = s.displayName.isNotEmpty() ? s.displayName : s.id;
                if (s.origin == MappingOrigin::Bundled)
                    label << " (bundled, read-only)";

                addItem (label, static_cast<int> (i) + 1);
            }

            if (activeId.isNotEmpty())
            {
                for (size_t i = 0; i < summaries.size(); ++i)
                {
                    if (summaries[i].id == activeId)
                    {
                        setSelectedId (static_cast<int> (i) + 1,
                                       juce::dontSendNotification);
                        return;
                    }
                }
            }
        }

        /** Returns the currently-selected summary, or nullopt if nothing is
            selected. */
        const MappingProfileSummary* getSelectedProfile() const noexcept
        {
            const int id = getSelectedId();
            if (id <= 0 || static_cast<size_t> (id - 1) >= summaries.size())
                return nullptr;
            return &summaries[static_cast<size_t> (id - 1)];
        }

    private:
        std::vector<MappingProfileSummary> summaries;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProfileDropdown)
    };
}
