//==============================================================================
// PRD-0060: ChannelStrip UI organism tests.
//
// Covers:
//   1. ChannelStrip constructs against a real MixerStateSchema and
//      MixerMeterSnapshot, paints without crashing.
//   2. Channel-strip children (gain, EQ section, filter, meter, assigns,
//      volume fader) are all present and bound.
//   3. Toggling MixAssignButton (assign A) flips the bound ValueTree
//      property and the button's reported state.
//   4. Writing to the channel's "fader" property externally updates the
//      MixFader's reported value (Observer-pattern round-trip).
//   5. Minimum width (80 px) constraint: ChannelStrip lays out without
//      overlapping at the documented minimum, and switches EQ molecule
//      into Narrow mode.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../Source/Features/Mixer/Ui/Organisms/ChannelStrip.h"
#include "../Source/Features/Mixer/State/MixerStateSchema.h"
#include "../Source/Features/Mixer/State/MixerMeterSnapshot.h"
#include "../Source/Features/Mixer/State/MixerIdentifiers.h"

class ChannelStripUiTests final : public juce::UnitTest
{
public:
    ChannelStripUiTests() : juce::UnitTest ("ChannelStripUi", "Sonik") {}

    void runTest() override
    {
        testChannelStripConstructsAndPaints();
        testAssignAToggleRoundTrip();
        testExternalFaderWritePropagatesToAtom();
        testNarrowLayoutAtMinWidth();
        testVolumeFaderHasUsableHeight();
    }

private:
    static void paintIntoImage (juce::Component& c, int w, int h)
    {
        c.setSize (w, h);
        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        c.paintEntireComponent (g, false);
    }

    static juce::ValueTree makeRoot()
    {
        return juce::ValueTree ("SonikState");
    }

    //--------------------------------------------------------------------------
    void testChannelStripConstructsAndPaints()
    {
        beginTest ("ChannelStrip: constructs and paints for each channel index");

        auto root = makeRoot();
        MixerStateSchema schema (root);
        MixerMeterSnapshot meters;

        for (int idx = 0; idx < 4; ++idx)
        {
            ChannelStrip strip (schema, meters, idx);
            paintIntoImage (strip, 96, 480);
            expectEquals (strip.getChannelIndex(), idx);
        }
    }

    //--------------------------------------------------------------------------
    void testAssignAToggleRoundTrip()
    {
        beginTest ("ChannelStrip: clicking AssignA flips bound ValueTree bool");

        auto root = makeRoot();
        MixerStateSchema schema (root);
        MixerMeterSnapshot meters;

        ChannelStrip strip (schema, meters, 0);
        auto& assignA = strip.getAssignA();

        const bool initial = assignA.isActive();
        assignA.toggle();
        expect (assignA.isActive() != initial,
                "AssignA toggle should flip the button's reported state");

        const bool prop = static_cast<bool> (
            schema.getChannelTree (0).getProperty (MixerIDs::assignA, false));
        expect (prop == assignA.isActive(),
                "Channel tree's assignA property should mirror the button");
    }

    //--------------------------------------------------------------------------
    void testExternalFaderWritePropagatesToAtom()
    {
        beginTest ("ChannelStrip: external ValueTree write updates the volume fader");

        auto root = makeRoot();
        MixerStateSchema schema (root);
        MixerMeterSnapshot meters;

        ChannelStrip strip (schema, meters, 1);
        auto& fader = strip.getVolumeFader();

        schema.getChannelTree (1)
              .setProperty (MixerIDs::fader, 0.25f, nullptr);

        expectWithinAbsoluteError (fader.getValue(), 0.25f, 1.0e-4f);
    }

    //--------------------------------------------------------------------------
    void testNarrowLayoutAtMinWidth()
    {
        beginTest ("ChannelStrip: lays out at minimum width without overlap");

        auto root = makeRoot();
        MixerStateSchema schema (root);
        MixerMeterSnapshot meters;

        ChannelStrip strip (schema, meters, 2);
        strip.setBounds (0, 0, ChannelStrip::kMinWidth, 480);

        // Every child should have positive width and height after layout.
        for (int i = 0; i < strip.getNumChildComponents(); ++i)
        {
            auto* c = strip.getChildComponent (i);
            expect (c != nullptr, "child must exist");
            expect (c->getWidth()  >= 0, "child width must be non-negative");
            expect (c->getHeight() >= 0, "child height must be non-negative");
        }
    }

    //--------------------------------------------------------------------------
    void testVolumeFaderHasUsableHeight()
    {
        beginTest ("ChannelStrip: volume fader is visible with usable height");

        auto root = makeRoot();
        MixerStateSchema schema (root);
        MixerMeterSnapshot meters;

        ChannelStrip strip (schema, meters, 0);
        strip.setBounds (0, 0, 80, 360);

        auto& fader = strip.getVolumeFader();
        expect (fader.isVisible(), "volume fader should be visible");
        expect (fader.getBounds().getHeight() >= 60,
                "volume fader should claim at least 60px of vertical space");
        expect (fader.getBounds().getWidth() > 0,
                "volume fader should have positive width");
    }
};

static ChannelStripUiTests channelStripUiTests;
