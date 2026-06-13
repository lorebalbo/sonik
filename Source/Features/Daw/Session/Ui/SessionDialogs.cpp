//==============================================================================
// PRD-0096: Session lifecycle dialogs implementation (DESIGN.md monochrome).
//==============================================================================

#include "SessionDialogs.h"
#include "Features/Shared/Ui/SonikDraw.h"

namespace Daw::Session::Ui
{
namespace
{
    constexpr juce::uint32 kInkARGB     = 0xFF2D2D2D; // primary
    constexpr juce::uint32 kSurfaceARGB = 0xFFFDFDFD; // surface

    const juce::Colour kInk     { kInkARGB };
    const juce::Colour kSurface { kSurfaceARGB };

    juce::Font monoFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height,
                                              bold ? juce::Font::bold : juce::Font::plain));
    }

    //--------------------------------------------------------------------------
    // A flat, square, 2px-bordered monochrome button with active/inactive fill
    // inversion (DESIGN.md §5). "active" here means hovered/pressed (instant,
    // no fade), giving the DJ zero-latency tactile feedback.
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
            const bool active = highlighted || down;
            auto r = getLocalBounds().toFloat();

            g.setColour (active ? kInk : kSurface);
            g.fillRect (r);

            g.setColour (kInk);
            g.drawRect (r, 2.0f);                 // 2px solid border, zero radius

            g.setColour (active ? kSurface : kInk);
            g.setFont (monoFont (12.0f, true));
            g.drawText (getButtonText(), getLocalBounds(),
                        juce::Justification::centred, false);
        }
    };

    //--------------------------------------------------------------------------
    // The dialog surface: a monochrome panel carrying a title, a body message,
    // and one or more MonoButtons. Drawn with a dithered (2px checkerboard,
    // zero-blur) drop shadow per DESIGN.md §4. Self-deletes on dismissal.
    //--------------------------------------------------------------------------
    class MonoDialog final : public juce::Component
    {
    public:
        MonoDialog (juce::String titleIn, juce::String messageIn,
                    juce::StringArray buttonLabels,
                    std::function<void (int)> onButtonIn)
            : title (std::move (titleIn)),
              message (std::move (messageIn)),
              onButton (std::move (onButtonIn))
        {
            for (int i = 0; i < buttonLabels.size(); ++i)
            {
                auto* b = new MonoButton (buttonLabels[i]);
                const int index = i;
                b->onClick = [this, index] { dismissWith (index); };
                addAndMakeVisible (b);
                buttons.add (b);
            }

            const int width  = 420 + kShadowOffset;
            const int height = 188 + kShadowOffset;
            setSize (width, height);
            setWantsKeyboardFocus (true);
        }

        void paint (juce::Graphics& g) override
        {
            auto panel = panelBounds();

            // Dithered drop shadow: a 2px-offset 50% checkerboard of ink, zero
            // blur (DESIGN.md §4 "The Dithered Drop").
            paintDitheredShadow (g, panel);

            // Panel body.
            g.setColour (kSurface);
            g.fillRect (panel);
            g.setColour (kInk);
            g.drawRect (panel, 2);

            auto inner = panel.reduced (16);

            // Title.
            g.setFont (monoFont (15.0f, true));
            g.drawText (title.toUpperCase(),
                        inner.removeFromTop (24),
                        juce::Justification::centredLeft, false);

            inner.removeFromTop (8);

            // Body message (wrapped).
            g.setFont (monoFont (12.0f));
            g.drawFittedText (message,
                              inner.removeFromTop (inner.getHeight() - kButtonHeight - 12),
                              juce::Justification::topLeft, 4);
        }

        void resized() override
        {
            auto panel = panelBounds();
            auto row = panel.reduced (16);
            row = row.removeFromBottom (kButtonHeight);

            // Right-align the buttons: last label (e.g. Save / OK) sits rightmost.
            const int gap = 8;
            int x = row.getRight();
            for (int i = buttons.size(); --i >= 0;)
            {
                const int w = buttonWidth (buttons[i]->getButtonText());
                x -= w;
                buttons[i]->setBounds (x, row.getY(), w, kButtonHeight);
                x -= gap;
            }
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::escapeKey)
            {
                dismissWith (cancelIndex);
                return true;
            }
            if (key == juce::KeyPress::returnKey && ! buttons.isEmpty())
            {
                dismissWith (buttons.size() - 1); // confirm = last (Save / OK)
                return true;
            }
            return false;
        }

        void inputAttemptWhenModal() override
        {
            dismissWith (cancelIndex); // clicking the dim background = Cancel
        }

        // The index reported when the dialog is dismissed without a button
        // click (Esc / background). For prompts this is the Cancel button.
        int cancelIndex { -1 };

    private:
        static constexpr int kButtonHeight = 30;
        static constexpr int kShadowOffset = 4;

        juce::Rectangle<int> panelBounds() const
        {
            return getLocalBounds().withTrimmedRight (kShadowOffset)
                                   .withTrimmedBottom (kShadowOffset);
        }

        static int buttonWidth (const juce::String& label)
        {
            return juce::jmax (84, label.length() * 11 + 28);
        }

        static void paintDitheredShadow (juce::Graphics& g, juce::Rectangle<int> panel)
        {
            sonik::ui::draw::drawDitheredShadow (g, panel, kShadowOffset);
        }

        void dismissWith (int index)
        {
            if (dismissed)
                return;
            dismissed = true;

            auto cb = onButton;
            // Exit modal first so the callback can present a follow-on dialog.
            if (isCurrentlyModal())
                exitModalState (index);
            setVisible (false);
            if (auto* parent = getParentComponent())
                parent->removeChildComponent (this);
            removeFromDesktop();

            if (cb)
                cb (index);

            // Defer self-deletion off the current call stack so neither the JUCE
            // modal manager nor the callback (which may present a follow-on
            // dialog) is unwound while `this` is being destroyed.
            juce::MessageManager::callAsync ([self = this] { delete self; });
        }

        juce::String title, message;
        juce::OwnedArray<MonoButton> buttons;
        std::function<void (int)> onButton;
        bool dismissed { false };
    };

    void presentModal (MonoDialog* dialog, juce::Component* parent)
    {
        if (parent != nullptr)
        {
            // Centre over the parent's top-level window.
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
            if (auto* display = juce::Desktop::getInstance().getDisplays()
                                    .getPrimaryDisplay())
                dialog->setCentrePosition (display->userArea.getCentre());
        }

        dialog->setVisible (true);
        dialog->toFront (true);
        dialog->grabKeyboardFocus();
        dialog->enterModalState (true, nullptr, false);
    }
} // namespace

void showUnsavedChangesPrompt (juce::Component* parent,
                               const juce::String& sessionTitle,
                               std::function<void (UnsavedChoice)> onChoice)
{
    juce::StringArray labels { "Cancel", "Don't Save", "Save" };

    auto* dialog = new MonoDialog (
        "Unsaved Changes",
        "\"" + sessionTitle + "\" has unsaved changes.\n"
        "Do you want to save them before continuing?",
        labels,
        [onChoice] (int index)
        {
            if (! onChoice)
                return;
            switch (index)
            {
                case 2:  onChoice (UnsavedChoice::Save);     break;
                case 1:  onChoice (UnsavedChoice::DontSave); break;
                case 0:
                default: onChoice (UnsavedChoice::Cancel);   break;
            }
        });

    dialog->cancelIndex = 0; // Esc / background => Cancel
    presentModal (dialog, parent);
}

void showSessionError (juce::Component* parent,
                       const juce::String& title,
                       const juce::String& message,
                       std::function<void()> onDismissed)
{
    auto* dialog = new MonoDialog (
        title, message, juce::StringArray { "OK" },
        [onDismissed] (int) { if (onDismissed) onDismissed(); });

    dialog->cancelIndex = 0; // only one button
    presentModal (dialog, parent);
}

} // namespace Daw::Session::Ui
