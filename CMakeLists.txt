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
        SQLITE_ENABLE_FTS5=1
    )
    if(UNIX)
        target_link_libraries(sqlite3 PRIVATE pthread dl)
    endif()
endif()

# Essentia (installed via pkg-config)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ESSENTIA REQUIRED essentia)

add_library(essentia STATIC IMPORTED)
set_target_properties(essentia PROPERTIES
    IMPORTED_LOCATION "/usr/local/lib/libessentia.a"
)
target_include_directories(essentia INTERFACE ${ESSENTIA_INCLUDE_DIRS})
target_link_directories(essentia INTERFACE ${ESSENTIA_LIBRARY_DIRS})

# Collect Essentia's transitive dependencies (fftw3f, avformat, etc.)
set(ESSENTIA_TRANSITIVE_LIBS "")
foreach(_lib ${ESSENTIA_LIBRARIES})
    if(NOT _lib STREQUAL "essentia")
        list(APPEND ESSENTIA_TRANSITIVE_LIBS ${_lib})
    endif()
endforeach()
target_link_libraries(essentia INTERFACE ${ESSENTIA_TRANSITIVE_LIBS})

# Rubber Band (installed via Homebrew, found via pkg-config)
pkg_check_modules(RUBBERBAND REQUIRED rubberband)

# ONNX Runtime pre-built release (macOS arm64, MIT license)
CPMAddPackage(
    NAME onnxruntime
    URL "https://github.com/microsoft/onnxruntime/releases/download/v1.19.2/onnxruntime-osx-arm64-1.19.2.tgz"
    DOWNLOAD_ONLY YES
)

if(onnxruntime_ADDED)
    add_library(onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.1.19.2.dylib"
        IMPORTED_SONAME   "@rpath/libonnxruntime.1.19.2.dylib"
    )
    target_include_directories(onnxruntime INTERFACE "${onnxruntime_SOURCE_DIR}/include")
endif()

# Sonik Application
juce_add_gui_app(Sonik
    PRODUCT_NAME "Sonik"
    VERSION "0.1.0"
    COMPANY_NAME "Sonik"
    BUNDLE_ID "com.sonik.app"
)

# Bundled MIDI mapping profiles (PRD-0043).  Generates a static library
# `SonikBinaryData` with `BinaryData::*` symbols for the embedded JSON files.
juce_add_binary_data(SonikBinaryData
    SOURCES
        Resources/MidiMappings/behringer-ddm4000.json
        Resources/MidiMappings/generic-midi.json
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
    Source/Features/Deck/UI/PitchFaderComponent.h
    Source/Features/Deck/UI/PitchFaderComponent.cpp
    Source/Features/Deck/UI/GainKnobComponent.h
    Source/Features/Deck/UI/GainKnobComponent.cpp
    Source/Features/Deck/UI/TrackInfoComponent.h
    Source/Features/Deck/UI/TrackInfoComponent.cpp
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
    Source/Features/BeatGrid/BeatGridData.h
    Source/Features/BeatGrid/BeatGridAnalyzer.h
    Source/Features/BeatGrid/BeatGridAnalyzer.cpp
    Source/Features/BeatGrid/BeatGridManager.h
    Source/Features/BeatGrid/BeatGridManager.cpp
    Source/Features/KeyDetection/KeyUtils.h
    Source/Features/KeyDetection/KeyDetectionAnalyzer.h
    Source/Features/KeyDetection/KeyDetectionAnalyzer.cpp
    Source/Features/KeyDetection/KeyDetectionManager.h
    Source/Features/KeyDetection/KeyDetectionManager.cpp
    Source/Features/TimeStretch/TimeStretcher.h
    Source/Features/TimeStretch/TimeStretcher.cpp
    Source/Features/Cue/HotCueData.h
    Source/Features/Cue/HotCueManager.h
    Source/Features/Cue/HotCueManager.cpp
    Source/Features/Cue/HotCuePadComponent.h
    Source/Features/Cue/HotCuePadComponent.cpp
    Source/Features/Quantize/QuantizeService.h
    Source/Features/Quantize/QuantizeButtonComponent.h
    Source/Features/Quantize/QuantizeButtonComponent.cpp
    Source/Features/Loop/LoopEngine.h
    Source/Features/Loop/LoopEngine.cpp
    Source/Features/Loop/LoopControlComponent.h
    Source/Features/Loop/LoopControlComponent.cpp
    Source/Features/BeatJump/BeatJumpEngine.h
    Source/Features/BeatJump/BeatJumpEngine.cpp
    Source/Features/BeatJump/BeatJumpComponent.h
    Source/Features/BeatJump/BeatJumpComponent.cpp
    Source/Features/SlipMode/SlipButtonComponent.h
    Source/Features/SlipMode/SlipButtonComponent.cpp
    Source/Features/StemSeparation/OnnxInference.h
    Source/Features/StemSeparation/OnnxInference.cpp
    Source/Features/StemSeparation/ModelManager.h
    Source/Features/StemSeparation/ModelManager.cpp
    Source/Features/StemSeparation/StemData.h
    Source/Features/StemSeparation/SpectralProcessor.h
    Source/Features/StemSeparation/SpectralProcessor.cpp
    Source/Features/StemSeparation/StemCache.h
    Source/Features/StemSeparation/StemCache.cpp
    Source/Features/StemSeparation/StemSeparationEngine.h
    Source/Features/StemSeparation/StemSeparationEngine.cpp
    Source/Features/StemSeparation/StemSeparationManager.h
    Source/Features/StemSeparation/StemSeparationManager.cpp
    Source/Features/StemSeparation/UI/StemSeparateButton.h
    Source/Features/StemSeparation/UI/StemSeparateButton.cpp
    Source/Features/StemSeparation/UI/StemToggleComponent.h
    Source/Features/StemSeparation/UI/StemToggleComponent.cpp
    Source/Features/Sync/MasterClockSnapshot.h
    Source/Features/Sync/MasterClockPublisher.h
    Source/Features/Sync/MasterClockPublisher.cpp
    Source/Features/Sync/MasterClockManager.h
    Source/Features/Sync/MasterClockManager.cpp
    Source/Features/Sync/SyncEngine.h
    Source/Features/Sync/SyncEngine.cpp
    Source/Features/Sync/PhaseLockEngine.h
    Source/Features/Sync/PhaseLockEngine.cpp
    Source/Features/Sync/UI/SyncButton.h
    Source/Features/Sync/UI/SyncButton.cpp
    Source/Features/Sync/UI/MasterButton.h
    Source/Features/Sync/UI/MasterButton.cpp
    Source/Features/Library/WatchFolderScanner.h
    Source/Features/Library/WatchFolderScanner.cpp
    Source/Features/Library/LibraryAnalysisService.h
    Source/Features/Library/LibraryAnalysisService.cpp
    Source/Features/Library/LibraryAnalysisQueue.h
    Source/Features/Library/LibraryAnalysisQueue.cpp
    Source/Features/Library/LibraryQueryThread.h
    Source/Features/Library/LibraryQueryThread.cpp
    Source/Features/Library/UI/LibraryPalette.h
    Source/Features/Library/UI/SearchBarAtom.h
    Source/Features/Library/UI/SearchBarAtom.cpp
    Source/Features/Library/UI/SidebarItemAtom.h
    Source/Features/Library/UI/SidebarItemAtom.cpp
    Source/Features/Library/UI/RatingAtom.h
    Source/Features/Library/UI/RatingAtom.cpp
    Source/Features/Library/UI/FilterBarMolecule.h
    Source/Features/Library/UI/FilterBarMolecule.cpp
    Source/Features/Library/UI/SidebarMolecule.h
    Source/Features/Library/UI/SidebarMolecule.cpp
    Source/Features/Library/UI/TrackTableMolecule.h
    Source/Features/Library/UI/TrackTableMolecule.cpp
    Source/Features/Library/UI/LibraryComponent.h
    Source/Features/Library/UI/LibraryComponent.cpp
    Source/Features/Library/UI/LibraryStatusText.h
    Source/Features/Library/UI/LibraryStatusText.cpp
    Source/Features/Midi/Sha1.h
    Source/Features/Midi/MidiInboundEvent.h
    Source/Features/Midi/MidiInputSubscriber.h
    Source/Features/Midi/MidiOpenResult.h
    Source/Features/Midi/MidiDeviceRecord.h
    Source/Features/Midi/DeviceListChangeListener.h
    Source/Features/Midi/MidiHostInterface.h
    Source/Features/Midi/JuceMidiHost.h
    Source/Features/Midi/JuceMidiHost.cpp
    Source/Features/Midi/MidiDeviceManager.h
    Source/Features/Midi/MidiDeviceManager.cpp
    Source/Features/Midi/MidiTargetCategory.h
    Source/Features/Midi/MidiAudioEvent.h
    Source/Features/Midi/MidiMessageEvent.h
    Source/Features/Midi/MidiMessageBridge.h
    Source/Features/Midi/MidiMessageBridge.cpp
    Source/Features/Midi/ControlTargetRegistry.h
    Source/Features/Midi/MappingTypes.h
    Source/Features/Midi/Migrations/MigrationRegistry.h
    Source/Features/Midi/Migrations/MigrationRegistry.cpp
    Source/Features/Midi/MappingParser.h
    Source/Features/Midi/MappingParser.cpp
    Source/Features/Midi/BindingResolver.h
    Source/Features/Midi/BindingResolver.cpp
    Source/Features/Midi/MappingSerializer.h
    Source/Features/Midi/MappingSerializer.cpp
    Source/Features/Midi/MappingStore.h
    Source/Features/Midi/MappingStore.cpp
    Source/Features/Midi/Bundle/SonikMidiBundle.h
    Source/Features/Midi/Bundle/SonikMidiBundleSerializer.h
    Source/Features/Midi/Bundle/SonikMidiBundleSerializer.cpp
    Source/Features/Midi/Bundle/ImportPipeline.h
    Source/Features/Midi/Bundle/MappingExportService.h
    Source/Features/Midi/Bundle/MappingExportService.cpp
    Source/Features/Midi/Bundle/MappingImportService.h
    Source/Features/Midi/Bundle/MappingImportService.cpp
    Source/Features/Midi/UI/MidiSettingsWindow.h
    Source/Features/Midi/UI/MidiSettingsWindow.cpp
    Source/Features/Midi/UI/MidiSettingsPanel.h
    Source/Features/Midi/UI/MidiSettingsPanel.cpp
    Source/Features/Midi/UI/Organisms/MidiImportDialog.h
    Source/Features/Midi/UI/Organisms/MidiImportDialog.cpp
    Source/Features/Midi/UI/Organisms/MidiExportDialog.h
    Source/Features/Midi/UI/Organisms/MidiExportDialog.cpp
    Source/Features/Midi/UI/Atoms/DevicePill.h
    Source/Features/Midi/UI/Atoms/ProfileDropdown.h
    Source/Features/Midi/UI/Atoms/LearnButton.h
    Source/Features/Midi/UI/Atoms/LoadErrorBanner.h
    Source/Features/Midi/UI/Atoms/ModifierBanner.h
    Source/Features/Midi/UI/Atoms/DisengagedList.h
    Source/Features/Midi/UI/Molecules/TargetPicker.h
    Source/Features/Midi/UI/Molecules/DeviceHeader.h
    Source/Features/Midi/UI/Molecules/DeviceHeader.cpp
    Source/Features/Midi/UI/Molecules/BindingTable.h
    Source/Features/Midi/UI/Molecules/BindingTable.cpp
    Source/Features/Midi/MidiFeedbackEngine.h
    Source/Features/Midi/MidiFeedbackEngine.cpp
    Source/Features/Midi/MidiOutputThread.h
    Source/Features/Midi/MidiOutputThread.cpp
    Source/Features/Midi/MidiCommandHandler.h
    Source/Features/Midi/MidiInboundRouter.h
    Source/Features/Midi/MidiInboundRouter.cpp
    Source/Features/Midi/SoftTakeoverManager.h
    Source/Features/Midi/SoftTakeoverManager.cpp
    Source/MidiHandlers/DeckMidiHandler.h
    Source/MidiHandlers/DeckMidiHandler.cpp
    Source/MidiHandlers/MixerMidiHandler.h
    Source/MidiHandlers/MixerMidiHandler.cpp
    Source/MidiHandlers/LibraryMidiHandler.h
    Source/MidiHandlers/LibraryMidiHandler.cpp
    Source/MidiHandlers/CompositeMidiCommandHandler.h
    Source/MidiHandlers/CompositeMidiCommandHandler.cpp
    Source/Features/AudioEngine/AudioEngineMidiBridge.h
)

target_include_directories(Sonik PRIVATE Source)
target_include_directories(Sonik PRIVATE ${RUBBERBAND_INCLUDE_DIRS})

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
    juce::juce_audio_devices
    juce::juce_audio_basics
    juce::juce_cryptography
    juce::juce_data_structures
    juce::juce_core
    juce::juce_events
    juce::juce_dsp
    sqlite3
    essentia
    onnxruntime
    ${RUBBERBAND_LINK_LIBRARIES}
    SonikBinaryData
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# Conditionally link Apple frameworks for CoreML EP (macOS only)
if(APPLE)
    target_link_libraries(Sonik PRIVATE
        "-framework CoreML"
        "-framework Foundation"
    )
endif()

# Test target
add_executable(SonikTests
    Tests/TestRunner.cpp
    Tests/DeckStateTests.cpp
    Tests/DeckLayoutTests.cpp
    Tests/AudioEngineTests.cpp
    Tests/AudioFileLoaderTests.cpp
    Tests/TransportTests.cpp
    Tests/WaveformTests.cpp
    Tests/TrackInfoTests.cpp
    Tests/BeatGridTests.cpp
    Tests/KeyDetectionTests.cpp
    Tests/PitchGainTests.cpp
    Tests/TimeStretchTests.cpp
    Tests/HotCueTests.cpp
    Tests/QuantizeTests.cpp
    Tests/LoopTests.cpp
    Tests/BeatJumpTests.cpp
    Tests/NeedleDropTests.cpp
    Tests/SlipModeTests.cpp
    Tests/OnnxInferenceTests.cpp
    Tests/StemSeparationTests.cpp
    Tests/StemPlaybackTests.cpp
    Tests/StemStretchTests.cpp
    Tests/MasterClockTests.cpp
    Tests/SyncEngineTests.cpp
    Tests/PhaseLockTests.cpp
    Tests/LibraryDatabaseSchemaTests.cpp
    Tests/WatchFolderScannerTests.cpp
    Tests/LibraryQueryThreadTests.cpp
    Tests/LibraryAnalysisQueueTests.cpp
    Tests/LibraryContextMenuTests.cpp
    Tests/SidebarMoleculeTests.cpp
    Tests/MissingFileTests.cpp
    Tests/MidiDeviceManagerTests.cpp
    Tests/MidiMessageBridgeTests.cpp
    Tests/ControlTargetRegistryTests.cpp
    Tests/MappingParserTests.cpp
    Tests/MappingMigrationTests.cpp
    Tests/BindingResolverTests.cpp
    Tests/MappingStoreTests.cpp
    Tests/MappingImportExportTests.cpp
    Tests/MidiInboundRouterTests.cpp
    Tests/ModifierLayerTests.cpp
    Tests/SoftTakeoverManagerTests.cpp
    Tests/MidiFeedbackEngineTests.cpp
    Tests/MidiSettingsPanelTests.cpp
    Source/Features/Library/LibraryQueryThread.cpp
    Source/Features/Library/LibraryAnalysisService.cpp
    Source/Features/Library/LibraryAnalysisQueue.cpp
    Source/Features/Library/WatchFolderScanner.cpp
    Source/Features/Library/UI/SidebarItemAtom.cpp
    Source/Features/Library/UI/SidebarMolecule.cpp
    Source/Features/Library/UI/LibraryStatusText.cpp
    Source/Features/Deck/DeckStateManager.cpp
    Source/Features/Deck/Database/TrackDatabase.cpp
    Source/Features/AudioEngine/AudioEngine.cpp
    Source/Features/AudioEngine/AudioFileLoader.cpp
    Source/Features/Deck/UI/DeckShellComponent.cpp
    Source/Features/Deck/UI/PitchFaderComponent.cpp
    Source/Features/Deck/UI/GainKnobComponent.cpp
    Source/Features/Deck/UI/TrackInfoComponent.cpp
    Source/Features/Deck/UI/DeckLayoutManager.cpp
    Source/Features/Waveform/WaveformAnalyzer.cpp
    Source/Features/Waveform/WaveformManager.cpp
    Source/Features/Waveform/OverviewWaveform.cpp
    Source/Features/Waveform/DetailWaveform.cpp
    Source/Features/BeatGrid/BeatGridAnalyzer.cpp
    Source/Features/BeatGrid/BeatGridManager.cpp
    Source/Features/KeyDetection/KeyDetectionAnalyzer.cpp
    Source/Features/KeyDetection/KeyDetectionManager.cpp
    Source/Features/TimeStretch/TimeStretcher.cpp
    Source/Features/Cue/HotCueManager.cpp
    Source/Features/Cue/HotCuePadComponent.cpp
    Source/Features/Quantize/QuantizeButtonComponent.cpp
    Source/Features/Loop/LoopEngine.cpp
    Source/Features/Loop/LoopControlComponent.cpp
    Source/Features/BeatJump/BeatJumpEngine.cpp
    Source/Features/BeatJump/BeatJumpComponent.cpp
    Source/Features/SlipMode/SlipButtonComponent.cpp
    Source/Features/StemSeparation/OnnxInference.cpp
    Source/Features/StemSeparation/ModelManager.cpp
    Source/Features/StemSeparation/SpectralProcessor.cpp
    Source/Features/StemSeparation/StemCache.cpp
    Source/Features/StemSeparation/StemSeparationEngine.cpp
    Source/Features/StemSeparation/StemSeparationManager.cpp
    Source/Features/StemSeparation/UI/StemSeparateButton.cpp
    Source/Features/StemSeparation/UI/StemToggleComponent.cpp
    Source/Features/Sync/MasterClockPublisher.cpp
    Source/Features/Sync/MasterClockManager.cpp
    Source/Features/Sync/SyncEngine.cpp
    Source/Features/Sync/PhaseLockEngine.cpp
    Source/Features/Sync/UI/SyncButton.cpp
    Source/Features/Sync/UI/MasterButton.cpp
    Source/Features/Midi/JuceMidiHost.cpp
    Source/Features/Midi/MidiDeviceManager.cpp
    Source/Features/Midi/MidiMessageBridge.cpp
    Source/Features/Midi/Migrations/MigrationRegistry.cpp
    Source/Features/Midi/MappingParser.cpp
    Source/Features/Midi/BindingResolver.cpp
    Source/Features/Midi/MappingSerializer.cpp
    Source/Features/Midi/MappingStore.cpp
    Source/Features/Midi/Bundle/SonikMidiBundleSerializer.cpp
    Source/Features/Midi/Bundle/MappingExportService.cpp
    Source/Features/Midi/Bundle/MappingImportService.cpp
    Source/Features/Midi/UI/MidiSettingsWindow.cpp
    Source/Features/Midi/UI/MidiSettingsPanel.cpp
    Source/Features/Midi/UI/Molecules/DeviceHeader.cpp
    Source/Features/Midi/UI/Molecules/BindingTable.cpp
    Source/Features/Midi/UI/Organisms/MidiImportDialog.cpp
    Source/Features/Midi/UI/Organisms/MidiExportDialog.cpp
    Source/Features/Midi/MidiFeedbackEngine.cpp
    Source/Features/Midi/MidiOutputThread.cpp
    Source/Features/Midi/MidiInboundRouter.cpp
    Source/Features/Midi/SoftTakeoverManager.cpp
    Source/MidiHandlers/DeckMidiHandler.cpp
    Source/MidiHandlers/MixerMidiHandler.cpp
    Source/MidiHandlers/LibraryMidiHandler.cpp
    Source/MidiHandlers/CompositeMidiCommandHandler.cpp
)

target_include_directories(SonikTests PRIVATE Source Tests ${RUBBERBAND_INCLUDE_DIRS})

target_compile_definitions(SonikTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_UNIT_TESTS=1
    JUCE_MODAL_LOOPS_PERMITTED=1
)

target_link_libraries(SonikTests PRIVATE
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_audio_basics
    juce::juce_cryptography
    juce::juce_data_structures
    juce::juce_core
    juce::juce_events
    juce::juce_dsp
    sqlite3
    essentia
    onnxruntime
    ${RUBBERBAND_LINK_LIBRARIES}
    SonikBinaryData
    juce::juce_recommended_config_flags
)

if(APPLE)
    target_link_libraries(SonikTests PRIVATE
        "-framework CoreML"
        "-framework Foundation"
    )
endif()
