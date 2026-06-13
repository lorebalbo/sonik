cmake_minimum_required(VERSION 3.24)
project(Sonik VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# macOS deployment target
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment version")
endif()

#==============================================================================
# Build accelerators (zero effect on the produced binaries)
#==============================================================================
# ccache transparently caches compiler output, so rebuilding after a clean or
# a branch switch reuses previous results. Auto-detected and a no-op when not
# installed (`brew install ccache` to enable it).
find_program(SONIK_CCACHE_PROGRAM ccache)
if(SONIK_CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${SONIK_CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${SONIK_CCACHE_PROGRAM}")
    message(STATUS "Sonik: ccache enabled (${SONIK_CCACHE_PROGRAM})")
endif()

# Download CPM.cmake once; previously this re-downloaded on every configure.
set(SONIK_CPM_PATH "${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake")
if(NOT EXISTS "${SONIK_CPM_PATH}")
    file(DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.2/CPM.cmake
        "${SONIK_CPM_PATH}")
endif()
include("${SONIK_CPM_PATH}")

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

# LAME MP3 encoder (PRD-0100 §1.5.1) — pluggable MP3 backend dependency.
# Detection order: pkg-config (mp3lame), then the Homebrew keg path, then a
# generic find_library/find_path. When found we compile LameMp3EncoderBackend.cpp
# and define SONIK_HAVE_LAME=1; when NOT found we compile NoMp3EncoderBackend.cpp
# and leave SONIK_HAVE_LAME undefined (capability table reports MP3 unsupported).
set(SONIK_LAME_FOUND FALSE)

pkg_check_modules(MP3LAME QUIET mp3lame)
if(MP3LAME_FOUND)
    set(SONIK_LAME_FOUND TRUE)
    set(SONIK_LAME_LIBRARY ${MP3LAME_LINK_LIBRARIES})
    set(SONIK_LAME_INCLUDE_DIR ${MP3LAME_INCLUDE_DIRS})
else()
    # Homebrew keg-only path first, then a generic search.
    find_library(SONIK_LAME_LIBRARY
        NAMES mp3lame libmp3lame
        HINTS /opt/homebrew/opt/lame/lib /usr/local/opt/lame/lib /usr/local/lib /usr/lib)
    find_path(SONIK_LAME_INCLUDE_DIR
        NAMES lame/lame.h
        HINTS /opt/homebrew/opt/lame/include /usr/local/opt/lame/include /usr/local/include /usr/include)
    if(SONIK_LAME_LIBRARY AND SONIK_LAME_INCLUDE_DIR)
        set(SONIK_LAME_FOUND TRUE)
    endif()
endif()

if(SONIK_LAME_FOUND)
    message(STATUS "Sonik: LAME found -> MP3 export ENABLED "
                   "(lib=${SONIK_LAME_LIBRARY}, include=${SONIK_LAME_INCLUDE_DIR})")
    set(SONIK_MP3_BACKEND_SOURCES
        Source/Features/Daw/Export/LameMp3EncoderBackend.h
        Source/Features/Daw/Export/LameMp3EncoderBackend.cpp)
else()
    message(STATUS "Sonik: LAME NOT found -> MP3 export DISABLED (WAV + FLAC only)")
    set(SONIK_MP3_BACKEND_SOURCES
        Source/Features/Daw/Export/NoMp3EncoderBackend.cpp)
endif()

# Bundled MIDI mapping profiles (PRD-0043) and the DESIGN.md typeface (Space
# Mono, SIL OFL — see Assets/Fonts/OFL.txt). Embedding the font guarantees the
# design-system typography renders identically on every machine instead of
# silently falling back to the platform monospaced font.  Generates a static
# library `SonikBinaryData` with `BinaryData::*` symbols.
juce_add_binary_data(SonikBinaryData
    SOURCES
        Resources/MidiMappings/behringer-ddm4000.json
        Resources/MidiMappings/generic-midi.json
        Assets/Fonts/SpaceMono-Regular.ttf
        Assets/Fonts/SpaceMono-Bold.ttf
)

#==============================================================================
# SonikJuce — every JUCE module, compiled exactly ONCE.
#
# JUCE modules are INTERFACE targets: their sources are compiled into whatever
# target links them *directly*. Previously Sonik, SonikTests and
# AutomationUiScreenshot each linked the modules directly, so every JUCE
# translation unit was compiled three times. Linking the modules PRIVATE into
# this static library compiles them once; the two generator expressions below
# re-export the modules' include paths and compile definitions to consumers
# (this is the standard JUCE "shared code" pattern — PRIVATE linkage stops the
# module sources from propagating, the re-export keeps headers and JUCE config
# macros identical in every consuming translation unit).
#==============================================================================
add_library(SonikJuce STATIC)

target_link_libraries(SonikJuce
    PRIVATE
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
        juce::juce_graphics
    PUBLIC
        juce::juce_recommended_config_flags
)

# Note: juce_recommended_warning_flags is deliberately NOT applied here. JUCE
# 8.0.6 itself is not warning-clean against newer macOS SDKs (e.g. the
# NSEventTypeMouseCancelled -Wswitch-enum warning), and third-party code is
# not ours to fix. Project targets keep the strict warning set.

# JUCE configuration: PUBLIC so every consuming translation unit sees the same
# JUCE config macros as the module sources compiled into this library.
target_compile_definitions(SonikJuce
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        # The tests drive the message loop with MessageManager::runDispatchLoopUntil,
        # which only exists when this macro is set. It must match in the module
        # sources (compiled here) and in every consumer, so it is PUBLIC.
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_APPLICATION_NAME_STRING="Sonik"
        JUCE_APPLICATION_VERSION_STRING="0.1.0"
    INTERFACE
        $<TARGET_PROPERTY:SonikJuce,COMPILE_DEFINITIONS>
)

target_include_directories(SonikJuce
    INTERFACE
        $<TARGET_PROPERTY:SonikJuce,INCLUDE_DIRECTORIES>
)

#==============================================================================
# SonikCore — every feature slice shared by the app, the tests and the tools,
# compiled exactly ONCE. Previously SonikTests recompiled ~115 of these files
# a second time and AutomationUiScreenshot a third time for a handful of them.
#==============================================================================
add_library(SonikCore STATIC
    Source/Features/Shared/Ui/SonikTheme.h
    Source/Features/Shared/Ui/SonikDraw.h
    Source/Features/Shared/Ui/SonikDraw.cpp
    Source/Features/Shared/Ui/SonikLookAndFeel.h
    Source/Features/Shared/Ui/SonikLookAndFeel.cpp
    Source/Features/Deck/DeckIdentifiers.h
    Source/Features/Deck/DeckStateManager.h
    Source/Features/Deck/DeckStateManager.cpp
    Source/Features/Deck/AudioThreadState.h
    Source/Features/Daw/State/DawState.h
    Source/Features/Daw/State/DawState.cpp
    Source/Features/Daw/State/DawClipModel.h
    Source/Features/Daw/Session/SessionSchema.h
    Source/Features/Daw/Session/SessionMigrator.h
    Source/Features/Daw/Session/SessionMigrator.cpp
    Source/Features/Daw/Session/SessionSerializer.h
    Source/Features/Daw/Session/SessionSerializer.cpp
    Source/Features/Daw/Session/SourceIdResolver.h
    Source/Features/Daw/Session/SourceIdResolver.cpp
    Source/Features/Daw/Session/SessionSourceResolution.h
    Source/Features/Daw/Session/SessionSourceResolution.cpp
    Source/Features/Daw/Session/SessionController.h
    Source/Features/Daw/Session/SessionController.cpp
    Source/Features/Daw/Session/Ui/SessionDialogs.h
    Source/Features/Daw/Session/Ui/SessionDialogs.cpp
    Source/Features/Daw/Session/Ui/UnresolvedSourcesDialog.h
    Source/Features/Daw/Session/Ui/UnresolvedSourcesDialog.cpp
    Source/Features/Daw/Session/Ui/SessionMenu.h
    Source/Features/Daw/Session/Ui/SessionMenu.cpp
    Source/Features/Daw/Import/ImportSource.h
    Source/Features/Daw/Import/ImportSourcePublisher.h
    Source/Features/Daw/Import/ImportClipPlacer.h
    Source/Features/Daw/Import/ImportClipPlacer.cpp
    Source/Features/Daw/Import/AudioFileImporter.h
    Source/Features/Daw/Import/AudioFileImporter.cpp
    Source/Features/Daw/Model/DawClip.h
    Source/Features/Daw/Model/ChannelGroup.h
    Source/Features/Daw/Model/MuteSolo.h
    Source/Features/Daw/Model/MasterGridService.h
    Source/Features/Daw/Model/MasterGridService.cpp
    Source/Features/Daw/Transform/TimelineTransform.h
    Source/Features/Daw/Transform/TimelineTransform.cpp
    Source/Features/Daw/Ui/Atoms/RulerTick.h
    Source/Features/Daw/Ui/Molecules/TimeRuler.h
    Source/Features/Daw/Ui/Molecules/TimeRuler.cpp
    Source/Features/Daw/Ui/DawLayoutMetrics.h
    Source/Features/Daw/Ui/Atoms/ClipBlock.h
    Source/Features/Daw/Ui/Atoms/ClipBlock.cpp
    Source/Features/Daw/Ui/Molecules/LaneView.h
    Source/Features/Daw/Ui/Molecules/LaneView.cpp
    Source/Features/Daw/Ui/Molecules/ChannelGroupHeader.h
    Source/Features/Daw/Ui/Atoms/MuteSoloButton.h
    Source/Features/Daw/Ui/Molecules/GroupOverviewStrip.h
    Source/Features/Daw/Ui/Molecules/GroupOverviewStrip.cpp
    Source/Features/Daw/Ui/Organisms/ChannelGroupView.h
    Source/Features/Daw/Ui/Organisms/ChannelGroupView.cpp
    Source/Features/Daw/Ui/Organisms/ChannelGroupStack.h
    Source/Features/Daw/Ui/Organisms/ChannelGroupStack.cpp
    Source/Features/Daw/Ui/Organisms/DawPanel.h
    Source/Features/Daw/Ui/Organisms/DawPanel.cpp
    Source/Features/Daw/Ui/Atoms/Playhead.h
    Source/Features/Daw/Ui/FollowController.h
    Source/Features/Daw/Projection/SourceModeReader.h
    Source/Features/Daw/Projection/LiveProjectionTimer.h
    Source/Features/Daw/Projection/LiveProjectionTimer.cpp
    Source/Features/Daw/Projection/DeckManagerProjectionSource.h
    Source/Features/Daw/Recording/RecordingSessionController.h
    Source/Features/Daw/Recording/RecordingSessionController.cpp
    Source/Features/Daw/Automation/AutomationCaptureTaps.h
    Source/Features/Daw/Automation/AutomationCaptureTaps.cpp
    Source/Features/Daw/Automation/MasterTempoAutomationCapture.h
    Source/Features/Daw/Automation/ChannelContinuousAutomationCapture.h
    Source/Features/Daw/Automation/ChannelBooleanAutomationCapture.h
    Source/Features/Daw/Automation/AutomationApplier.h
    Source/Features/Daw/Automation/Ui/AutomationLaneMetrics.h
    Source/Features/Daw/Automation/Ui/AutomationLaneViewBase.h
    Source/Features/Daw/Automation/Ui/ContinuousAutomationLaneView.h
    Source/Features/Daw/Automation/Ui/BooleanAutomationLaneView.h
    Source/Features/Daw/Automation/Ui/AutomationLaneStackView.h
    Source/Features/Daw/Recording/PerformanceEventFifo.h
    Source/Features/Daw/Recording/PerformanceEventFifo.cpp
    Source/Features/Daw/Recording/ClipPlacementEngine.h
    Source/Features/Daw/Recording/ClipPlacementEngine.cpp
    Source/Features/Daw/Recording/AlignmentResolver.h
    Source/Features/Daw/Recording/AlignmentResolver.cpp
    Source/Features/Daw/Recording/PerformanceCaptureSink.h
    # PRD-0099: Offline Render Driver (non-real-time engine driver, no UI)
    Source/Features/Daw/Export/OfflineRenderDriver.h
    Source/Features/Daw/Export/OfflineRenderDriver.cpp
    # PRD-0100: Audio Exporter (WAV / FLAC / MP3) — programmatic API, no UI
    Source/Features/Daw/Export/ExportOptions.h
    Source/Features/Daw/Export/Mp3EncoderBackend.h
    Source/Features/Daw/Export/AudioExporter.h
    Source/Features/Daw/Export/AudioExporter.cpp
    # PRD-0101: Export Dialog, Progress & Cancel (UI capstone)
    Source/Features/Daw/Export/ExportRunner.h
    Source/Features/Daw/Export/ExportJobBuilder.h
    Source/Features/Daw/Export/Ui/ExportDialog.h
    Source/Features/Daw/Export/Ui/ExportDialog.cpp
    ${SONIK_MP3_BACKEND_SOURCES}
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
    Source/Features/StemSeparation/UI/SourceModeToggleComponent.h
    Source/Features/StemSeparation/UI/SourceModeToggleComponent.cpp
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
    Source/Features/Midi/UI/Atoms/BindToPortToggle.h
    Source/Features/Midi/UI/Molecules/TargetPicker.h
    Source/Features/Midi/UI/Molecules/DeviceHeader.h
    Source/Features/Midi/UI/Molecules/DeviceHeader.cpp
    Source/Features/Midi/UI/Molecules/BindingTable.h
    Source/Features/Midi/UI/Molecules/BindingTable.cpp
    Source/Features/Midi/UI/Molecules/PortBindingControls.h
    Source/Features/Midi/UI/Molecules/PortBindingControls.cpp
    Source/Features/Midi/MidiFeedbackEngine.h
    Source/Features/Midi/MidiFeedbackEngine.cpp
    Source/Features/Midi/MidiOutputThread.h
    Source/Features/Midi/MidiOutputThread.cpp
    Source/Features/Midi/MidiCommandHandler.h
    Source/Features/Midi/MidiInboundRouter.h
    Source/Features/Midi/MidiInboundRouter.cpp
    Source/Features/Midi/SoftTakeoverManager.h
    Source/Features/Midi/SoftTakeoverManager.cpp
    Source/Features/Mixer/State/MixerIdentifiers.h
    Source/Features/Mixer/State/MixerIdentifiers.cpp
    Source/Features/Mixer/State/MixerParam.h
    Source/Features/Mixer/State/MixerAtomicSnapshot.h
    Source/Features/Mixer/State/MixerMeterSnapshot.h
    Source/Features/Mixer/State/MixerStateSchema.h
    Source/Features/Mixer/State/MixerStateSchema.cpp
    Source/Features/Mixer/State/MixerStateBridge.h
    Source/Features/Mixer/State/MixerStateBridge.cpp
    Source/Features/Mixer/Routing/ChannelStripSnapshot.h
    Source/Features/Mixer/Routing/CrossfaderSnapshot.h
    Source/Features/Mixer/Routing/MasterSnapshot.h
    Source/Features/Mixer/Routing/ChannelStripProcessor.h
    Source/Features/Mixer/Routing/ChannelStripProcessor.cpp
    Source/Features/Mixer/Dsp/ThreeBandEq.h
    Source/Features/Mixer/Dsp/ThreeBandEq.cpp
    Source/Features/Mixer/Dsp/ChannelFilter.h
    Source/Features/Mixer/Dsp/ChannelFilter.cpp
    Source/Features/Mixer/Routing/ABBus.h
    Source/Features/Mixer/Routing/CrossfaderStage.h
    Source/Features/Mixer/Routing/MasterStage.h
    Source/Features/Mixer/Routing/MasterStage.cpp
    Source/Features/Mixer/Routing/ChannelMeter.h
    Source/Features/Mixer/Ui/Atoms/MixRotaryKnob.h
    Source/Features/Mixer/Ui/Atoms/MixRotaryKnob.cpp
    Source/Features/Mixer/Ui/Atoms/MixKillButton.h
    Source/Features/Mixer/Ui/Atoms/MixKillButton.cpp
    Source/Features/Mixer/Ui/Atoms/MixAssignButton.h
    Source/Features/Mixer/Ui/Atoms/MixAssignButton.cpp
    Source/Features/Mixer/Ui/Atoms/MixLevelMeter.h
    Source/Features/Mixer/Ui/Atoms/MixLevelMeter.cpp
    Source/Features/Mixer/Ui/Atoms/MixFader.h
    Source/Features/Mixer/Ui/Atoms/MixFader.cpp
    Source/Features/Mixer/Ui/Molecules/ChannelStripEqSection.h
    Source/Features/Mixer/Ui/Molecules/ChannelStripEqSection.cpp
    Source/Features/Mixer/Ui/Molecules/ChannelStripMeter.h
    Source/Features/Mixer/Ui/Molecules/ChannelStripMeter.cpp
    Source/Features/Mixer/Ui/Molecules/CrossfaderRail.h
    Source/Features/Mixer/Ui/Molecules/CrossfaderRail.cpp
    Source/Features/Mixer/Ui/Organisms/ChannelStrip.h
    Source/Features/Mixer/Ui/Organisms/ChannelStrip.cpp
    Source/Features/Mixer/Ui/Organisms/MasterSection.h
    Source/Features/Mixer/Ui/Organisms/MasterSection.cpp
    Source/Features/Mixer/Ui/Organisms/MixerComponent.h
    Source/Features/Mixer/Ui/Organisms/MixerComponent.cpp
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

target_include_directories(SonikCore PUBLIC Source)
target_include_directories(SonikCore SYSTEM PUBLIC ${RUBBERBAND_INCLUDE_DIRS})

target_link_libraries(SonikCore
    PUBLIC
        SonikJuce
        SonikBinaryData
        sqlite3
        essentia
        onnxruntime
        ${RUBBERBAND_LINK_LIBRARIES}
    PRIVATE
        juce::juce_recommended_warning_flags
)

# Conditionally link Apple frameworks for CoreML EP (macOS only)
if(APPLE)
    target_link_libraries(SonikCore PUBLIC
        "-framework CoreML"
        "-framework Foundation"
    )
endif()

# PRD-0100: LAME MP3 backend. The SONIK_HAVE_LAME define is PUBLIC because
# Mp3EncoderBackend.h (consumed by app and test translation units) checks it.
if(SONIK_LAME_FOUND)
    target_compile_definitions(SonikCore PUBLIC SONIK_HAVE_LAME=1)
    target_include_directories(SonikCore PRIVATE ${SONIK_LAME_INCLUDE_DIR})
    target_link_libraries(SonikCore PUBLIC ${SONIK_LAME_LIBRARY})
endif()

# Precompiled header: the JUCE module headers below dominate the parse time of
# every translation unit. Precompiling them once cuts compile time for the
# whole library without changing any generated code.
target_precompile_headers(SonikCore PRIVATE
    <juce_gui_basics/juce_gui_basics.h>
    <juce_data_structures/juce_data_structures.h>
    <juce_audio_formats/juce_audio_formats.h>
    <juce_audio_basics/juce_audio_basics.h>
)

#==============================================================================
# Sonik Application — only the application entry point lives here; everything
# else comes from SonikCore/SonikJuce.
#==============================================================================
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
)

target_link_libraries(Sonik PRIVATE
    SonikCore
    juce::juce_recommended_warning_flags
)

#==============================================================================
# Test target — only the test translation units live here; all production code
# is linked from SonikCore (previously it was recompiled a second time).
#==============================================================================
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
    Tests/StemSeparationUiTests.cpp
    Tests/StemPlaybackTests.cpp
    Tests/DeckSourceModeTests.cpp
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
    Tests/MixerMidiWiringTests.cpp
    Tests/MidiSettingsPanelTests.cpp
    Tests/UsbPortDisambiguationTests.cpp
    Tests/MixerStateSchemaTests.cpp
    Tests/DawStateSchemaTests.cpp
    Tests/DawClipModelTests.cpp
    Tests/MasterGridServiceTests.cpp
    Tests/TimelineTransformTests.cpp
    Tests/DawPanelShellTests.cpp
    Tests/ChannelGroupLayoutTests.cpp
    Tests/GroupedTracksTests.cpp
    Tests/ClipBlockTests.cpp
    Tests/WaveformCacheTests.cpp
    Tests/LiveProjectionBridgeTests.cpp
    Tests/DawTimelineInteractionTests.cpp
    Tests/RecordingSessionControllerTests.cpp
    Tests/PerformanceEventFifoTests.cpp
    Tests/ClipPlacementEngineTests.cpp
    Tests/AlignmentResolverTests.cpp
    Tests/HotCueBeatJumpCaptureTests.cpp
    Tests/LoopCaptureTests.cpp
    Tests/SourceModeCaptureTests.cpp
    Tests/RecordingCaptureTests.cpp
    Tests/SignalFlowTests.cpp
    Tests/CrossfaderTests.cpp
    Tests/ChannelStripTests.cpp
    Tests/ThreeBandEqTests.cpp
    Tests/ChannelFilterTests.cpp
    Tests/MasterMeterTests.cpp
    Tests/MixerUiAtomsTests.cpp
    Tests/MixerComponentTests.cpp
    Tests/ChannelStripUiTests.cpp
    # PRD-0079 – PRD-0086: Timeline Playback Engine & Non-Destructive Clip Editing
    Tests/ArrangementSnapshotTests.cpp
    Tests/ClipStreamerTests.cpp
    Tests/TimelineRendererTests.cpp
    Tests/DawTransportTests.cpp
    Tests/EditCommandTests.cpp
    # EPIC-0011: DAW Automation System
    Tests/AutomationModelTests.cpp
    Tests/AutomationCaptureTapsTests.cpp
    Tests/MasterTempoAutomationCaptureTests.cpp
    Tests/ContinuousLaneCaptureTests.cpp
    Tests/ChannelBooleanLaneCaptureTests.cpp
    Tests/AutomationApplierTests.cpp
    Tests/AutomationLaneUiTests.cpp
    Tests/AutomationEditingTests.cpp
    # EPIC-0012: Session Persistence & Audio Export
    Tests/SessionSerializerTests.cpp
    Tests/SourceIdResolverTests.cpp
    Tests/SourceResolutionIntegrationTests.cpp
    Tests/SessionControllerTests.cpp
    Tests/SessionIntegrationTests.cpp
    Tests/ImportClipTests.cpp
    Tests/ImportPipelineTests.cpp
    # PRD-0099: Offline Render Driver (driven headlessly by OfflineRenderDriverTests)
    Tests/OfflineRenderDriverTests.cpp
    # PRD-0100 / PRD-0101: Audio Exporter + Export Dialog seams
    Tests/AudioExporterTests.cpp
    Tests/AudioExportTests.cpp
    Tests/SessionRoundTripTests.cpp
)

target_include_directories(SonikTests PRIVATE Tests)

target_link_libraries(SonikTests PRIVATE SonikCore)

# Same precompiled header set as SonikCore: test translation units include the
# same heavyweight JUCE headers.
target_precompile_headers(SonikTests PRIVATE
    <juce_gui_basics/juce_gui_basics.h>
    <juce_data_structures/juce_data_structures.h>
    <juce_audio_formats/juce_audio_formats.h>
    <juce_audio_basics/juce_audio_basics.h>
)

#==============================================================================
# PRD-0093: AutomationUiScreenshot — standalone deterministic PNG render of the
# automation lane organisms (the seed demo data lives only in the tool).
#==============================================================================
add_executable(AutomationUiScreenshot
    Tools/AutomationUiScreenshot.cpp
)

target_link_libraries(AutomationUiScreenshot PRIVATE SonikCore)
