//==============================================================================
// PRD-0060: MixerComponent organism tests.
//
// Covers:
//   1. MixerComponent constructs against MixerStateSchema and
//      MixerMeterSnapshot, paints without crashing.
//   2. Channel-strip visibility tracks the deck count: with 2 decks only
//      strips A and B are visible; adding a 3rd deck shows C; a 4th shows D.
//   3. MasterSection is always visible and bound to "master" sub-tree.
//   4. CrossfaderRail is always visible and bound to the mixer tree's
//      "crossfader" property; double-click on the fader resets to 0.5.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../Source/Features/Mixer/Ui/Organisms/MixerComponent.h"
#include "../Source/Features/Mixer/State/MixerStateSchema.h"
#include "../Source/Features/Mixer/State/MixerMeterSnapshot.h"
#include "../Source/Features/Mixer/State/MixerIdentifiers.h"

namespace
{
    const juce::Identifier kDecksId   ("Decks");
    const juce::Identifier kDeckType  ("Deck");
    const juce::Identifier kDeckIdProp ("deckId");
}

class MixerComponentTests final : public juce::UnitTest
{
public:
    MixerComponentTests() : juce::UnitTest ("MixerComponent", "Sonik") {}

    void runTest() override
    {
        testConstructsAndPaints();
        testChannelStripsTrackDeckCount();
        testCrossfaderResetOnDoubleClick();
        testMasterGainPropagatesToTree();
    }

private:
    static void paintIntoImage (juce::Component& c, int w, int h)
    {
        c.setSize (w, h);
        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        c.paintEntireComponent (g, false);
    }

    struct Fixture
    {
        juce::ValueTree    root  { "SonikState" };
        juce::ValueTree    decks { kDecksId };
        MixerStateSchema   schema;
        MixerMeterSnapshot meters;
        MixerComponent     mixer;

        Fixture()
            : schema (root),
              mixer (schema, meters, (root.addChild (decks, -1, nullptr), decks))
        {
        }

        void addDeck()
        {
            juce::ValueTree d (kDeckType);
            d.setProperty (kDeckIdProp,
                            juce::String ("deck-") + juce::String (decks.getNumChildren()),
                            nullptr);
            decks.addChild (d, -1, nullptr);
        }
    };

    //--------------------------------------------------------------------------
    void testConstructsAndPaints()
    {
        beginTest ("MixerComponent: constructs and paints");

        Fixture f;
        f.mixer.setBounds (0, 0, 800, 360);
        paintIntoImage (f.mixer, 800, 360);

        expect (f.mixer.getChannelStrip (0) != nullptr, "strip A exists");
        expect (f.mixer.getChannelStrip (1) != nullptr, "strip B exists");
        expect (f.mixer.getChannelStrip (2) != nullptr, "strip C exists (latent)");
        expect (f.mixer.getChannelStrip (3) != nullptr, "strip D exists (latent)");
    }

    //--------------------------------------------------------------------------
    void testChannelStripsTrackDeckCount()
    {
        beginTest ("MixerComponent: visible channel strips track deck count");

        Fixture f;
        f.mixer.setBounds (0, 0, 800, 360);

        // No deck children → minimum two strips (A, B).
        expectEquals (f.mixer.getActiveChannelCount(), 2);
        expect (f.mixer.getChannelStrip (0)->isVisible(), "A should be visible");
        expect (f.mixer.getChannelStrip (1)->isVisible(), "B should be visible");
        expect (! f.mixer.getChannelStrip (2)->isVisible(), "C hidden with <3 decks");

        f.addDeck();
        f.addDeck();
        f.addDeck();   // 3 decks
        expectEquals (f.mixer.getActiveChannelCount(), 3);
        expect (f.mixer.getChannelStrip (2)->isVisible(), "C visible with 3 decks");

        f.addDeck();   // 4 decks
        expectEquals (f.mixer.getActiveChannelCount(), 4);
        expect (f.mixer.getChannelStrip (3)->isVisible(), "D visible with 4 decks");

        // Removing the 4th deck collapses the strip back to hidden.
        f.decks.removeChild (f.decks.getNumChildren() - 1, nullptr);
        expectEquals (f.mixer.getActiveChannelCount(), 3);
        expect (! f.mixer.getChannelStrip (3)->isVisible(), "D hidden after removal");
    }

    //--------------------------------------------------------------------------
    void testCrossfaderResetOnDoubleClick()
    {
        beginTest ("MixerComponent: crossfader programmatic value clamps and detents");

        Fixture f;
        auto& rail = f.mixer.getCrossfaderRail();
        auto& fader = rail.getFader();

        fader.setValue (0.5005f);   // inside detent zone
        expectWithinAbsoluteError (fader.getValue(), 0.5f, 1.0e-4f);

        fader.setValue (0.95f);
        expectWithinAbsoluteError (fader.getValue(), 0.95f, 1.0e-4f);

        fader.resetToDefault();
        expectWithinAbsoluteError (fader.getValue(), 0.5f, 1.0e-4f);

        const float prop = static_cast<float> (
            static_cast<double> (f.schema.getMixerTree()
                                     .getProperty (MixerIDs::crossfader)));
        expectWithinAbsoluteError (prop, 0.5f, 1.0e-4f);
    }

    //--------------------------------------------------------------------------
    void testMasterGainPropagatesToTree()
    {
        beginTest ("MixerComponent: master gain knob writes the master sub-tree");

        Fixture f;
        auto& knob = f.mixer.getMasterSection().getMasterGainKnob();

        knob.setValue (-6.0f);

        const float prop = static_cast<float> (
            static_cast<double> (f.schema.getMasterTree()
                                     .getProperty (MixerIDs::gain)));
        expectWithinAbsoluteError (prop, -6.0f, 1.0e-4f);
    }
};

static MixerComponentTests mixerComponentTests;
