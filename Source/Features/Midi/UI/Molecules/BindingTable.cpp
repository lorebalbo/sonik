//==============================================================================
// PRD-0048 Molecule: BindingTable — Phases 4 (read-only) + 5 (Learn) + 6 (edit)
//==============================================================================

#include "BindingTable.h"

#include "../../ControlTargetRegistry.h"
#include "../../MidiInboundEvent.h"
#include "TargetPicker.h"

#include <array>
#include <memory>

namespace sonik::midi::ui
{
    namespace
    {
        constexpr juce::uint32 kBg     = 0xFFFDFDFD;
        constexpr juce::uint32 kFg     = 0xFF2D2D2D;
        constexpr juce::uint32 kBorder = 0xFF2D2D2D;
        constexpr juce::uint32 kMuted  = 0xFF6E6E6E;

        constexpr int kHeaderHeight = 24;
        constexpr int kRowHeight    = 30; // grew from 26 to host ComboBoxes
        constexpr int kHPad         = 8;
        constexpr int kCellInset    = 4;

        constexpr int kLearnSeconds = 10;

        struct ColumnSpec { const char* title; float weight; };

        constexpr ColumnSpec kColumns[] = {
            { "TARGET",        0.23f },
            { "MIDI KEY",      0.14f },
            { "MODIFIER",      0.10f },
            { "TRANSFORM",     0.15f },
            { "SOFT-TAKEOVER", 0.13f },
            { "FEEDBACK",      0.10f },
            { "ACTIONS",       0.15f },
        };
        constexpr int kNumColumns        = (int) (sizeof (kColumns) / sizeof (kColumns[0]));
        constexpr int kColTransform      = 3;
        constexpr int kColSoftTakeover   = 4;
        constexpr int kColActions        = 6;

        std::array<int, kNumColumns> computeColumnWidths (int totalWidth)
        {
            std::array<int, kNumColumns> w {};
            int used = 0;
            for (int i = 0; i < kNumColumns - 1; ++i)
            {
                w[(size_t) i] = juce::roundToInt ((float) totalWidth * kColumns[(size_t) i].weight);
                used += w[(size_t) i];
            }
            w[(size_t) (kNumColumns - 1)] = juce::jmax (0, totalWidth - used);
            return w;
        }

        juce::String formatTarget (TargetIndex idx)
        {
            if (idx == InvalidTargetIndex || idx >= ControlTargetRegistry::size())
                return "(invalid)";
            return juce::String::fromUTF8 (ControlTargetRegistry::get (idx).id);
        }

        juce::String formatMidiKey (std::uint32_t midiKey, std::uint8_t lsbData1)
        {
            const auto channel = static_cast<std::uint8_t> ((midiKey >> 16) & 0xFFu);
            const auto status  = static_cast<std::uint8_t> ((midiKey >> 8)  & 0xFFu);
            const auto data1   = static_cast<std::uint8_t> ( midiKey        & 0xFFu);

            const char* kind = "?";
            switch (status & 0xF0u)
            {
                case 0x90: kind = "Note"; break;
                case 0xB0: kind = "CC";   break;
                case 0xE0: kind = "PB";   break;
                default:   break;
            }

            juce::String s;
            s << "Ch " << (int) channel << " " << kind << " " << (int) data1;
            if (lsbData1 != 0xFFu)
                s << "+" << (int) lsbData1;
            return s;
        }

        juce::String formatModifier (std::uint32_t mask,
                                     const std::vector<juce::String>& names)
        {
            if (mask == 0)
                return "(none)";
            juce::StringArray parts;
            for (int bit = 0; bit < 32; ++bit)
            {
                if ((mask & (1u << bit)) == 0)
                    continue;
                if ((size_t) bit < names.size() && names[(size_t) bit].isNotEmpty())
                    parts.add (names[(size_t) bit]);
                else
                    parts.add ("bit" + juce::String (bit));
            }
            return parts.joinIntoString ("+");
        }

        juce::String formatTransform (Transform t)
        {
            switch (t)
            {
                case Transform::Momentary:           return "Momentary";
                case Transform::Toggle:              return "Toggle";
                case Transform::Linear:              return "Linear";
                case Transform::Linear14:            return "Linear14";
                case Transform::SignedBitDelta:      return "SignedBitDelta";
                case Transform::TwosComplementDelta: return "TwosComplementDelta";
            }
            return "?";
        }

        juce::String formatSoftTakeover (SoftTakeoverPolicy p)
        {
            switch (p)
            {
                case SoftTakeoverPolicy::Pickup: return "Pickup";
                case SoftTakeoverPolicy::Always: return "Always";
                case SoftTakeoverPolicy::Never:  return "Never";
            }
            return "?";
        }

        juce::String formatFeedback (const BindingFeedback& fb)
        {
            if (fb.midiKey == 0)
                return "(none)";
            switch (fb.style)
            {
                case FeedbackStyle::None:       return "(none)";
                case FeedbackStyle::Binary:     return "Binary LED";
                case FeedbackStyle::Colour:     return "Colour LED";
                case FeedbackStyle::Continuous: return "Continuous";
            }
            return "?";
        }

        void drawCellText (juce::Graphics& g,
                           const juce::String& text,
                           juce::Rectangle<int> bounds,
                           juce::Colour colour,
                           juce::Justification just = juce::Justification::centredLeft)
        {
            g.setColour (colour);
            g.drawText (text, bounds.reduced (kHPad, 0), just, true);
        }

        void styleMonochromeButton (juce::TextButton& b)
        {
            b.setColour (juce::TextButton::buttonColourId,   juce::Colour (kBg));
            b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (kFg));
            b.setColour (juce::TextButton::textColourOnId,   juce::Colour (kBg));
            b.setColour (juce::TextButton::textColourOffId,  juce::Colour (kFg));
        }

        void styleMonochromeCombo (juce::ComboBox& c)
        {
            c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (kBg));
            c.setColour (juce::ComboBox::outlineColourId,    juce::Colour (kFg));
            c.setColour (juce::ComboBox::textColourId,       juce::Colour (kFg));
            c.setColour (juce::ComboBox::arrowColourId,      juce::Colour (kFg));
            c.setColour (juce::ComboBox::buttonColourId,     juce::Colour (kBg));
        }

        struct TransformOption { Transform value; const char* label; };
        constexpr TransformOption kTransformOptions[] = {
            { Transform::Momentary,           "Momentary"           },
            { Transform::Toggle,              "Toggle"              },
            { Transform::Linear,              "Linear"              },
            { Transform::Linear14,            "Linear14"            },
            { Transform::SignedBitDelta,      "SignedBitDelta"      },
            { Transform::TwosComplementDelta, "TwosComplementDelta" },
        };
        constexpr int kNumTransformOptions =
            (int) (sizeof (kTransformOptions) / sizeof (kTransformOptions[0]));

        struct SoftTakeoverOption { SoftTakeoverPolicy value; const char* label; };
        constexpr SoftTakeoverOption kSoftTakeoverOptions[] = {
            { SoftTakeoverPolicy::Pickup, "Pickup" },
            { SoftTakeoverPolicy::Always, "Always" },
            { SoftTakeoverPolicy::Never,  "Never"  },
        };
        constexpr int kNumSoftTakeoverOptions =
            (int) (sizeof (kSoftTakeoverOptions) / sizeof (kSoftTakeoverOptions[0]));

        int transformOptionIndex (Transform t)
        {
            for (int i = 0; i < kNumTransformOptions; ++i)
                if (kTransformOptions[i].value == t) return i;
            return 0;
        }

        int softTakeoverOptionIndex (SoftTakeoverPolicy p)
        {
            for (int i = 0; i < kNumSoftTakeoverOptions; ++i)
                if (kSoftTakeoverOptions[i].value == p) return i;
            return 0;
        }
    } // namespace

    //==========================================================================
    BindingTable::BindingTable (std::uint64_t      did,
                                MidiDeviceManager& mgr)
        : deviceId (did), deviceManager (mgr) {}

    BindingTable::~BindingTable()
    {
        if (subscribed.exchange (false))
            deviceManager.removeInputSubscriber (this);
        stopTimer();
    }

    void BindingTable::setMapping (std::shared_ptr<const Mapping> m, bool ro)
    {
        cancelLearning();

        mapping  = std::move (m);
        readOnly = ro;
        rebuildInteractiveControls();
        resized();
        repaint();
    }

    int BindingTable::getPreferredHeight() const noexcept
    {
        const int rows      = (mapping == nullptr) ? 0 : (int) mapping->bindings.size();
        const int displayed = juce::jmax (1, rows);
        return kHeaderHeight + displayed * kRowHeight + 4;
    }

    //--------------------------------------------------------------------------
    void BindingTable::rebuildInteractiveControls()
    {
        learnButtons.clear (true);
        deleteButtons.clear (true);
        transformCombos.clear (true);
        softTakeoverCombos.clear (true);

        if (readOnly || mapping == nullptr)
            return;

        const size_t n = mapping->bindings.size();
        for (size_t i = 0; i < n; ++i)
        {
            const auto& b = mapping->bindings[i];

            // ---- LEARN button -------------------------------------------
            auto* learn = learnButtons.add (new LearnButton());
            learn->setIdle();
            learn->onClick = [this, rowIdx = (int) i]() { startLearningRow (rowIdx); };
            addAndMakeVisible (learn);

            // ---- DELETE button ------------------------------------------
            auto* del = deleteButtons.add (new juce::TextButton ("DEL"));
            styleMonochromeButton (*del);
            del->onClick = [this, rowIdx = i]()
            {
                if (onDeleteRow) onDeleteRow (rowIdx);
            };
            addAndMakeVisible (del);

            // ---- TRANSFORM combo ----------------------------------------
            auto* tCombo = transformCombos.add (new juce::ComboBox());
            styleMonochromeCombo (*tCombo);
            for (int j = 0; j < kNumTransformOptions; ++j)
                tCombo->addItem (kTransformOptions[j].label, j + 1);
            tCombo->setSelectedItemIndex (transformOptionIndex (b.transform),
                                          juce::dontSendNotification);
            tCombo->onChange = [this, rowIdx = i, raw = tCombo]()
            {
                const int sel = raw->getSelectedItemIndex();
                if (sel < 0 || sel >= kNumTransformOptions) return;
                if (onTransformChanged)
                    onTransformChanged (rowIdx, kTransformOptions[(size_t) sel].value);
            };
            addAndMakeVisible (tCombo);

            // ---- SOFT-TAKEOVER combo ------------------------------------
            auto* sCombo = softTakeoverCombos.add (new juce::ComboBox());
            styleMonochromeCombo (*sCombo);
            for (int j = 0; j < kNumSoftTakeoverOptions; ++j)
                sCombo->addItem (kSoftTakeoverOptions[j].label, j + 1);
            sCombo->setSelectedItemIndex (softTakeoverOptionIndex (b.softTakeover),
                                          juce::dontSendNotification);
            sCombo->onChange = [this, rowIdx = i, raw = sCombo]()
            {
                const int sel = raw->getSelectedItemIndex();
                if (sel < 0 || sel >= kNumSoftTakeoverOptions) return;
                if (onSoftTakeoverChanged)
                    onSoftTakeoverChanged (rowIdx, kSoftTakeoverOptions[(size_t) sel].value);
            };
            addAndMakeVisible (sCombo);
        }
    }

    //--------------------------------------------------------------------------
    void BindingTable::resized()
    {
        if (learnButtons.isEmpty())
            return;

        auto inner = getLocalBounds().reduced (2);
        inner.removeFromTop (kHeaderHeight);
        const auto widths = computeColumnWidths (inner.getWidth());

        // Pre-compute X offsets per column.
        std::array<int, kNumColumns> colX {};
        {
            int x = inner.getX();
            for (int i = 0; i < kNumColumns; ++i) { colX[(size_t) i] = x; x += widths[(size_t) i]; }
        }

        const int rows = learnButtons.size();
        int y = inner.getY();
        for (int row = 0; row < rows; ++row)
        {
            // ---- TRANSFORM combo ----------------------------------------
            if (auto* c = transformCombos[row])
            {
                juce::Rectangle<int> cell { colX[kColTransform], y,
                                            widths[kColTransform], kRowHeight };
                c->setBounds (cell.reduced (kCellInset, kCellInset));
            }
            // ---- SOFT-TAKEOVER combo ------------------------------------
            if (auto* c = softTakeoverCombos[row])
            {
                juce::Rectangle<int> cell { colX[kColSoftTakeover], y,
                                            widths[kColSoftTakeover], kRowHeight };
                c->setBounds (cell.reduced (kCellInset, kCellInset));
            }
            // ---- ACTIONS: LEARN | DEL -----------------------------------
            juce::Rectangle<int> actions { colX[kColActions], y,
                                           widths[kColActions], kRowHeight };
            actions = actions.reduced (kCellInset, kCellInset);
            const int half = actions.getWidth() / 2;
            juce::Rectangle<int> left  = actions.removeFromLeft (half - 2);
            actions.removeFromLeft (2); // gap
            juce::Rectangle<int> right = actions;

            if (auto* b = learnButtons[row])  b->setBounds (left);
            if (auto* b = deleteButtons[row]) b->setBounds (right);

            y += kRowHeight;
        }
    }

    //--------------------------------------------------------------------------
    void BindingTable::mouseDown (const juce::MouseEvent& e)
    {
        // Phase 11: click on TARGET column in editable mode opens the
        // searchable target picker. All other interactions are handled by
        // child components (ComboBoxes, buttons).
        if (readOnly || mapping == nullptr || mapping->bindings.empty())
            return;

        auto inner = getLocalBounds().reduced (2);
        inner.removeFromTop (kHeaderHeight);
        const auto widths = computeColumnWidths (inner.getWidth());

        // Column 0 = TARGET. Hit-test the X range.
        const int targetColX     = inner.getX();
        const int targetColRight = targetColX + widths[0];
        if (e.x < targetColX || e.x >= targetColRight)
            return;

        // Determine which row was clicked (uniform kRowHeight).
        const int relY = e.y - inner.getY();
        if (relY < 0) return;
        const auto rowIdx = static_cast<std::size_t> (relY / kRowHeight);
        if (rowIdx >= mapping->bindings.size())
            return;

        // Build the picker and present it via a CallOutBox anchored to the
        // clicked cell.  The CallOutBox takes ownership; the picker fires
        // `onTargetChanged` and the CallOutBox dismisses itself on pick.
        auto picker = std::make_unique<TargetPicker>();
        picker->onPicked = [this, rowIdx] (TargetIndex idx)
        {
            if (onTargetChanged)
                onTargetChanged (rowIdx, idx);
        };

        const juce::Rectangle<int> cellRect {
            targetColX,
            inner.getY() + (int) rowIdx * kRowHeight,
            widths[0],
            kRowHeight
        };
        const auto screenAnchor = localAreaToGlobal (cellRect);

        juce::CallOutBox::launchAsynchronously (std::move (picker),
                                                screenAnchor,
                                                nullptr);
    }

    //--------------------------------------------------------------------------
    void BindingTable::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour (kBg));

        g.setColour (juce::Colour (kBorder));
        g.drawRect (getLocalBounds(), 2);

        auto inner = getLocalBounds().reduced (2);
        const auto widths = computeColumnWidths (inner.getWidth());

        auto headerRow = inner.removeFromTop (kHeaderHeight);
        g.setColour (juce::Colour (kFg));
        g.fillRect (headerRow);

        g.setFont (juce::FontOptions ("Space Mono", 11.0f, juce::Font::plain));
        {
            int x = headerRow.getX();
            for (int i = 0; i < kNumColumns; ++i)
            {
                juce::Rectangle<int> cell { x, headerRow.getY(),
                                            widths[(size_t) i], headerRow.getHeight() };
                drawCellText (g, kColumns[(size_t) i].title, cell, juce::Colour (kBg));
                x += widths[(size_t) i];
                if (i < kNumColumns - 1)
                {
                    g.setColour (juce::Colour (kBg));
                    g.fillRect (juce::Rectangle<int> { x - 1, headerRow.getY(),
                                                       1, headerRow.getHeight() });
                }
            }
        }

        g.setFont (juce::FontOptions ("Space Mono", 11.0f, juce::Font::plain));

        if (mapping == nullptr || mapping->bindings.empty())
        {
            auto emptyRow = inner.removeFromTop (kRowHeight);
            drawCellText (g, "(no bindings)", emptyRow,
                          juce::Colour (kMuted), juce::Justification::centred);
            return;
        }

        int y = inner.getY();
        for (size_t rowIdx = 0; rowIdx < mapping->bindings.size(); ++rowIdx)
        {
            const auto& b = mapping->bindings[rowIdx];
            juce::Rectangle<int> row { inner.getX(), y, inner.getWidth(), kRowHeight };

            if ((rowIdx & 1u) == 1u)
            {
                g.setColour (juce::Colour (kFg).withAlpha (0.05f));
                g.fillRect (row);
            }

            g.setColour (juce::Colour (kFg).withAlpha (0.20f));
            g.fillRect (juce::Rectangle<int> { row.getX(), row.getBottom() - 1,
                                               row.getWidth(), 1 });

            const juce::String paintedCells[kNumColumns] = {
                formatTarget       (b.target),
                formatMidiKey      (b.midiKey, b.lsbData1),
                formatModifier     (b.requiredModifierMask, mapping->modifierNames),
                formatTransform    (b.transform),
                formatSoftTakeover (b.softTakeover),
                formatFeedback     (b.feedback),
                juce::String(),
            };

            int x = row.getX();
            for (int i = 0; i < kNumColumns; ++i)
            {
                juce::Rectangle<int> cell { x, row.getY(),
                                            widths[(size_t) i], row.getHeight() };

                if (i > 0)
                {
                    g.setColour (juce::Colour (kFg).withAlpha (0.20f));
                    g.fillRect (juce::Rectangle<int> { x, cell.getY(), 1, cell.getHeight() });
                }

                // In editable mode, TRANSFORM/SOFT-TAKEOVER/ACTIONS host live
                // children — paint() leaves those cells empty.
                const bool isHostedByChild =
                    (! readOnly) && (i == kColTransform
                                   || i == kColSoftTakeover
                                   || i == kColActions);

                if (! isHostedByChild)
                {
                    if (i == kColActions && readOnly)
                    {
                        drawCellText (g, "—", cell, juce::Colour (kMuted),
                                      juce::Justification::centred);
                    }
                    else
                    {
                        const bool isMuted = paintedCells[(size_t) i] == "(none)";
                        drawCellText (g, paintedCells[(size_t) i], cell,
                                      isMuted ? juce::Colour (kMuted) : juce::Colour (kFg));
                    }
                }

                x += widths[(size_t) i];
            }

            y += kRowHeight;
        }
    }

    //==========================================================================
    // MIDI Learn
    //==========================================================================
    void BindingTable::startLearningRow (int rowIdx)
    {
        if (readOnly || mapping == nullptr) return;
        if (rowIdx < 0 || rowIdx >= (int) mapping->bindings.size()) return;

        if (learningRow >= 0)
            cancelLearning();

        learningRow      = rowIdx;
        secondsRemaining = kLearnSeconds;
        capturedMidiKey.store (0, std::memory_order_relaxed);

        if (auto* btn = learnButtons[rowIdx])
            btn->setLearning (secondsRemaining);

        if (! subscribed.exchange (true))
            deviceManager.addInputSubscriber (this);

        startTimerHz (4);
    }

    void BindingTable::cancelLearning()
    {
        if (learningRow < 0) return;

        const int row = learningRow;
        learningRow      = -1;
        secondsRemaining = 0;

        if (subscribed.exchange (false))
            deviceManager.removeInputSubscriber (this);
        stopTimer();
        capturedMidiKey.store (0, std::memory_order_relaxed);

        if (row >= 0 && row < learnButtons.size())
            if (auto* btn = learnButtons[row])
                btn->setIdle();
    }

    void BindingTable::handleLearnedOnMessageThread (std::uint32_t midiKey)
    {
        if (learningRow < 0) return;

        const auto rowIdx = (size_t) learningRow;
        cancelLearning();

        if (onMidiLearned)
            onMidiLearned (rowIdx, midiKey);
    }

    void BindingTable::timerCallback()
    {
        if (auto key = capturedMidiKey.exchange (0, std::memory_order_acquire); key != 0)
        {
            handleLearnedOnMessageThread (key);
            return;
        }

        static thread_local int subTick = 0;
        if (++subTick < 4) return;
        subTick = 0;

        if (--secondsRemaining <= 0)
        {
            cancelLearning();
            return;
        }

        if (learningRow >= 0 && learningRow < learnButtons.size())
            if (auto* btn = learnButtons[learningRow])
                btn->setLearning (secondsRemaining);
    }

    //--------------------------------------------------------------------------
    // MIDI callback thread — RT-safe.
    //--------------------------------------------------------------------------
    void BindingTable::onMidiInbound (const MidiInboundEvent& e) noexcept
    {
        if (e.deviceId != deviceId) return;
        if (capturedMidiKey.load (std::memory_order_relaxed) != 0) return;

        const std::uint8_t statusNibble = (std::uint8_t) (e.statusByte & 0xF0u);
        switch (statusNibble)
        {
            case 0x90:
                if (e.data2 == 0) return;
                break;
            case 0xB0:
            case 0xE0:
                break;
            default:
                return;
        }

        const std::uint8_t channel0 = (std::uint8_t) (e.statusByte & 0x0Fu);
        const std::uint8_t channel1 = (std::uint8_t) (channel0 + 1u);
        const std::uint32_t key =
            ((std::uint32_t) channel1 << 16)
          | ((std::uint32_t) statusNibble << 8)
          |  (std::uint32_t) e.data1;
        capturedMidiKey.store (key, std::memory_order_release);
    }
}
