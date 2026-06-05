//==============================================================================
// PRD-0098: ImportPipelineTests — exercises the NEW import-pipeline surface that
// ImportClipTests does NOT cover:
//
//   * AudioFileImporter static format gate (isSupportedExtension /
//     supportedFormatsWildcard) — the single source of truth for "what Sonik
//     can decode".
//   * ImportSourceRegistry::toSourceRefs() — the PRD-0095/0097 persistence
//     round-trip (External sourceKind, relocation hints).
//   * ImportSourcePublisher + BufferAudioFormatReader — publish/contains/
//     makeReader/withdraw, exact sample read-back, and the "withdraw does not
//     invalidate an already-handed-out reader" guarantee.
//   * ClipSourceResolver import wiring — "import:<hash>" resolves to a reader
//     once published, to nullptr (silence) when unpublished / unset.
//   * AudioFileImporter end-to-end (covers the PRIVATE decodeOne indirectly):
//     a real on-disk WAV at 96 kHz imported onto a lane on a 44.1 kHz session
//     publishes a session-rate, stereo, length-reconciled buffer; an
//     unsupported/empty file reports an error and publishes nothing.
//
// JUCE UnitTest, category "Sonik". Headless. No audio thread.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>

#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Session/SessionSchema.h"
#include "Features/Daw/Import/ImportSource.h"
#include "Features/Daw/Import/ImportSourcePublisher.h"
#include "Features/Daw/Import/ImportClipPlacer.h"
#include "Features/Daw/Import/AudioFileImporter.h"
#include "Features/Daw/Playback/ClipSourceResolver.h"

using namespace Daw::Import;

class ImportPipelineTests final : public juce::UnitTest
{
public:
    ImportPipelineTests() : juce::UnitTest ("Import Pipeline", "Sonik") {}

    void runTest() override
    {
        testSupportedExtensionGate();
        testSupportedFormatsWildcard();
        testReconcileEdgeCases();
        testRegistryToSourceRefs();
        testPublisherReadBack();
        testPublisherWithdrawAndDedup();
        testResolverImportWiring();
        testEndToEndImportResampleAndPublish();
        testEndToEndUnsupportedFileReportsError();
    }

private:
    //==========================================================================
    // Helpers
    //==========================================================================
    static AudioBufferHolder::Ptr makeRamp (int numChannels, int numFrames, double sr)
    {
        juce::AudioBuffer<float> buf (numChannels, numFrames);
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < numFrames; ++i)
                buf.setSample (ch, i, (float) (ch + 1) * (float) i * 0.001f);
        return new AudioBufferHolder (std::move (buf), sr, (int64_t) numFrames);
    }

    // Writes a WAV with a per-channel ramp at the given rate/channels. Returns
    // the (caller-owned) temp file path; caller deletes.
    static juce::File writeTempWav (int numChannels, int numFrames, double sampleRate,
                                    const juce::String& suffix = ".wav")
    {
        auto file = juce::File::createTempFile (suffix);
        file.deleteFile();

        juce::WavAudioFormat fmt;
        if (auto* stream = file.createOutputStream().release())
        {
            std::unique_ptr<juce::AudioFormatWriter> writer (
                fmt.createWriterFor (stream, sampleRate, (unsigned int) numChannels, 16, {}, 0));
            if (writer == nullptr)
            {
                delete stream;
                return {};
            }

            juce::AudioBuffer<float> buf (numChannels, numFrames);
            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = 0; i < numFrames; ++i)
                    buf.setSample (ch, i,
                                   std::sin ((float) i * 0.05f) * (ch == 0 ? 0.5f : 0.25f));
            writer->writeFromAudioSampleBuffer (buf, 0, numFrames);
            writer->flush();
        }
        return file;
    }

    static juce::ValueTree lane0 (juce::ValueTree daw)
    {
        auto t = DawState::ensureTrackForDeck (daw, 0);
        return ChannelGroup::findLane (t.getChildWithName (DawIDs::lanes),
                                       ChannelGroup::LaneKind::Original);
    }

    // Pumps the message loop until predicate() is true or the timeout elapses.
    template <typename Pred>
    bool pumpUntil (Pred predicate, int timeoutMs = 8000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (predicate())
                return true;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        }
        return predicate();
    }

    // Spins the message loop for a fixed window so any background-posted
    // continuations (e.g. WaveformAnalyzer's completion callAsync) are delivered
    // while the owning objects are still alive — preventing a stale message from
    // outliving its captures when the test scope ends.
    void drainMessages (int durationMs = 400)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) durationMs;
        while (juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    }

    //==========================================================================
    void testSupportedExtensionGate()
    {
        beginTest ("isSupportedExtension delegates to the single decode whitelist");

        expect (AudioFileImporter::isSupportedExtension (".wav"),  ".wav supported");
        expect (AudioFileImporter::isSupportedExtension ("wav"),   "no-dot form supported");
        expect (AudioFileImporter::isSupportedExtension (".WAV"),  "case-insensitive");
        expect (AudioFileImporter::isSupportedExtension (".aiff"), ".aiff supported");
        expect (AudioFileImporter::isSupportedExtension (".flac"), ".flac supported");
        expect (AudioFileImporter::isSupportedExtension (".mp3"),  ".mp3 supported");

        expect (! AudioFileImporter::isSupportedExtension (".ogg"), ".ogg not supported");
        expect (! AudioFileImporter::isSupportedExtension (".txt"), ".txt not supported");
        expect (! AudioFileImporter::isSupportedExtension (""),     "empty not supported");
        expect (! AudioFileImporter::isSupportedExtension ("   "),  "whitespace not supported");
    }

    //==========================================================================
    void testSupportedFormatsWildcard()
    {
        beginTest ("supportedFormatsWildcard lists every decodable extension once");

        const auto wildcard = AudioFileImporter::supportedFormatsWildcard();
        expect (wildcard.contains ("*.wav"),  "wildcard includes *.wav");
        expect (wildcard.contains ("*.aiff") || wildcard.contains ("*.aif"),
                "wildcard includes an AIFF form");

        // Every token must be a "*.ext" form for the native chooser, and there
        // must be no duplicates.
        juce::StringArray tokens;
        tokens.addTokens (wildcard, ";", "");
        tokens.removeEmptyStrings();
        expect (tokens.size() > 0, "wildcard is non-empty");

        juce::StringArray seen;
        for (const auto& t : tokens)
        {
            expect (t.startsWith ("*."), "token is a *.ext wildcard: " + t);
            expect (! seen.contains (t), "no duplicate token: " + t);
            seen.add (t);
        }
    }

    //==========================================================================
    void testReconcileEdgeCases()
    {
        beginTest ("reconcileLengthToSessionRate handles fractional + invalid rates");

        // 48 kHz -> 44.1 kHz: 48000 native samples ~ 44100 session samples.
        expectEquals ((int) reconcileLengthToSessionRate (48000, 48000.0, 44100.0), 44100);
        // 44.1 kHz -> 48 kHz upsample.
        expectEquals ((int) reconcileLengthToSessionRate (44100, 44100.0, 48000.0), 48000);
        // Negative / zero session rate is guarded -> input returned unchanged.
        expectEquals ((int) reconcileLengthToSessionRate (7777, 44100.0, 0.0), 7777);
        expectEquals ((int) reconcileLengthToSessionRate (7777, 44100.0, -1.0), 7777);
        expectEquals ((int) reconcileLengthToSessionRate (7777, -1.0, 44100.0), 7777);
        // Zero-length source stays zero regardless of rates.
        expectEquals ((int) reconcileLengthToSessionRate (0, 96000.0, 44100.0), 0);
    }

    //==========================================================================
    void testRegistryToSourceRefs()
    {
        beginTest ("toSourceRefs emits a persistable External SOURCE_REF per source");

        ImportSourceRegistry reg;

        ImportedSourceDescriptor a;
        a.contentHash   = "AAA";
        a.lastKnownPath = "/music/a.wav";
        a.displayName   = "a.wav";
        const auto idA = reg.registerSource (a);

        ImportedSourceDescriptor b;
        b.contentHash   = "BBB";
        b.lastKnownPath = "/music/b.flac";
        b.displayName   = "b.flac";
        const auto idB = reg.registerSource (b);

        // Re-register an identical hash: must NOT add a second ref node.
        reg.registerSource (a);

        auto refs = reg.toSourceRefs();
        expect (refs.hasType (Daw::Session::IDs::SOURCE_REFS), "container is SOURCE_REFS");
        expectEquals (refs.getNumChildren(), 2, "one SOURCE_REF per distinct source");

        // Find the node for source A and assert its persisted fields.
        bool foundA = false, foundB = false;
        for (auto ref : refs)
        {
            expect (ref.hasType (Daw::Session::IDs::SOURCE_REF));
            const auto id = ref.getProperty (Daw::Session::IDs::sourceFileId).toString();
            expectEquals (ref.getProperty (Daw::Session::IDs::sourceKind).toString(),
                          juce::String (Daw::Session::SourceKindStrings::kExternal),
                          "imported sources persist as External");

            if (id == idA)
            {
                foundA = true;
                expectEquals (ref.getProperty (Daw::Session::IDs::lastKnownPath).toString(),
                              juce::String ("/music/a.wav"));
                expectEquals (ref.getProperty (Daw::Session::IDs::displayName).toString(),
                              juce::String ("a.wav"));
            }
            else if (id == idB)
            {
                foundB = true;
            }
        }
        expect (foundA, "SOURCE_REF for source A present");
        expect (foundB, "SOURCE_REF for source B present");

        // find()/contains() round-trip on the minted ids.
        expect (reg.contains (idA));
        expect (reg.find (idB).has_value());
        expect (! reg.contains ("import:NOPE"), "unknown id not present");
    }

    //==========================================================================
    void testPublisherReadBack()
    {
        beginTest ("publisher + BufferAudioFormatReader read the published PCM exactly");

        ImportSourcePublisher pub;
        const juce::String id = "import:READBACK";

        expect (! pub.contains (id), "nothing published yet");
        expect (pub.makeReader (id) == nullptr, "unpublished id -> nullptr reader");

        const int frames = 1024;
        const double sr = 48000.0;
        auto holder = makeRamp (2, frames, sr);
        pub.publish (id, holder);

        expect (pub.contains (id), "source present after publish");

        auto reader = pub.makeReader (id);
        expect (reader != nullptr, "published id -> non-null reader");
        expectEquals ((int) reader->numChannels, 2);
        expectEquals (reader->sampleRate, sr, "reader reports the baked rate");
        expectEquals ((int) reader->lengthInSamples, frames);

        // Read all samples back and compare against the known ramp.
        juce::AudioBuffer<float> out (2, frames);
        out.clear();
        const bool ok = reader->read (&out, 0, frames, 0, true, true);
        expect (ok, "read succeeded");

        bool exact = true;
        for (int ch = 0; ch < 2 && exact; ++ch)
            for (int i = 0; i < frames; ++i)
            {
                const float expected = (float) (ch + 1) * (float) i * 0.001f;
                if (std::abs (out.getSample (ch, i) - expected) > 1.0e-6f)
                {
                    exact = false;
                    break;
                }
            }
        expect (exact, "read-back samples match the published buffer exactly");

        // Publishing null / empty id is a no-op (defensive).
        pub.publish ({}, holder);
        pub.publish ("import:X", nullptr);
        expect (! pub.contains ("import:X"), "null holder is not published");
    }

    //==========================================================================
    void testPublisherWithdrawAndDedup()
    {
        beginTest ("withdraw drops new readers but never invalidates a handed-out one");

        ImportSourcePublisher pub;
        const juce::String id = "import:WITHDRAW";

        auto holder = makeRamp (2, 256, 44100.0);
        pub.publish (id, holder);

        // Hand out a reader, THEN withdraw the entry.
        auto liveReader = pub.makeReader (id);
        expect (liveReader != nullptr);

        pub.withdraw (id);
        expect (! pub.contains (id), "entry removed after withdraw");
        expect (pub.makeReader (id) == nullptr, "no new reader after withdraw -> silence");

        // The already-handed-out reader still reads its retained PCM.
        juce::AudioBuffer<float> out (2, 256);
        out.clear();
        expect (liveReader->read (&out, 0, 256, 0, true, true),
                "in-flight reader survives withdraw");
        expectWithinAbsoluteError (out.getSample (1, 10),
                                   2.0f * 10.0f * 0.001f, 1.0e-6f);

        // Re-publishing the same id (a byte-identical re-import) restores it.
        pub.publish (id, holder);
        expect (pub.contains (id), "re-publish restores the source");
    }

    //==========================================================================
    void testResolverImportWiring()
    {
        beginTest ("ClipSourceResolver routes import:<hash> ids through the publisher");

        auto dbFile = juce::File::createTempFile ("sonik_import_resolver.db");
        {
            TrackDatabase db (dbFile);
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();

            Daw::ClipSourceResolver resolver (db, fm);

            const juce::String importId = "import:RESOLVE";

            // No publisher wired yet -> import ids resolve to silence (nullptr).
            expect (resolver.resolve (importId, "Original") == nullptr,
                    "no publisher -> import id resolves to nullptr");

            ImportSourcePublisher pub;
            resolver.setImportPublisher (&pub);

            // Wired but not published -> still nullptr.
            expect (resolver.resolve (importId, "Original") == nullptr,
                    "unpublished import id -> nullptr (silence)");

            // Publish -> resolves to a real reader regardless of laneKind.
            pub.publish (importId, makeRamp (2, 512, 44100.0));
            auto r = resolver.resolve (importId, "Original");
            expect (r != nullptr, "published import id -> reader");
            expectEquals ((int) r->lengthInSamples, 512);

            // Non-import ids are unaffected by the publisher wiring (unknown hash
            // with no DB entry -> nullptr, never crash).
            expect (resolver.resolve ("unknownhash", "Original") == nullptr,
                    "non-import id with no DB row -> nullptr");
            expect (resolver.resolve ("", "Original") == nullptr,
                    "empty hash -> nullptr");
        }
        dbFile.deleteFile();
    }

    //==========================================================================
    void testEndToEndImportResampleAndPublish()
    {
        beginTest ("import of a 96 kHz WAV bakes a session-rate stereo buffer (decodeOne)");

        const double sessionRate = 44100.0;
        const double nativeRate  = 96000.0;
        const int    nativeFrames = 96000;          // exactly 1 second
        const int    nativeChannels = 1;            // mono -> dual-mono normalisation

        auto wav = writeTempWav (nativeChannels, nativeFrames, nativeRate);
        expect (wav.existsAsFile(), "temp WAV written");

        auto dbFile = juce::File::createTempFile ("sonik_import_e2e.db");
        {
            TrackDatabase db (dbFile);
            auto daw = DawState::createDawBranch();
            auto lane = lane0 (daw);
            juce::UndoManager undo;

            ImportSourceRegistry  registry;
            ImportSourcePublisher publisher;
            ImportClipPlacer      placer (daw, undo, registry);

            std::atomic<bool> placed { false };
            std::atomic<int>  errorCount { 0 };

            AudioFileImporter::Callbacks cb;
            cb.getSessionSampleRate = [sessionRate] { return sessionRate; };
            cb.onClipsPlaced        = [&placed] { placed = true; };
            cb.onError              = [&errorCount] (const juce::String&) { ++errorCount; };

            AudioFileImporter importer (registry, publisher, placer, db, std::move (cb));

            AudioFileImporter::Target target;
            target.lane               = lane;
            target.dropTimelineSample = 0;
            target.snap               = nullptr;

            importer.importFile (wav, target);

            const bool done = pumpUntil ([&] { return placed.load() || errorCount.load() > 0; });
            expect (done, "import finalised on the message thread");
            expectEquals (errorCount.load(), 0, "a valid file produced no error");

            // A clip was placed on the lane.
            auto clips = lane.getChildWithName (DawIDs::clips);
            expectEquals (clips.getNumChildren(), 1, "one clip placed");
            auto clip = DawClip::fromValueTree (clips.getChild (0));

            // The session-rate length must match reconciliation of the native
            // length (within the resampler's ceil rounding of a sample or two).
            const auto expectedLen = reconcileLengthToSessionRate (nativeFrames, nativeRate, sessionRate);
            expect (std::llabs ((long long) clip.sourceLengthSamples - (long long) expectedLen) <= 2,
                    "clip source length reconciled to the session rate");

            // The source was registered + published; its buffer is session-rate stereo.
            expect (registry.size() >= 1, "source registered");
            const auto id = clip.sourceFileId;
            expect (id.startsWith ("import:"), "clip references an import:<hash> id");
            expect (publisher.contains (id), "baked buffer published under the clip id");

            auto reader = publisher.makeReader (id);
            expect (reader != nullptr, "published buffer is readable");
            expectEquals (reader->sampleRate, sessionRate, "baked at the session rate");
            expectEquals ((int) reader->numChannels, 2, "mono normalised to stereo");
            expect (std::llabs ((long long) reader->lengthInSamples - (long long) expectedLen) <= 2,
                    "baked buffer length is session-rate length");

            // The registry descriptor records the native facts for display.
            auto desc = registry.find (id);
            expect (desc.has_value(), "descriptor registered");
            expectEquals (desc->nativeSampleRate, nativeRate, "native rate retained");
            expectEquals ((int) desc->nativeLengthSamples, nativeFrames, "native length retained");

            // Let the WaveformAnalyzer's completion continuation land while the
            // importer (and its analyzer) are still alive, so nothing dangles.
            drainMessages();
        }
        dbFile.deleteFile();
        wav.deleteFile();
    }

    //==========================================================================
    void testEndToEndUnsupportedFileReportsError()
    {
        beginTest ("an unsupported/garbage file reports an error and publishes nothing");

        // A .txt file with non-audio content: createReaderFor returns nullptr.
        auto bogus = juce::File::createTempFile (".txt");
        bogus.replaceWithText ("this is not audio data, just plain text");

        auto dbFile = juce::File::createTempFile ("sonik_import_err.db");
        {
            TrackDatabase db (dbFile);
            auto daw = DawState::createDawBranch();
            auto lane = lane0 (daw);
            juce::UndoManager undo;

            ImportSourceRegistry  registry;
            ImportSourcePublisher publisher;
            ImportClipPlacer      placer (daw, undo, registry);

            std::atomic<bool> placed { false };
            std::atomic<int>  errorCount { 0 };

            AudioFileImporter::Callbacks cb;
            cb.getSessionSampleRate = [] { return 44100.0; };
            cb.onClipsPlaced        = [&placed] { placed = true; };
            cb.onError              = [&errorCount] (const juce::String&) { ++errorCount; };

            AudioFileImporter importer (registry, publisher, placer, db, std::move (cb));

            AudioFileImporter::Target target;
            target.lane               = lane;
            target.dropTimelineSample = 0;
            target.snap               = nullptr;

            importer.importFile (bogus, target);

            const bool gotError = pumpUntil ([&] { return errorCount.load() > 0 || placed.load(); });
            expect (gotError, "import callback fired");
            expectEquals (errorCount.load(), 1, "decode failure reported exactly one error");
            expect (! placed.load(), "no clip placement for an undecodable file");

            expectEquals (lane.getChildWithName (DawIDs::clips).getNumChildren(), 0,
                          "no clip created");
            expectEquals (registry.size(), 0, "no source registered for a failed decode");

            drainMessages();
        }
        dbFile.deleteFile();
        bogus.deleteFile();
    }
};

static ImportPipelineTests importPipelineTests;
