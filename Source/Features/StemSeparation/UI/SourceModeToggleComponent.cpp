#include "SourceModeToggleComponent.h"
#include "../../AudioEngine/AudioEngine.h"

// Design-system palette (DESIGN.md §1 — strictly monochrome)
static const juce::Colour kLight { 0xFFFDFDFD };   // surface
static const juce::Colour kDark  { 0xFF2D2D2D };   // ink

// ---------------------------------------------------------------------------
SourceModeToggleComponent::SourceModeToggleComponent (juce::ValueTree deckTree,
                                                      AudioEngine& engine,
                                                      const juce::String& deck)
    : tree (std::move (deckTree)),
      audioEngine (engine),
      deckId (deck)
{
    jassert (tree.isValid());
    stemsNode = tree.getChildWithName (IDs::Stems);
    jassert (stemsNode.isValid());

    setTooltip ("Choose the deck source: ORIG plays the original file, "
                "STEMS plays the separated stems");

    tree.addListener (this);   // covers deck-level sourceMode + Stems-child status
    refreshState();
}

SourceModeToggleComponent::~SourceModeToggleComponent()
{
    tree.removeListener (this);
}

// ---------------------------------------------------------------------------
void SourceModeToggleComponent::refreshState()
{
    stemsReady = stemsNode.getProperty (IDs::status, "none").toString() == "ready";
    onStems    = tree.getProperty (IDs::sourceMode, "original").toString() == "stems";

    // Safety net: if stems are not ready the deck cannot be on the stem source.
    if (! stemsReady)
        onStems = false;
}

// ---------------------------------------------------------------------------
void SourceModeToggleComponent::selectMode (bool useStems)
{
    if (useStems && ! stemsReady)
        return;                          // STEMS locked until a ready set exists

    const bool wantStems = useStems && stemsReady;
    if (wantStems == onStems)
        return;                          // no change

    // Drive the audio thread first for an immediate, click-free response, then
    // publish the canonical ValueTree property (the single source of truth).
    audioEngine.setDeckSourceMode (deckId, wantStems);
    tree.setProperty (IDs::sourceMode, wantStems ? "stems" : "original", nullptr);
}

// ---------------------------------------------------------------------------
void SourceModeToggleComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    const bool vertical = bounds.getHeight() > bounds.getWidth();

    // Split into two equal segments: ORIG then STEMS. In the vertical deck
    // sidebar ORIG sits on top and STEMS below; otherwise ORIG is left.
    juce::Rectangle<int> origCell, stemsCell;
    if (vertical)
    {
        origCell  = bounds.removeFromTop (bounds.getHeight() / 2);
        stemsCell = bounds;
    }
    else
    {
        origCell  = bounds.removeFromLeft (bounds.getWidth() / 2);
        stemsCell = bounds;
    }

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  12.0f, juce::Font::plain));

    auto drawLabel = [&g, vertical] (juce::Rectangle<int> cell, const juce::String& label)
    {
        if (vertical)
        {
            juce::Graphics::ScopedSaveState save (g);
            const float cx = static_cast<float> (cell.getCentreX());
            const float cy = static_cast<float> (cell.getCentreY());
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, cx, cy));
            juce::Rectangle<int> rotated (
                static_cast<int> (cx) - cell.getHeight() / 2,
                static_cast<int> (cy) - cell.getWidth()  / 2,
                cell.getHeight(),
                cell.getWidth());
            g.drawText (label, rotated, juce::Justification::centred, false);
        }
        else
        {
            g.drawText (label, cell, juce::Justification::centred, false);
        }
    };

    auto paintSegment = [&] (juce::Rectangle<int> cell, const juce::String& label,
                             bool active, bool enabled)
    {
        if (! enabled)
        {
            g.setColour (kLight);
            g.fillRect (cell);
            g.setColour (kDark.withAlpha (0.3f));
            g.drawRect (cell, 2);
            g.setColour (kDark.withAlpha (0.3f));
        }
        else if (active)
        {
            g.setColour (kDark);
            g.fillRect (cell);
            g.setColour (kDark);
            g.drawRect (cell, 2);
            g.setColour (kLight);
        }
        else
        {
            g.setColour (kLight);
            g.fillRect (cell);
            g.setColour (kDark);
            g.drawRect (cell, 2);
            g.setColour (kDark);
        }

        drawLabel (cell, label);
    };

    // ORIG is always selectable; STEMS only once a ready set exists.
    paintSegment (origCell,  "ORIG",  ! onStems, true);
    paintSegment (stemsCell, "STEMS", onStems,   stemsReady);
}

// ---------------------------------------------------------------------------
void SourceModeToggleComponent::mouseDown (const juce::MouseEvent& e)
{
    const bool vertical = getHeight() > getWidth();
    const bool clickedStems = vertical
        ? e.position.y > static_cast<float> (getHeight()) * 0.5f
        : e.position.x > static_cast<float> (getWidth())  * 0.5f;
    selectMode (clickedStems);
}

// ---------------------------------------------------------------------------
void SourceModeToggleComponent::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                          const juce::Identifier& property)
{
    const bool relevant =
        (changedTree == tree && property == IDs::sourceMode)
        || (changedTree == stemsNode && property == IDs::status);

    if (! relevant)
        return;

    auto safe = juce::Component::SafePointer<SourceModeToggleComponent> (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe == nullptr)
            return;
        safe->refreshState();
        safe->repaint();
    });
}
