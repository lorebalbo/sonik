//==============================================================================
// PRD-0101: ExportDialog implementation (DESIGN.md monochrome "1-Bit Deck").
//==============================================================================

#include "ExportDialog.h"

namespace Daw::Export::Ui
{
namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };
    const juce::Colour kDim     { 0xFFF3F3F4 };
    const juce::Colour kHeader  { 0xFFE2E2E2 };

    juce::Font monoFont (float height, bool bold = false)
    {
        return juce::Font (juce::Font::getDefaultMonospacedFontName(), height,
                           bold ? juce::Font::bold : juce::Font::plain);
    }

    //--------------------------------------------------------------------------
    // Flat, square, 2px-bordered monochrome button with active/inactive fill
    // inversion (DESIGN.md §5). Disabled => dimmed via tonal layering.
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

            g.setColour (! isEnabled() ? kDim : (active ? kInk : kSurface));
            g.fillRect (r);
            g.setColour (! isEnabled() ? kInk.withAlpha (0.35f) : kInk);
            g.drawRect (r, 2.0f);
            g.setColour (! isEnabled() ? kInk.withAlpha (0.4f) : (active ? kSurface : kInk));
            g.setFont (monoFont (11.0f, true));
            g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
        }
    };

    //--------------------------------------------------------------------------
    // A monochrome two-state radio toggle (used as a "radio" with manual mutual
    // exclusion for the export-range pair). 2px border, square, fill inversion.
    //--------------------------------------------------------------------------
    class MonoToggle final : public juce::ToggleButton
    {
    public:
        explicit MonoToggle (const juce::String& text) : juce::ToggleButton (text)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paintButton (juce::Graphics& g, bool /*highlighted*/, bool /*down*/) override
        {
            const bool on = getToggleState();
            auto r = getLocalBounds();

            // Square indicator box on the left.
            auto box = r.removeFromLeft (r.getHeight()).reduced (3).toFloat();
            g.setColour (! isEnabled() ? kDim : (on ? kInk : kSurface));
            g.fillRect (box);
            g.setColour (! isEnabled() ? kInk.withAlpha (0.35f) : kInk);
            g.drawRect (box, 2.0f);

            g.setColour (! isEnabled() ? kInk.withAlpha (0.4f) : kInk);
            g.setFont (monoFont (11.0f));
            g.drawText (getButtonText(), r.reduced (6, 0),
                        juce::Justification::centredLeft, false);
        }
    };

    //--------------------------------------------------------------------------
    // DESIGN.md monochrome look-and-feel for the ComboBoxes (no rounded JUCE
    // chrome, no colour). 2px ink border, square, Space Mono, dimmed when
    // disabled via tonal layering.
    //--------------------------------------------------------------------------
    class MonoLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        void drawComboBox (juce::Graphics& g, int width, int height, bool,
                           int, int, int, int, juce::ComboBox& box) override
        {
            auto r = juce::Rectangle<int> (0, 0, width, height).toFloat();
            g.setColour (box.isEnabled() ? kSurface : kDim);
            g.fillRect (r);
            g.setColour (box.isEnabled() ? kInk : kInk.withAlpha (0.35f));
            g.drawRect (r, 2.0f);

            // A pixel-art down chevron at the right (no anti-aliased icon).
            const int s = 4;
            const int cx = width - 14;
            const int cy = height / 2 - 2;
            g.setColour (box.isEnabled() ? kInk : kInk.withAlpha (0.4f));
            for (int i = 0; i < 3; ++i)
                g.fillRect (cx - i + 2, cy + i, s + (2 - i) * 2 - s + 2, 2);
        }

        juce::Font getComboBoxFont (juce::ComboBox&) override { return monoFont (11.0f); }
        juce::Font getPopupMenuFont() override                { return monoFont (11.0f); }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (6, 1, box.getWidth() - 22, box.getHeight() - 2);
            label.setFont (monoFont (11.0f));
            label.setColour (juce::Label::textColourId,
                             box.isEnabled() ? kInk : kInk.withAlpha (0.4f));
        }
    };

    MonoLookAndFeel& sharedLookAndFeel()
    {
        static MonoLookAndFeel laf;
        return laf;
    }

    //--------------------------------------------------------------------------
    // Dithered drop shadow (DESIGN.md §4: 4px offset, 50% checkerboard, zero blur).
    //--------------------------------------------------------------------------
    void paintDitheredShadow (juce::Graphics& g, juce::Rectangle<int> panel, int offset)
    {
        auto shadow = panel.translated (offset, offset);
        g.saveState();
        g.reduceClipRegion (shadow);
        g.setColour (kInk);
        for (int y = shadow.getY(); y < shadow.getBottom(); y += 2)
            for (int x = shadow.getX() + ((y / 2) % 2) * 2; x < shadow.getRight(); x += 4)
                g.fillRect (x, y, 2, 2);
        g.restoreState();
    }

    juce::String formatElapsed (double ms)
    {
        if (ms < 0.0) return "--:--";
        const int totalSec = (int) (ms / 1000.0);
        return juce::String::formatted ("%02d:%02d", totalSec / 60, totalSec % 60);
    }
} // namespace

//==============================================================================
// RenderThread — custom juce::Thread (§1.5.1). Runs ExportRunner::runBlocking on
// a background thread, writes the atomic progress, polls the atomic cancel, and
// marshals the final result to the message thread via callAsync.
//==============================================================================
class ExportDialog::RenderThread final : public juce::Thread
{
public:
    RenderThread (ExportDialog& owner, ExportRunner::Job job)
        : juce::Thread ("Sonik Export Render"),
          owner_ (owner),
          job_ (std::move (job))
    {}

    void run() override
    {
        // The runner writes owner_.progress_ + reads owner_.cancel_ directly; the
        // onProgress lambda below is only used to nudge the message thread when a
        // fresh fraction lands (the timer also polls the atomic, so this is a
        // best-effort wake-up, not the sole update path).
        auto result = ExportRunner::runBlocking (
            job_,
            owner_.progress_,
            owner_.cancel_,
            {} /* progress nudges handled by the dialog's timer poll */,
            nullptr);

        // Marshal the outcome onto the message thread. The dialog is a modal
        // component owned by the desktop; guard with a SafePointer.
        juce::Component::SafePointer<ExportDialog> safe (&owner_);
        juce::MessageManager::callAsync ([safe, result]
        {
            if (safe != nullptr)
                safe->finishRender (result);
        });
    }

private:
    ExportDialog&     owner_;
    ExportRunner::Job job_;
};

//==============================================================================
// Construction
//==============================================================================
ExportDialog::ExportDialog (ExportContext context)
    : context_ (std::move (context))
{
    setLookAndFeel (&sharedLookAndFeel());

    //---- Options-state controls ------------------------------------------------
    formatBox_ = std::make_unique<juce::ComboBox>();
    formatBox_->addItem (formatName (Format::Wav),  1);
    formatBox_->addItem (formatName (Format::Flac), 2);
    {
        // MP3 only offered when this exporter's backend is actually available
        // (capability table single source of truth, §1.4 / PRD-0100).
        const auto mp3Caps = exporter_.capabilitiesFor (Format::Mp3);
        if (mp3Caps.supported)
            formatBox_->addItem (formatName (Format::Mp3), 3);
    }
    formatBox_->onChange = [this] { onFormatChanged(); };
    addAndMakeVisible (*formatBox_);

    sampleRateBox_ = std::make_unique<juce::ComboBox>();
    {
        int id = 1;
        for (double rate : AudioExporter::supportedSampleRates())
            sampleRateBox_->addItem (juce::String ((int) rate) + " Hz", id++);
    }
    addAndMakeVisible (*sampleRateBox_);

    bitDepthBox_ = std::make_unique<juce::ComboBox>();
    mp3BitrateBox_ = std::make_unique<juce::ComboBox>();
    addAndMakeVisible (*bitDepthBox_);
    addAndMakeVisible (*mp3BitrateBox_);

    mp3VbrToggle_   = std::make_unique<MonoToggle> ("VBR");
    normalizeToggle_ = std::make_unique<MonoToggle> ("NORMALIZE");
    addAndMakeVisible (*mp3VbrToggle_);
    addAndMakeVisible (*normalizeToggle_);

    rangeWholeButton_  = std::make_unique<MonoToggle> ("WHOLE ARRANGEMENT");
    rangeRegionButton_ = std::make_unique<MonoToggle> ("SELECTED REGION");
    rangeWholeButton_->setToggleState (true, juce::dontSendNotification);
    rangeWholeButton_->onClick = [this]
    {
        rangeWholeButton_->setToggleState (true, juce::dontSendNotification);
        if (rangeRegionButton_->isEnabled())
            rangeRegionButton_->setToggleState (false, juce::dontSendNotification);
    };
    rangeRegionButton_->onClick = [this]
    {
        rangeRegionButton_->setToggleState (true, juce::dontSendNotification);
        rangeWholeButton_->setToggleState (false, juce::dontSendNotification);
    };
    addAndMakeVisible (*rangeWholeButton_);
    addAndMakeVisible (*rangeRegionButton_);

    choosePathButton_ = std::make_unique<MonoButton> ("CHOOSE...");
    choosePathButton_->onClick = [this] { onChoosePath(); };
    addAndMakeVisible (*choosePathButton_);

    exportButton_ = std::make_unique<MonoButton> ("EXPORT");
    exportButton_->onClick = [this] { onExportPressed(); };
    addAndMakeVisible (*exportButton_);

    closeButtonOptions_ = std::make_unique<MonoButton> ("CANCEL");
    closeButtonOptions_->onClick = [this] { onClosePressed(); };
    addAndMakeVisible (*closeButtonOptions_);

    //---- Progress / Done / Error controls -------------------------------------
    cancelButton_ = std::make_unique<MonoButton> ("CANCEL");
    cancelButton_->onClick = [this] { onCancelPressed(); };
    addChildComponent (*cancelButton_);

    doneCloseButton_ = std::make_unique<MonoButton> ("CLOSE");
    doneCloseButton_->onClick = [this] { onClosePressed(); };
    addChildComponent (*doneCloseButton_);

    errorRetryButton_ = std::make_unique<MonoButton> ("BACK");
    errorRetryButton_->onClick = [this]
    {
        state_ = State::Options;
        rebuildForState();
    };
    addChildComponent (*errorRetryButton_);

    errorResolveButton_ = std::make_unique<MonoButton> ("RESOLVE SOURCES");
    errorResolveButton_->onClick = [this]
    {
        if (context_.showUnresolvedSourcesStep)
            context_.showUnresolvedSourcesStep();
    };
    addChildComponent (*errorResolveButton_);

    errorCancelButton_ = std::make_unique<MonoButton> ("CLOSE");
    errorCancelButton_->onClick = [this] { onClosePressed(); };
    addChildComponent (*errorCancelButton_);

    // Defaults: first-run = WAV / project rate / 24-bit / whole / normalize off
    // (§1.5.6). Populate the format-dependent boxes, then overlay any last-used.
    formatBox_->setSelectedId (1, juce::dontSendNotification); // WAV
    onFormatChanged(); // repopulates bit-depth/bitrate boxes + enablement

    // Default sample rate = project rate when present in the list, else first.
    {
        int selId = 1, id = 1;
        for (double rate : AudioExporter::supportedSampleRates())
        {
            if (std::abs (rate - context_.projectSampleRate) < 1.0)
                selId = id;
            ++id;
        }
        sampleRateBox_->setSelectedId (selId, juce::dontSendNotification);
    }

    loadPersistedOptions();
    refreshControlEnablement();

    const int width  = 480 + kShadowOffset;
    const int height = 360 + kShadowOffset;
    setSize (width, height);
    setWantsKeyboardFocus (true);

    rebuildForState();
    startTimerHz (15);
}

ExportDialog::~ExportDialog()
{
    stopTimer();

    // If a render is still running, request cancel and join so the thread never
    // outlives the dialog (its run() captures *this via SafePointer, but the Job
    // + atomics live in the dialog).
    if (renderThread_ != nullptr)
    {
        cancel_.store (true, std::memory_order_release);
        renderThread_->stopThread (5000);
        renderThread_.reset();
    }

    setLookAndFeel (nullptr);
}

//==============================================================================
// Persistence (§1.5.6) — last-used options app-wide, output PATH excluded.
//==============================================================================
void ExportDialog::loadPersistedOptions()
{
    auto* p = context_.properties;
    if (p == nullptr)
        return;

    // Format (only restore if its item exists — MP3 may be unavailable).
    if (p->containsKey (PersistenceKeys::format))
    {
        const int f = p->getIntValue (PersistenceKeys::format, 0);
        const int targetId = (f == (int) Format::Wav)  ? 1
                           : (f == (int) Format::Flac) ? 2
                           : (f == (int) Format::Mp3)  ? 3 : 1;
        // Only select if that item id was actually added.
        for (int i = 0; i < formatBox_->getNumItems(); ++i)
            if (formatBox_->getItemId (i) == targetId)
            {
                formatBox_->setSelectedId (targetId, juce::dontSendNotification);
                break;
            }
        onFormatChanged();
    }

    if (p->containsKey (PersistenceKeys::sampleRate))
    {
        const int rate = p->getIntValue (PersistenceKeys::sampleRate, 0);
        int id = 1;
        for (double r : AudioExporter::supportedSampleRates())
        {
            if ((int) r == rate)
            {
                sampleRateBox_->setSelectedId (id, juce::dontSendNotification);
                break;
            }
            ++id;
        }
    }

    if (p->containsKey (PersistenceKeys::bitDepth))
    {
        const int bd = p->getIntValue (PersistenceKeys::bitDepth, 24);
        for (int i = 0; i < bitDepthBox_->getNumItems(); ++i)
            if (bitDepthBox_->getItemText (i).getIntValue() == bd)
                bitDepthBox_->setSelectedId (bitDepthBox_->getItemId (i), juce::dontSendNotification);
    }

    if (p->containsKey (PersistenceKeys::mp3Bitrate))
    {
        const int kbps = p->getIntValue (PersistenceKeys::mp3Bitrate, 320);
        for (int i = 0; i < mp3BitrateBox_->getNumItems(); ++i)
            if (mp3BitrateBox_->getItemText (i).getIntValue() == kbps)
                mp3BitrateBox_->setSelectedId (mp3BitrateBox_->getItemId (i), juce::dontSendNotification);
    }

    mp3VbrToggle_->setToggleState (p->getBoolValue (PersistenceKeys::mp3Vbr, false),
                                   juce::dontSendNotification);
    normalizeToggle_->setToggleState (p->getBoolValue (PersistenceKeys::normalize, false),
                                      juce::dontSendNotification);

    const bool whole = p->getBoolValue (PersistenceKeys::rangeWhole, true);
    rangeWholeButton_->setToggleState (whole, juce::dontSendNotification);
    rangeRegionButton_->setToggleState (! whole, juce::dontSendNotification);
}

void ExportDialog::persistOptions()
{
    auto* p = context_.properties;
    if (p == nullptr)
        return;

    const auto opts = buildExportOptions();
    p->setValue (PersistenceKeys::format,     (int) opts.format);
    p->setValue (PersistenceKeys::sampleRate, (int) opts.sampleRate);
    p->setValue (PersistenceKeys::bitDepth,   opts.bitDepth);
    p->setValue (PersistenceKeys::mp3Bitrate, opts.mp3BitrateKbps);
    p->setValue (PersistenceKeys::mp3Vbr,     opts.mp3Vbr);
    p->setValue (PersistenceKeys::normalize,  opts.normalize);
    p->setValue (PersistenceKeys::rangeWhole, rangeWholeButton_->getToggleState());
    p->saveIfNeeded();
}

//==============================================================================
// Format-driven control population + enablement (§1.5.4).
//==============================================================================
void ExportDialog::onFormatChanged()
{
    const Format f = buildExportOptions().format; // reads the box
    const auto caps = exporter_.capabilitiesFor (f);

    // Repopulate bit-depth box from the capability table (single source of truth).
    const int prevBd = bitDepthBox_->getText().getIntValue();
    bitDepthBox_->clear (juce::dontSendNotification);
    {
        int id = 1, restore = 0;
        for (int bd : caps.bitDepths)
        {
            const juce::String label = (bd == 32) ? "32-bit float" : juce::String (bd) + "-bit";
            bitDepthBox_->addItem (label, id);
            if (bd == prevBd) restore = id;
            if (bd == 24 && restore == 0) restore = id; // sensible default
            ++id;
        }
        if (! caps.bitDepths.empty())
            bitDepthBox_->setSelectedId (restore > 0 ? restore : 1, juce::dontSendNotification);
    }

    // Repopulate MP3 bitrate box from the capability table.
    const int prevKbps = mp3BitrateBox_->getText().getIntValue();
    mp3BitrateBox_->clear (juce::dontSendNotification);
    {
        int id = 1, restore = 0;
        for (int kbps : caps.mp3Bitrates)
        {
            mp3BitrateBox_->addItem (juce::String (kbps) + " kbps", id);
            if (kbps == prevKbps) restore = id;
            if (kbps == 320 && restore == 0) restore = id; // sensible default
            ++id;
        }
        if (! caps.mp3Bitrates.empty())
            mp3BitrateBox_->setSelectedId (restore > 0 ? restore : 1, juce::dontSendNotification);
    }

    // Reconcile the chosen output path's extension to the new format.
    if (outputFile_ != juce::File())
        outputFile_ = outputFile_.withFileExtension (formatExtension (f));

    refreshControlEnablement();
    repaint();
}

ExportControlState ExportDialog::computeControlState() const
{
    ExportControlState st;
    const Format f = buildExportOptions().format;
    const bool isMp3 = (f == Format::Mp3);

    st.bitDepthEnabled   = ! isMp3;            // WAV / FLAC only
    st.mp3BitrateEnabled = isMp3;              // MP3 only
    st.mp3VbrEnabled     = isMp3;              // MP3 only

    st.selectedRegionEnabled =
        context_.hasSelectedRegion && context_.hasSelectedRegion();

    return st;
}

void ExportDialog::refreshControlEnablement()
{
    const auto st = computeControlState();

    bitDepthBox_->setEnabled (st.bitDepthEnabled);
    mp3BitrateBox_->setEnabled (st.mp3BitrateEnabled);
    mp3VbrToggle_->setEnabled (st.mp3VbrEnabled);

    rangeRegionButton_->setEnabled (st.selectedRegionEnabled);

    // "Selected region" disabled => force whole arrangement (§1.4).
    if (! st.selectedRegionEnabled && rangeRegionButton_->getToggleState())
    {
        rangeRegionButton_->setToggleState (false, juce::dontSendNotification);
        rangeWholeButton_->setToggleState (true, juce::dontSendNotification);
    }

    repaint();
}

//==============================================================================
// Pure option mapping (§1.5.8).
//==============================================================================
ExportOptions ExportDialog::buildExportOptions() const
{
    ExportOptions opts;

    // Format from the selected item id (matches addItem ids above).
    const int fid = formatBox_->getSelectedId();
    opts.format = (fid == 2) ? Format::Flac
                : (fid == 3) ? Format::Mp3
                             : Format::Wav;

    // Sample rate from the box (fall back to project rate).
    {
        const auto& rates = AudioExporter::supportedSampleRates();
        const int idx = sampleRateBox_->getSelectedId() - 1;
        opts.sampleRate = (idx >= 0 && idx < (int) rates.size())
                              ? rates[(size_t) idx]
                              : context_.projectSampleRate;
    }

    // Bit depth (WAV/FLAC) — text-derived so it always matches the populated box.
    {
        const int bd = bitDepthBox_->getText().getIntValue();
        opts.bitDepth = bd > 0 ? bd : 24;
    }

    // MP3 bitrate + VBR.
    {
        const int kbps = mp3BitrateBox_->getText().getIntValue();
        opts.mp3BitrateKbps = kbps > 0 ? kbps : 320;
        opts.mp3Vbr = mp3VbrToggle_->getToggleState();
    }

    opts.normalize = normalizeToggle_->getToggleState();

    // Range: empty => whole arrangement (the driver derives [0, end) from the
    // snapshot). Selected region => the live bounds at the export rate.
    if (! rangeWholeButton_->getToggleState()
        && context_.hasSelectedRegion && context_.hasSelectedRegion()
        && context_.selectedRegion)
    {
        opts.range = context_.selectedRegion (opts.sampleRate);
    }
    else
    {
        opts.range = ExportRange{}; // whole arrangement
    }

    // Output path with the extension reconciled to the format.
    if (outputFile_ != juce::File())
        opts.outputFile = outputFile_.withFileExtension (formatExtension (opts.format));

    return opts;
}

//==============================================================================
// On-Export validation (§1.5.4).
//==============================================================================
ExportValidation ExportDialog::validateForExport() const
{
    const auto opts = buildExportOptions();

    if (opts.outputFile == juce::File() || opts.outputFile.getFileName().isEmpty())
        return ExportValidation::bad ("Choose an output file before exporting.");

    // Non-writable target directory: the parent dir must exist (or be creatable)
    // and be writable.
    auto dir = opts.outputFile.getParentDirectory();
    if (! dir.isDirectory())
    {
        const auto created = dir.createDirectory();
        if (created.failed())
            return ExportValidation::bad (
                "The output folder does not exist and could not be created.");
    }
    if (! dir.hasWriteAccess())
        return ExportValidation::bad (
            "The output folder is not writable. Choose another location.");

    // "Selected region" with no actual selection (transient state can change
    // between dialog open and Export).
    if (! rangeWholeButton_->getToggleState())
    {
        const bool haveRegion = context_.hasSelectedRegion && context_.hasSelectedRegion();
        if (! haveRegion)
            return ExportValidation::bad (
                "No region is selected. Choose Whole arrangement or select a region.");

        if (opts.range.isEmpty())
            return ExportValidation::bad ("The selected region is empty.");
    }

    return ExportValidation::good();
}

//==============================================================================
// Export flow: validate -> PRD-0097 gate -> start render thread.
//==============================================================================
void ExportDialog::onChoosePath()
{
    const auto opts = buildExportOptions();
    const juce::String ext = formatExtension (opts.format);

    juce::File start = (outputFile_ != juce::File())
        ? outputFile_
        : juce::File::getSpecialLocation (juce::File::userMusicDirectory)
              .getChildFile ("Sonik Export" + ext);

    fileChooser_ = std::make_unique<juce::FileChooser> (
        "Export As", start, "*" + ext);

    fileChooser_->launchAsync (
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<ExportDialog> (this)] (const juce::FileChooser& fc)
        {
            if (safe == nullptr)
                return;
            const auto result = fc.getResult();
            if (result == juce::File())
                return; // cancelled
            // Reconcile extension to the current format.
            const auto fmt = safe->buildExportOptions().format;
            safe->outputFile_ = result.withFileExtension (formatExtension (fmt));
            safe->repaint();
        });
}

void ExportDialog::onExportPressed()
{
    // 1. Validation (§1.5.4).
    const auto v = validateForExport();
    if (! v.ok)
    {
        showError (v.message);
        return;
    }

    // 2. PRD-0097 export gate: never start a render with a Missing source.
    if (context_.areAllSourcesResolved && ! context_.areAllSourcesResolved())
    {
        juce::String msg = "Some clips reference a missing source and cannot be exported.";
        if (context_.describeMissingSources)
        {
            const auto detail = context_.describeMissingSources();
            if (detail.isNotEmpty())
                msg += "\n\n" + detail;
        }
        msg += "\n\nRelocate or re-derive the missing sources, then export again.";
        showError (msg);
        return;
    }

    // 3. The host must be able to build the render Job.
    if (! context_.buildJob)
    {
        showError ("Export is unavailable: no arrangement is loaded.");
        return;
    }

    startRender (buildExportOptions());
}

void ExportDialog::startRender (const ExportOptions& options)
{
    // Persist last-used options now (the DJ committed to them). Output path is
    // deliberately NOT persisted (§1.5.6).
    persistOptions();

    // Capture the transport so Cancel can restore it (§1.5.2).
    transportCaptured_ = false;
    if (context_.captureTransport)
    {
        transportToken_ = context_.captureTransport();
        transportCaptured_ = true;
    }

    // Reset cross-thread signalling.
    progress_.store (0.0f, std::memory_order_release);
    cancel_.store (false, std::memory_order_release);
    smoothedRemainingMs_ = -1.0;
    renderStartTimeMs_ = juce::Time::getMillisecondCounterHiRes();

    // Build the Job on the message thread (snapshot compile + reader provider),
    // then hand it to the background thread.
    auto job = context_.buildJob (options);

    state_ = State::Progress;
    rebuildForState();

    renderThread_ = std::make_unique<RenderThread> (*this, std::move (job));
    renderThread_->startThread();
}

void ExportDialog::onCancelPressed()
{
    // Signal the render thread to stop at the next block boundary (§1.5.2). The
    // exporter closes + deletes the partial file on Cancelled (PRD-0100). The
    // final marshalled result lands in finishRender, which restores the transport
    // and returns to the option view.
    cancel_.store (true, std::memory_order_release);
    cancelButton_->setEnabled (false);
}

void ExportDialog::finishRender (ExportResult result)
{
    // Join the worker (it has already posted this callback, so this returns
    // promptly) so a subsequent retry starts a fresh thread cleanly.
    if (renderThread_ != nullptr)
    {
        renderThread_->stopThread (5000);
        renderThread_.reset();
    }

    // Restore the transport in every terminal case (§1.5.2): the offline driver
    // may have advanced a shared playhead.
    if (transportCaptured_ && context_.restoreTransport)
    {
        context_.restoreTransport (transportToken_);
        transportCaptured_ = false;
    }

    switch (result.status)
    {
        case ExportResult::Status::Completed:
            donePath_ = outputFile_.getFullPathName();
            state_ = State::Done;
            break;

        case ExportResult::Status::Cancelled:
            // Partial file already deleted by the exporter (§1.5.2). Return to the
            // option view so the DJ can retry.
            state_ = State::Options;
            break;

        case ExportResult::Status::Failed:
        case ExportResult::Status::Unsupported:
        case ExportResult::Status::InvalidOptions:
        default:
        {
            juce::String msg = result.message.isNotEmpty()
                ? result.message
                : juce::String ("The export failed. No file was written.");
            errorMessage_ = msg;
            state_ = State::Error;
            break;
        }
    }

    rebuildForState();
}

//==============================================================================
// Error state (§1.5.5) — single inline error funnel, never a native alert.
//==============================================================================
void ExportDialog::showError (const juce::String& message)
{
    errorMessage_ = message;
    state_ = State::Error;
    rebuildForState();
}

//==============================================================================
// State view management.
//==============================================================================
void ExportDialog::rebuildForState()
{
    const bool opt   = (state_ == State::Options);
    const bool prog  = (state_ == State::Progress);
    const bool done  = (state_ == State::Done);
    const bool error = (state_ == State::Error);

    // Options controls.
    formatBox_->setVisible (opt);
    sampleRateBox_->setVisible (opt);
    bitDepthBox_->setVisible (opt);
    mp3BitrateBox_->setVisible (opt);
    mp3VbrToggle_->setVisible (opt);
    normalizeToggle_->setVisible (opt);
    rangeWholeButton_->setVisible (opt);
    rangeRegionButton_->setVisible (opt);
    choosePathButton_->setVisible (opt);
    exportButton_->setVisible (opt);
    closeButtonOptions_->setVisible (opt);

    // Progress.
    cancelButton_->setVisible (prog);
    cancelButton_->setEnabled (prog);

    // Done.
    doneCloseButton_->setVisible (done);

    // Error.
    errorRetryButton_->setVisible (error);
    errorResolveButton_->setVisible (error && context_.showUnresolvedSourcesStep != nullptr);
    errorCancelButton_->setVisible (error);

    resized();
    repaint();
}

//==============================================================================
// Layout.
//==============================================================================
juce::Rectangle<int> ExportDialog::panelBounds() const
{
    return getLocalBounds().withTrimmedRight (kShadowOffset)
                           .withTrimmedBottom (kShadowOffset);
}

void ExportDialog::resized()
{
    auto inner = panelBounds().reduced (16);
    inner.removeFromTop (24 + 8); // title + gap (drawn in paint)

    switch (state_)
    {
        case State::Options:   layoutOptions (inner);  break;
        case State::Progress:  layoutProgress (inner); break;
        case State::Done:
            doneCloseButton_->setBounds (inner.removeFromBottom (30)
                                              .removeFromRight (110).withHeight (30));
            break;
        case State::Error:
        {
            auto row = inner.removeFromBottom (30);
            errorCancelButton_->setBounds (row.removeFromRight (110).withHeight (30));
            row.removeFromRight (8);
            if (errorResolveButton_->isVisible())
            {
                errorResolveButton_->setBounds (row.removeFromRight (150).withHeight (30));
                row.removeFromRight (8);
            }
            errorRetryButton_->setBounds (row.removeFromRight (90).withHeight (30));
            break;
        }
    }
}

void ExportDialog::layoutOptions (juce::Rectangle<int> inner)
{
    const int labelW = 120;
    const int rowH   = 26;
    const int gap    = 10;

    auto rowField = [&] (juce::Component& c, int height = -1)
    {
        auto row = inner.removeFromTop (height < 0 ? rowH : height);
        c.setBounds (row.withTrimmedLeft (labelW));
        inner.removeFromTop (gap);
    };

    rowField (*formatBox_);
    rowField (*sampleRateBox_);
    rowField (*bitDepthBox_);    // dimmed/disabled when MP3
    rowField (*mp3BitrateBox_);  // dimmed/disabled when WAV/FLAC
    rowField (*mp3VbrToggle_);

    // Range pair on one row (two toggles).
    {
        auto row = inner.removeFromTop (rowH).withTrimmedLeft (labelW);
        rangeWholeButton_->setBounds (row.removeFromLeft (row.getWidth() / 2 - 4));
        row.removeFromLeft (8);
        rangeRegionButton_->setBounds (row);
        inner.removeFromTop (gap);
    }

    rowField (*normalizeToggle_);

    // Output path row: a Choose... button on the right (path text drawn in paint).
    {
        auto row = inner.removeFromTop (rowH).withTrimmedLeft (labelW);
        choosePathButton_->setBounds (row.removeFromRight (110));
        inner.removeFromTop (gap);
    }

    // Bottom button row: Cancel (left) + Export (right).
    auto buttons = panelBounds().reduced (16).removeFromBottom (30);
    exportButton_->setBounds (buttons.removeFromRight (110).withHeight (30));
    buttons.removeFromRight (8);
    closeButtonOptions_->setBounds (buttons.removeFromRight (90).withHeight (30));
}

void ExportDialog::layoutProgress (juce::Rectangle<int> inner)
{
    // The progress bar + time labels are drawn in paint; reserve the bar band and
    // place the Cancel button at the bottom.
    cancelButton_->setBounds (inner.removeFromBottom (30)
                                   .removeFromRight (110).withHeight (30));
}

//==============================================================================
// Paint — shared border / font / dither across every state.
//==============================================================================
void ExportDialog::paint (juce::Graphics& g)
{
    auto panel = panelBounds();
    paintDitheredShadow (g, panel, kShadowOffset);

    g.setColour (kSurface);
    g.fillRect (panel);
    g.setColour (kInk);
    g.drawRect (panel, 2);

    auto inner = panel.reduced (16);

    // Title bar.
    g.setColour (kInk);
    g.setFont (monoFont (15.0f, true));
    g.drawText ("EXPORT ARRANGEMENT", inner.removeFromTop (24),
                juce::Justification::centredLeft, false);
    inner.removeFromTop (8);

    switch (state_)
    {
        case State::Options:
        {
            // Field labels (left column) aligned to the laid-out rows.
            const int labelW = 120;
            const int rowH = 26, gap = 10;
            g.setFont (monoFont (11.0f, true));

            auto labelRow = [&] (const juce::String& text, bool dimmed)
            {
                auto row = inner.removeFromTop (rowH);
                g.setColour (dimmed ? kInk.withAlpha (0.4f) : kInk);
                g.drawText (text, row.removeFromLeft (labelW).reduced (0, 0),
                            juce::Justification::centredLeft, false);
                inner.removeFromTop (gap);
            };

            const auto st = computeControlState();
            labelRow ("FORMAT", false);
            labelRow ("SAMPLE RATE", false);
            labelRow ("BIT DEPTH", ! st.bitDepthEnabled);
            labelRow ("MP3 BITRATE", ! st.mp3BitrateEnabled);
            labelRow ("MP3 MODE", ! st.mp3VbrEnabled);
            labelRow ("RANGE", false);
            labelRow ("OPTIONS", false);

            // Output path row label + the chosen path text.
            {
                auto row = inner.removeFromTop (rowH);
                g.setColour (kInk);
                g.drawText ("OUTPUT", row.removeFromLeft (labelW),
                            juce::Justification::centredLeft, false);
                // Path text to the left of the Choose... button.
                auto pathArea = row.withTrimmedRight (120);
                g.setFont (monoFont (10.0f));
                const juce::String shown = (outputFile_ != juce::File())
                    ? outputFile_.getFileName()
                    : juce::String ("<no file chosen>");
                g.setColour ((outputFile_ != juce::File()) ? kInk : kInk.withAlpha (0.45f));
                g.drawText (shown, pathArea, juce::Justification::centredLeft, true);
            }
            break;
        }

        case State::Progress:
        {
            const float frac = juce::jlimit (0.0f, 1.0f,
                                             progress_.load (std::memory_order_acquire));

            // Percentage (massive Space Mono — DESIGN.md §3 display scale).
            g.setColour (kInk);
            g.setFont (monoFont (40.0f, true));
            g.drawText (juce::String ((int) std::round (frac * 100.0f)) + "%",
                        inner.removeFromTop (52), juce::Justification::centredLeft, false);
            inner.removeFromTop (10);

            // The dithered checkerboard progress bar.
            auto bar = inner.removeFromTop (28);
            g.setColour (kSurface);
            g.fillRect (bar);
            g.setColour (kInk);
            g.drawRect (bar, 2);

            // Fill region grows left-to-right; dither DENSITY increases with
            // completion (sparser near the start, solid near the end). Strictly
            // monochrome checkerboard — no colour, no gradient (§1.4 / DESIGN.md).
            auto fillArea = bar.reduced (2);
            const int fillW = juce::roundToInt (fillArea.getWidth() * frac);
            if (fillW > 0)
            {
                auto fill = fillArea.withWidth (fillW);
                g.saveState();
                g.reduceClipRegion (fill);
                g.setColour (kInk);
                // Step in {1,2,3,4} px: smaller step = denser dither. Map frac to
                // a density so the pattern thickens toward 100%.
                const int step = juce::jmax (1, 4 - (int) std::round (frac * 3.0f));
                for (int y = fill.getY(); y < fill.getBottom(); y += step + 1)
                    for (int x = fill.getX() + ((y / (step + 1)) % 2) * step;
                         x < fill.getRight(); x += (step + 1) * 2)
                        g.fillRect (x, y, step, step + 1);
                g.restoreState();
            }
            inner.removeFromTop (14);

            // Elapsed + estimated-remaining (Space Mono).
            const double elapsed = juce::Time::getMillisecondCounterHiRes() - renderStartTimeMs_;
            g.setColour (kInk);
            g.setFont (monoFont (12.0f));
            g.drawText ("ELAPSED   " + formatElapsed (elapsed),
                        inner.removeFromTop (18), juce::Justification::centredLeft, false);
            g.drawText ("REMAINING " + formatElapsed (smoothedRemainingMs_),
                        inner.removeFromTop (18), juce::Justification::centredLeft, false);
            break;
        }

        case State::Done:
        {
            g.setColour (kInk);
            g.setFont (monoFont (14.0f, true));
            g.drawText ("EXPORT COMPLETE", inner.removeFromTop (24),
                        juce::Justification::centredLeft, false);
            inner.removeFromTop (10);
            g.setFont (monoFont (11.0f));
            g.drawFittedText ("Written to:\n" + donePath_,
                              inner.removeFromTop (80), juce::Justification::topLeft, 4);
            break;
        }

        case State::Error:
        {
            g.setColour (kInk);
            g.setFont (monoFont (14.0f, true));
            g.drawText ("EXPORT ERROR", inner.removeFromTop (24),
                        juce::Justification::centredLeft, false);
            inner.removeFromTop (10);
            g.setFont (monoFont (11.0f));
            g.drawFittedText (errorMessage_,
                              inner.removeFromTop (inner.getHeight() - 40),
                              juce::Justification::topLeft, 8);
            break;
        }
    }
}

//==============================================================================
// Timer — polls the atomic progress, recomputes the smoothed ETA, repaints.
//==============================================================================
void ExportDialog::timerCallback()
{
    if (state_ != State::Progress)
        return;

    const float frac = juce::jlimit (0.0f, 1.0f, progress_.load (std::memory_order_acquire));

    // ETA = elapsed/frac - elapsed, smoothed (§1.5.3). Block count drives the
    // exact percentage; wall-clock extrapolation drives the (approximate) time.
    const double elapsed = juce::Time::getMillisecondCounterHiRes() - renderStartTimeMs_;
    if (frac > 0.01)
    {
        const double rawRemaining = elapsed / frac - elapsed;
        if (smoothedRemainingMs_ < 0.0)
            smoothedRemainingMs_ = rawRemaining;
        else
            smoothedRemainingMs_ = smoothedRemainingMs_ * 0.7 + rawRemaining * 0.3;
    }

    repaint();
}

void ExportDialog::updateProgressUi (float /*fraction*/)
{
    repaint();
}

//==============================================================================
// Dismiss / keyboard.
//==============================================================================
void ExportDialog::onClosePressed()
{
    dismiss();
}

void ExportDialog::dismiss()
{
    // If a render is in flight, cancel + join before tearing down (the dtor also
    // guards this, but doing it here keeps the modal exit clean).
    if (renderThread_ != nullptr)
    {
        cancel_.store (true, std::memory_order_release);
        renderThread_->stopThread (5000);
        renderThread_.reset();
        if (transportCaptured_ && context_.restoreTransport)
        {
            context_.restoreTransport (transportToken_);
            transportCaptured_ = false;
        }
    }

    if (isCurrentlyModal())
        exitModalState (0);
    setVisible (false);
    if (auto* parent = getParentComponent())
        parent->removeChildComponent (this);
    removeFromDesktop();

    juce::MessageManager::callAsync ([self = this] { delete self; });
}

bool ExportDialog::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        // Esc cancels a running render or dismisses an idle dialog.
        if (state_ == State::Progress)
            onCancelPressed();
        else
            dismiss();
        return true;
    }
    return false;
}

//==============================================================================
// Format helpers.
//==============================================================================
juce::String ExportDialog::formatExtension (Format f)
{
    switch (f)
    {
        case Format::Wav:  return ".wav";
        case Format::Flac: return ".flac";
        case Format::Mp3:  return ".mp3";
    }
    return ".wav";
}

juce::String ExportDialog::formatName (Format f)
{
    switch (f)
    {
        case Format::Wav:  return "WAV";
        case Format::Flac: return "FLAC";
        case Format::Mp3:  return "MP3";
    }
    return "WAV";
}

//==============================================================================
// Test setters (headless).
//==============================================================================
void ExportDialog::setFormatForTest (Format f)
{
    const int id = (f == Format::Flac) ? 2 : (f == Format::Mp3) ? 3 : 1;
    formatBox_->setSelectedId (id, juce::dontSendNotification);
    onFormatChanged();
}

void ExportDialog::setSampleRateForTest (double rate)
{
    int id = 1;
    for (double r : AudioExporter::supportedSampleRates())
    {
        if (std::abs (r - rate) < 1.0)
        {
            sampleRateBox_->setSelectedId (id, juce::dontSendNotification);
            return;
        }
        ++id;
    }
}

void ExportDialog::setBitDepthForTest (int bd)
{
    for (int i = 0; i < bitDepthBox_->getNumItems(); ++i)
        if (bitDepthBox_->getItemText (i).getIntValue() == bd)
            bitDepthBox_->setSelectedId (bitDepthBox_->getItemId (i), juce::dontSendNotification);
}

void ExportDialog::setMp3BitrateForTest (int kbps)
{
    for (int i = 0; i < mp3BitrateBox_->getNumItems(); ++i)
        if (mp3BitrateBox_->getItemText (i).getIntValue() == kbps)
            mp3BitrateBox_->setSelectedId (mp3BitrateBox_->getItemId (i), juce::dontSendNotification);
}

void ExportDialog::setNormalizeForTest (bool on)
{
    normalizeToggle_->setToggleState (on, juce::dontSendNotification);
}

void ExportDialog::setRangeWholeForTest (bool whole)
{
    rangeWholeButton_->setToggleState (whole, juce::dontSendNotification);
    if (rangeRegionButton_->isEnabled())
        rangeRegionButton_->setToggleState (! whole, juce::dontSendNotification);
}

void ExportDialog::setOutputFileForTest (const juce::File& f)
{
    outputFile_ = f;
}

//==============================================================================
// Presentation + test seam.
//==============================================================================
namespace
{
    void presentModal (ExportDialog* dialog, juce::Component* parent)
    {
        if (parent != nullptr)
        {
            if (auto* top = parent->getTopLevelComponent())
            {
                top->addAndMakeVisible (dialog);
                // Centre within the top-level's LOCAL bounds (the dialog is a child
                // of `top`, so its position is in `top`'s coordinate space — using
                // screen-space centre coords would push it off-window).
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
}

void showExportDialog (juce::Component* parent, ExportContext context)
{
    auto* dialog = new ExportDialog (std::move (context));
    presentModal (dialog, parent);
}

std::unique_ptr<ExportDialog> createExportDialogForTest (ExportContext context)
{
    auto dialog = std::make_unique<ExportDialog> (std::move (context));
    dialog->setVisible (true);
    return dialog;
}

} // namespace Daw::Export::Ui
