#pragma once

namespace sonik::midi
{
    enum class MidiOpenResult
    {
        Ok,
        AlreadyOpen,
        DeviceNotFound,
        OsRefused
    };
}
