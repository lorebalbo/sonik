#include "ModelManager.h"
#include "../Deck/DeckIdentifiers.h"

// Embedded separation script — written to model directory on first run
static const char* kSeparationScript = R"PYSCRIPT(#!/usr/bin/env python3
"""Stem separation using audio_separator (BS-RoFormer)."""
import sys, os, json

def check():
    try:
        from audio_separator.separator import Separator
        print(json.dumps({"status": "ok"}))
    except ImportError as e:
        print(json.dumps({"status": "error", "message": str(e)}))
        sys.exit(1)

def separate(input_wav, output_dir, model_dir):
    from audio_separator.separator import Separator
    separator = Separator(log_level=30, model_file_dir=model_dir,
                          output_dir=output_dir, output_format="WAV")
    model_filename = None
    for f in sorted(os.listdir(model_dir)):
        if f.endswith(".ckpt"):
            model_filename = f
            break
    if model_filename is None:
        print(json.dumps({"error": "No .ckpt model found in " + model_dir}))
        sys.exit(1)
    separator.load_model(model_filename=model_filename)
    output_files = separator.separate(input_wav)
    vocals = instrumental = None
    for f in output_files:
        b = os.path.basename(f)
        if "(Vocals)" in b or "_Vocals_" in b:
            vocals = f
        elif "(Instrumental)" in b or "_Instrumental_" in b:
            instrumental = f
    if vocals and instrumental:
        print(json.dumps({"vocals": vocals, "instrumental": instrumental}))
    else:
        print(json.dumps({"error": "Could not identify output files", "files": output_files}))
        sys.exit(1)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps({"error": "Usage: separate_stems.py [--check | --separate ...]"}))
        sys.exit(1)
    if sys.argv[1] == "--check":
        check()
    elif sys.argv[1] == "--separate" and len(sys.argv) == 5:
        separate(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(json.dumps({"error": "Bad arguments"}))
        sys.exit(1)
)PYSCRIPT";

ModelManager::ModelManager (juce::ValueTree stateTree)
    : juce::Thread ("Sonik-ModelInit"),
      rootState (std::move (stateTree))
{
    startThread (juce::Thread::Priority::low);
}

ModelManager::~ModelManager()
{
    stopThread (5000);
}

juce::File ModelManager::getModelDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Application Support")
               .getChildFile ("Sonik")
               .getChildFile ("Models");
}

bool ModelManager::isModelReady() const
{
    return modelReady.load (std::memory_order_acquire);
}

juce::String ModelManager::getPythonPath() const
{
    return discoveredPythonPath;
}

juce::File ModelManager::getScriptPath() const
{
    return scriptPath;
}

// ---------------------------------------------------------------------------
// Background thread
// ---------------------------------------------------------------------------
void ModelManager::run()
{
    // 1. Ensure model directory exists
    auto modelDir = getModelDirectory();
    if (! modelDir.isDirectory())
    {
        auto result = modelDir.createDirectory();
        if (result.failed())
        {
            updateStemsStatus ("model_error",
                               "Failed to create model directory: " + result.getErrorMessage());
            return;
        }
    }

    if (threadShouldExit())
        return;

    // 2. Locate and validate the model file
    auto modelFile = modelDir.getChildFile (getModelFilename());

    if (! modelFile.existsAsFile())
    {
        updateStemsStatus ("model_unavailable");
        return;
    }

    if (modelFile.getSize() == 0)
    {
        updateStemsStatus ("model_error",
                           "Model file is empty: " + modelFile.getFullPathName());
        return;
    }

    if (threadShouldExit())
        return;

    // 3. Write the separation script to model directory
    ensureSeparationScript();

    if (threadShouldExit())
        return;

    // 4. Find Python 3 with audio_separator installed
    discoveredPythonPath = findPythonWithSeparator();

    if (discoveredPythonPath.isEmpty())
    {
        updateStemsStatus ("model_error",
                           "No Python 3 with audio_separator found. "
                           "Install: pip install audio-separator[cpu]");
        return;
    }

    if (threadShouldExit())
        return;

    // 5. Success
    modelReady.store (true, std::memory_order_release);
    updateStemsStatus ("model_ready");
}

// ---------------------------------------------------------------------------
// Python discovery
// ---------------------------------------------------------------------------
juce::String ModelManager::findPythonWithSeparator()
{
    // Check common Python 3 locations on macOS
    juce::String home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
    juce::StringArray candidates = {
        home + "/.pyenv/shims/python3",
        "/opt/homebrew/bin/python3",
        "/usr/local/bin/python3",
        "/usr/bin/python3"
    };

    for (const auto& pythonPath : candidates)
    {
        if (threadShouldExit())
            return {};

        if (! juce::File (pythonPath).existsAsFile())
            continue;

        // Verify audio_separator is importable (PyTorch import can be slow)
        juce::ChildProcess check;
        juce::StringArray args;
        args.add (pythonPath);
        args.add (scriptPath.getFullPathName());
        args.add ("--check");

        if (check.start (args))
        {
            if (check.waitForProcessToFinish (120000) && check.getExitCode() == 0)
                return pythonPath;

            check.kill();
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// Script management
// ---------------------------------------------------------------------------
void ModelManager::ensureSeparationScript()
{
    scriptPath = getModelDirectory().getChildFile ("separate_stems.py");
    scriptPath.replaceWithText (kSeparationScript);
}

// ---------------------------------------------------------------------------
// ValueTree update — dispatched to message thread
// ---------------------------------------------------------------------------
void ModelManager::updateStemsStatus (const juce::String& newStatus,
                                      const juce::String& error)
{
    juce::MessageManager::callAsync ([stateCopy = this->rootState,
                                      newStatus, error]() mutable
    {
        auto decksNode = stateCopy.getChildWithName (IDs::Decks);
        for (int i = 0; i < decksNode.getNumChildren(); ++i)
        {
            auto stems = decksNode.getChild (i).getChildWithName (IDs::Stems);
            if (stems.isValid())
            {
                stems.setProperty (IDs::status,       newStatus, nullptr);
                stems.setProperty (IDs::stemError,    error,     nullptr);
                stems.setProperty (IDs::modelVersion,
                                   juce::String (getModelVersion()), nullptr);
            }
        }
    });
}
