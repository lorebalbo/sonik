//==============================================================================
// PRD-0059: Mixer UI Atom tests.
//
// Covers:
//   1. MixRotaryKnob constructs, paints into a juce::Image without
//      crashing, round-trips setValue → getValue, propagates to its bound
//      ValueTree property, and double-click resets to declared default.
//   2. Bipolar MixRotaryKnob detents (snap to centre within deadzone).
//   3. MixKillButton constructs, paints, toggles its internal state and
//      its bound bool ValueTree property, and reflects external writes.
//   4. MixAssignButton — same contract as MixKillButton.
//   5. MixLevelMeter constructs against a real ChannelMeterSlots block,
//      pulls fresh atomics via pollNow(), paints without crashing, and
//      clears the latched clip flag on click.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../Source/Features/Mixer/Ui/Atoms/MixRotaryKnob.h"
#include "../Source/Features/Mixer/Ui/Atoms/MixKillButton.h"
#include "../Source/Features/Mixer/Ui/Atoms/MixAssignButton.h"
#include "../Source/Features/Mixer/Ui/Atoms/MixLevelMeter.h"
#include "../Source/Features/Mixer/State/MixerMeterSnapshot.h"

#include <atomic>
#include <cmath>

class MixerUiAtomsTests final : public juce::UnitTest
{
public:
    MixerUiAtomsTests() : juce::UnitTest ("MixerUiAtoms", "Sonik") {}

    void runTest() override
    {
        testRotaryKnobConstructsAndBinds();
        testRotaryKnobDoubleClickResetsToDefault();
        testRotaryKnobBipolarDetent();
        testKillButtonToggleFlipsTreeAndPaints();
        testKillButtonReflectsExternalWrite();
        testAssignButtonToggleFlipsTreeAndPaints();
        testLevelMeterPollsSnapshotAndPaints();
        testLevelMeterClearsClipOnClick();
    }

private:
    // Small helper: paint a component into an off-screen image. If anything
    // throws / crashes, the test fails.
    static void paintIntoImage (juce::Component& c, int w = 64, int h = 80)
    {
        c.setSize (w, h);
        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        c.paintEntireComponent (g, false);
    }

    //--------------------------------------------------------------------------
    void testRotaryKnobConstructsAndBinds()
    {
        beginTest ("MixRotaryKnob: constructs, two-way binds to ValueTree, paints");

        juce::ValueTree tree ("Channel");
        const juce::Identifier propId ("gain");

        MixRotaryKnob::Config cfg;
        cfg.label        = "GAIN";
        cfg.taper        = MixRotaryKnob::Normalisation::DbTapered;
        cfg.minValue     = -60.0f;
        cfg.maxValue     =  12.0f;
        cfg.defaultValue =   0.0f;

        MixRotaryKnob knob (tree, propId, cfg);

        // Initial value = default (no property on tree yet).
        expectWithinAbsoluteError (knob.getValue(), 0.0f, 1.0e-5f);

        // setValue → ValueTree updates.
        knob.setValue (-6.0f);
        expectWithinAbsoluteError (knob.getValue(), -6.0f, 1.0e-5f);
        expectWithinAbsoluteError (static_cast<float> (
            static_cast<double> (tree.getProperty (propId))), -6.0f, 1.0e-5f);

        // External write → knob picks it up via listener.
        tree.setProperty (propId, 3.0f, nullptr);
        expectWithinAbsoluteError (knob.getValue(), 3.0f, 1.0e-5f);

        // Out-of-range clamp.
        knob.setValue (999.0f);
        expectWithinAbsoluteError (knob.getValue(), 12.0f, 1.0e-5f);
        knob.setValue (-999.0f);
        expectWithinAbsoluteError (knob.getValue(), -60.0f, 1.0e-5f);

        // Paint should not crash.
        paintIntoImage (knob);
    }

    void testRotaryKnobDoubleClickResetsToDefault()
    {
        beginTest ("MixRotaryKnob: resetToDefault() snaps back to declared default");

        juce::ValueTree tree ("Channel");
        const juce::Identifier propId ("filter");

        MixRotaryKnob::Config cfg;
        cfg.label        = "FILTER";
        cfg.taper        = MixRotaryKnob::Normalisation::Bipolar;
        cfg.minValue     = -1.0f;
        cfg.maxValue     =  1.0f;
        cfg.defaultValue =  0.0f;

        MixRotaryKnob knob (tree, propId, cfg);
        knob.setValue (0.75f);
        expectWithinAbsoluteError (knob.getValue(), 0.75f, 1.0e-5f);

        // Double-click invokes resetToDefault() — exercise the documented
        // public reset entry point directly (mouseDoubleClick simply calls
        // resetToDefault, which is also the testable API path).
        knob.resetToDefault();
        expectWithinAbsoluteError (knob.getValue(), 0.0f, 1.0e-5f);
        expectWithinAbsoluteError (static_cast<float> (
            static_cast<double> (tree.getProperty (propId))), 0.0f, 1.0e-5f);
    }

    void testRotaryKnobBipolarDetent()
    {
        beginTest ("MixRotaryKnob: bipolar taper snaps to centre inside deadzone");

        juce::ValueTree tree ("Channel");
        const juce::Identifier propId ("filter");

        MixRotaryKnob::Config cfg;
        cfg.label           = "FILTER";
        cfg.taper           = MixRotaryKnob::Normalisation::Bipolar;
        cfg.minValue        = -1.0f;
        cfg.maxValue        =  1.0f;
        cfg.defaultValue    =  0.0f;
        cfg.bipolarDeadzone = 0.05f;

        MixRotaryKnob knob (tree, propId, cfg);

        knob.setValue (0.02f);
        expect (knob.getValue() == 0.0f, "0.02 should snap to centre");
        knob.setValue (-0.04f);
        expect (knob.getValue() == 0.0f, "-0.04 should snap to centre");

        knob.setValue (0.5f);
        expectWithinAbsoluteError (knob.getValue(), 0.5f, 1.0e-5f);
    }

    //--------------------------------------------------------------------------
    void testKillButtonToggleFlipsTreeAndPaints()
    {
        beginTest ("MixKillButton: toggle flips ValueTree bool and paint inverts");

        juce::ValueTree tree ("Channel");
        const juce::Identifier propId ("killMid");

        MixKillButton btn (tree, propId, "KILL");
        expect (! btn.isActive(), "initial inactive");

        btn.toggle();
        expect (btn.isActive(), "after toggle: active");
        expect (static_cast<bool> (tree.getProperty (propId, false)),
                "tree property flipped to true");

        // Paint both states.
        paintIntoImage (btn, 32, 16);
        btn.toggle();
        expect (! btn.isActive(), "second toggle returns to inactive");
        paintIntoImage (btn, 32, 16);
    }

    void testKillButtonReflectsExternalWrite()
    {
        beginTest ("MixKillButton: external tree write propagates via listener");

        juce::ValueTree tree ("Channel");
        const juce::Identifier propId ("killHigh");

        MixKillButton btn (tree, propId, "KILL");
        expect (! btn.isActive());

        tree.setProperty (propId, true, nullptr);
        expect (btn.isActive(), "external write → button active");

        tree.setProperty (propId, false, nullptr);
        expect (! btn.isActive(), "external write → button inactive");
    }

    void testAssignButtonToggleFlipsTreeAndPaints()
    {
        beginTest ("MixAssignButton: toggle flips ValueTree bool and label paints");

        juce::ValueTree tree ("Channel");
        const juce::Identifier propA ("assignA");
        const juce::Identifier propB ("assignB");

        MixAssignButton a (tree, propA, "A");
        MixAssignButton b (tree, propB, "B");

        expect (! a.isActive());
        expect (! b.isActive());

        a.toggle();
        expect (a.isActive());
        expect (static_cast<bool> (tree.getProperty (propA, false)));

        b.toggle();
        expect (b.isActive());
        expect (static_cast<bool> (tree.getProperty (propB, false)));

        expect (a.getLabelText() == juce::String ("A"));
        expect (b.getLabelText() == juce::String ("B"));

        paintIntoImage (a, 32, 16);
        paintIntoImage (b, 32, 16);
    }

    //--------------------------------------------------------------------------
    void testLevelMeterPollsSnapshotAndPaints()
    {
        beginTest ("MixLevelMeter: pollNow() reads atomic snapshot; paint succeeds");

        MixerMeterSnapshot snap;
        ChannelMeterSlots& slots = snap.getChannel (0);

        // Audio-thread style stores into the snapshot.
        slots.levelPeakL    .store (0.5f,  std::memory_order_relaxed);
        slots.levelPeakR    .store (0.25f, std::memory_order_relaxed);
        slots.levelPeakHoldL.store (0.75f, std::memory_order_relaxed);
        slots.levelPeakHoldR.store (0.40f, std::memory_order_relaxed);
        slots.clip          .store (false, std::memory_order_relaxed);

        MixLevelMeter meter (slots, "CH");
        meter.pollNow();      // exercises atomic load + repaint trigger

        paintIntoImage (meter, MixLevelMeter::kMinimumWidth,
                                 MixLevelMeter::kMinimumHeight);

        // Update snapshot, re-poll, re-paint.
        slots.levelPeakL.store (0.9f, std::memory_order_relaxed);
        slots.clip      .store (true, std::memory_order_relaxed);
        meter.pollNow();
        paintIntoImage (meter, MixLevelMeter::kMinimumWidth,
                                 MixLevelMeter::kMinimumHeight);
    }

    void testLevelMeterClearsClipOnClick()
    {
        beginTest ("MixLevelMeter: clearClip() resets the latched clip atomic");

        MixerMeterSnapshot snap;
        ChannelMeterSlots& slots = snap.getChannel (1);

        slots.clip.store (true, std::memory_order_relaxed);

        MixLevelMeter meter (slots, "CH");
        meter.pollNow();
        expect (slots.clip.load (std::memory_order_relaxed),
                "precondition: clip latched");

        meter.clearClip();
        expect (! slots.clip.load (std::memory_order_relaxed),
                "clearClip() resets the clip atomic to false");
    }
};

static MixerUiAtomsTests s_mixerUiAtomsTests;
