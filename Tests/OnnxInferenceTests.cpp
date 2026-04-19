#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Features/StemSeparation/OnnxInference.h"
#include "Features/StemSeparation/ModelManager.h"
#include "Features/Deck/DeckIdentifiers.h"

class OnnxInferenceTests : public juce::UnitTest
{
public:
    OnnxInferenceTests() : juce::UnitTest ("ONNX Inference & Model Manager", "Sonik") {}

    void runTest() override
    {
        testOnnxDefaultConstruction();
        testOnnxCreateSessionNonExistentPath();
        testOnnxCreateSessionEmptyFile();
        testModelManagerGetModelDirectory();
        testModelManagerGetModelVersion();
        testModelManagerGetModelFilename();
        testModelManagerIsModelReadyWhenMissing();
        testModelManagerStemsStatusWhenMissing();
    }

private:
    // -----------------------------------------------------------------------
    // OnnxInference tests
    // -----------------------------------------------------------------------
    void testOnnxDefaultConstruction()
    {
        beginTest ("OnnxInference - default constructed has no valid session");
        OnnxInference inference;
        expect (! inference.hasValidSession(),
                "Default-constructed OnnxInference should not have a valid session");
    }

    void testOnnxCreateSessionNonExistentPath()
    {
        beginTest ("OnnxInference - createSession with non-existent path fails");
        OnnxInference inference;
        auto result = inference.createSession ("/nonexistent/path/to/model.onnx");

        expect (! result.success,
                "createSession with a non-existent path should fail");
        expect (! result.errorMessage.empty(),
                "errorMessage should be non-empty on failure");
        expect (! inference.hasValidSession(),
                "Session should remain invalid after failed createSession");
    }

    void testOnnxCreateSessionEmptyFile()
    {
        beginTest ("OnnxInference - createSession with empty/invalid file fails");

        // Create a temporary empty file
        auto tempFile = juce::File::createTempFile ("test_model.onnx");
        tempFile.create();  // creates an empty file

        OnnxInference inference;
        auto result = inference.createSession (tempFile.getFullPathName().toStdString());

        expect (! result.success,
                "createSession with an empty file should fail");
        expect (! result.errorMessage.empty(),
                "errorMessage should be non-empty for empty/invalid file");
        expect (! inference.hasValidSession(),
                "Session should remain invalid after failed createSession with empty file");

        tempFile.deleteFile();
    }

    // -----------------------------------------------------------------------
    // ModelManager tests
    // -----------------------------------------------------------------------
    void testModelManagerGetModelDirectory()
    {
        beginTest ("ModelManager - getModelDirectory returns valid path with Sonik/Models");
        auto dir = ModelManager::getModelDirectory();
        auto path = dir.getFullPathName();

        expect (path.contains ("Sonik"),
                "Model directory path should contain 'Sonik'");
        expect (path.contains ("Models"),
                "Model directory path should contain 'Models'");
    }

    void testModelManagerGetModelVersion()
    {
        beginTest ("ModelManager - getModelVersion returns htdemucs_v4");
        juce::String version (ModelManager::getModelVersion());
        expectEquals (version, juce::String ("htdemucs_v4"));
    }

    void testModelManagerGetModelFilename()
    {
        beginTest ("ModelManager - getModelFilename returns htdemucs.onnx");
        juce::String filename (ModelManager::getModelFilename());
        expectEquals (filename, juce::String ("htdemucs.onnx"));
    }

    void testModelManagerIsModelReadyWhenMissing()
    {
        beginTest ("ModelManager - isModelReady returns false when no valid model");

        // Build a minimal ValueTree that ModelManager expects
        juce::ValueTree root (IDs::SonikState);
        juce::ValueTree decks (IDs::Decks);
        juce::ValueTree deck (IDs::Deck);
        juce::ValueTree stems (IDs::Stems);
        stems.setProperty (IDs::status, "none", nullptr);
        stems.setProperty (IDs::stemError, "", nullptr);
        stems.setProperty (IDs::modelVersion, "", nullptr);
        deck.addChild (stems, -1, nullptr);
        decks.addChild (deck, -1, nullptr);
        root.addChild (decks, -1, nullptr);

        // Ensure the model file does NOT exist for this test
        auto modelFile = ModelManager::getModelDirectory()
                             .getChildFile (ModelManager::getModelFilename());
        bool modelExists = modelFile.existsAsFile();

        if (modelExists)
        {
            // Can't safely remove user's real model file; just test what we can
            logMessage ("Skipping isModelReady test — actual model file present");
            return;
        }

        ModelManager mgr (root);

        // Wait for the background thread to complete (max 5 seconds)
        auto deadline = juce::Time::getMillisecondCounter() + 5000;
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            // Pump the message loop so callAsync dispatches happen
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

            // Check if thread has finished by testing the status
            auto stemsNode = root.getChildWithName (IDs::Decks)
                                 .getChild (0)
                                 .getChildWithName (IDs::Stems);
            auto status = stemsNode.getProperty (IDs::status).toString();
            if (status != "none")
                break;
        }

        expect (! mgr.isModelReady(),
                "isModelReady should be false when model file is missing");
    }

    void testModelManagerStemsStatusWhenMissing()
    {
        beginTest ("ModelManager - Stems status is model_unavailable when file missing");

        juce::ValueTree root (IDs::SonikState);
        juce::ValueTree decks (IDs::Decks);
        juce::ValueTree deck (IDs::Deck);
        juce::ValueTree stems (IDs::Stems);
        stems.setProperty (IDs::status, "none", nullptr);
        stems.setProperty (IDs::stemError, "", nullptr);
        stems.setProperty (IDs::modelVersion, "", nullptr);
        deck.addChild (stems, -1, nullptr);
        decks.addChild (deck, -1, nullptr);
        root.addChild (decks, -1, nullptr);

        auto modelFile = ModelManager::getModelDirectory()
                             .getChildFile (ModelManager::getModelFilename());

        if (modelFile.existsAsFile())
        {
            logMessage ("Skipping Stems status test — actual model file present");
            return;
        }

        ModelManager mgr (root);

        // Wait for the background thread + callAsync to complete
        auto deadline = juce::Time::getMillisecondCounter() + 5000;
        juce::String finalStatus;
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

            auto stemsNode = root.getChildWithName (IDs::Decks)
                                 .getChild (0)
                                 .getChildWithName (IDs::Stems);
            finalStatus = stemsNode.getProperty (IDs::status).toString();
            if (finalStatus != "none")
                break;
        }

        expectEquals (finalStatus, juce::String ("model_unavailable"),
                      "Stems status should be 'model_unavailable' when model file is missing");

        // Also verify modelVersion was set
        auto stemsNode = root.getChildWithName (IDs::Decks)
                             .getChild (0)
                             .getChildWithName (IDs::Stems);
        auto mv = stemsNode.getProperty (IDs::modelVersion).toString();
        expectEquals (mv, juce::String ("htdemucs_v4"),
                      "modelVersion should be set even when model is unavailable");
    }
};

static OnnxInferenceTests onnxInferenceTests;
