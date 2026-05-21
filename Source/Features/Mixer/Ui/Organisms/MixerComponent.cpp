#include "MixerComponent.h"

#include "../../State/MixerStateSchema.h"
#include "../../State/MixerMeterSnapshot.h"

namespace
{
    const juce::Colour kInk     { 0xFF2D2D2D };
    const juce::Colour kSurface { 0xFFFDFDFD };
    const juce::Colour kHigh    { 0xFFE2E2E2 };

    constexpr int kMasterColW = 64;
    constexpr int kStripGap   = 2;
}

MixerComponent::MixerComponent (MixerStateSchema& schemaIn,
                                  MixerMeterSnapshot& metersIn,
                                  juce::ValueTree   decksNodeIn)
    : schema (schemaIn),
      meters (metersIn),
      decksNode (decksNodeIn),
      masterSection (schema, meters),
      crossfaderRail (schema.getMixerTree())
{
    setOpaque (true);

    for (int i = 0; i < 4; ++i)
    {
        channelStrips[(std::size_t) i]
            = std::make_unique<ChannelStrip> (schema, meters, i);
        addChildComponent (*channelStrips[(std::size_t) i]);
    }

    addAndMakeVisible (crossfaderRail);

    recomputeActiveChannelCount();

    if (decksNode.isValid())
        decksNode.addListener (this);
}

MixerComponent::~MixerComponent()
{
    if (decksNode.isValid())
        decksNode.removeListener (this);
}

ChannelStrip* MixerComponent::getChannelStrip (int idx) noexcept
{
    if (idx < 0 || idx >= 4) return nullptr;
    return channelStrips[(std::size_t) idx].get();
}

void MixerComponent::recomputeActiveChannelCount()
{
    int count = 2;   // A + B always present
    if (decksNode.isValid())
    {
        const int deckCount = decksNode.getNumChildren();
        count = juce::jlimit (2, 4, deckCount);
    }

    activeChannelCount = count;

    for (int i = 0; i < 4; ++i)
    {
        const bool visible = (i < activeChannelCount);
        if (channelStrips[(std::size_t) i] != nullptr)
            channelStrips[(std::size_t) i]->setVisible (visible);
    }
    resized();
}

void MixerComponent::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == decksNode) recomputeActiveChannelCount();
}

void MixerComponent::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent == decksNode) recomputeActiveChannelCount();
}

void MixerComponent::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == decksNode) recomputeActiveChannelCount();
}

void MixerComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    g.setColour (kHigh);
    g.fillRect (bounds);
    g.setColour (kInk);
    g.drawRect (bounds, 2);
}

void MixerComponent::resized()
{
    auto bounds = getLocalBounds().reduced (4);
    if (bounds.isEmpty()) return;

    // Reserve crossfader rail along the bottom.
    auto railArea = bounds.removeFromBottom (kCrossfaderRailH);
    crossfaderRail.setBounds (railArea);

    bounds.removeFromBottom (kStripGap);

    // PRD-0060 (revised): the master section now lives in the global toolbar
    // (compact master knob + meter to the left of the MIDI button). We
    // therefore no longer carve a column off the right of the mixer
    // strip rack. The MasterSection instance is still constructed and
    // bound to the master ValueTree so the master knob test fixture
    // (and any programmatic callers) can manipulate gain via the
    // ValueTree — it simply is not added as a visible child.

    // Distribute remaining width across visible channel strips, clamped to a
    // pleasing per-strip range, and centre the resulting block in the slot.
    const int count = juce::jmax (1, activeChannelCount);
    const int totalGap = kStripGap * (count - 1);
    const int rawStripW = (bounds.getWidth() - totalGap) / count;
    const int stripW = juce::jlimit (80, 160, rawStripW);

    const int blockW = stripW * count + totalGap;
    const int slack  = juce::jmax (0, bounds.getWidth() - blockW);
    int x = bounds.getX() + slack / 2;
    for (int i = 0; i < count; ++i)
    {
        if (auto* strip = channelStrips[(std::size_t) i].get())
            strip->setBounds (x, bounds.getY(), stripW, bounds.getHeight());
        x += stripW + kStripGap;
    }
}
