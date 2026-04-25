#include "StemSeparateButton.h"
#include "../StemSeparationManager.h"
#include "../../AudioEngine/AudioEngine.h"

// Design-system palette
static const juce::Colour kLight  { 0xFFF9F9F9 };
static const juce::Colour kDark   { 0xFF2D2D2D };
static const juce::Colour kGreen  { 0xFF0AD691 };
static const juce::Colour kRed    { 0xFFFF3C3B };

static constexpr double kMinDurationForStems = 5.0;

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
    stemsNode.removeListener (this);
    tree.removeListener (this);
}

// ---------------------------------------------------------------------------
void StemSeparateButton::refreshState()
{
    currentStatus   = stemsNode.getProperty (IDs::status, "none").toString();
    currentProgress = static_cast<float> (stemsNode.getProperty (IDs::progress, 0.0f));

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
        text = juce::String (juce::roundToInt (currentProgress * 100.0f)) + "%";
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

    // ── Label ─────────────────────────────────────────────────────────────
    g.setColour (fg);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    g.drawText (text, bounds.reduced (4, 0), juce::Justification::centred, false);
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
