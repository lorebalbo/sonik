#pragma once
//==============================================================================
// PRD-0096: Session lifecycle dialogs, rendered in the DESIGN.md monochrome
// "1-Bit Deck" language — strict #2d2d2d / #fdfdfd palette, Space Mono
// (default monospaced) typeface, 2px solid #2d2d2d button borders with
// active/inactive fill inversion, ZERO border-radius, and a dithered (2px
// checkerboard, zero-blur) drop shadow. No juce::AlertWindow is used because
// its native chrome cannot satisfy the design system.
//
// Two presenters:
//   * showUnsavedChangesPrompt — modal Save / Don't Save / Cancel, delivering
//     a Daw::Session::UnsavedChoice asynchronously.
//   * showSessionError — modal single-button error notice (unreadable / corrupt
//     / future-version file, or a save failure).
//
// Message/UI thread only. The dialogs own themselves while modal and self-
// delete on dismissal (deleteWhenDismissed), so callers fire-and-forget.
//==============================================================================

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../SessionController.h"

namespace Daw::Session::Ui
{
    //--------------------------------------------------------------------------
    // Presents the modal unsaved-changes prompt centred over `parent` (or the
    // main desktop when parent is null). The callback is always invoked exactly
    // once with the chosen branch; closing via Esc / the dim background maps to
    // Cancel (the zero-side-effect abort, §1.5.2).
    //--------------------------------------------------------------------------
    void showUnsavedChangesPrompt (juce::Component* parent,
                                   const juce::String& sessionTitle,
                                   std::function<void (UnsavedChoice)> onChoice);

    //--------------------------------------------------------------------------
    // Presents a modal monochrome error notice with a single OK button.
    //--------------------------------------------------------------------------
    void showSessionError (juce::Component* parent,
                           const juce::String& title,
                           const juce::String& message,
                           std::function<void()> onDismissed = {});
}
