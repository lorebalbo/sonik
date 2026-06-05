#pragma once
//==============================================================================
// PRD-0100 §1.5.1: LameMp3EncoderBackend — the concrete LAME-backed MP3 encoder.
//
// This is the ONE seam allowed to depend on libmp3lame. The LAME header/link is
// confined to LameMp3EncoderBackend.cpp; this header exposes only the abstract
// Mp3EncoderBackend interface so no LAME type leaks into other translation units.
// The class is only declared/compiled when SONIK_HAVE_LAME is defined.
//
// THREADING: offline export background thread only. Allocates / does file I/O.
//==============================================================================

#include <memory>

#include <juce_core/juce_core.h>

#include "Mp3EncoderBackend.h"

namespace Daw::Export
{

class LameMp3EncoderBackend final : public Mp3EncoderBackend
{
public:
    LameMp3EncoderBackend();
    ~LameMp3EncoderBackend() override;

    bool initialise (const juce::File& outputFile,
                     const Mp3EncoderConfig& config) override;

    bool encodeBlock (const float* const* channelData, int numSamples) override;

    bool finish() override;

    juce::String getLastError() const override { return lastError_; }

private:
    struct Impl;                       // hides the LAME global flags + file stream
    std::unique_ptr<Impl> impl_;
    juce::String          lastError_;
};

} // namespace Daw::Export
