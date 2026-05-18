#pragma once
//==============================================================================
// PRD-0048 Molecule: TargetPicker
//
// Searchable popover for selecting a `TargetIndex` from the
// `ControlTargetRegistry` (PRD-0042). Presented inside a `juce::CallOutBox`
// when the TARGET cell of an editable binding row is clicked.
//
// Layout:
//   ┌───────────────────────────────────┐
//   │ SEARCH: ____________________      │  <- juce::TextEditor
//   ├───────────────────────────────────┤
//   │ deck.A.transport.play             │  <- juce::ListBox, full-text
//   │ deck.A.transport.cue              │     filtered by the search text
//   │ deck.A.eq.high                    │     (case-insensitive substring).
//   │ ...                               │
//   └───────────────────────────────────┘
//
// Rows are sorted alphabetically by registry id (which naturally groups by
// feature prefix: `deck.A.*`, `deck.B.*`, `library.*`, `mixer.*`, ...).
// Picking a row fires `onPicked(TargetIndex)` and dismisses the call-out
// (`juce::CallOutBox::dismiss()` walks the parent chain).
//
// Styling per DESIGN.md: monochrome #FDFDFD / #2D2D2D, Space Mono, 2px
// borders, zero border-radius, inverted-fill selection state.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../ControlTargetRegistry.h"
#include "../../MappingTypes.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace sonik::midi::ui
{
    class TargetPicker final : public juce::Component,
                               private juce::ListBoxModel,
                               private juce::TextEditor::Listener
    {
    public:
        TargetPicker()
        {
            // Build sorted index of all registry entries.
            allIndices.reserve (ControlTargetRegistry::size());
            for (std::size_t i = 0; i < ControlTargetRegistry::size(); ++i)
                allIndices.push_back (static_cast<TargetIndex> (i));

            std::sort (allIndices.begin(), allIndices.end(),
                       [] (TargetIndex a, TargetIndex b)
                       {
                           return std::string_view (ControlTargetRegistry::get (a).id)
                                < std::string_view (ControlTargetRegistry::get (b).id);
                       });

            visibleIndices = allIndices;

            searchBox.setFont (juce::FontOptions ("Space Mono", 12.0f, juce::Font::plain));
            searchBox.setTextToShowWhenEmpty ("search target...", juce::Colour (0x802D2D2D));
            searchBox.setColour (juce::TextEditor::backgroundColourId,  juce::Colour (0xFFFDFDFD));
            searchBox.setColour (juce::TextEditor::textColourId,        juce::Colour (0xFF2D2D2D));
            searchBox.setColour (juce::TextEditor::outlineColourId,     juce::Colour (0xFF2D2D2D));
            searchBox.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xFF2D2D2D));
            searchBox.addListener (this);
            addAndMakeVisible (searchBox);

            list.setModel (this);
            list.setRowHeight (22);
            list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xFFFDFDFD));
            list.setColour (juce::ListBox::outlineColourId,    juce::Colour (0xFF2D2D2D));
            list.setOutlineThickness (2);
            addAndMakeVisible (list);

            setSize (380, 360);
        }

        std::function<void (TargetIndex)> onPicked;

        void resized() override
        {
            auto bounds = getLocalBounds().reduced (8);
            searchBox.setBounds (bounds.removeFromTop (24));
            bounds.removeFromTop (6);
            list.setBounds (bounds);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xFFFDFDFD));
            g.setColour (juce::Colour (0xFF2D2D2D));
            g.drawRect (getLocalBounds(), 2);
        }

    private:
        // ---- juce::ListBoxModel ------------------------------------------
        int getNumRows() override { return (int) visibleIndices.size(); }

        void paintListBoxItem (int rowNumber, juce::Graphics& g,
                               int width, int height, bool rowIsSelected) override
        {
            if (rowNumber < 0 || rowNumber >= (int) visibleIndices.size()) return;

            if (rowIsSelected)
            {
                g.fillAll (juce::Colour (0xFF2D2D2D));
                g.setColour (juce::Colour (0xFFFDFDFD));
            }
            else
            {
                g.setColour (juce::Colour (0xFF2D2D2D));
            }

            const auto idx = visibleIndices[(std::size_t) rowNumber];
            g.setFont (juce::Font (juce::FontOptions ("Space Mono", 11.5f, juce::Font::plain)));
            g.drawText (juce::String::fromUTF8 (ControlTargetRegistry::get (idx).id),
                        4, 0, width - 8, height,
                        juce::Justification::centredLeft, true);
        }

        void listBoxItemClicked (int rowNumber, const juce::MouseEvent&) override
        {
            commitRow (rowNumber);
        }

        void returnKeyPressed (int lastRowSelected) override
        {
            commitRow (lastRowSelected);
        }

        // ---- juce::TextEditor::Listener ----------------------------------
        void textEditorTextChanged (juce::TextEditor& ed) override
        {
            if (&ed != &searchBox) return;
            applyFilter (ed.getText().trim().toLowerCase());
        }

        void textEditorReturnKeyPressed (juce::TextEditor&) override
        {
            if (! visibleIndices.empty())
                commitRow (0);
        }

        // ---- helpers -----------------------------------------------------
        void applyFilter (const juce::String& needle)
        {
            visibleIndices.clear();
            if (needle.isEmpty())
            {
                visibleIndices = allIndices;
            }
            else
            {
                const auto needleRaw = needle.toRawUTF8();
                for (auto idx : allIndices)
                {
                    const juce::String id (juce::CharPointer_UTF8 (
                        ControlTargetRegistry::get (idx).id));
                    if (id.toLowerCase().contains (needleRaw))
                        visibleIndices.push_back (idx);
                }
            }
            list.updateContent();
            list.selectRow (visibleIndices.empty() ? -1 : 0);
        }

        void commitRow (int rowNumber)
        {
            if (rowNumber < 0 || rowNumber >= (int) visibleIndices.size()) return;
            const auto idx = visibleIndices[(std::size_t) rowNumber];
            const auto cb  = onPicked;
            // Dismiss the surrounding CallOutBox (if any) before firing the
            // callback so the parent can safely re-layout / repaint.
            if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
                callout->dismiss();
            if (cb) cb (idx);
        }

        juce::TextEditor   searchBox;
        juce::ListBox      list;

        std::vector<TargetIndex> allIndices;
        std::vector<TargetIndex> visibleIndices;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TargetPicker)
    };
}
