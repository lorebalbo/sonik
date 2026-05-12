#include "StemSeparateButton.h"
#include "../StemSeparationManager.h"
#include "../../AudioEngine/AudioEngine.h"
#include <cmath>

// Design-system palette
static const juce::Colour kLight  { 0xFFF9F9F9 };
static const juce::Colour kDark   { 0xFF2D2D2D };
static const juce::Colour kGreen  { 0xFF0AD691 };
static const juce::Colour kRed    { 0xFFFF3C3B };

static constexpr double kMinDurationForStems = 5.0;

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

    // Advance phase label every 8 seconds, clamp to the last one so
    // labels never loop — they are shown once each and then stay put.
    static constexpr double kLabelDurationSeconds = 8.0;
    int rawIndex = static_cast<int> (elapsedSec / kLabelDurationSeconds);
    currentLabelIndex = juce::jmin (rawIndex, phaseLabels().size() - 1);

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

    // Track consecutive errors
    if (currentStatus == "error")
        ++consecutiveErrors;
    else if (currentStatus != "error")
        consecutiveErrors = 0;
}

// ---------------------------------------------------------------------------
void StemSeparateButton::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    const bool disabled = isEmpty || (isShortTrack && currentStatus == "none")
                          || currentStatus == "model_unavailable";

    // ── Determine background / foreground / label ─────────────────────────
    juce::Colour bg   = kLight;
    juce::Colour fg   = kDark;
    juce::String text = "SEPARATE STEMS";

    if (disabled)
    {
        // light bg, dimmed text
        fg = kDark.withAlpha (0.3f);
    }
    else if (currentStatus == "separating")
    {
        bg   = kDark;
        fg   = kLight;
        text = "";  // drawn specially below
    }
    else if (currentStatus == "queued")
    {
        bg = kDark;
        fg = kLight;
    }
    else if (currentStatus == "ready" || currentStatus == "loading_cached")
    {
        bg = kDark;
        fg = kLight;
    }
    else if (currentStatus == "error")
    {
        bg = kRed;
        fg = juce::Colours::white;
    }

    // ── Fill + border ─────────────────────────────────────────────────────
    g.setColour (bg);
    g.fillRect (bounds);

    g.setColour (disabled ? kDark.withAlpha (0.3f) : kDark);
    g.drawRect (bounds, 2);

    // ── Animated progress bar (separating only) ──────────────────────────
    if (currentStatus == "separating")
    {
        // Slightly lighter fill shows completed portion within dark background
        g.setColour (juce::Colour (0xFF505050));
        auto fillBounds = bounds.toFloat();
        const bool vertical = bounds.getHeight() > bounds.getWidth();
        if (vertical)
        {
            // Fill from the bottom up so progress reads naturally on a vertical bar
            const float h = animatedProgress * fillBounds.getHeight();
            fillBounds = fillBounds.withTop (fillBounds.getBottom() - h);
        }
        else
        {
            fillBounds = fillBounds.withWidth (animatedProgress * fillBounds.getWidth());
        }
        g.fillRect (fillBounds);

        // Border
        g.setColour (kDark);
        g.drawRect (bounds, 2);

        const auto& labels = phaseLabels();
        juce::String label = labels[currentLabelIndex];
        juce::String pct = juce::String (juce::roundToInt (animatedProgress * 100)) + " %";

        if (vertical)
        {
            // Draw both lines rotated 90° CCW, centred in the bounds.
            juce::Graphics::ScopedSaveState save (g);
            const float cx = static_cast<float> (bounds.getCentreX());
            const float cy = static_cast<float> (bounds.getCentreY());
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, cx, cy));

            // Rotated rect: width ↔ height swapped around the centre.
            juce::Rectangle<int> rotated (
                static_cast<int> (cx) - bounds.getHeight() / 2,
                static_cast<int> (cy) - bounds.getWidth()  / 2,
                bounds.getHeight(),
                bounds.getWidth());

            auto leftHalf  = rotated.withWidth (rotated.getWidth() / 2);
            auto rightHalf = rotated.withTrimmedLeft (rotated.getWidth() / 2);

            g.setColour (fg);
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, juce::Font::plain));
            g.drawText (label, leftHalf.reduced (3, 0), juce::Justification::centredRight, false);

            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::bold));
            g.drawText (pct, rightHalf.reduced (3, 0), juce::Justification::centredLeft, false);
        }
        else
        {
            // Phase label (top line, smaller)
            auto topHalf    = bounds.withHeight (bounds.getHeight() / 2);
            auto bottomHalf = bounds.withTrimmedTop (bounds.getHeight() / 2);

            g.setColour (fg);
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, juce::Font::plain));
            g.drawText (label, topHalf.reduced (3, 0), juce::Justification::centredLeft, false);

            // Percentage (bottom line, larger)
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::bold));
            g.drawText (pct, bottomHalf.reduced (3, 0), juce::Justification::centredLeft, false);
        }
        return;
    }

    // ── Label ─────────────────────────────────────────────────────────────
    g.setColour (fg);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));

    if (bounds.getHeight() > bounds.getWidth())
    {
        // Vertical sidebar: draw the label rotated 90° CCW so it reads bottom-to-top.
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
