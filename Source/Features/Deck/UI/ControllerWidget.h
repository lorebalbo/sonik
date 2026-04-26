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
// Tabbed panel at the bottom of the deck UI.
// Matches the Figma "Controller Widget" component with 4 variants:
//   LOOP (Default)  – LoopControlComponent
//   CUE  (Variant2) – HotCuePadComponent
//   JUMP (Variant3) – transport controls + BeatJumpComponent
//   GRID (Variant4) – beatgrid editor (BPM / SET / DEL / << < > >>)
//
// Panel width : 474 px   Tabs width : 70 px   Gap : 8 px
// Total height: 86 px (panel + 2px border)
// ---------------------------------------------------------------------------
class ControllerWidget final : public juce::Component,
                                private juce::ValueTree::Listener
{
public:
    // -----------------------------------------------------------------------
    // Constructor — takes non-owning pointers to the three main subcomponents
    // -----------------------------------------------------------------------
    ControllerWidget (juce::ValueTree deckTree,
                      LoopControlComponent* loopCtrl,
                      HotCuePadComponent*  cuePads,
                      BeatJumpComponent*   beatJump)
        : tree (deckTree),
          loopControl (loopCtrl),
          hotCuePads  (cuePads),
          beatJumpCtrl (beatJump)
    {
        activeTab = tree.getProperty (IDs::controllerTab, "loop").toString();

        if (loopControl  != nullptr) addAndMakeVisible (*loopControl);
        if (hotCuePads   != nullptr) addAndMakeVisible (*hotCuePads);
        if (beatJumpCtrl != nullptr) addAndMakeVisible (*beatJumpCtrl);

        tree.addListener (this);
        updateTabVisibility();
    }

    ~ControllerWidget() override
    {
        tree.removeListener (this);
    }

    // -----------------------------------------------------------------------
    // Callbacks wired from DeckShellComponent for transport (JUMP tab)
    // -----------------------------------------------------------------------
    std::function<void()> onCuePress;     // set/go-to temp cue
    std::function<void()> onStopPress;    // stop + go to cue
    std::function<void()> onPlayPress;    // toggle play/pause

    // Callbacks for beatgrid editor (GRID tab)
    std::function<void()> onGridSet;      // set anchor at playhead
    std::function<void()> onGridDelete;   // reset/delete beatgrid
    std::function<void (int)> onGridNudge; // nudge anchor: -2, -1, +1, +2

    // Returns current BPM string for GRID tab display
    std::function<juce::String()> getBpmString;

    // -----------------------------------------------------------------------
    // Component overrides
    // -----------------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        // NOTE: No outer widget border here — the panel and each tab draw their
        // own 2px borders.  An extra outer border would create a connecting line
        // across the top and bottom that the design intentionally avoids.

        // Tab column background
        auto tabColumnArea = getTabColumnBounds();
        g.setColour (juce::Colour (0xFFE5E5E5));
        g.fillRect (tabColumnArea);

        // Draw tabs with -2px gap (adjacent tabs share a single 2px border)
        // With tabH = 23 and -2px overlap: total visual height = 4*23 - 3*2 = 86px = kControllerH
        const juce::StringArray tabNames { "LOOP", "CUE", "JUMP", "GRID" };
        const int tabH     = 23;
        const int tabStride = tabH - 2; // each tab starts 2px before the previous one ends
        int ty = tabColumnArea.getY();

        for (int i = 0; i < 4; ++i)
        {
            juce::Rectangle<int> tabRect (tabColumnArea.getX(), ty + i * tabStride,
                                          tabColumnArea.getWidth(), tabH);

            bool isActive = (tabNames[i].toLowerCase() == activeTab);

            g.setColour (isActive ? juce::Colour (0xFF2D2D2D) : juce::Colour (0xFFF9F9F9));
            g.fillRect (tabRect);

            g.setColour (juce::Colour (0xFF2D2D2D));
            g.drawRect (tabRect, 2);

            g.setColour (isActive ? juce::Colour (0xFFF9F9F9) : juce::Colour (0xFF2D2D2D));
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
            g.drawText (tabNames[i], tabRect, juce::Justification::centred);
        }

        // Panel background
        auto panelArea = getPanelBounds();
        g.setColour (juce::Colour (0xFFF9F9F9));
        g.fillRect (panelArea);
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawRect (panelArea, 2);

        // Render tab-specific content that is painted directly (JUMP transport, GRID)
        if (activeTab == "jump")
            paintJumpTransport (g, panelArea.reduced (10));
        else if (activeTab == "grid")
            paintGridEditor (g, panelArea.reduced (10));
    }

    void resized() override
    {
        auto panelArea = getPanelBounds().reduced (10);

        if (activeTab == "loop" && loopControl != nullptr)
            loopControl->setBounds (panelArea);

        if (activeTab == "cue" && hotCuePads != nullptr)
            hotCuePads->setBounds (panelArea);

        if (activeTab == "jump" && beatJumpCtrl != nullptr)
        {
            // Position beat jump component to the right of the centred transport group.
            // BeatJumpComponent self-centres both axes within its own bounds.
            const int contentX = jumpContentX (panelArea);
            beatJumpCtrl->setBounds (contentX + kJumpTransportW + kJumpGroupGap,
                                     panelArea.getY(),
                                     kJumpBeatW,
                                     panelArea.getHeight());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Tab clicks
        auto tabColumn = getTabColumnBounds();
        if (tabColumn.contains (e.x, e.y))
        {
// Tab hit-testing: use the same tabH and stride as paint()
        const int tabH     = 23;
        const int tabStride = tabH - 2;
        int relY     = e.y - tabColumn.getY();
        int tabIndex = relY / tabStride;

            const juce::StringArray tabNames { "loop", "cue", "jump", "grid" };
            if (juce::isPositiveAndBelow (tabIndex, 4))
            {
                activeTab = tabNames[tabIndex];
                tree.setProperty (IDs::controllerTab, activeTab, nullptr);
                updateTabVisibility();
                resized();
                repaint();
            }
            return;
        }

        // JUMP tab transport button clicks
        if (activeTab == "jump")
        {
            handleJumpTransportClick (e.x, e.y);
            return;
        }

        // GRID tab button clicks
        if (activeTab == "grid")
        {
            handleGridEditorClick (e.x, e.y);
        }
    }

private:
    // -----------------------------------------------------------------------
    // Layout helpers
    // -----------------------------------------------------------------------
    static constexpr int kPanelW   = 474;
    static constexpr int kTabW     = 70;
    static constexpr int kGap      = 8;
    static constexpr int kTabCount = 4;

    // Dynamic panel width: the widget is sized to (panelW + kGap + kTabW) by the
    // parent, so we derive panelW from the actual component width rather than the
    // Figma-fixed kPanelW constant.  This ensures the panel always stretches to
    // match the waveform area and the tab column stays flush with the KEY button.
    int dynamicPanelW() const noexcept
    {
        return juce::jmax (1, getWidth() - kGap - kTabW);
    }

    juce::Rectangle<int> getPanelBounds() const
    {
        return getLocalBounds().withWidth (dynamicPanelW());
    }

    juce::Rectangle<int> getTabColumnBounds() const
    {
        return getLocalBounds().withTrimmedLeft (dynamicPanelW() + kGap);
    }

    // -----------------------------------------------------------------------
    // JUMP and GRID centering constants
    // All buttons match the GRID section size (50 × 46 px).
    // Arrow buttons (< >) are half-width (25 px).
    // -----------------------------------------------------------------------
    // JUMP: transport group (CUE+STOP+PLAY=146) + 8px gap + beat-jump group (240)
    static constexpr int kJumpTransportW = 3 * 50 - 2 * 2;           // 146
    static constexpr int kJumpBeatW      = 2 * 25 + 4 * 50 - 5 * 2; // 240
    static constexpr int kJumpGroupGap   = 8;
    static constexpr int kJumpTotalW     = kJumpTransportW + kJumpGroupGap + kJumpBeatW; // 394

    // GRID: BPM(70) + sep+gap(10) + SET+DEL(2×48) + gap(8) + <<+<+>+>>(3×48+50) = 378
    static constexpr int kGridContentW  = 378;

    // Return the starting X for content centred inside the given panel inner area.
    static int jumpContentX (juce::Rectangle<int> inner) noexcept
    {
        return inner.getX() + juce::jmax (0, (inner.getWidth() - kJumpTotalW) / 2);
    }
    static int gridContentX (juce::Rectangle<int> inner) noexcept
    {
        return inner.getX() + juce::jmax (0, (inner.getWidth() - kGridContentW) / 2);
    }

    // -----------------------------------------------------------------------
    // Visibility management
    // -----------------------------------------------------------------------
    void updateTabVisibility()
    {
        if (loopControl  != nullptr) loopControl->setVisible  (activeTab == "loop");
        if (hotCuePads   != nullptr) hotCuePads->setVisible   (activeTab == "cue");
        if (beatJumpCtrl != nullptr) beatJumpCtrl->setVisible (activeTab == "jump");
    }

    // -----------------------------------------------------------------------
    // JUMP tab – painted transport controls (CUE / STOP / PLAY)
    // -----------------------------------------------------------------------
    void paintJumpTransport (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const int btnW = 50;
        const int btnH = 46;
        // Centre the transport group horizontally inside the area.
        int startX  = jumpContentX (area);
        int centreY = area.getCentreY();
        int btnY    = centreY - btnH / 2;

        // CUE button
        drawSquareButton (g, startX,                btnY, btnW, btnH, "CUE",           false, false);
        // STOP button (filled-square icon)
        drawSquareButton (g, startX + btnW - 2,     btnY, btnW, btnH, nullptr,         false, false, true);
        // PLAY button
        drawSquareButton (g, startX + 2*(btnW - 2), btnY, btnW, btnH, "\xe2\x96\xb6", false, false);
    }

    void paintGridEditor (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const int btnW = 50;
        const int btnH = 46;
        int centreY = area.getCentreY();
        int btnY    = centreY - btnH / 2;
        // Centre the entire GRID content block horizontally inside the area.
        int x       = gridContentX (area);

        // BPM display

        // BPM label + value box
        auto monoFont = juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain);
        g.setFont (monoFont);
        g.setColour (juce::Colour (0xFF2D2D2D));

        juce::Rectangle<int> bpmLabelRect (x, btnY, 60, 23);
        g.drawText ("BPM", bpmLabelRect, juce::Justification::centredLeft);

        juce::String bpmText = getBpmString ? getBpmString() : "--";
        juce::Rectangle<int> bpmValueRect (x, btnY + 23, 60, 23);
        g.fillRect (bpmValueRect);  // background
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawRect (bpmValueRect, 2);
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawText (bpmText, bpmValueRect, juce::Justification::centred);

        // Separator line
        int sepX = x + 70;
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawVerticalLine (sepX, static_cast<float> (btnY), static_cast<float> (btnY + btnH));

        // SET / DEL buttons
        int bx = sepX + 10;
        drawSquareButton (g, bx,          btnY, btnW, btnH, "SET", false, false);
        drawSquareButton (g, bx + btnW - 2, btnY, btnW, btnH, "DEL", false, false);

        // Nudge buttons: << < > >>
        int nx = bx + 2 * (btnW - 2) + 8;
        drawSquareButton (g, nx,                 btnY, btnW, btnH, "<<", false, false);
        drawSquareButton (g, nx +   (btnW - 2),  btnY, btnW, btnH, "<",  false, false);
        drawSquareButton (g, nx + 2*(btnW - 2),  btnY, btnW, btnH, ">",  false, false);
        drawSquareButton (g, nx + 3*(btnW - 2),  btnY, btnW, btnH, ">>", false, false);
    }

    /// Draw a Figma-style square button (border-2, no border-radius)
    static void drawSquareButton (juce::Graphics& g, int x, int y, int w, int h,
                                  const char* label, bool active, bool /*disabled*/,
                                  bool stopIcon = false)
    {
        juce::Rectangle<int> r (x, y, w, h);

        g.setColour (active ? juce::Colour (0xFF2D2D2D) : juce::Colour (0xFFF9F9F9));
        g.fillRect (r);
        g.setColour (juce::Colour (0xFF2D2D2D));
        g.drawRect (r, 2);

        if (stopIcon)
        {
            // Draw a filled square (■) as the stop icon
            int iconSize = 10;
            juce::Rectangle<int> icon (r.getCentreX() - iconSize / 2,
                                       r.getCentreY() - iconSize / 2,
                                       iconSize, iconSize);
            g.setColour (active ? juce::Colour (0xFFF9F9F9) : juce::Colour (0xFF2D2D2D));
            g.fillRect (icon);
        }
        else if (label != nullptr)
        {
            g.setColour (active ? juce::Colour (0xFFF9F9F9) : juce::Colour (0xFF2D2D2D));
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
            g.drawText (juce::String::fromUTF8 (label), r, juce::Justification::centred);
        }
    }

    // -----------------------------------------------------------------------
    // Hit-test helpers for JUMP and GRID tab interactive areas
    // -----------------------------------------------------------------------
    void handleJumpTransportClick (int mx, int my)
    {
        auto panelArea = getPanelBounds().reduced (10);
        const int btnW = 50;
        const int btnH = 46;
        int startX  = jumpContentX (panelArea);
        int centreY = panelArea.getCentreY();
        int btnY    = centreY - btnH / 2;

        juce::Rectangle<int> cueRect  (startX,               btnY, btnW, btnH);
        juce::Rectangle<int> stopRect (startX + btnW - 2,    btnY, btnW, btnH);
        juce::Rectangle<int> playRect (startX + 2*(btnW-2),  btnY, btnW, btnH);

        if (cueRect.contains (mx, my))
        {
            if (onCuePress) onCuePress();
        }
        else if (stopRect.contains (mx, my))
        {
            if (onStopPress) onStopPress();
        }
        else if (playRect.contains (mx, my))
        {
            if (onPlayPress) onPlayPress();
        }
    }

    void handleGridEditorClick (int mx, int my)
    {
        auto panelArea = getPanelBounds().reduced (10);
        const int btnW = 50;
        const int btnH = 46;
        int centreY  = panelArea.getCentreY();
        int btnY     = centreY - btnH / 2;
        // Use the same centred starting X as paintGridEditor.
        int x        = gridContentX (panelArea);
        int sepX     = x + 70;
        int bx       = sepX + 10;
        int nx       = bx + 2 * (btnW - 2) + 8;

        juce::Rectangle<int> setRect (bx,           btnY, btnW, btnH);
        juce::Rectangle<int> delRect (bx + btnW - 2, btnY, btnW, btnH);
        juce::Rectangle<int> nudgeLL (nx,                 btnY, btnW, btnH);
        juce::Rectangle<int> nudgeL  (nx +   (btnW-2),   btnY, btnW, btnH);
        juce::Rectangle<int> nudgeR  (nx + 2*(btnW-2),   btnY, btnW, btnH);
        juce::Rectangle<int> nudgeRR (nx + 3*(btnW-2),   btnY, btnW, btnH);

        if (setRect.contains (mx, my))  { if (onGridSet)   onGridSet(); }
        else if (delRect.contains (mx, my)) { if (onGridDelete) onGridDelete(); }
        else if (nudgeLL.contains (mx, my)) { if (onGridNudge) onGridNudge (-2); }
        else if (nudgeL.contains  (mx, my)) { if (onGridNudge) onGridNudge (-1); }
        else if (nudgeR.contains  (mx, my)) { if (onGridNudge) onGridNudge (+1); }
        else if (nudgeRR.contains (mx, my)) { if (onGridNudge) onGridNudge (+2); }
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
            if (newTab != activeTab)
            {
                activeTab = newTab;
                juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer (this)]()
                {
                    if (safeThis != nullptr)
                    {
                        safeThis->updateTabVisibility();
                        safeThis->resized();
                        safeThis->repaint();
                    }
                });
            }
        }
    }

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    juce::ValueTree tree;
    juce::String    activeTab { "loop" };

    LoopControlComponent* loopControl  = nullptr;
    HotCuePadComponent*   hotCuePads   = nullptr;
    BeatJumpComponent*    beatJumpCtrl = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControllerWidget)
};
