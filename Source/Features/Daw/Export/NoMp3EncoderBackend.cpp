//==============================================================================
// PRD-0100 §1.5.1: the "no MP3 backend" registration seam.
//
// This translation unit is compiled ONLY when SONIK_HAVE_LAME is undefined (no
// MP3 encoder library was found at configure time). It provides the factory that
// reports MP3 as unsupported: makeMp3EncoderBackend() returns nullptr and
// isMp3BackendAvailable() returns false. The capability table reads these so
// Format::Mp3 is reported UNSUPPORTED and exportArrangement returns Unsupported
// without creating a file.
//
// When SONIK_HAVE_LAME IS defined this file must NOT be in the build (the LAME
// TU provides the factory instead) — CMake selects exactly one.
//==============================================================================

#include "Mp3EncoderBackend.h"

namespace Daw::Export
{

std::unique_ptr<Mp3EncoderBackend> makeMp3EncoderBackend()
{
    return nullptr; // no concrete MP3 encoder compiled in
}

bool isMp3BackendAvailable() noexcept
{
    return false;
}

} // namespace Daw::Export
