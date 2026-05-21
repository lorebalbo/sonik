#pragma once
//==============================================================================
// PRD-0060: ChannelStrip — two-column organism for one mixer channel.
//
// Each strip is divided into two vertical columns after the full-width
// channel-letter cap (A / B / C / D).  The column order mirrors the deck
// position so the gain column is always on the inward (deck-facing) side:
//
//   Left deck  (even channelIndex):   col-1 = Gain | col-2 = EQ
//   Right deck (odd  channelIndex):   col-1 = EQ   | col-2 = Gain
//
// Gain column (top → bottom):
//   1. GAIN knob        (DbTapered, compact — no dB label)
//   2. FILTER knob      (Bipolar, compact — no value label)
//   3. A / B crossfader assign buttons
//   4. Level meter      (stereo LR, fills remaining height)
//
// EQ column (top → bottom):
//   1. HIGH knob        (DbTapered, compact, kill overlay)
//   2. MID  knob        (DbTapered, compact, kill overlay)
//   3. LOW  knob        (DbTapered, compact, kill overlay)
//   4. Volume fader     (vertical, fills remaining height)
//
// Min width: 80 px (two columns of ≥ 40 px each).
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../Atoms/MixRotaryKnob.h"
#include "../Atoms/MixAssignButton.h"
#include "../Atoms/MixFader.h"
#include "../Molecules/ChannelStripEqSection.h"
#include "../Molecules/ChannelStripMeter.h"

class MixerStateSchema;
struct MixerMeterSnapshot;

class ChannelStrip final : public juce::Component
{
public:
    ChannelStrip (MixerStateSchema& schema,
                   MixerMeterSnapshot& meters,
                   int channelIndex);

    ~ChannelStrip() override = default;

    void resized() override;
    void paint (juce::Graphics& g) override;

    int getChannelIndex() const noexcept { return channelIndex; }

    // Testing accessors.
    MixRotaryKnob&         getGainKnob()    noexcept { return gainKnob; }
    MixRotaryKnob&         getFilterKnob()  noexcept { return filterKnob; }
    ChannelStripEqSection& getEqSection()   noexcept { return eqSection; }
    ChannelStripMeter&     getMeter()       noexcept { return meterMolecule; }
    MixAssignButton&       getAssignA()     noexcept { return assignA; }
    MixAssignButton&       getAssignB()     noexcept { return assignB; }
    MixFader&              getVolumeFader() noexcept { return volumeFader; }

    static constexpr int kMinWidth = 80;
    static constexpr juce::juce_wchar kChannelLetters[5] = { 'A', 'B', 'C', 'D', '\0' };

private:
    static MixRotaryKnob::Config makeGainConfig();
    static MixRotaryKnob::Config makeFilterConfig();
    static MixFader::Config      makeVolumeFaderConfig();

    int               channelIndex;
    juce::String      channelLetter;

    MixRotaryKnob         gainKnob;
    ChannelStripEqSection eqSection;
    MixRotaryKnob         filterKnob;
    ChannelStripMeter     meterMolecule;
    MixAssignButton       assignA;
    MixAssignButton       assignB;
    MixFader              volumeFader;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
