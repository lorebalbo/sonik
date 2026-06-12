#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include "../DeckIdentifiers.h"
#include "../../Loop/LoopControlComponent.h"
#include "../../Cue/HotCuePadComponent.h"
#include "../../BeatJump/BeatJumpComponent.h"
#include <functional>

// ---------------------------------------------------------------------------
// ControllerWidget
//
// Compact 23px controller row between the waveform and the cue-pads row.
//
// Layout:
//   LEFT  — transport: [CUE 50px][● 36px][▶ 36px]  (left-aligned)
//   RIGHT — mode btn:  [LOOP/JUMP/GRID 50px]        (right-aligned, dark)
//   MID   — mode-specific controls (right-aligned, left of mode btn):
//             LOOP / JUMP:  [< 25px][2 38px][4 38px][8 38px][16 38px][> 25px]
//             GRID:         [SET 50px][DEL 50px][<< 38px][< 25px][> 25px][>> 38px]
//
// Clicking the mode button opens a PopupMenu to switch between LOOP/JUMP/GRID.
// All buttons are painted; no child components are rendered by this widget.
// ---------------------------------------------------------------------------

class ControllerWidget final : public juce::Component,
                               private juce::ValueTree::Listener
{
public:
    class BpmInputFilter final : public juce::TextEditor::InputFilter
    {
    public:
        juce::String filterNewText (juce::TextEditor&, const juce::String& newInput) override
        {
            juce::String accepted;
            for (auto c : newInput)
            {
                if ((c >= '0' && c <= '9') || c == '.')
                    accepted << c;
            }
            return accepted;
        }
    };

    // -----------------------------------------------------------------------
    // Constructor — takes non-owning pointers (kept for API compatibility;
    // child components are NOT rendered inside this widget in the new design)
    // -----------------------------------------------------------------------
    ControllerWidget (juce::ValueTree deckTree,
                      LoopControlComponent* loopCtrl,
                      HotCuePadComponent*  cuePads,
                      BeatJumpComponent*   beatJump)
        : tree        (deckTree),
          loopControl (loopCtrl),
          hotCuePads  (cuePads),
          beatJumpCtrl (beatJump)
    {
        // Normalise: "cue" tab removed in new design — fall back to "loop"
        activeTab = tree.getProperty (IDs::controllerTab, "loop").toString();
        if (activeTab == "cue")
            activeTab = "loop";

        // bpmEditor kept for callback wiring; never shown in compact layout
        bpmEditor       = std::make_unique<juce::TextEditor>();
        bpmEditorFilter = std::make_unique<BpmInputFilter>();
        bpmEditor->setInputFilter (bpmEditorFilter.get(), false);
        bpmEditor->onReturnKey = [this]() { commitBpmEdit(); };
        bpmEditor->onEscapeKey = [this]() { revertBpmEdit(); };
        bpmEditor->onFocusLost = [this]() { revertBpmEdit(); };
        addChildComponent (*bpmEditor);   // invisible

        tree.addListener (this);
    }

    ~ControllerWidget() override
    {
        tree.removeListener (this);
    }

    // -----------------------------------------------------------------------
    // Transport callbacks (wired by DeckShellComponent)
    // -----------------------------------------------------------------------
    std::function<void()>       onCuePress;
    std::function<void()>       onStopPress;
    std::function<void()>       onPlayPress;

    // -----------------------------------------------------------------------
    // Loop callbacks
    // -----------------------------------------------------------------------
    std::function<void (float)> onAutoLoop;    // size button in LOOP mode
    std::function<void()>       onLoopHalve;   // < in LOOP mode
    std::function<void()>       onLoopDouble;  // > in LOOP mode

    // -----------------------------------------------------------------------
    // Jump callbacks
    // -----------------------------------------------------------------------
    std::function<void()>       onJumpForward;   // > in JUMP mode
    std::function<void()>       onJumpBackward;  // < in JUMP mode

    // -----------------------------------------------------------------------
    // Beatgrid callbacks (GRID mode)
    // -----------------------------------------------------------------------
    std::function<void()>         onGridSet;
    std::function<void()>         onGridDelete;
    std::function<void (int)>     onGridNudge;

    std::function<juce::String()> getBpmString;
    std::function<void (double)>  onBpmSave;

    // -----------------------------------------------------------------------
    // Called by DeckShellComponent when the active auto-loop size changes
    // -----------------------------------------------------------------------
    void setActiveAutoLoopBeats (float beats)
    {
        activeLoopBeats = beats;
        repaint();
    }

    // -----------------------------------------------------------------------
    // Component overrides
    // -----------------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        const bool isPlaying = tree.getProperty (IDs::playbackStatus).toString() == "playing";

        // Transport buttons — left-aligned, always visible
        drawBtn (g, getTransportBtnBounds (0), "CUE",   false,    hoveredBtn == 0);
        drawBtn (g, getTransportBtnBounds (1), nullptr, false,    hoveredBtn == 1, true,  false); // ●
        drawBtn (g, getTransportBtnBounds (2), nullptr, isPlaying, hoveredBtn == 2, false, true); // ▶

        // Mode-specific controls — right-aligned, left of mode button
        if (activeTab == "loop" || activeTab == "jump")
        {
            static constexpr double kSizes[] = { 2.0, 4.0, 8.0, 16.0 };
            const char* labels[] = { "<", "2", "4", "8", "16", ">" };

            double activeSize = (activeTab == "jump")
                ? static_cast<double> (tree.getProperty (IDs::beatJumpSize, 4.0))
                : static_cast<double> (activeLoopBeats);

            for (int i = 0; i < 6; ++i)
            {
                const bool active = (i >= 1 && i <= 4) && juce::exactlyEqual (kSizes[i - 1], activeSize);
                drawBtn (g, getSizeBtnBounds (i), labels[i], active, hoveredBtn == 10 + i);
            }
        }
        else if (activeTab == "grid")
        {
            const char* labels[] = { "SET", "DEL", "<<", "<", ">", ">>" };
            for (int i = 0; i < 6; ++i)
                drawBtn (g, getGridBtnBounds (i), labels[i], false, hoveredBtn == 20 + i);
        }

        // Mode button — always dark-filled, right edge
        drawBtn (g, getModeBtnBounds(), activeTab.toUpperCase().toUTF8(), true, false);
    }

    void resized() override {}   // pure-paint component; nothing to lay out

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int mx = e.x, my = e.y;

        // Transport
        if (getTransportBtnBounds (0).contains (mx, my)) { if (onCuePress)  onCuePress();  return; }
        if (getTransportBtnBounds (1).contains (mx, my)) { if (onStopPress) onStopPress(); return; }
        if (getTransportBtnBounds (2).contains (mx, my)) { if (onPlayPress) onPlayPress(); return; }

        // Mode button
        if (getModeBtnBounds().contains (mx, my)) { showModePopup(); return; }

        // Mode-specific
        if (activeTab == "loop" || activeTab == "jump")
        {
            for (int i = 0; i < 6; ++i)
            {
                if (getSizeBtnBounds (i).contains (mx, my))
                {
                    if (activeTab == "loop") handleLoopSizeClick (i);
                    else                     handleJumpSizeClick (i);
                    return;
                }
            }
        }
        else if (activeTab == "grid")
        {
            for (int i = 0; i < 6; ++i)
            {
                if (getGridBtnBounds (i).contains (mx, my))
                {
                    handleGridBtnClick (i);
                    return;
                }
            }
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int newHover = getButtonAt (e.x, e.y);
        if (newHover != hoveredBtn) { hoveredBtn = newHover; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoveredBtn = -1;
        repaint();
    }

    void paintOverChildren (juce::Graphics&) override {}

private:
    // -----------------------------------------------------------------------
    // Layout constants  (all heights adapt to getHeight() = 23px)
    // -----------------------------------------------------------------------
    static constexpr int kBorderW  = 2;   // shared border between adjacent buttons
    static constexpr int kCueBtnW  = 50;  // CUE, SET, DEL transport buttons
    static constexpr int kIconBtnW = 50;  // ● and ▶  (matches CUE width)
    static constexpr int kSizeBtnW = 50;  // 2 4 8 16 and << >>
    static constexpr int kArrowW   = 50;  // < and >   (matches other buttons)
    static constexpr int kModeBtnW = 70;  // LOOP / JUMP / GRID  (matches CUE label below)
    static constexpr int kModeGap  = 8;   // gap between controls and mode button

    // < + [2+4+8+16 with shared borders] + >:
    // 50 + (50-2)*4 + (50-2) = 50 + 192 + 48 = 290
    static constexpr int kSizeCtrlW = kArrowW
                                    + (kSizeBtnW - kBorderW) * 4
                                    + (kArrowW   - kBorderW);   // 290

    // SET + DEL + << + < + > + >> with shared borders:
    // 50 + (50-2)*5 = 50 + 240 = 290
    static constexpr int kGridCtrlW = kCueBtnW
                                    + (kCueBtnW  - kBorderW)
                                    + (kSizeBtnW - kBorderW)
                                    + (kArrowW   - kBorderW)
                                    + (kArrowW   - kBorderW)
                                    + (kSizeBtnW - kBorderW);   // 290

    // -----------------------------------------------------------------------
    // Geometry helpers
    // -----------------------------------------------------------------------
    juce::Rectangle<int> getTransportBtnBounds (int idx) const noexcept
    {
        const int h = getHeight();
        switch (idx)
        {
            case 0: return { 0,                                        0, kCueBtnW,  h }; // CUE
            case 1: return { kCueBtnW - kBorderW,                     0, kIconBtnW, h }; // ●
            case 2: return { kCueBtnW + kIconBtnW - 2 * kBorderW,     0, kIconBtnW, h }; // ▶
            default: break;
        }
        return {};
    }

    juce::Rectangle<int> getModeBtnBounds() const noexcept
    {
        return { getWidth() - kModeBtnW, 0, kModeBtnW, getHeight() };
    }

    // idx: 0=< 1=2 2=4 3=8 4=16 5=>   (LOOP and JUMP modes share this layout)
    juce::Rectangle<int> getSizeBtnBounds (int idx) const noexcept
    {
        const int groupRight = getWidth() - kModeBtnW - kModeGap;
        const int groupLeft  = groupRight - kSizeCtrlW;
        const int h          = getHeight();
        static constexpr int widths[] = { kArrowW, kSizeBtnW, kSizeBtnW, kSizeBtnW, kSizeBtnW, kArrowW };
        int x = groupLeft;
        for (int i = 0; i < idx; ++i)
            x += widths[i] - kBorderW;
        return { x, 0, widths[idx], h };
    }

    // idx: 0=SET 1=DEL 2=<< 3=< 4=> 5=>>   (GRID mode)
    juce::Rectangle<int> getGridBtnBounds (int idx) const noexcept
    {
        const int groupRight = getWidth() - kModeBtnW - kModeGap;
        const int groupLeft  = groupRight - kGridCtrlW;
        const int h          = getHeight();
        static constexpr int widths[] = { kCueBtnW, kCueBtnW, kSizeBtnW, kArrowW, kArrowW, kSizeBtnW };
        int x = groupLeft;
        for (int i = 0; i < idx; ++i)
            x += widths[i] - kBorderW;
        return { x, 0, widths[idx], h };
    }

    int getButtonAt (int mx, int my) const noexcept
    {
        for (int i = 0; i < 3; ++i)
            if (getTransportBtnBounds (i).contains (mx, my)) return i;
        if (getModeBtnBounds().contains (mx, my)) return 100;
        if (activeTab == "loop" || activeTab == "jump")
        {
            for (int i = 0; i < 6; ++i)
                if (getSizeBtnBounds (i).contains (mx, my)) return 10 + i;
        }
        else if (activeTab == "grid")
        {
            for (int i = 0; i < 6; ++i)
                if (getGridBtnBounds (i).contains (mx, my)) return 20 + i;
        }
        return -1;
    }

    // -----------------------------------------------------------------------
    // Drawing helper
    // -----------------------------------------------------------------------
    void drawBtn (juce::Graphics& g, juce::Rectangle<int> r, const char* label,
                  bool active, bool hovered,
                  bool stopIcon = false, bool playIcon = false)
    {
        const juce::Colour fill = active   ? juce::Colour (0xFF2D2D2D)
                                : hovered  ? juce::Colour (0xFFE2E2E2)
                                           : juce::Colour (0xFFF9F9F9);
        g.setColour (fill);
        g.fillRect (r);
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawRect (r, kBorderW);

        const juce::Colour fg = active ? juce::Colour (0xFFF9F9F9) : juce::Colour (0xFF2D2D2D);

        if (stopIcon)
        {
            constexpr int s = 8;
            g.setColour (fg);
            g.fillRect (juce::Rectangle<int> (r.getCentreX() - s / 2,
                                              r.getCentreY() - s / 2, s, s));
        }
        else if (playIcon)
        {
            const float cx = static_cast<float> (r.getCentreX());
            const float cy = static_cast<float> (r.getCentreY());
            constexpr float sz = 7.0f;
            juce::Path tri;
            tri.addTriangle (cx - sz * 0.55f, cy - sz,
                             cx - sz * 0.55f, cy + sz,
                             cx + sz * 0.85f, cy);
            g.setColour (fg);
            g.fillPath (tri);
        }
        else if (label != nullptr)
        {
            g.setColour (fg);
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
            g.drawText (juce::String::fromUTF8 (label), r, juce::Justification::centred);
        }
    }

    // -----------------------------------------------------------------------
    // Action handlers
    // -----------------------------------------------------------------------
    void showModePopup()
    {
        juce::PopupMenu menu;
        menu.addItem (1, "LOOP", true, activeTab == "loop");
        menu.addItem (2, "JUMP", true, activeTab == "jump");
        menu.addItem (3, "GRID", true, activeTab == "grid");

        menu.showMenuAsync (
            juce::PopupMenu::Options()
                .withTargetComponent (this)
                .withTargetScreenArea (localAreaToGlobal (getModeBtnBounds()))
                .withMinimumWidth (getModeBtnBounds().getWidth()),
            [safeThis = juce::Component::SafePointer<ControllerWidget> (this)] (int result)
            {
                if (safeThis == nullptr) return;
                juce::String newTab;
                if      (result == 1) newTab = "loop";
                else if (result == 2) newTab = "jump";
                else if (result == 3) newTab = "grid";
                else return;
                safeThis->tree.setProperty (IDs::controllerTab, newTab, nullptr);
            });
    }

    void handleLoopSizeClick (int idx)
    {
        static constexpr float kSizes[] = { 2.0f, 4.0f, 8.0f, 16.0f };
        if      (idx == 0) { if (onLoopHalve)  onLoopHalve();             }
        else if (idx == 5) { if (onLoopDouble) onLoopDouble();             }
        else               { if (onAutoLoop)   onAutoLoop (kSizes[idx-1]); }
    }

    void handleJumpSizeClick (int idx)
    {
        static constexpr double kSizes[] = { 2.0, 4.0, 8.0, 16.0 };
        if      (idx == 0) { if (onJumpBackward) onJumpBackward(); }
        else if (idx == 5) { if (onJumpForward)  onJumpForward();  }
        else               { tree.setProperty (IDs::beatJumpSize, kSizes[idx - 1], nullptr); }
    }

    void handleGridBtnClick (int idx)
    {
        switch (idx)
        {
            case 0: if (onGridSet)    onGridSet();       break;  // SET
            case 1: if (onGridDelete) onGridDelete();    break;  // DEL
            case 2: if (onGridNudge)  onGridNudge (-2);  break;  // <<
            case 3: if (onGridNudge)  onGridNudge (-1);  break;  // <
            case 4: if (onGridNudge)  onGridNudge (+1);  break;  // >
            case 5: if (onGridNudge)  onGridNudge (+2);  break;  // >>
            default: break;
        }
    }

    // -----------------------------------------------------------------------
    // ValueTree::Listener
    // -----------------------------------------------------------------------
    void valueTreePropertyChanged (juce::ValueTree& changedTree,
                                   const juce::Identifier& property) override
    {
        if (changedTree == tree && property == IDs::controllerTab)
        {
            juce::String newTab = changedTree[property].toString();
            if (newTab == "cue") newTab = "loop";
            if (newTab != activeTab)
            {
                activeTab = newTab;
                juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
                {
                    if (safeThis != nullptr)
                        safeThis->repaint();
                });
            }
        }

        if (changedTree == tree && property == IDs::beatJumpSize)
        {
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
            {
                if (safeThis != nullptr)
                    safeThis->repaint();
            });
        }
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    // -----------------------------------------------------------------------
    // BPM editor — kept for callback wiring only; never shown in compact layout
    // -----------------------------------------------------------------------
    void commitBpmEdit()
    {
        if (bpmEditor == nullptr) return;
        const double bpm = bpmEditor->getText().trim().getDoubleValue();
        if (bpm >= 20.0 && bpm <= 300.0)
        {
            if (onBpmSave) onBpmSave (bpm);
        }
        bpmEditor->giveAwayKeyboardFocus();
    }

    void revertBpmEdit()
    {
        if (bpmEditor != nullptr)
            bpmEditor->giveAwayKeyboardFocus();
    }

    bool isGridEnabled() const
    {
        auto bg = tree.getChildWithName (IDs::BeatGrid);
        return bg.isValid() && static_cast<double> (bg.getProperty (IDs::bpm, 0.0)) > 0.0;
    }

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    juce::ValueTree tree;
    juce::String    activeTab      { "loop" };
    float           activeLoopBeats = 0.0f;
    int             hoveredBtn      = -1;

    std::unique_ptr<juce::TextEditor> bpmEditor;
    std::unique_ptr<BpmInputFilter>   bpmEditorFilter;

    LoopControlComponent* loopControl  = nullptr;
    HotCuePadComponent*   hotCuePads   = nullptr;
    BeatJumpComponent*    beatJumpCtrl = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControllerWidget)
};

