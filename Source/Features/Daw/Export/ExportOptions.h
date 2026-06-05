#pragma once
//==============================================================================
// PRD-0100: ExportOptions / ExportResult — the value types that parameterise an
// offline arrangement export and report its outcome.
//
// These are PURE VALUE TYPES (no behaviour, no JUCE component dependency) so the
// export dialog (PRD-0101) can construct an ExportOptions from UI state and the
// headless tests can construct one directly. They live OFF the real-time audio
// thread (the whole export path is offline), so juce::File / juce::String fields
// are fine here — none of this is touched by processBlock.
//==============================================================================

#include <cstdint>
#include <optional>

#include <juce_core/juce_core.h>

namespace Daw::Export
{

//==============================================================================
// Format — the container/codec the arrangement is encoded to.
//==============================================================================

enum class Format
{
    Wav,   // juce::WavAudioFormat  (PCM 16 / 24 / 32-bit-float)
    Flac,  // juce::FlacAudioFormat (PCM 16 / 24-bit integer)
    Mp3    // pluggable Mp3EncoderBackend (LAME when present, else Unsupported)
};

//==============================================================================
// ChannelLayout — stereo only for this PRD (PRD-0100 §1.5.5). The enum exists so
// the type can expand (mono fold-down, multichannel) without an API break; the
// exporter returns InvalidOptions for any non-stereo request.
//==============================================================================

enum class ChannelLayout
{
    Stereo
};

//==============================================================================
// ExportRange — the half-open [startSample, endSample) span to render, in samples
// at the EXPORT sample rate. The whole-arrangement case is
// { 0, arrangementLengthSamples }. This is passed THROUGH to PRD-0099's driver;
// the exporter never seeks the timeline (PRD-0100 §1.5.6).
//==============================================================================

struct ExportRange
{
    int64_t startSample { 0 };
    int64_t endSample   { 0 };

    int64_t lengthSamples() const noexcept
    {
        return endSample > startSample ? endSample - startSample : 0;
    }

    bool isEmpty() const noexcept { return lengthSamples() <= 0; }
};

//==============================================================================
// ExportMetadata — optional container tags written to WAV (INFO chunk), FLAC
// (Vorbis comments) and MP3 (ID3) when present. Empty by default so the audio
// payload is unaffected by its presence/absence (PRD-0100 §1.5.8).
//==============================================================================

struct ExportMetadata
{
    juce::String title;
    juce::String artist;
    juce::String comment;

    bool isEmpty() const noexcept
    {
        return title.isEmpty() && artist.isEmpty() && comment.isEmpty();
    }
};

//==============================================================================
// ExportOptions — the full export contract supplied by the caller.
//==============================================================================

struct ExportOptions
{
    Format        format        { Format::Wav };

    /// One of { 44100, 48000, 96000 } for every format (PRD-0100 §1.5.2).
    double        sampleRate    { 44100.0 };

    /// WAV: { 16, 24, 32 }; FLAC: { 16, 24 }. The value 32 denotes 32-bit FLOAT
    /// for WAV (the only float container). Ignored for MP3.
    int           bitDepth      { 24 };

    /// MP3 only. One of { 128, 192, 256, 320 } kbps (PRD-0100 §1.5.2). For VBR
    /// this is the target/mean bitrate that selects the VBR quality band.
    int           mp3BitrateKbps { 320 };

    /// MP3 only. When true the encoder runs in VBR mode targeting mp3BitrateKbps;
    /// when false it runs CBR at mp3BitrateKbps.
    bool          mp3Vbr        { false };

    ChannelLayout layout        { ChannelLayout::Stereo };

    ExportRange   range         {};

    /// Optional loudness/safety pass (PRD-0100 §1.5.4). OFF by default => the
    /// export is bit-faithful to the summed render (only required dither applies).
    bool          normalize     { false };

    /// The output peak ceiling in dBFS used only when normalize == true.
    float         peakCeilingDb { -1.0f };

    juce::File    outputFile;

    /// Optional container tags. Empty => no tags written.
    std::optional<ExportMetadata> metadata {};
};

//==============================================================================
// ExportResult — the structured outcome handed back from exportArrangement.
//==============================================================================

struct ExportResult
{
    enum class Status
    {
        Completed,      // file written and closed successfully
        Cancelled,      // cancelRequested set mid-export; partial file deleted
        Failed,         // encoder/stream/disk error; partial file deleted
        Unsupported,    // format has no registered backend (MP3, no LAME); no file
        InvalidOptions  // options failed capability validation; no file created
    };

    Status      status         { Status::Failed };

    /// Human-readable diagnostic, populated for Failed / Unsupported / Invalid.
    juce::String message;

    /// True iff an integer-format export encountered samples that clipped at the
    /// integer full-scale ceiling (non-fatal warning, PRD-0100 §1.5.7).
    bool        clipped        { false };

    /// Samples-per-channel written to the file (0 for the no-file statuses).
    int64_t     samplesWritten { 0 };

    bool ok() const noexcept { return status == Status::Completed; }

    static ExportResult invalid (const juce::String& why)
    {
        ExportResult r; r.status = Status::InvalidOptions; r.message = why; return r;
    }
    static ExportResult unsupported (const juce::String& why)
    {
        ExportResult r; r.status = Status::Unsupported; r.message = why; return r;
    }
    static ExportResult failed (const juce::String& why)
    {
        ExportResult r; r.status = Status::Failed; r.message = why; return r;
    }
};

} // namespace Daw::Export
