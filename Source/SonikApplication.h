#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioFileLoader.h"
#include <memory>

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (AudioFileLoader& loader, DeckStateManager& deckState, AudioEngine& engine)
        : DocumentWindow ("Sonik",
                          juce::Colour (0xfff9f9f9),
                          DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new DropZoneComponent (loader, deckState, engine), true);
        centreWithSize (1280, 800);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    /// Placeholder content component that accepts audio file drops and keyboard transport controls.
    class DropZoneComponent final : public juce::Component,
                                    public juce::FileDragAndDropTarget
    {
    public:
        DropZoneComponent (AudioFileLoader& loader, DeckStateManager& deckState, AudioEngine& engine)
            : fileLoader (loader), deckStateManager (deckState), audioEngine (engine)
        {
            setSize (1280, 800);
            setWantsKeyboardFocus (true);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xfff9f9f9));

            if (isDragOver)
            {
                g.setColour (juce::Colour (0x20000000));
                g.fillRect (getLocalBounds());
            }

            g.setColour (juce::Colours::black);
            g.setFont (16.0f);
            g.drawText (isDragOver ? "Release to load" : "Drag an audio file here",
                        getLocalBounds(), juce::Justification::centred);
        }

        bool isInterestedInFileDrag (const juce::StringArray& files) override
        {
            for (const auto& f : files)
            {
                juce::File file (f);
                if (AudioFileLoader::isSupportedExtension (file.getFileExtension()))
                    return true;
            }
            return false;
        }

        void fileDragEnter (const juce::StringArray&, int, int) override
        {
            isDragOver = true;
            repaint();
        }

        void fileDragExit (const juce::StringArray&) override
        {
            isDragOver = false;
            repaint();
        }

        void filesDropped (const juce::StringArray& files, int, int) override
        {
            isDragOver = false;
            repaint();

            for (const auto& f : files)
            {
                juce::File file (f);
                if (AudioFileLoader::isSupportedExtension (file.getFileExtension()))
                {
                    auto activeDeck = deckStateManager.getActiveDeckId();
                    fileLoader.loadFile (activeDeck, file);
                    break; // load first valid file only
                }
            }
        }

        void visibilityChanged() override
        {
            if (isVisible())
                grabKeyboardFocus();
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::spaceKey)
            {
                auto activeDeck = deckStateManager.getActiveDeckId();
                auto* state = deckStateManager.getAudioState (activeDeck);
                if (state != nullptr)
                {
                    auto st = static_cast<PlaybackStatusCode> (
                        state->playbackStatus.load (std::memory_order_relaxed));
                    if (st == PlaybackStatusCode::playing)
                        audioEngine.sendTransportCommand (activeDeck, TransportCommand::Pause);
                    else if (st == PlaybackStatusCode::stopped
                             || st == PlaybackStatusCode::paused)
                        audioEngine.sendTransportCommand (activeDeck, TransportCommand::Play);
                }
                return true;
            }

            if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S')
            {
                auto activeDeck = deckStateManager.getActiveDeckId();
                audioEngine.sendTransportCommand (activeDeck, TransportCommand::Stop);
                return true;
            }

            return false;
        }

    private:
        AudioFileLoader&  fileLoader;
        DeckStateManager& deckStateManager;
        AudioEngine&      audioEngine;
        bool isDragOver = false;
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

class SonikApplication final : public juce::JUCEApplication
{
public:
    SonikApplication() = default;

    const juce::String getApplicationName() override    { return "Sonik"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override;
    void shutdown() override;
    void systemRequestedQuit() override;

private:
    std::unique_ptr<TrackDatabase>    trackDatabase;
    std::unique_ptr<DeckStateManager> deckStateManager;
    std::unique_ptr<AudioEngine>      audioEngine;
    std::unique_ptr<AudioFileLoader>  audioFileLoader;
    std::unique_ptr<MainWindow>       mainWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SonikApplication)
};
