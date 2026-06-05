#pragma once
//==============================================================================
// PRD-0101: ExportDialog — the human-facing capstone of the DAW export pipeline.
//
// A single DESIGN.md-compliant monochrome MODAL organism with several in-place
// states sharing ONE border / font / dither language:
//   - Options : format / sample rate / bit depth | MP3 bitrate / range /
//               output path / normalize. Format-driven enable/disable computed
//               LIVE; disabled controls dimmed (tonal layering), never hidden.
//   - Progress: a dithered checkerboard progress bar (density increases with
//               completion, 2px border, zero radius) + Space Mono percentage,
//               elapsed, and estimated-remaining time + a live Cancel button.
//   - Done    : the output path + a Close button.
//   - Error   : a Space Mono message block replacing the bar (never a native
//               alert), with Retry / Cancel.
//
// It composes the PRD-0099 OfflineRenderDriver + PRD-0100 AudioExporter through
// the headless ExportRunner (PRD-0101 §1.5.8) and adds NO render/encode logic.
// The render runs on a custom juce::Thread (NOT ThreadWithProgressWindow, per
// §1.5.1): the thread writes an std::atomic<float> progress + reads an
// std::atomic<bool> cancel and marshals UI updates via MessageManager::callAsync.
//
// The capability table (PRD-0100 AudioExporter::capabilitiesFor / isSupported /
// supportedSampleRates) is the SINGLE source of truth that populates every
// control, so the dialog can never offer an invalid combination.
//
// HEADLESS SEAMS (so the test agent can exercise it without entering modal
// state, mirroring PRD-0097's UnresolvedSourcesDialog):
//   - buildExportOptions()  : the option -> ExportOptions mapping (pure query).
//   - computeControlState() : the format-driven enable/disable computation.
//   - validateForExport()   : the on-Export validation (path/range), pure.
//   - createForTest()       : builds the dialog component WITHOUT presenting it.
//
// Message/UI thread only (the render thread is owned internally). Self-deletes on
// dismissal when presented modally.
//==============================================================================

#include <atomic>
#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../AudioExporter.h"
#include "../ExportOptions.h"
#include "../ExportRunner.h"
#include "../OfflineRenderDriver.h"
#include "../../Playback/ArrangementSnapshot.h"

namespace Daw::Export::Ui
{

//==============================================================================
// ExportContext — everything the dialog needs from the live app, injected so the
// dialog owns no singletons and the test can supply fakes (PRD-0101 wiring).
//==============================================================================

struct ExportContext
{
    /// The project / device sample rate used as the first-run default and as the
    /// rate the whole-arrangement length is expressed in.
    double projectSampleRate { 44100.0 };

    /// Builds the export Job (compiled snapshot at the chosen rate + a
    /// per-clip ReaderProvider) for the given options. Supplied by the host
    /// (production wraps a ClipSourceResolver; the test supplies in-memory
    /// readers). When null, the dialog cannot export (Export shows an error).
    std::function<ExportRunner::Job (const ExportOptions& options)> buildJob;

    /// Whether a region/loop is currently selected (drives the "Selected region"
    /// radio availability, §1.4). When false, "Whole arrangement" is forced.
    std::function<bool()> hasSelectedRegion;

    /// The currently-selected region/loop half-open bounds in samples at the
    /// EXPORT sample rate. Consulted only when hasSelectedRegion() is true.
    std::function<ExportRange (double exportSampleRate)> selectedRegion;

    /// PRD-0097 export gate: true iff every referenced source is Resolved. The
    /// dialog gates on this BEFORE starting a render (§1.4 missing-source bullet).
    std::function<bool()> areAllSourcesResolved;

    /// Routes the DJ to the PRD-0097 batch resolution step when a source is
    /// Missing (offered from the inline error state). Optional.
    std::function<void()> showUnresolvedSourcesStep;

    /// Names the missing sources for the inline error message (PRD-0097). Optional.
    std::function<juce::String()> describeMissingSources;

    /// App-wide last-used persistence store (PRD-0096's PropertiesFile). The
    /// output PATH is deliberately NOT persisted (§1.5.6). Optional (null => no
    /// persistence; first-run defaults always apply).
    juce::PropertiesFile* properties { nullptr };

    /// Captures the transport playhead + play-state before the export begins so
    /// Cancel can restore it (§1.5.2). Returns an opaque token the matching
    /// restore consumes. Optional (null => no transport to restore).
    std::function<juce::int64()> captureTransport;
    std::function<void (juce::int64)> restoreTransport;
};

//==============================================================================
// ExportControlState — the format-driven enable/disable result (§1.5.4). Pure
// data so a headless test can assert it without touching any widget.
//==============================================================================

struct ExportControlState
{
    bool bitDepthEnabled    { true };   // WAV / FLAC
    bool mp3BitrateEnabled  { false };  // MP3
    bool mp3VbrEnabled      { false };  // MP3
    bool selectedRegionEnabled { false };
};

//==============================================================================
// ExportValidation — the on-Export validation result (§1.5.4). Pure data.
//==============================================================================

struct ExportValidation
{
    bool         ok { true };
    juce::String message;   // populated when !ok; the inline error text

    static ExportValidation good()                         { return { true,  {} }; }
    static ExportValidation bad (const juce::String& why)  { return { false, why }; }
};

//==============================================================================
// ExportDialog
//==============================================================================

class ExportDialog final : public juce::Component,
                           private juce::Timer
{
public:
    explicit ExportDialog (ExportContext context);
    ~ExportDialog() override;

    //--------------------------------------------------------------------------
    // The four in-place states (§1.3 / §1.5.5). Test-queryable.
    //--------------------------------------------------------------------------
    enum class State { Options, Progress, Done, Error };
    State currentState() const noexcept { return state_; }

    //--------------------------------------------------------------------------
    // HEADLESS SEAMS (no modal state required).
    //--------------------------------------------------------------------------

    /// The option -> ExportOptions mapping (pure). Reflects the live control
    /// values, reconciles the output path's extension to the format, and resolves
    /// the export range (empty => whole arrangement). §1.5.8.
    ExportOptions buildExportOptions() const;

    /// The format-driven enable/disable computation (pure). §1.5.4.
    ExportControlState computeControlState() const;

    /// The on-Export validation: empty path, non-writable dir, and "Selected
    /// region" with no selection each fail here (pure; no render). §1.5.4.
    ExportValidation validateForExport() const;

    //--------------------------------------------------------------------------
    // Programmatic setters used by the headless test to drive the pure queries
    // without simulating mouse events on the modal widgets.
    //--------------------------------------------------------------------------
    void setFormatForTest (Format f);
    void setSampleRateForTest (double rate);
    void setBitDepthForTest (int bd);
    void setMp3BitrateForTest (int kbps);
    void setNormalizeForTest (bool on);
    void setRangeWholeForTest (bool whole);
    void setOutputFileForTest (const juce::File& f);

    /// The current last-used persistence keys (exposed so a test can assert the
    /// exact key strings round-trip through a PropertiesFile). §1.5.6.
    struct PersistenceKeys
    {
        static constexpr const char* format        = "export.format";
        static constexpr const char* sampleRate    = "export.sampleRate";
        static constexpr const char* bitDepth      = "export.bitDepth";
        static constexpr const char* mp3Bitrate    = "export.mp3Bitrate";
        static constexpr const char* mp3Vbr        = "export.mp3Vbr";
        static constexpr const char* rangeWhole    = "export.rangeWhole";
        static constexpr const char* normalize     = "export.normalize";
    };

    //--------------------------------------------------------------------------
    // juce::Component
    //--------------------------------------------------------------------------
    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void inputAttemptWhenModal() override {} // modal; clicking outside is inert

private:
    //--------------------------------------------------------------------------
    // The render-thread: a custom juce::Thread (NOT ThreadWithProgressWindow,
    // §1.5.1) that runs ExportRunner::runBlocking and marshals UI updates via
    // MessageManager::callAsync.
    //--------------------------------------------------------------------------
    class RenderThread;

    void rebuildForState();          // show/hide controls for the current state
    void layoutOptions (juce::Rectangle<int> inner);
    void layoutProgress (juce::Rectangle<int> inner);

    void refreshControlEnablement(); // apply computeControlState() to the widgets
    void onFormatChanged();
    void onChoosePath();             // launches the FileChooser
    void onExportPressed();          // validate -> gate -> start render thread
    void onCancelPressed();          // stop render, delete partial, restore xport
    void onClosePressed();           // dismiss

    void startRender (const ExportOptions& options);
    void finishRender (ExportResult result);          // message thread
    void updateProgressUi (float fraction);           // message thread
    void showError (const juce::String& message);     // inline error state
    void dismiss();

    void loadPersistedOptions();     // restore last-used (§1.5.6)
    void persistOptions();           // store last-used (output path excluded)

    static juce::String formatExtension (Format f);
    static juce::String formatName (Format f);

    //--------------------------------------------------------------------------
    ExportContext context_;
    AudioExporter exporter_;         // the capability table source of truth

    State state_ { State::Options };

    // Options-state controls.
    std::unique_ptr<juce::ComboBox> formatBox_;
    std::unique_ptr<juce::ComboBox> sampleRateBox_;
    std::unique_ptr<juce::ComboBox> bitDepthBox_;
    std::unique_ptr<juce::ComboBox> mp3BitrateBox_;
    std::unique_ptr<juce::ToggleButton> mp3VbrToggle_;
    std::unique_ptr<juce::ToggleButton> normalizeToggle_;
    std::unique_ptr<juce::ToggleButton> rangeWholeButton_;
    std::unique_ptr<juce::ToggleButton> rangeRegionButton_;
    std::unique_ptr<juce::Button> choosePathButton_;
    std::unique_ptr<juce::Button> exportButton_;
    std::unique_ptr<juce::Button> closeButtonOptions_;

    // Progress/Done/Error-state controls.
    std::unique_ptr<juce::Button> cancelButton_;
    std::unique_ptr<juce::Button> doneCloseButton_;
    std::unique_ptr<juce::Button> errorRetryButton_;
    std::unique_ptr<juce::Button> errorResolveButton_;
    std::unique_ptr<juce::Button> errorCancelButton_;

    juce::File   outputFile_;
    juce::String errorMessage_;
    juce::String donePath_;

    std::unique_ptr<juce::FileChooser> fileChooser_;

    // Cross-thread render signalling (lock-free; §1.5.1).
    std::atomic<float> progress_ { 0.0f };
    std::atomic<bool>  cancel_   { false };
    std::unique_ptr<RenderThread> renderThread_;

    // ETA smoothing (§1.5.3): wall-clock extrapolation over a short window.
    double  renderStartTimeMs_ { 0.0 };
    double  smoothedRemainingMs_ { -1.0 };
    juce::int64 transportToken_ { 0 };
    bool    transportCaptured_ { false };

    // Timer drives the progress-bar repaint at a steady cadence.
    void timerCallback() override;

    // DESIGN.md palette.
    static inline const juce::Colour kInk     { 0xFF2D2D2D };
    static inline const juce::Colour kSurface { 0xFFFDFDFD };
    static inline const juce::Colour kDim     { 0xFFF3F3F4 }; // container-low (tonal dim)
    static inline const juce::Colour kHeader  { 0xFFE2E2E2 }; // container-highest

    static constexpr int kShadowOffset = 4;

    juce::Rectangle<int> panelBounds() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportDialog)
};

//==============================================================================
// Presentation entry point (production) + headless test seam.
//==============================================================================

/// Presents the dialog modally centred over `parent`. Self-deletes on dismissal.
void showExportDialog (juce::Component* parent, ExportContext context);

/// Test seam (non-behavior-changing): builds the dialog WITHOUT presenting it
/// modally / grabbing focus / touching the desktop, and returns ownership to the
/// caller. Production never calls this — it exists so the dialog's construction,
/// the option mapping, the format-driven enable/disable, and the validation can
/// be exercised headlessly (modal presentation needs a real run loop). Mirrors
/// PRD-0097's createUnresolvedSourcesStepForTest.
std::unique_ptr<ExportDialog> createExportDialogForTest (ExportContext context);

} // namespace Daw::Export::Ui
