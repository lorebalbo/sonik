//==============================================================================
// PRD-0100 §1.5.1: LameMp3EncoderBackend implementation.
//
// Compiled ONLY when SONIK_HAVE_LAME is defined. This is the single translation
// unit that includes <lame/lame.h> and links libmp3lame; the dependency is fully
// contained here behind the Mp3EncoderBackend interface. It also defines the
// registration seam (makeMp3EncoderBackend / isMp3BackendAvailable) for the
// LAME-present build.
//
// Encoding model: the exporter pushes float blocks scaled to [-1, 1] (the same
// float the render engine produces). We feed them to lame_encode_buffer_ieee_*
// which expects full-scale +/- 1.0 float, so NO pre-quantisation happens here —
// MP3 is a perceptual codec with no integer bit depth (PRD-0100 §1.5.2).
//==============================================================================

#include "LameMp3EncoderBackend.h"

#include <vector>

#include <lame/lame.h>

namespace Daw::Export
{

//==============================================================================
// Pimpl: owns the LAME context, the output stream, and the scratch encode buffer.
//==============================================================================

struct LameMp3EncoderBackend::Impl
{
    lame_global_flags*                    gfp { nullptr };
    std::unique_ptr<juce::FileOutputStream> out;
    std::vector<unsigned char>            mp3Buffer;   // encoded-bytes scratch
    std::vector<float>                    leftScratch; // de-interleaved L
    std::vector<float>                    rightScratch;// de-interleaved R
    int                                   numChannels { 2 };

    ~Impl()
    {
        if (gfp != nullptr)
            lame_close (gfp);
    }
};

//==============================================================================

LameMp3EncoderBackend::LameMp3EncoderBackend()
    : impl_ (std::make_unique<Impl>())
{
}

LameMp3EncoderBackend::~LameMp3EncoderBackend() = default;

//==============================================================================

bool LameMp3EncoderBackend::initialise (const juce::File& outputFile,
                                        const Mp3EncoderConfig& config)
{
    if (config.numChannels != 2)
    {
        lastError_ = "LAME backend: only stereo (2-channel) export is supported.";
        return false;
    }

    impl_->numChannels = config.numChannels;

    impl_->gfp = lame_init();
    if (impl_->gfp == nullptr)
    {
        lastError_ = "LAME backend: lame_init() failed.";
        return false;
    }

    lame_set_in_samplerate (impl_->gfp, (int) config.sampleRate);
    lame_set_num_channels  (impl_->gfp, config.numChannels);
    lame_set_mode          (impl_->gfp, JOINT_STEREO);

    if (config.vbr)
    {
        lame_set_VBR                   (impl_->gfp, vbr_default);
        lame_set_VBR_mean_bitrate_kbps (impl_->gfp, config.bitrateKbps);
    }
    else
    {
        lame_set_VBR  (impl_->gfp, vbr_off);
        lame_set_brate (impl_->gfp, config.bitrateKbps);
    }

    // ID3 tags (PRD-0100 §1.5.8). Written into the bitstream by LAME.
    if (config.metadata.has_value() && ! config.metadata->isEmpty())
    {
        const auto& md = *config.metadata;
        id3tag_init (impl_->gfp);
        if (md.title.isNotEmpty())
            id3tag_set_title (impl_->gfp, md.title.toRawUTF8());
        if (md.artist.isNotEmpty())
            id3tag_set_artist (impl_->gfp, md.artist.toRawUTF8());
        if (md.comment.isNotEmpty())
            id3tag_set_comment (impl_->gfp, md.comment.toRawUTF8());
    }

    if (lame_init_params (impl_->gfp) < 0)
    {
        lastError_ = "LAME backend: lame_init_params() failed (invalid parameters).";
        return false;
    }

    // Open the destination AFTER the encoder validates — so an invalid config
    // never leaves a zero-byte file behind.
    outputFile.deleteFile();
    impl_->out = outputFile.createOutputStream();
    if (impl_->out == nullptr)
    {
        lastError_ = "LAME backend: could not open output file for writing: "
                     + outputFile.getFullPathName();
        return false;
    }

    return true;
}

//==============================================================================

bool LameMp3EncoderBackend::encodeBlock (const float* const* channelData, int numSamples)
{
    if (impl_->gfp == nullptr || impl_->out == nullptr)
    {
        lastError_ = "LAME backend: encodeBlock() before successful initialise().";
        return false;
    }
    if (numSamples <= 0)
        return true;

    // LAME's worst-case output size: 1.25 * numSamples + 7200 (per its docs).
    const int worstCase = (int) (1.25 * numSamples) + 7200;
    if ((int) impl_->mp3Buffer.size() < worstCase)
        impl_->mp3Buffer.resize ((size_t) worstCase);

    const float* left  = channelData[0];
    const float* right = channelData[1];

    const int encoded = lame_encode_buffer_ieee_float (impl_->gfp,
                                                       left,
                                                       right,
                                                       numSamples,
                                                       impl_->mp3Buffer.data(),
                                                       (int) impl_->mp3Buffer.size());
    if (encoded < 0)
    {
        lastError_ = "LAME backend: lame_encode_buffer_ieee_float() error code "
                     + juce::String (encoded);
        return false;
    }

    if (encoded > 0
        && ! impl_->out->write (impl_->mp3Buffer.data(), (size_t) encoded))
    {
        lastError_ = "LAME backend: failed to write encoded MP3 bytes (disk full?).";
        return false;
    }

    return true;
}

//==============================================================================

bool LameMp3EncoderBackend::finish()
{
    if (impl_->gfp == nullptr || impl_->out == nullptr)
    {
        lastError_ = "LAME backend: finish() before successful initialise().";
        return false;
    }

    // Flush padding + trailing frames (and ID3v1 tags).
    if ((int) impl_->mp3Buffer.size() < 7200)
        impl_->mp3Buffer.resize (7200);

    const int flushed = lame_encode_flush (impl_->gfp,
                                           impl_->mp3Buffer.data(),
                                           (int) impl_->mp3Buffer.size());
    if (flushed < 0)
    {
        lastError_ = "LAME backend: lame_encode_flush() error code " + juce::String (flushed);
        return false;
    }

    bool ok = true;
    if (flushed > 0)
        ok = impl_->out->write (impl_->mp3Buffer.data(), (size_t) flushed);

    impl_->out->flush();
    impl_->out.reset(); // close the stream

    if (! ok)
    {
        lastError_ = "LAME backend: failed to write final MP3 frames.";
        return false;
    }

    return true;
}

//==============================================================================
// Registration seam (LAME-present build).
//==============================================================================

std::unique_ptr<Mp3EncoderBackend> makeMp3EncoderBackend()
{
    return std::make_unique<LameMp3EncoderBackend>();
}

bool isMp3BackendAvailable() noexcept
{
    return true;
}

} // namespace Daw::Export
