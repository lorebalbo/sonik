#pragma once
//==============================================================================
// PRD-0097: Unresolved Sources — the session-scoped batch resolution step.
//
// Lists exactly ONE row per missing SOURCE (not per clip, §1.3.2), each showing
// the source display name, kind, broken last-known path, and the count of clips
// that reference it. Each row offers a kind-appropriate action:
//   * Library / External -> "Relocate…" — reuses the PRD-0039 FileChooser +
//     canonical-path dedup via SessionSourceResolution::relocateSource. A single
//     relocate rebinds ALL referencing clips (§1.5.6).
//   * StemCache          -> "Re-derive Stems" — re-runs EPIC-0002 separation for
//     the parent via SessionSourceResolution::reDeriveStems, falling back to a
//     parent relocate first when the parent track is itself missing (§1.5.5).
//
// On a successful relocate/re-derive the source flips to Resolved, its clips lose
// the Glitch treatment, the snapshot recompiles to admit them, and the row is
// removed; when the list empties the dialog closes.
//
// Rendered in the DESIGN.md monochrome "1-Bit Deck" language: strict
// #2d2d2d / #fdfdfd, Space Mono, 2px solid borders, zero radius, dithered drop
// shadow. Message/UI thread only. Self-deletes on dismissal.
//==============================================================================

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

namespace Daw::Session
{
    class SessionSourceResolution;
}

namespace Daw::Session::Ui
{
    //--------------------------------------------------------------------------
    // Presents the modal batch step centred over `parent`. `resolution` is the
    // live integration object (not owned); the dialog drives relocate/re-derive
    // through it and re-reads the missing-source list after each fix. `onClosed`
    // (optional) fires once when the dialog dismisses, so the host can refresh
    // its banner / re-evaluate gating.
    //--------------------------------------------------------------------------
    void showUnresolvedSourcesStep (juce::Component* parent,
                                    SessionSourceResolution& resolution,
                                    std::function<void()> onClosed = {});

    //--------------------------------------------------------------------------
    // Test seam (non-behavior-changing): builds the dialog component over the
    // given resolution WITHOUT presenting it modally / grabbing focus / touching
    // the desktop, and returns ownership to the caller. Production never calls
    // this — it exists only so the dialog's construction, row layout, and paint
    // surface can be exercised headlessly (modal presentation needs a real run
    // loop and is not headlessly automatable). The returned component is a
    // self-contained, non-modal copy of the same dialog class production presents.
    //--------------------------------------------------------------------------
    std::unique_ptr<juce::Component>
        createUnresolvedSourcesStepForTest (SessionSourceResolution& resolution);
}
