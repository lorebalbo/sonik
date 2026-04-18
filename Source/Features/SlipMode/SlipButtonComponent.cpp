#include "SlipButtonComponent.h"
#include "../AudioEngine/AudioEngine.h"

// Monochrome palette (DESIGN.md)
static constexpr juce::uint32 kBlack   = 0xff000000;
static constexpr juce::uint32 kWhite   = 0xfff9f9f9;
static constexpr juce::uint32 kSurface = 0xffe2e2e2;

static constexpr juce::int64 kShortPressThresholdMs = 300;

// ---------------------------------------------------------------------------
SlipButtonComponent::SlipButtonComponent (juce::ValueTree deckTree,
                                          DeckAudioState* state,
                                          AudioEngine& engine,
                                          const juce::String& id)
    : tree (deckTree),
      audioState (state),
      audioEngine (engine),
      deckId (id)
{
    jassert (tree.isValid());
    tree.addListener (this);

    slipEnabled = static_cast<bool> (tree.getProperty (IDs::slipEnabled, false));
    isEmpty     = tree.getProperty (IDs::playbackStatus).toString() == "empty";

    setTooltip ("Slip Mode (timeline continues during loops, jumps, and reverse)");

    startTimerHz (30);
}

SlipButtonComponent::~SlipButtonComponent()
{
    stopTimer();
    tree.removeListener (this);
}

// ---------------------------------------------------------------------------
void SlipButtonComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    float alpha = isEmpty ? 0.3f : 1.0f;

    if (slipEnabled)
    {
        // Displaced pulsing: alpha oscillates [0.55 .. 1.0]
        float pulseAlpha = 1.0f;
        if (slipDisplaced && ! isEmpty)
        {
            float sinVal = std::sin (pulsePhase); // [-1, 1]
            pulseAlpha = 0.55f + 0.45f * ((sinVal + 1.0f) * 0.5f); // [0.55, 1.0]
            alpha *= pulseAlpha;
        }

        // Active: black fill, white text
        g.setColour (juce::Colour (kBlack).withAlpha (alpha));
        g.fillRect (bounds);

        g.setColour (juce::Colour (kWhite).withAlpha (alpha));
    }
    else
    {
        // Inactive: surface fill, dark text at 50 % opacity
        g.setColour (juce::Colour (kSurface).withAlpha (alpha));
        g.fillRect (bounds);

        g.setColour (juce::Colour (kBlack).withAlpha (alpha * 0.5f));
    }

    // Border
    g.setColour (juce::Colour (kBlack).withAlpha (alpha));
    g.drawRect (bounds, 1.0f);

    // Label
    auto font = juce::Font (juce::FontOptions (10.0f));
    g.setFont (font);

    juce::String label = "SLIP";
    if (slipEnabled)
        g.setColour (juce::Colour (kWhite).withAlpha (alpha));
    else
        g.setColour (juce::Colour (kBlack).withAlpha (alpha * 0.5f));

    g.drawText (label, bounds, juce::Justification::centred, false);
}

// ---------------------------------------------------------------------------
void SlipButtonComponent::mouseDown (const juce::MouseEvent&)
{
    if (isEmpty)
        return;

    mouseDownTime = juce::Time::currentTimeMillis();
}

void SlipButtonComponent::mouseUp (const juce::MouseEvent&)
{
    if (isEmpty)
        return;

    auto elapsed = juce::Time::currentTimeMillis() - mouseDownTime;

    if (slipEnabled && slipDisplaced && elapsed < kShortPressThresholdMs)
    {
        // Deactivate any active loop before snap-back, otherwise the
        // loop wrap-back would immediately re-displace the playhead.
        auto loopNode = tree.getChildWithName (IDs::Loop);
        if (loopNode.isValid()
            && static_cast<bool> (loopNode.getProperty (IDs::active, false)))
        {
            loopNode.setProperty (IDs::active, false, nullptr);
        }

        // Short press while displaced → slip return (snap back)
        audioEngine.sendSlipReturn (deckId);
    }
    else
    {
        // Toggle slip mode
        tree.setProperty (IDs::slipEnabled,
                          ! static_cast<bool> (tree.getProperty (IDs::slipEnabled, false)),
                          nullptr);
    }
}

// ---------------------------------------------------------------------------
void SlipButtonComponent::valueTreePropertyChanged (juce::ValueTree& t,
                                                    const juce::Identifier& p)
{
    if (t != tree)
        return;

    if (p == IDs::slipEnabled || p == IDs::playbackStatus)
    {
        auto safe = juce::Component::SafePointer<SlipButtonComponent> (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr)
                return;
            safe->slipEnabled = static_cast<bool> (
                safe->tree.getProperty (IDs::slipEnabled, false));
            safe->isEmpty = safe->tree.getProperty (IDs::playbackStatus)
                                .toString() == "empty";
            safe->repaint();
        });
    }
}

// ---------------------------------------------------------------------------
void SlipButtonComponent::timerCallback()
{
    if (audioState == nullptr)
        return;

    bool displaced = audioState->slipDisplaced.load (std::memory_order_relaxed);

    if (displaced != slipDisplaced)
    {
        slipDisplaced = displaced;
        repaint();
    }

    // Advance pulse animation
    if (slipEnabled && slipDisplaced)
    {
        pulsePhase += kPulseSpeed;
        if (pulsePhase > juce::MathConstants<float>::twoPi)
            pulsePhase -= juce::MathConstants<float>::twoPi;
        repaint();
    }
    else
    {
        pulsePhase = 0.0f;
    }
}
