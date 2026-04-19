#include "StemSeparateButton.h"
#include "../StemSeparationManager.h"
#include "../../AudioEngine/AudioEngine.h"

// Monochrome palette (DESIGN.md)
static constexpr juce::uint32 kBlack   = 0xff000000;
static constexpr juce::uint32 kWhite   = 0xfff9f9f9;
static constexpr juce::uint32 kSurface = 0xffe2e2e2;

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
    auto bounds = getLocalBounds().toFloat();

    // Disabled states: empty deck or short track
    if (isEmpty || (isShortTrack && currentStatus == "none"))
    {
        g.setColour (juce::Colour (kSurface).withAlpha (0.3f));
        g.fillRect (bounds);
        g.setColour (juce::Colour (kBlack).withAlpha (0.3f));
        g.drawRect (bounds, 1.0f);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("STEMS", bounds, juce::Justification::centred, false);
        return;
    }

    if (currentStatus == "none")
    {
        // Inactive: surface fill, dark text at 50% opacity
        g.setColour (juce::Colour (kSurface));
        g.fillRect (bounds);
        g.setColour (juce::Colour (kBlack));
        g.drawRect (bounds, 1.0f);
        g.setColour (juce::Colour (kBlack).withAlpha (0.5f));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("STEMS", bounds, juce::Justification::centred, false);
    }
    else if (currentStatus == "separating")
    {
        // Progress bar: left-to-right fill of black
        g.setColour (juce::Colour (kSurface));
        g.fillRect (bounds);

        float fillWidth = bounds.getWidth() * juce::jlimit (0.0f, 1.0f, currentProgress);
        g.setColour (juce::Colour (kBlack));
        g.fillRect (bounds.withWidth (fillWidth));

        g.setColour (juce::Colour (kBlack));
        g.drawRect (bounds, 1.0f);

        // Percentage label
        int pct = juce::roundToInt (currentProgress * 100.0f);
        auto pctStr = juce::String (pct) + "%";
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        // Draw text in white over the filled part, black over unfilled
        g.setColour (juce::Colour (kWhite));
        g.drawText (pctStr, bounds, juce::Justification::centred, false);
    }
    else if (currentStatus == "queued")
    {
        // Queued: surface fill, "WAIT" label
        g.setColour (juce::Colour (kSurface));
        g.fillRect (bounds);
        g.setColour (juce::Colour (kBlack));
        g.drawRect (bounds, 1.0f);
        g.setColour (juce::Colour (kBlack).withAlpha (0.5f));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("WAIT", bounds, juce::Justification::centred, false);
    }
    else if (currentStatus == "ready" || currentStatus == "loading_cached")
    {
        // Active: black fill, white text
        g.setColour (juce::Colour (kBlack));
        g.fillRect (bounds);
        g.setColour (juce::Colour (kWhite));
        g.drawRect (bounds, 1.0f);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("STEMS", bounds, juce::Justification::centred, false);
    }
    else if (currentStatus == "error")
    {
        // Error: stippled background
        g.setColour (juce::Colour (kSurface));
        g.fillRect (bounds);

        // Stipple pattern
        g.setColour (juce::Colour (kBlack).withAlpha (0.15f));
        auto area = bounds.toNearestInt();
        for (int y = area.getY(); y < area.getBottom(); y += 2)
            for (int x = area.getX() + (y % 4 == 0 ? 0 : 1); x < area.getRight(); x += 2)
                g.fillRect (x, y, 1, 1);

        g.setColour (juce::Colour (kBlack));
        g.drawRect (bounds, 1.0f);
        g.setColour (juce::Colour (kBlack).withAlpha (0.7f));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("ERR", bounds, juce::Justification::centred, false);
    }
    else if (currentStatus == "model_unavailable")
    {
        // Model unavailable
        g.setColour (juce::Colour (kSurface).withAlpha (0.5f));
        g.fillRect (bounds);
        g.setColour (juce::Colour (kBlack).withAlpha (0.5f));
        g.drawRect (bounds, 1.0f);
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText ("NO MODEL", bounds, juce::Justification::centred, false);
    }
}

// ---------------------------------------------------------------------------
void StemSeparateButton::mouseDown (const juce::MouseEvent&)
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
