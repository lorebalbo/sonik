#include "StemSeparationEngine.h"
#include "ModelManager.h"
#include <cmath>

StemSeparationEngine::StemSeparationEngine (const juce::String& deckId_,
                                             const juce::String& contentHash_,
                                             AudioBufferHolder::Ptr sourceBuffer_,
                                             StemCache& cache,
                                             juce::ValueTree stemsNode_,
                                             double deviceRate,
                                             const juce::String& pythonPath_,
                                             const juce::File& scriptPath_,
                                             const juce::File& modelDir_,
                                             CompletionCallback callback,
                                             std::function<bool()> shouldCancelCallback)
    : juce::ThreadPoolJob ("StemSep_" + deckId_),
      deckId (deckId_),
      contentHash (contentHash_),
      sourceBuffer (std::move (sourceBuffer_)),
      stemCache (cache),
      stemsNode (std::move (stemsNode_)),
      deviceSampleRate (deviceRate),
      pythonPath (pythonPath_),
      scriptPath (scriptPath_),
      modelDir (modelDir_),
    completionCallback (std::move (callback)),
    externalShouldCancel (std::move (shouldCancelCallback))
{
}

// ============================================================================
// Main pipeline
// ============================================================================

StemSeparationEngine::JobStatus StemSeparationEngine::runJob()
{
    lastProgressTime = juce::Time::getMillisecondCounterHiRes();

    reportProgress (0.0f);

    // 1. Insert pending DB record
    stemCache.insertPendingRecord (contentHash,
                                    juce::String (ModelManager::getModelVersion()));

    if (shouldCancel())
    {
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    // 2. Resample source to model rate (44100 Hz) if needed
    juce::AudioBuffer<float> modelRateBuffer;
    if (! resampleToModelRate (modelRateBuffer))
        return jobHasFinished;

    if (shouldCancel())
    {
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.05f);

    // 3. Write source to temporary WAV file
    auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("sonik_stem_" + contentHash);
    tempDir.createDirectory();

    auto inputWav = tempDir.getChildFile ("input.wav");

    if (! writeSourceToWav (modelRateBuffer, inputWav))
        return jobHasFinished;

    if (shouldCancel())
    {
        tempDir.deleteRecursively();
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.10f);

    // 4. Run Python subprocess for separation
    auto outputDir = tempDir.getChildFile ("output");
    outputDir.createDirectory();

    juce::File vocalsFile, instrumentalFile;

    if (! runPythonSeparation (inputWav, outputDir, vocalsFile, instrumentalFile))
    {
        tempDir.deleteRecursively();
        return jobHasFinished;
    }

    if (shouldCancel())
    {
        tempDir.deleteRecursively();
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.85f);

    // 5. Read output WAV files
    auto vocalsHolder = readWavFile (vocalsFile);
    auto instrumentalHolder = readWavFile (instrumentalFile);

    if (vocalsHolder == nullptr || instrumentalHolder == nullptr)
    {
        reportError ("Failed to read separated stem WAV files");
        tempDir.deleteRecursively();
        return jobHasFinished;
    }

    if (shouldCancel())
    {
        tempDir.deleteRecursively();
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.90f);

    // 6. Assemble StemData (4 slots: vocals, instrumental, silence, silence)
    const int numSamples = modelRateBuffer.getNumSamples();
    auto stemResult = new StemData();

    if (! assembleStemData (vocalsHolder, instrumentalHolder, numSamples, *stemResult))
    {
        tempDir.deleteRecursively();
        return jobHasFinished;
    }

    StemData::Ptr stemPtr (stemResult);

    // 7. Resample stems back to device rate if needed
    if (std::abs (deviceSampleRate - kModelSampleRate) > 0.01)
    {
        for (int s = 0; s < StemData::NumStems; ++s)
        {
            auto stemHolder = stemPtr->stems[static_cast<size_t> (s)];
            const auto& buf = stemHolder->getBuffer();

            double ratio = kModelSampleRate / deviceSampleRate;
            int64_t outFrames = static_cast<int64_t> (
                std::ceil (static_cast<double> (buf.getNumSamples()) / ratio));

            juce::AudioBuffer<float> resampled (buf.getNumChannels(),
                                                  static_cast<int> (outFrames));
            resampled.clear();

            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            {
                juce::LagrangeInterpolator interpolator;
                interpolator.process (ratio,
                                      buf.getReadPointer (ch),
                                      resampled.getWritePointer (ch),
                                      static_cast<int> (outFrames));
            }

            stemPtr->stems[static_cast<size_t> (s)] =
                new AudioBufferHolder (std::move (resampled), deviceSampleRate, outFrames);
        }
    }

    if (shouldCancel())
    {
        tempDir.deleteRecursively();
        stemCache.deletePartialFiles (contentHash);
        return jobHasFinished;
    }

    reportProgress (0.95f);

    // 8. Write stems to disk cache
    double writeSampleRate = deviceSampleRate;

    if (! stemCache.writeStemsToDisk (contentHash,
                                       juce::String (ModelManager::getModelVersion()),
                                       *stemPtr, writeSampleRate))
    {
        reportError ("Failed to write stem files to disk");
        stemCache.deletePartialFiles (contentHash);
        tempDir.deleteRecursively();
        return jobHasFinished;
    }

    // Clean up temp files
    tempDir.deleteRecursively();

    if (shouldCancel())
        return jobHasFinished;

    reportProgress (1.0f);

    // 9. Deliver result via callback
    auto cb = completionCallback;
    auto dk = deckId;
    auto sp = stemPtr;
    juce::MessageManager::callAsync ([cb, dk, sp]()
    {
        cb (dk, sp, {});
    });

    return jobHasFinished;
}

// ============================================================================
// Pipeline stages
// ============================================================================

bool StemSeparationEngine::resampleToModelRate (juce::AudioBuffer<float>& output)
{
    const auto& srcBuf = sourceBuffer->getBuffer();
    const int numChannels = srcBuf.getNumChannels();
    const int numSamples  = srcBuf.getNumSamples();
    const double srcRate  = sourceBuffer->getSampleRate();

    if (std::abs (srcRate - kModelSampleRate) < 0.01)
    {
        output = srcBuf;
        return true;
    }

    double ratio = srcRate / kModelSampleRate;
    int64_t outFrames = static_cast<int64_t> (
        std::ceil (static_cast<double> (numSamples) / ratio));

    output.setSize (numChannels, static_cast<int> (outFrames));
    output.clear();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        juce::LagrangeInterpolator interpolator;
        interpolator.process (ratio,
                              srcBuf.getReadPointer (ch),
                              output.getWritePointer (ch),
                              static_cast<int> (outFrames));
    }

    return true;
}

bool StemSeparationEngine::writeSourceToWav (const juce::AudioBuffer<float>& buffer,
                                               const juce::File& outputFile)
{
    juce::WavAudioFormat wavFormat;
    auto stream = std::unique_ptr<juce::FileOutputStream> (outputFile.createOutputStream());

    if (stream == nullptr)
    {
        reportError ("Failed to create temp WAV file: " + outputFile.getFullPathName());
        return false;
    }

    auto writer = std::unique_ptr<juce::AudioFormatWriter> (
        wavFormat.createWriterFor (stream.get(),
                                   kModelSampleRate,
                                   static_cast<unsigned int> (buffer.getNumChannels()),
                                   32,      // 32-bit float
                                   {},
                                   0));

    if (writer == nullptr)
    {
        reportError ("Failed to create WAV writer");
        return false;
    }

    stream.release(); // writer takes ownership

    if (! writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()))
    {
        reportError ("Failed to write audio data to WAV");
        return false;
    }

    return true;
}

bool StemSeparationEngine::runPythonSeparation (const juce::File& inputWav,
                                                  const juce::File& outputDir,
                                                  juce::File& vocalsFile,
                                                  juce::File& instrumentalFile)
{
    juce::StringArray args;
    args.add (pythonPath);
    args.add (scriptPath.getFullPathName());
    args.add ("--separate");
    args.add (inputWav.getFullPathName());
    args.add (outputDir.getFullPathName());
    args.add (modelDir.getFullPathName());

    juce::ChildProcess process;

    if (! process.start (args))
    {
        reportError ("Failed to start Python separation process");
        return false;
    }

    // Wait for completion (separation can take minutes)
    // Check shouldExit periodically
    while (process.isRunning())
    {
        if (shouldCancel())
        {
            process.kill();
            stemCache.deletePartialFiles (contentHash);
            return false;
        }

        juce::Thread::sleep (500);
    }

    auto exitCode = process.getExitCode();
    auto output = process.readAllProcessOutput();

    if (exitCode != 0)
    {
        // Try to parse JSON error
        auto parsed = juce::JSON::parse (output);

        if (parsed.isObject())
        {
            auto errorMsg = parsed.getProperty ("error", "Unknown error").toString();
            reportError ("Separation failed: " + errorMsg);
        }
        else
        {
            reportError ("Separation process failed (exit code "
                        + juce::String (exitCode) + "): " + output.substring (0, 500));
        }

        return false;
    }

    // Parse JSON output to get file paths — extract last line containing JSON
    // (output may contain progress bars and other text before the JSON)
    juce::String jsonLine;
    auto lines = juce::StringArray::fromLines (output);
    for (int i = lines.size() - 1; i >= 0; --i)
    {
        auto trimmed = lines[i].trim();
        if (trimmed.startsWith ("{") && trimmed.endsWith ("}"))
        {
            jsonLine = trimmed;
            break;
        }
    }

    auto parsed = juce::JSON::parse (jsonLine);

    if (! parsed.isObject())
    {
        reportError ("Invalid JSON output from separation script");
        return false;
    }

    auto vocalsPath = parsed.getProperty ("vocals", "").toString();
    auto instrumentalPath = parsed.getProperty ("instrumental", "").toString();

    if (vocalsPath.isEmpty() || instrumentalPath.isEmpty())
    {
        reportError ("Separation script did not return expected file paths");
        return false;
    }

    vocalsFile = juce::File (vocalsPath);
    instrumentalFile = juce::File (instrumentalPath);

    // If paths are relative (just filenames), resolve against output directory
    if (! vocalsFile.existsAsFile())
        vocalsFile = outputDir.getChildFile (vocalsPath);
    if (! instrumentalFile.existsAsFile())
        instrumentalFile = outputDir.getChildFile (instrumentalPath);

    if (! vocalsFile.existsAsFile() || ! instrumentalFile.existsAsFile())
    {
        reportError ("Separation output files not found on disk");
        return false;
    }

    return true;
}

AudioBufferHolder::Ptr StemSeparationEngine::readWavFile (const juce::File& file)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader> (
        formatManager.createReaderFor (file));

    if (reader == nullptr)
        return nullptr;

    const auto numChannels = juce::jmax (2, static_cast<int> (reader->numChannels));
    const auto numSamples  = static_cast<int> (reader->lengthInSamples);

    juce::AudioBuffer<float> buffer (numChannels, numSamples);
    buffer.clear();
    reader->read (&buffer, 0, numSamples, 0, true, numChannels > 1);

    return new AudioBufferHolder (std::move (buffer), reader->sampleRate,
                                   static_cast<int64_t> (numSamples));
}

bool StemSeparationEngine::assembleStemData (AudioBufferHolder::Ptr vocals,
                                              AudioBufferHolder::Ptr instrumental,
                                              int numSamples,
                                              StemData& output)
{
    // Slot 0: Vocals
    output.stems[StemData::Vocals] = vocals;

    // Slot 1: Instrumental (mapped to "Drums" slot for UI compatibility)
    output.stems[StemData::Drums] = instrumental;

    // Slots 2-3: Silence (Bass, Other)
    for (int s = StemData::Bass; s <= StemData::Other; ++s)
    {
        juce::AudioBuffer<float> silentBuf (2, numSamples);
        silentBuf.clear();
        output.stems[static_cast<size_t> (s)] =
            new AudioBufferHolder (std::move (silentBuf), kModelSampleRate, numSamples);
    }

    return true;
}

// ============================================================================
// Progress & error reporting
// ============================================================================

void StemSeparationEngine::reportProgress (float prog)
{
    double now = juce::Time::getMillisecondCounterHiRes();
    if (prog < 1.0f && (now - lastProgressTime) < 1000.0)
        return;

    lastProgressTime = now;

    auto node = stemsNode;
    juce::MessageManager::callAsync ([node, prog]() mutable
    {
        if (node.isValid())
        {
            node.setProperty (IDs::progress, prog, nullptr);
            if (prog > 0.0f && prog < 1.0f)
                node.setProperty (IDs::status, "separating", nullptr);
        }
    });
}

void StemSeparationEngine::reportError (const juce::String& message)
{
    DBG ("StemSeparationEngine ERROR: " + message);
    stemCache.deletePartialFiles (contentHash);

    auto node = stemsNode;
    auto dk   = deckId;
    auto cb   = completionCallback;
    juce::MessageManager::callAsync ([node, message, dk, cb]() mutable
    {
        if (node.isValid())
        {
            node.setProperty (IDs::status,    "error",  nullptr);
            node.setProperty (IDs::stemError, message,  nullptr);
            node.setProperty (IDs::progress,  0.0f,     nullptr);
        }
        cb (dk, nullptr, message);
    });
}

bool StemSeparationEngine::shouldCancel() const
{
    return shouldExit() || (externalShouldCancel && externalShouldCancel());
}

