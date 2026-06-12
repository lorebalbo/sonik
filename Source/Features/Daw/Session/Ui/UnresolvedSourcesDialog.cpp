//==============================================================================
// PRD-0097: Unresolved Sources batch step implementation (DESIGN.md monochrome).
//==============================================================================

#include "UnresolvedSourcesDialog.h"

#include "SessionDialogs.h"
#include "../SessionSourceResolution.h"
#include "../SourceIdResolver.h"

namespace Daw::Session::Ui
{
namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D }; // primary
    const juce::Colour kSurface { 0xFFFDFDFD }; // surface
    const juce::Colour kRowAlt  { 0xFFF3F3F4 }; // container-low (zebra striping)

    juce::Font monoFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height,
                                              bold ? juce::Font::bold : juce::Font::plain));
    }

    //--------------------------------------------------------------------------
    // Flat, square, 2px-bordered monochrome button with active/inactive fill
    // inversion (DESIGN.md §5). Shared visual with SessionDialogs' MonoButton.
    //--------------------------------------------------------------------------
    class MonoButton final : public juce::Button
    {
    public:
        explicit MonoButton (const juce::String& text) : juce::Button (text)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const bool active = (highlighted || down) && isEnabled();
            auto r = getLocalBounds().toFloat();

            g.setColour (! isEnabled() ? kRowAlt : (active ? kInk : kSurface));
            g.fillRect (r);
            g.setColour (kInk);
            g.drawRect (r, 2.0f);
            g.setColour (! isEnabled() ? kInk.withAlpha (0.4f) : (active ? kSurface : kInk));
            g.setFont (monoFont (11.0f, true));
            g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
        }
    };

    //--------------------------------------------------------------------------
    // One row: display name + kind + broken path + clip count, plus the action.
    //--------------------------------------------------------------------------
    class SourceRow final : public juce::Component
    {
    public:
        SourceRow (const ResolvedSource& src, bool altShade, std::function<void()> onAction)
            : source (src), alt (altShade)
        {
            const bool isStem = (src.kind == SourceKind::StemCache);
            action = std::make_unique<MonoButton> (isStem ? "Re-derive Stems" : "Relocate...");
            action->onClick = [handler = std::move (onAction)] { if (handler) handler(); };
            addAndMakeVisible (*action);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            g.setColour (alt ? kRowAlt : kSurface);
            g.fillRect (r);

            auto text = r.reduced (10, 6).withTrimmedRight (kActionWidth + 16);

            // Display name (bold) + clip count badge.
            g.setColour (kInk);
            g.setFont (monoFont (12.0f, true));
            const juce::String head = source.displayName
                + "   [" + juce::String (source.clipCount)
                + (source.clipCount == 1 ? " clip]" : " clips]");
            g.drawText (head, text.removeFromTop (16), juce::Justification::centredLeft, false);

            // Kind + broken last-known path (smaller, secondary).
            g.setFont (monoFont (10.0f));
            const juce::String kindStr =
                source.kind == SourceKind::Library   ? "LIBRARY"
              : source.kind == SourceKind::External  ? "EXTERNAL"
                                                     : "STEM CACHE";
            const juce::String path = source.lastKnownPath.isNotEmpty()
                ? source.lastKnownPath : juce::String ("<no stored path>");
            g.drawText (kindStr + "  -  " + path,
                        text.removeFromTop (14), juce::Justification::centredLeft, false);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10, 8);
            action->setBounds (r.removeFromRight (kActionWidth)
                                  .withSizeKeepingCentre (kActionWidth, 26));
        }

        static constexpr int kRowHeight   = 44;
        static constexpr int kActionWidth = 130;

        ResolvedSource source;

    private:
        bool alt;
        std::unique_ptr<MonoButton> action;
    };

    //--------------------------------------------------------------------------
    // The batch dialog: title + count, a scrollable list of source rows, and a
    // Close button. Re-reads the missing-source list after each fix; closes when
    // the list empties. Dithered drop shadow, zero radius. Self-deletes.
    //--------------------------------------------------------------------------
    class UnresolvedSourcesDialog final : public juce::Component
    {
    public:
        UnresolvedSourcesDialog (SessionSourceResolution& res, std::function<void()> onClosedIn)
            : resolution (res), onClosed (std::move (onClosedIn))
        {
            listHolder.setInterceptsMouseClicks (false, true);
            viewport.setViewedComponent (&listHolder, false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);

            closeButton = std::make_unique<MonoButton> ("Close");
            closeButton->onClick = [this] { dismiss(); };
            addAndMakeVisible (*closeButton);

            rebuildRows();

            const int width  = 560 + kShadowOffset;
            const int height = 420 + kShadowOffset;
            setSize (width, height);
            setWantsKeyboardFocus (true);
        }

        void paint (juce::Graphics& g) override
        {
            auto panel = panelBounds();
            paintDitheredShadow (g, panel);

            g.setColour (kSurface);
            g.fillRect (panel);
            g.setColour (kInk);
            g.drawRect (panel, 2);

            auto inner = panel.reduced (16);

            g.setFont (monoFont (15.0f, true));
            g.drawText ("UNRESOLVED SOURCES", inner.removeFromTop (24),
                        juce::Justification::centredLeft, false);

            inner.removeFromTop (4);
            g.setFont (monoFont (11.0f));
            const int n = resolution.missingSourceCount();
            g.drawText (juce::String (n) + (n == 1 ? " source could not be resolved."
                                                   : " sources could not be resolved.")
                        + "  Relocate or re-derive each to enable playback and export.",
                        inner.removeFromTop (18), juce::Justification::centredLeft, false);
        }

        void resized() override
        {
            auto panel = panelBounds();
            auto inner = panel.reduced (16);
            inner.removeFromTop (24 + 4 + 18 + 8); // title + count + gap

            auto buttonRow = inner.removeFromBottom (30);
            closeButton->setBounds (buttonRow.removeFromRight (96).withHeight (30));

            inner.removeFromBottom (10);
            viewport.setBounds (inner);
            layoutRows();
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::escapeKey)
            {
                dismiss();
                return true;
            }
            return false;
        }

        void inputAttemptWhenModal() override { /* clicking outside is harmless; keep open */ }

    private:
        static constexpr int kShadowOffset = 4;

        juce::Rectangle<int> panelBounds() const
        {
            return getLocalBounds().withTrimmedRight (kShadowOffset)
                                   .withTrimmedBottom (kShadowOffset);
        }

        void rebuildRows()
        {
            rows.clear();
            listHolder.removeAllChildren();

            const auto missing = resolution.missingSources();
            for (int i = 0; i < (int) missing.size(); ++i)
            {
                const auto& src = missing[(size_t) i];
                const juce::String id = src.sourceFileId;
                auto* row = new SourceRow (src, (i % 2) == 1,
                    [this, id] { handleAction (id); });
                listHolder.addAndMakeVisible (row);
                rows.add (row);
            }

            layoutRows();
            repaint();
        }

        void layoutRows()
        {
            const int w = juce::jmax (1, viewport.getWidth() - viewport.getScrollBarThickness());
            int y = 0;
            for (auto* row : rows)
            {
                row->setBounds (0, y, w, SourceRow::kRowHeight);
                y += SourceRow::kRowHeight;
            }
            listHolder.setSize (w, juce::jmax (y, viewport.getHeight()));
        }

        void handleAction (const juce::String& sourceFileId)
        {
            const auto missing = resolution.missingSources();
            const ResolvedSource* src = nullptr;
            for (const auto& s : missing)
                if (s.sourceFileId == sourceFileId) { src = &s; break; }
            if (src == nullptr)
                return;

            if (src->kind == SourceKind::StemCache)
            {
                // Re-derive from parent; if the parent track itself is missing,
                // fall back to relocating the parent file first (§1.5.5).
                const bool started = resolution.reDeriveStems (sourceFileId,
                    [safe = juce::Component::SafePointer<UnresolvedSourcesDialog> (this)] (bool)
                    {
                        if (safe != nullptr)
                            safe->refreshAfterFix();
                    });

                if (! started)
                    launchRelocate (sourceFileId); // relocate the missing parent first
                return;
            }

            launchRelocate (sourceFileId);
        }

        void launchRelocate (const juce::String& sourceFileId)
        {
            // PRD-0039 machinery: FileChooser "Choose replacement file", no
            // format filter, openMode + canSelectFiles.
            auto chooser = std::make_shared<juce::FileChooser> (
                "Choose replacement file", juce::File{}, juce::String());

            chooser->launchAsync (
                juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [safe = juce::Component::SafePointer<UnresolvedSourcesDialog> (this),
                 sourceFileId, chooser] (const juce::FileChooser& fc)
                {
                    if (safe == nullptr)
                        return;

                    const auto result = fc.getResult();
                    if (! result.existsAsFile())
                        return;

                    bool dedupRejected = false;
                    const bool ok = safe->resolution.relocateSource (sourceFileId, result, &dedupRejected);

                    if (! ok && dedupRejected)
                    {
                        showSessionError (safe.getComponent(), "Relocate Source",
                            "This file is already in your library under a different track.");
                        return;
                    }

                    safe->refreshAfterFix();
                });
        }

        void refreshAfterFix()
        {
            if (resolution.missingSourceCount() == 0)
            {
                dismiss();
                return;
            }
            rebuildRows();
        }

        void dismiss()
        {
            if (dismissed)
                return;
            dismissed = true;

            auto cb = onClosed;
            if (isCurrentlyModal())
                exitModalState (0);
            setVisible (false);
            if (auto* parent = getParentComponent())
                parent->removeChildComponent (this);
            removeFromDesktop();

            if (cb)
                cb();

            juce::MessageManager::callAsync ([self = this] { delete self; });
        }

        static void paintDitheredShadow (juce::Graphics& g, juce::Rectangle<int> panel)
        {
            auto shadow = panel.translated (kShadowOffset, kShadowOffset);
            g.saveState();
            g.reduceClipRegion (shadow);
            g.setColour (kInk);
            for (int y = shadow.getY(); y < shadow.getBottom(); y += 2)
                for (int x = shadow.getX() + ((y / 2) % 2) * 2; x < shadow.getRight(); x += 4)
                    g.fillRect (x, y, 2, 2);
            g.restoreState();
        }

        SessionSourceResolution& resolution;
        std::function<void()>    onClosed;

        juce::Viewport             viewport;
        juce::Component            listHolder;
        juce::OwnedArray<SourceRow> rows;
        std::unique_ptr<MonoButton> closeButton;
        bool dismissed { false };
    };

    void presentModal (UnresolvedSourcesDialog* dialog, juce::Component* parent)
    {
        if (parent != nullptr)
        {
            if (auto* top = parent->getTopLevelComponent())
            {
                top->addAndMakeVisible (dialog);
                // Centre within the top-level's LOCAL bounds (child coordinate space).
                dialog->setBounds (top->getLocalBounds()
                                       .withSizeKeepingCentre (dialog->getWidth(),
                                                               dialog->getHeight()));
            }
            else
            {
                parent->addAndMakeVisible (dialog);
                dialog->setCentrePosition (parent->getLocalBounds().getCentre());
            }
        }
        else
        {
            dialog->addToDesktop (juce::ComponentPeer::windowHasDropShadow);
            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                dialog->setCentrePosition (display->userArea.getCentre());
        }

        dialog->setVisible (true);
        dialog->toFront (true);
        dialog->grabKeyboardFocus();
        dialog->enterModalState (true, nullptr, false);
    }
} // namespace

void showUnresolvedSourcesStep (juce::Component* parent,
                                SessionSourceResolution& resolution,
                                std::function<void()> onClosed)
{
    if (resolution.missingSourceCount() == 0)
    {
        if (onClosed)
            onClosed();
        return;
    }

    auto* dialog = new UnresolvedSourcesDialog (resolution, std::move (onClosed));
    presentModal (dialog, parent);
}

std::unique_ptr<juce::Component>
    createUnresolvedSourcesStepForTest (SessionSourceResolution& resolution)
{
    // Construct the identical dialog class production presents, but hand back
    // plain ownership: never presented modally, never added to the desktop, never
    // grabs focus. A test can size it, paint it offscreen, then delete it.
    auto dialog = std::make_unique<UnresolvedSourcesDialog> (resolution, nullptr);
    dialog->setVisible (true);
    return dialog;
}

} // namespace Daw::Session::Ui
