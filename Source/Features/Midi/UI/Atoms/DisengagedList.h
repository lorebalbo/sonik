#pragma once
//==============================================================================
// PRD-0048 Atom: DisengagedList
//
// Per-device live list of soft-takeover bindings currently in the
// `Disengaged` state.  Subscribes to `SoftTakeoverManager` for transitions,
// renders one row per disengaged target with a "FORCE ENGAGE" button.
//
// Click -> `SoftTakeoverManager::forceEngage(deviceId, target, 0.5, 0.5)`.
// The hardware/software value pair is a neutral placeholder: `forceEngage`
// unconditionally sets the entry to `Engaged`, so the values are only used
// to seed the cached `lastHardware`/`lastSoftware` fields for future delta
// checks — they will be overwritten on the very next inbound MIDI event.
//
// Styling per DESIGN.md: monochrome #FDFDFD bg, 2px ink border, Space Mono.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../ControlTargetRegistry.h"
#include "../../MappingStore.h"
#include "../../MappingTypes.h"
#include "../../SoftTakeoverManager.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace sonik::midi::ui
{
    class DisengagedList final : public juce::Component,
                                 private SoftTakeoverManagerListener
    {
    public:
        DisengagedList (std::uint64_t        deviceIdIn,
                        SoftTakeoverManager& softTakeoverIn,
                        MappingStore&        storeIn)
            : deviceId     (deviceIdIn),
              softTakeover (softTakeoverIn),
              store        (storeIn)
        {
            softTakeover.addListener (this);
            refresh();
        }

        ~DisengagedList() override
        {
            softTakeover.removeListener (this);
        }

        /** Called by the parent when the active mapping changes. Re-seeds
            the list by querying `getState` for every Pickup/Always binding
            in the new mapping. */
        void refresh()
        {
            disengagedTargets.clear();
            if (auto mapping = store.getActiveMappingForDevice (deviceId))
            {
                for (const auto& b : mapping->bindings)
                {
                    if (b.target == InvalidTargetIndex) continue;
                    if (b.softTakeover == SoftTakeoverPolicy::Never) continue;
                    if (softTakeover.getState (deviceId, b.target)
                        == TakeoverState::Disengaged)
                    {
                        // de-dup (a single target may appear in multiple
                        // bindings if mapped via different modifier layers).
                        if (std::find (disengagedTargets.begin(),
                                       disengagedTargets.end(), b.target)
                            == disengagedTargets.end())
                            disengagedTargets.push_back (b.target);
                    }
                }
            }
            rebuildRows();
        }

        int getPreferredHeight() const noexcept
        {
            if (disengagedTargets.empty()) return 0;
            return kHeader + kRowHeight * (int) disengagedTargets.size() + 4;
        }

        void paint (juce::Graphics& g) override
        {
            if (disengagedTargets.empty()) return;
            g.fillAll (juce::Colour (0xFFFDFDFD));
            g.setColour (juce::Colour (0xFF2D2D2D));
            g.drawRect (getLocalBounds(), 2);

            g.setFont (juce::Font (juce::FontOptions { "Space Mono", 11.0f, juce::Font::plain }));
            g.drawText ("DISENGAGED (SOFT-TAKEOVER) — " + juce::String ((int) disengagedTargets.size()),
                        getLocalBounds().reduced (8, 2).withHeight (kHeader),
                        juce::Justification::centredLeft, true);
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced (8, 4);
            bounds.removeFromTop (kHeader);
            for (auto& row : rows)
            {
                auto rb = bounds.removeFromTop (kRowHeight);
                if (row.button != nullptr)
                {
                    auto btn = rb.removeFromRight (110);
                    btn.removeFromRight (2);
                    row.button->setBounds (btn.reduced (0, 2));
                }
                if (row.label != nullptr)
                    row.label->setBounds (rb.reduced (2, 0));
            }
        }

    private:
        void takeoverStateChanged (std::uint64_t devId,
                                   TargetIndex   target,
                                   TakeoverState newState) override
        {
            if (devId != deviceId) return;

            const auto it = std::find (disengagedTargets.begin(),
                                       disengagedTargets.end(), target);
            if (newState == TakeoverState::Disengaged)
            {
                if (it == disengagedTargets.end())
                    disengagedTargets.push_back (target);
                else return;
            }
            else
            {
                if (it == disengagedTargets.end()) return;
                disengagedTargets.erase (it);
            }
            rebuildRows();

            if (auto* parent = getParentComponent())
                parent->resized();
            repaint();
        }

        struct Row
        {
            std::unique_ptr<juce::Label>     label;
            std::unique_ptr<juce::TextButton> button;
        };

        void rebuildRows()
        {
            rows.clear();
            for (auto target : disengagedTargets)
            {
                Row row;

                row.label = std::make_unique<juce::Label>();
                const juce::String idStr = ((std::size_t) target < ControlTargetRegistry::size())
                                           ? juce::String (ControlTargetRegistry::get (target).id)
                                           : "(unknown)";
                row.label->setText (idStr, juce::dontSendNotification);
                row.label->setFont (juce::FontOptions ("Space Mono", 11.0f, juce::Font::plain));
                row.label->setColour (juce::Label::textColourId, juce::Colour (0xFF2D2D2D));
                addAndMakeVisible (*row.label);

                row.button = std::make_unique<juce::TextButton>();
                row.button->setButtonText ("FORCE ENGAGE");
                row.button->setColour (juce::TextButton::buttonColourId,    juce::Colour (0xFFFDFDFD));
                row.button->setColour (juce::TextButton::buttonOnColourId,  juce::Colour (0xFF2D2D2D));
                row.button->setColour (juce::TextButton::textColourOffId,   juce::Colour (0xFF2D2D2D));
                row.button->setColour (juce::TextButton::textColourOnId,    juce::Colour (0xFFFDFDFD));
                row.button->setColour (juce::ComboBox::outlineColourId,     juce::Colour (0xFF2D2D2D));
                const auto tgt = target;
                row.button->onClick = [this, tgt]()
                {
                    // Neutral seed values; forceEngage transitions state to
                    // Engaged unconditionally.  Subsequent MIDI events will
                    // overwrite lastHardwareValue with the real hardware
                    // position before any delta check is performed.
                    softTakeover.forceEngage (deviceId, tgt, 0.5f, 0.5f);
                };
                addAndMakeVisible (*row.button);

                rows.push_back (std::move (row));
            }
            resized();
            repaint();
        }

        static constexpr int kHeader    = 18;
        static constexpr int kRowHeight = 22;

        std::uint64_t        deviceId;
        SoftTakeoverManager& softTakeover;
        MappingStore&        store;

        std::vector<TargetIndex> disengagedTargets;
        std::vector<Row>         rows;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisengagedList)
    };
}
