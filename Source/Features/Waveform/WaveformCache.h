#pragma once
//==============================================================================
// WaveformCache: a shared, message-thread-only memoizing cache for the PRD-0006
// WaveformData mipmaps.
//
// WHY THIS EXISTS (recording-lag fix):
// A DawClip references its source audio by a stable `sourceFileId` (content
// hash). The peak mipmap for that source is immutable, but ClipBlock::paint()
// resolves it through an injected accessor on every repaint — and the production
// accessor read the blob from SQLite and ran a full WaveformData::deserialize
// each time (a multi-megabyte allocation + copy for a multi-minute track). With
// the live recorder repainting the growing clip at 60 Hz, and every loop pass /
// jump spawning more clips that re-deserialize on any full repaint, this turned
// into hundreds of MB/sec of disk I/O + allocation on the message thread — the
// root cause of the recording-mode lag.
//
// This cache loads + deserializes each source exactly ONCE and hands out the
// shared, reference-counted Ptr to every clip thereafter (the "one sample, many
// clip references" model used by professional DAWs). A miss (source not yet
// analysed) is intentionally NOT cached, so a clip whose analysis lands later
// still picks it up on a subsequent paint.
//
// MESSAGE / UI THREAD ONLY. WaveformData is reference-counted, so the shared
// Ptr keeps the peaks alive for as long as any clip references them; no locks.
//==============================================================================

#include <functional>
#include <string>
#include <unordered_map>

#include <juce_core/juce_core.h>

#include "WaveformData.h"

namespace Daw
{

class WaveformCache
{
public:
    // The expensive loader (SQLite read + deserialize in production). Returns
    // nullptr when the source's peaks are not available yet.
    using Loader = std::function<WaveformData::Ptr (const juce::String& sourceFileId)>;

    explicit WaveformCache (Loader loader)
        : loader_ (std::move (loader))
    {
    }

    // Returns the shared peaks for `sourceFileId`, loading them on first request
    // only. Subsequent calls for the same id are O(1) map lookups returning the
    // same Ptr — no SQLite, no deserialize, no allocation.
    WaveformData::Ptr get (const juce::String& sourceFileId)
    {
        if (sourceFileId.isEmpty())
            return nullptr;

        const std::string key = sourceFileId.toStdString();

        auto it = map_.find (key);
        if (it != map_.end())
            return it->second;          // already loaded -> shared, free.

        WaveformData::Ptr data = loader_ ? loader_ (sourceFileId) : nullptr;

        // Only cache a successful load. A miss means the source has not been
        // analysed yet; caching the nullptr would permanently hide a waveform
        // that becomes available later, so we let the next paint retry.
        if (data != nullptr)
            map_.emplace (key, data);

        return data;
    }

    // Drop all cached entries (e.g. on project close). The audio path never
    // touches this cache, so this is safe to call any time on the message thread.
    void clear() noexcept { map_.clear(); }

    // Number of distinct sources currently cached. Test / diagnostic hook.
    int size() const noexcept { return static_cast<int> (map_.size()); }

    // True if `sourceFileId` is already resident (no load would occur). Test hook.
    bool contains (const juce::String& sourceFileId) const
    {
        return map_.find (sourceFileId.toStdString()) != map_.end();
    }

private:
    Loader                                              loader_;
    std::unordered_map<std::string, WaveformData::Ptr>  map_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformCache)
};

} // namespace Daw
