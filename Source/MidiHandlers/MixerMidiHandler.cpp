#include "MixerMidiHandler.h"

using sonik::midi::MidiMessageEvent;
using sonik::midi::MidiTargetCategory;

bool MixerMidiHandler::tryHandle (const MidiMessageEvent& event)
{
    switch (event.category)
    {
        case MidiTargetCategory::Crossfader:
        case MidiTargetCategory::MasterGain:
        case MidiTargetCategory::HeadphonesGain:
        case MidiTargetCategory::HeadphoneCueToggle:
            return false; // No Mixer feature wired; defer to composite warning.
        default:
            return false;
    }
}
