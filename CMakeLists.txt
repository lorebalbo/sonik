cmake_minimum_required(VERSION 3.24)
project(Sonik VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# macOS deployment target
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment version")
endif()

# Download CPM.cmake
file(DOWNLOAD
    https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.2/CPM.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

# JUCE 8.0.6
CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG 8.0.6
    OPTIONS
        "JUCE_BUILD_EXAMPLES OFF"
        "JUCE_BUILD_EXTRAS OFF"
)

# SQLite3 amalgamation
CPMAddPackage(
    NAME sqlite3
    URL "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip"
    DOWNLOAD_ONLY YES
)

if(sqlite3_ADDED)
    add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_THREADSAFE=1
        SQLITE_OMIT_LOAD_EXTENSION=1
    )
    if(UNIX)
        target_link_libraries(sqlite3 PRIVATE pthread dl)
    endif()
endif()

# Sonik Application
juce_add_gui_app(Sonik
    PRODUCT_NAME "Sonik"
    VERSION "0.1.0"
    COMPANY_NAME "Sonik"
    BUNDLE_ID "com.sonik.app"
)

target_sources(Sonik PRIVATE
    Source/Main.cpp
    Source/SonikApplication.h
    Source/SonikApplication.cpp
    Source/Features/Deck/DeckIdentifiers.h
    Source/Features/Deck/DeckStateManager.h
    Source/Features/Deck/DeckStateManager.cpp
    Source/Features/Deck/AudioThreadState.h
    Source/Features/Deck/Database/TrackDatabase.h
    Source/Features/Deck/Database/TrackDatabase.cpp
    Source/Features/AudioEngine/AudioEngine.h
    Source/Features/AudioEngine/AudioEngine.cpp
    Source/Features/AudioEngine/DeckAudioSource.h
    Source/Features/AudioEngine/AudioBufferHolder.h
    Source/Features/AudioEngine/AudioFileLoader.h
    Source/Features/AudioEngine/AudioFileLoader.cpp
    Source/Features/Deck/UI/DeckShellComponent.h
    Source/Features/Deck/UI/DeckShellComponent.cpp
    Source/Features/Deck/UI/DeckLayoutManager.h
    Source/Features/Deck/UI/DeckLayoutManager.cpp
    Source/Features/Deck/UI/GlobalToolbar.h
    Source/Features/Deck/UI/MainContentComponent.h
    Source/Features/Waveform/WaveformData.h
    Source/Features/Waveform/WaveformAnalyzer.h
    Source/Features/Waveform/WaveformAnalyzer.cpp
    Source/Features/Waveform/WaveformManager.h
    Source/Features/Waveform/WaveformManager.cpp
    Source/Features/Waveform/WaveformComponent.h
    Source/Features/Waveform/OverviewWaveform.h
    Source/Features/Waveform/OverviewWaveform.cpp
    Source/Features/Waveform/DetailWaveform.h
    Source/Features/Waveform/DetailWaveform.cpp
)

target_include_directories(Sonik PRIVATE Source)

target_compile_definitions(Sonik PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Sonik,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Sonik,JUCE_VERSION>"
)

target_link_libraries(Sonik PRIVATE
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_cryptography
    juce::juce_data_structures
    juce::juce_core
    juce::juce_events
    sqlite3
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# Test target
add_executable(SonikTests
    Tests/TestRunner.cpp
    Tests/DeckStateTests.cpp
    Tests/DeckLayoutTests.cpp
    Tests/AudioEngineTests.cpp
    Tests/AudioFileLoaderTests.cpp
    Tests/TransportTests.cpp
    Tests/WaveformTests.cpp
    Source/Features/Deck/DeckStateManager.cpp
    Source/Features/Deck/Database/TrackDatabase.cpp
    Source/Features/AudioEngine/AudioEngine.cpp
    Source/Features/AudioEngine/AudioFileLoader.cpp
    Source/Features/Deck/UI/DeckShellComponent.cpp
    Source/Features/Deck/UI/DeckLayoutManager.cpp
    Source/Features/Waveform/WaveformAnalyzer.cpp
    Source/Features/Waveform/WaveformManager.cpp
    Source/Features/Waveform/OverviewWaveform.cpp
    Source/Features/Waveform/DetailWaveform.cpp
)

target_include_directories(SonikTests PRIVATE Source Tests)

target_compile_definitions(SonikTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_UNIT_TESTS=1
)

target_link_libraries(SonikTests PRIVATE
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_cryptography
    juce::juce_data_structures
    juce::juce_core
    juce::juce_events
    sqlite3
    juce::juce_recommended_config_flags
)
