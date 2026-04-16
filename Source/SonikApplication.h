#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/AudioEngine/AudioEngine.h"
#include "Features/AudioEngine/AudioFileLoader.h"
#include "Features/Waveform/WaveformManager.h"
#include "Features/BeatGrid/BeatGridManager.h"
#include "Features/KeyDetection/KeyDetectionManager.h"
#include "Features/Deck/UI/MainContentComponent.h"
#include <memory>

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (AudioFileLoader& loader, DeckStateManager& deckState,
                AudioEngine& engine, WaveformManager& waveformMgr,
                BeatGridManager& beatGridMgr)
        : DocumentWindow ("Sonik",
                          juce::Colour (0xfff9f9f9),
                          DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainContentComponent (deckState, engine, loader, waveformMgr, beatGridMgr), true);
        setResizable (true, true);
        setResizeLimits (960, 600, 3840, 2160);
        centreWithSize (1280, 800);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
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
    std::unique_ptr<WaveformManager>  waveformManager;
    std::unique_ptr<BeatGridManager>  beatGridManager;
    std::unique_ptr<KeyDetectionManager> keyDetectionManager;
    std::unique_ptr<MainWindow>       mainWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SonikApplication)
};
