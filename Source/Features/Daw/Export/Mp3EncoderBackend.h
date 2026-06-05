#pragma once
//==============================================================================
// PRD-0100 §1.5.1: Mp3EncoderBackend — the pluggable MP3 encoder seam.
//
// AudioExporter has NO direct dependency on any concrete MP3 encoder library.
// It talks ONLY to this interface and obtains an instance through the factory
// makeMp3EncoderBackend() (the "registration seam"). Exactly one concrete
// backend is compiled in at build-configuration time:
//
//   - SONIK_HAVE_LAME defined  -> LameMp3EncoderBackend.cpp is compiled and the
//                                 factory returns a LAME-backed instance.
//   - SONIK_HAVE_LAME undefined -> NO concrete backend TU is compiled, the
//                                 factory (defined in NoMp3EncoderBackend.cpp)
//                                 returns nullptr, and the capability table
//                                 reports MP3 as UNSUPPORTED.
//
// The concrete LAME include/link is confined to LameMp3EncoderBackend.cpp; it
// never leaks into AudioExporter or any other translation unit.
//
// THREADING: every method runs on the offline export BACKGROUND thread. The
// backend may allocate / lock / do I/O freely (it is NOT real-time code), but it
// must never touch the audio device or processBlock.
//==============================================================================

#include <memory>

#include <juce_core/juce_core.h>

#include "ExportOptions.h"

namespace Daw::Export
{

//==============================================================================
// Mp3EncoderConfig — the init contract handed to a backend before encoding.
//==============================================================================

struct Mp3EncoderConfig
{
    double sampleRate   { 44100.0 };
    int    numChannels  { 2 };          // stereo only for this PRD
    int    bitrateKbps  { 320 };        // CBR rate, or VBR mean/target band
    bool   vbr          { false };

    /// Optional ID3 tags written into the bitstream (empty => none).
    std::optional<ExportMetadata> metadata {};
};

//==============================================================================
// Mp3EncoderBackend — the abstract MP3 sink. The exporter pushes float blocks
// (range [-1, 1], one pointer per channel) and finalises with finish(), which
// flushes the encoder's internal buffers and writes any trailing frames.
//
// Each call returns true on success; false signals an encoder/stream error and
// the exporter aborts to a Failed result (deleting the partial file).
//==============================================================================

class Mp3EncoderBackend
{
public:
    virtual ~Mp3EncoderBackend() = default;

    /// Open the destination file and initialise the encoder. Must be called once
    /// before any encodeBlock(). Returns false if the file cannot be opened or
    /// the encoder cannot be initialised (the exporter then returns Failed).
    virtual bool initialise (const juce::File& outputFile,
                             const Mp3EncoderConfig& config) = 0;

    /// Encode one block of `numSamples` per-channel float samples. `channelData`
    /// holds `config.numChannels` pointers, each `numSamples` long, in [-1, 1].
    /// Returns false on an encode/write error.
    virtual bool encodeBlock (const float* const* channelData, int numSamples) = 0;

    /// Flush the encoder, write trailing frames + ID3v1, and close the file.
    /// Returns false on a flush/write error. After finish() the backend is spent.
    virtual bool finish() = 0;

    /// The last diagnostic message (empty when no error occurred).
    virtual juce::String getLastError() const = 0;
};

//==============================================================================
// Registration seam. Returns a fresh backend instance, or nullptr when no MP3
// backend is compiled into this build (SONIK_HAVE_LAME undefined). The exporter
// and the capability table use a nullptr return to mean "MP3 unsupported".
//
// Implemented in LameMp3EncoderBackend.cpp (SONIK_HAVE_LAME) OR in
// NoMp3EncoderBackend.cpp (otherwise) — exactly one is compiled.
//==============================================================================

std::unique_ptr<Mp3EncoderBackend> makeMp3EncoderBackend();

/// Compile-time-resolved capability flag mirroring the factory. True iff a
/// concrete MP3 backend is compiled in.
bool isMp3BackendAvailable() noexcept;

} // namespace Daw::Export
