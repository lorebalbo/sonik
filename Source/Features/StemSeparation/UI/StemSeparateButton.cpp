#include "StemSeparateButton.h"
#include "../StemSeparationManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include <cmath>

// Design-system palette (DESIGN.md §1 — strictly monochrome)
static const juce::Colour kLight  { 0xFFFDFDFD };   // surface
static const juce::Colour kDark   { 0xFF2D2D2D };   // ink

static constexpr double kMinDurationForStems = 5.0;

namespace
{
    /// DESIGN.md §2 "Dithered Gradients": a checkerboard of #2d2d2d ink whose
    /// coverage rises with `density` (0 = empty, 1 = solid). No gradients, no
    /// colour. Used for the in-progress separation fill in place of a coloured
    /// progress bar.
    void fillDithered (juce::Graphics& g, juce::Rectangle<int> area, float density)
    {
        if (area.isEmpty())
            return;

        density = juce::jlimit (0.0f, 1.0f, density);

        const int x0 = area.getX();
        const int y0 = area.getY();
        const int w  = area.getWidth();
        const int h  = area.getHeight();

        // 2x2 ordered (Bayer-ish) dither thresholds — same vocabulary as the
        // mixer VU meters so the whole app shares one dithering language.
        static const float kThresholds[4] = { 0.20f, 0.60f, 0.80f, 0.40f };

        g.setColour (kDark);
        for (int row = 0; row < h; ++row)
        {
            const int y = y0 + row;
            for (int col = 0; col < w; ++col)
            {
                const int x = x0 + col;
                const int cellIdx = ((x & 1) << 1) | (y & 1);
                if (density >= kThresholds[cellIdx])
                    g.fillRect (x, y, 1, 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
const juce::StringArray& StemSeparateButton::phaseLabels() noexcept
{
    static const juce::StringArray labels {
        "ANALYZING AUDIO",
        "LOADING MODEL",
        "COMPUTING SPECTROGRAM",
        "SEPARATING STEMS",
        "PROCESSING VOCALS",
        "PROCESSING INSTRUMENTS",
        "REFINING SEPARATION",
        "APPLYING CORRECTIONS",
        "MIXING LAYERS",
        "FINALIZING"
    };
    return labels;
}

// ---------------------------------------------------------------------------
StemSeparateButton::StemSeparateButton (juce::ValueTree deckTree,
                                        StemSeparationManager& mgr,
                                        AudioEngine& engine,
                                        const juce::String& id)
    : tree (deckTree),
      stemsNode (deckTree.getChildWithName (IDs::Stems)),
      stemManager (mgr),
      audioEngine (engine),
      deckId (id)
{
    jassert (tree.isValid());
    jassert (stemsNode.isValid());

    tree.addListener (this);
    stemsNode.addListener (this);

    refreshState();
}

StemSeparateButton::~StemSeparateButton()
{
    stopTimer();
    stemsNode.removeListener (this);
    tree.removeListener (this);
}

// ---------------------------------------------------------------------------
void StemSeparateButton::timerCallback()
{
    if (currentStatus != "separating")
    {
        stopTimer();
        return;
    }

    double elapsedSec = (juce::Time::getMillisecondCounterHiRes() - separationStartMs) / 1000.0;

    // Fake progress schedule — races to 85 % quickly, then almost stalls.
    // Real progress (currentProgress) takes over whenever it exceeds the fake.
    //   0 – 3 s  : 0  → 20 %  (fast initial feedback)
    //   3 – 20 s : 20 → 55 %  (medium: "model is running")
    //  20 – 60 s : 55 → 78 %  (slowing down)
    //  60 s+     : 78 → 85 %  (almost stopped — waiting for completion)
    float fakeTarget;
    if (elapsedSec < 3.0)
        fakeTarget = static_cast<float> (elapsedSec / 3.0 * 0.20);
    else if (elapsedSec < 20.0)
        fakeTarget = 0.20f + static_cast<float> ((elapsedSec - 3.0) / 17.0 * 0.35f);
    else if (elapsedSec < 60.0)
        fakeTarget = 0.55f + static_cast<float> ((elapsedSec - 20.0) / 40.0 * 0.23f);
    else
        fakeTarget = 0.78f + static_cast<float> (std::min ((elapsedSec - 60.0) / 180.0, 1.0) * 0.07f);

    // Real progress dominates when it overtakes the fake curve.
    float displayTarget = std::max (fakeTarget, currentProgress);

    // Smoothly animate toward the target (ease-in)
    animatedProgress += (displayTarget - animatedProgress) * 0.12f;

    // Never show 100 % until the real process has finished
    if (currentProgress < 0.99f)
        animatedProgress = std::min (animatedProgress, 0.97f);

    // Cycle the phase label every 3 seconds (PRD §1.3) so the indicator never
    // looks frozen even while the coarse percentage idles. Labels wrap around.
    static constexpr double kLabelDurationSeconds = 3.0;
    const int rawIndex = static_cast<int> (elapsedSec / kLabelDurationSeconds);
    currentLabelIndex = rawIndex % phaseLabels().size();

    repaint();
}

// ---------------------------------------------------------------------------
void StemSeparateButton::refreshState()
{
    currentStatus   = stemsNode.getProperty (IDs::status, "none").toString();
    currentProgress = static_cast<float> (stemsNode.getProperty (IDs::progress, 0.0f));

    // Start / stop the animation timer
    if (currentStatus == "separating" && ! isTimerRunning())
    {
        animatedProgress  = 0.0f;
        currentLabelIndex = 0;
        separationStartMs = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (15);  // 15 fps is enough for smooth progress
    }
    else if (currentStatus != "separating" && isTimerRunning())
    {
        stopTimer();
        if (currentStatus == "ready" || currentStatus == "loading_cached")
            animatedProgress = 1.0f;
    }

    isEmpty = tree.getProperty (IDs::playbackStatus).toString() == "empty";

    auto trackMeta = tree.getChildWithName (IDs::TrackMetadata);
    double dur = trackMeta.isValid()
                     ? static_cast<double> (trackMeta.getProperty (IDs::duration, 0.0))
                     : 0.0;
    isShortTrack = (dur > 0.0 && dur < kMinDurationForStems);

    // Proactively check model availability when status is "none" and deck is loaded
    if (currentStatus == "none" && ! isEmpty && ! stemManager.isModelReady())
        currentStatus = "model_unavailable";

    // Update tooltip
    if (currentStatus == "error")
    {
        auto errMsg = stemsNode.getProperty (IDs::stemError, "").toString();
        setTooltip (errMsg.isNotEmpty() ? errMsg : "Stem separation error");
    }
    else if (currentStatus == "model_unavailable")
    {
        setTooltip ("Place htdemucs.onnx in ~/Library/Application Support/Sonik/Models/");
    }
    else if (isShortTrack && ! isEmpty)
    {
        setTooltip ("Track too short for stem separation");
    }
    else
    {
        setTooltip ("Stem separation");
    }

    // Track consecutive errors by status transition (§1.5.5). Increment only
    // when entering "error" from a non-error status, so a burst of unrelated
    // property changes can never inflate the count. Reset on success/idle.
    if (currentStatus == "error" && lastStatus != "error")
        ++consecutiveErrors;
    else if (currentStatus == "ready" || currentStatus == "none")
        consecutiveErrors = 0;

    lastStatus = currentStatus;
}

// ---------------------------------------------------------------------------
namespace
{
    // Draw a single centred label, rotated 90° CCW on a tall vertical sidebar
    // so it reads bottom-to-top, or horizontally otherwise.
    void drawRotatableLabel (juce::Graphics& g, juce::Rectangle<int> bounds,
                             const juce::String& text, juce::Colour colour,
                             float fontSize, bool vertical)
    {
        g.setColour (colour);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), fontSize, juce::Font::plain));

        if (vertical)
        {
            juce::Graphics::ScopedSaveState save (g);
            const float cx = static_cast<float> (bounds.getCentreX());
            const float cy = static_cast<float> (bounds.getCentreY());
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, cx, cy));

            juce::Rectangle<int> rotated (
                static_cast<int> (cx) - bounds.getHeight() / 2,
                static_cast<int> (cy) - bounds.getWidth()  / 2,
                bounds.getHeight(),
                bounds.getWidth());

            g.drawText (text, rotated.reduced (4, 0), juce::Justification::centred, false);
        }
        else
        {
            g.drawText (text, bounds.reduced (4, 0), juce::Justification::centred, false);
        }
    }
}

void StemSeparateButton::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    const bool vertical = bounds.getHeight() > bounds.getWidth();

    const bool disabled = isEmpty || (isShortTrack && currentStatus == "none")
                          || currentStatus == "model_unavailable";

    // ── Separating: dithered progress fill + phase label + percentage ─────
    if (currentStatus == "separating")
    {
        // Light surface base; #2d2d2d dithered fill grows over the completed
        // portion (DESIGN.md §2 — density in place of a coloured gradient).
        g.setColour (kLight);
        g.fillRect (bounds);

        const float density = 0.40f + 0.60f * animatedProgress;
        auto fill = bounds;
        if (vertical)
        {
            const int fh = juce::roundToInt (animatedProgress * static_cast<float> (bounds.getHeight()));
            fill = bounds.withTop (bounds.getBottom() - fh);
        }
        else
        {
            fill = bounds.withWidth (juce::roundToInt (animatedProgress * static_cast<float> (bounds.getWidth())));
        }
        fillDithered (g, fill, density);

        g.setColour (kDark);
        g.drawRect (bounds, 2);

        const juce::String label = phaseLabels()[currentLabelIndex];
        const juce::String pct   = juce::String (juce::roundToInt (animatedProgress * 100)) + " %";

        if (vertical)
        {
            juce::Graphics::ScopedSaveState save (g);
            const float cx = static_cast<float> (bounds.getCentreX());
            const float cy = static_cast<float> (bounds.getCentreY());
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, cx, cy));

            juce::Rectangle<int> rotated (
                static_cast<int> (cx) - bounds.getHeight() / 2,
                static_cast<int> (cy) - bounds.getWidth()  / 2,
                bounds.getHeight(),
                bounds.getWidth());

            auto leftHalf  = rotated.withWidth (rotated.getWidth() / 2);
            auto rightHalf = rotated.withTrimmedLeft (rotated.getWidth() / 2);

            g.setColour (kDark);
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, juce::Font::plain));
            g.drawText (label, leftHalf.reduced (3, 0), juce::Justification::centredRight, false);

            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::bold));
            g.drawText (pct, rightHalf.reduced (3, 0), juce::Justification::centredLeft, false);
        }
        else
        {
            auto topHalf    = bounds.withHeight (bounds.getHeight() / 2);
            auto bottomHalf = bounds.withTrimmedTop (bounds.getHeight() / 2);

            g.setColour (kDark);
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, juce::Font::plain));
            g.drawText (label, topHalf.reduced (3, 0), juce::Justification::centredLeft, false);

            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::bold));
            g.drawText (pct, bottomHalf.reduced (3, 0), juce::Justification::centredLeft, false);
        }
        return;
    }

    // ── All other states: monochrome tactile-button presentation ──────────
    juce::Colour bg        = kLight;
    juce::Colour fg        = kDark;
    juce::String text      = "SEPARATE STEMS";
    bool         hatchFill = false;   // persistent-failure dead-end hatch

    if (disabled)
    {
        bg = kLight;
        fg = kDark.withAlpha (0.30f);
        if (currentStatus == "model_unavailable")
            text = "MODEL N/A";
    }
    else if (currentStatus == "queued")
    {
        bg = kDark; fg = kLight; text = "QUEUED";
    }
    else if (currentStatus == "loading_cached")
    {
        // Distinct from "separating": no percentage, no phase cycling (§1.5.7).
        bg = kLight; fg = kDark; text = "LOADING";
    }
    else if (currentStatus == "ready")
    {
        bg = kDark; fg = kLight; text = "STEMS READY";
    }
    else if (currentStatus == "error")
    {
        if (consecutiveErrors >= kPersistentErrorThreshold)
        {
            // Persistent failure (§1.5.5): de-emphasised, hatched, retry quiet.
            bg = kLight; fg = kDark; text = "SEP. FAILED"; hatchFill = true;
        }
        else
        {
            bg = kLight; fg = kDark; text = "ERROR — RETRY";
        }
    }

    g.setColour (bg);
    g.fillRect (bounds);

    if (hatchFill)
        fillDithered (g, bounds, 0.50f);   // 50% hatch reads as non-inviting

    g.setColour (disabled ? kDark.withAlpha (0.30f) : kDark);
    g.drawRect (bounds, 2);

    drawRotatableLabel (g, bounds, text, fg, 13.0f, vertical);
}
// ---------------------------------------------------------------------------
void StemSeparateButton::mouseDown (const juce::MouseEvent& /*e*/)
{
    if (isEmpty)
        return;

    if (isShortTrack && currentStatus == "none")
        return;

    if (currentStatus == "model_unavailable")
        return;

    if (currentStatus == "none" || currentStatus == "error")
    {
        // Start separation
        stemManager.startSeparation (deckId);
    }
    else if (currentStatus == "separating" || currentStatus == "queued")
    {
        // Cancel
        stemManager.cancelSeparation (deckId);
    }
    else if (currentStatus == "ready")
    {
        // Toggle off: clear buffers and reset status
        audioEngine.clearDeckStemBuffers (deckId);
        stemsNode.setProperty (IDs::status, "none", nullptr);
    }
}

// ---------------------------------------------------------------------------
void StemSeparateButton::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                   const juce::Identifier& property)
{
    bool relevant = false;

    if (changedTree == stemsNode
        && (property == IDs::status || property == IDs::progress || property == IDs::stemError))
    {
        relevant = true;
    }

    if (changedTree == tree
        && (property == IDs::playbackStatus))
    {
        relevant = true;
    }

    if (changedTree.hasType (IDs::TrackMetadata) && changedTree.getParent() == tree
        && property == IDs::duration)
    {
        relevant = true;
    }

    if (relevant)
    {
        auto safe = juce::Component::SafePointer<StemSeparateButton> (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr)
                return;
            safe->refreshState();
            safe->repaint();
        });
    }
}
