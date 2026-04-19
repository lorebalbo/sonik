#include "ModelManager.h"
#include "../Deck/DeckIdentifiers.h"

ModelManager::ModelManager (juce::ValueTree stateTree)
    : juce::Thread ("Sonik-ModelInit"),
      rootState (std::move (stateTree))
{
    // Kick off model validation + session creation on a background thread
    // (off audio thread AND off message thread).
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

    if (! modelFile.hasReadAccess())
    {
        updateStemsStatus ("model_error",
                           "Model file is not readable: " + modelFile.getFullPathName());
        return;
    }

    if (modelFile.getSize() == 0)
    {
        updateStemsStatus ("model_error",
                           "Model file is empty (zero bytes): " + modelFile.getFullPathName());
        return;
    }

    if (threadShouldExit())
        return;

    // 3. Create ONNX session
    auto sessionResult = inference.createSession (modelFile.getFullPathName().toStdString());

    if (threadShouldExit())
        return;

    if (! sessionResult.success)
    {
        updateStemsStatus ("model_error",
                           juce::String ("ONNX session creation failed: ")
                               + sessionResult.errorMessage);
        return;
    }

    // 4. Success
    modelReady.store (true, std::memory_order_release);
    updateStemsStatus ("model_ready");
}

// ---------------------------------------------------------------------------
// ValueTree update — dispatched to message thread
// ---------------------------------------------------------------------------
void ModelManager::updateStemsStatus (const juce::String& newStatus,
                                      const juce::String& error)
{
    juce::MessageManager::callAsync ([rootState = this->rootState,
                                      newStatus, error]() mutable
    {
        auto decksNode = rootState.getChildWithName (IDs::Decks);
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
