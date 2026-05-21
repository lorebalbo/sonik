#pragma once
//==============================================================================
// PRD-0060: MasterSection — master gain knob + master level meter.
//
// Lives in the top-right of the mixer organism. Bound to the "master"
// sub-tree (PRD-0052). Master meter sources from MixerMeterSnapshot::master.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../Atoms/MixRotaryKnob.h"
#include "../Atoms/MixLevelMeter.h"

class MixerStateSchema;
struct MixerMeterSnapshot;

class MasterSection final : public juce::Component
{
public:
    MasterSection (MixerStateSchema& schema,
                    MixerMeterSnapshot& meters);

    ~MasterSection() override = default;

    void resized() override;
    void paint (juce::Graphics& g) override;

    MixRotaryKnob& getMasterGainKnob() noexcept { return masterGainKnob; }
    MixLevelMeter& getMasterMeter()    noexcept { return masterMeter; }

private:
    static MixRotaryKnob::Config makeMasterGainConfig();

    MixRotaryKnob masterGainKnob;
    MixLevelMeter masterMeter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterSection)
};
