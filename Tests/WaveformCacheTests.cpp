//==============================================================================
// WaveformCache tests (recording-lag fix).
//
// The cache is the core of the recording-performance fix: each source's
// multi-megabyte mipmap must be loaded/deserialized EXACTLY ONCE and then shared
// by reference with every clip, instead of being re-read from SQLite and
// re-deserialized on every ClipBlock::paint. These tests pin that contract:
//   - first request loads, repeat requests are free and return the SAME object,
//   - distinct sources are loaded independently,
//   - a miss is never cached (so late analysis is still picked up),
//   - an empty id never touches the loader,
//   - clear() forces a reload.
//==============================================================================

#include <juce_core/juce_core.h>

#include "Features/Waveform/WaveformCache.h"
#include "Features/Waveform/WaveformData.h"

namespace
{

using namespace Daw;

WaveformData::Ptr makeWaveform()
{
    auto data = WaveformData::Ptr (new WaveformData());
    data->sampleRate   = 44100.0;
    data->totalSamples = 44100;
    data->levels.resize (1);
    data->levels[0].resize (4);
    return data;
}

} // namespace

class WaveformCacheTests final : public juce::UnitTest
{
public:
    WaveformCacheTests() : juce::UnitTest ("Waveform Cache (recording-lag fix)", "Sonik") {}

    void runTest() override
    {
        beginTest ("A source is loaded exactly once and shared by reference");
        {
            int loads = 0;
            auto shared = makeWaveform();

            WaveformCache cache ([&] (const juce::String& id) -> WaveformData::Ptr
            {
                ++loads;
                return id == "hashA" ? shared : nullptr;
            });

            auto a1 = cache.get ("hashA");
            auto a2 = cache.get ("hashA");
            auto a3 = cache.get ("hashA");

            expect (a1 != nullptr);
            expectEquals (loads, 1);                 // loaded once despite 3 gets
            expect (a1.get() == a2.get());           // same shared object
            expect (a2.get() == a3.get());
            expect (a1.get() == shared.get());
            expect (cache.contains ("hashA"));
            expectEquals (cache.size(), 1);
        }

        beginTest ("Distinct sources are loaded and cached independently");
        {
            int loads = 0;
            WaveformCache cache ([&] (const juce::String&) -> WaveformData::Ptr
            {
                ++loads;
                return makeWaveform();
            });

            auto a = cache.get ("hashA");
            auto b = cache.get ("hashB");
            (void) cache.get ("hashA"); // cached
            (void) cache.get ("hashB"); // cached

            expect (a != nullptr && b != nullptr);
            expect (a.get() != b.get());
            expectEquals (loads, 2);                 // one load per distinct id
            expectEquals (cache.size(), 2);
        }

        beginTest ("A miss is NOT cached, so a later-available analysis is picked up");
        {
            int  loads     = 0;
            bool available = false;
            auto shared    = makeWaveform();

            WaveformCache cache ([&] (const juce::String&) -> WaveformData::Ptr
            {
                ++loads;
                return available ? shared : nullptr;
            });

            expect (cache.get ("hashA") == nullptr); // not analysed yet -> miss
            expect (cache.get ("hashA") == nullptr); // retried (not cached)
            expectEquals (loads, 2);
            expect (! cache.contains ("hashA"));
            expectEquals (cache.size(), 0);

            available = true;
            auto now = cache.get ("hashA");          // analysis has landed
            expect (now != nullptr);
            expectEquals (loads, 3);
            expect (cache.contains ("hashA"));

            (void) cache.get ("hashA");              // now cached
            expectEquals (loads, 3);
        }

        beginTest ("An empty id short-circuits to nullptr without touching the loader");
        {
            int loads = 0;
            WaveformCache cache ([&] (const juce::String&) -> WaveformData::Ptr
            {
                ++loads;
                return makeWaveform();
            });

            expect (cache.get ({}) == nullptr);
            expect (cache.get (juce::String()) == nullptr);
            expectEquals (loads, 0);
            expectEquals (cache.size(), 0);
        }

        beginTest ("clear() drops the cache so the next request reloads");
        {
            int loads = 0;
            WaveformCache cache ([&] (const juce::String&) -> WaveformData::Ptr
            {
                ++loads;
                return makeWaveform();
            });

            (void) cache.get ("hashA");
            expectEquals (loads, 1);

            cache.clear();
            expectEquals (cache.size(), 0);
            expect (! cache.contains ("hashA"));

            (void) cache.get ("hashA");
            expectEquals (loads, 2);                 // reloaded after clear
        }
    }
};

static WaveformCacheTests waveformCacheTests;
