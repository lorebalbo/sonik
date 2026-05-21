#pragma once

#include <juce_core/juce_core.h>

namespace IDs
{

#define DECLARE_ID(name) const juce::Identifier name (#name);

DECLARE_ID (SonikState)
DECLARE_ID (Decks)
DECLARE_ID (Deck)
DECLARE_ID (id)
DECLARE_ID (activeDeckId)
DECLARE_ID (deckCount)
DECLARE_ID (playbackStatus)
DECLARE_ID (isMasterTempo)
DECLARE_ID (gain)
DECLARE_ID (quantizeEnabled)
DECLARE_ID (slipEnabled)
DECLARE_ID (keyLockEnabled)
DECLARE_ID (pitchRange)
DECLARE_ID (pitch)
DECLARE_ID (speedMultiplier)
DECLARE_ID (TrackMetadata)
DECLARE_ID (filePath)
DECLARE_ID (contentHash)
DECLARE_ID (title)
DECLARE_ID (artist)
DECLARE_ID (album)
DECLARE_ID (duration)
DECLARE_ID (sampleRate)
DECLARE_ID (bitDepth)
DECLARE_ID (totalSamples)
DECLARE_ID (hasAlbumArt)
DECLARE_ID (Playhead)
DECLARE_ID (position)
DECLARE_ID (TempCue)
DECLARE_ID (CuePoints)
DECLARE_ID (CuePoint)
DECLARE_ID (index)
DECLARE_ID (colorIndex)
DECLARE_ID (label)
DECLARE_ID (isValid)
DECLARE_ID (BeatGrid)
DECLARE_ID (bpm)
DECLARE_ID (anchorSample)
DECLARE_ID (beatIntervalSamples)
DECLARE_ID (confidence)
DECLARE_ID (manuallyAdjusted)
DECLARE_ID (KeyInfo)
DECLARE_ID (keyIndex)
DECLARE_ID (Loop)
DECLARE_ID (loopIn)
DECLARE_ID (loopOut)
DECLARE_ID (active)
DECLARE_ID (loopMode)
DECLARE_ID (beatJumpSize)
DECLARE_ID (slipDisplaced)
DECLARE_ID (Stems)
DECLARE_ID (status)
DECLARE_ID (progress)
DECLARE_ID (vocalsMuted)
DECLARE_ID (drumsMuted)
DECLARE_ID (bassMuted)
DECLARE_ID (otherMuted)
DECLARE_ID (vocalsSolo)
DECLARE_ID (drumsSolo)
DECLARE_ID (bassSolo)
DECLARE_ID (otherSolo)
DECLARE_ID (stemError)
DECLARE_ID (stemStretchDegraded)
DECLARE_ID (modelVersion)
DECLARE_ID (Waveform)
DECLARE_ID (analysisStatus)
DECLARE_ID (analysisProgress)

// Audio device state (PRD-0002)
DECLARE_ID (AudioDevice)
DECLARE_ID (deviceName)
DECLARE_ID (bufferSize)
DECLARE_ID (outputLatencyMs)
DECLARE_ID (cpuLoad)
DECLARE_ID (cpuOverload)
DECLARE_ID (deviceError)

// Loading state (PRD-0003)
DECLARE_ID (loadingStatus)
DECLARE_ID (loadingProgress)
DECLARE_ID (loadingError)
DECLARE_ID (channelCount)

// Key transpose (UI — semitone offset applied on top of detected key)
DECLARE_ID (keyShift)

// Sync mode (UI — deck BPM sync with master)
DECLARE_ID (syncEnabled)

// Master Clock (PRD-0026)
DECLARE_ID (isMaster)         // per-deck bool: this deck is the master clock source
DECLARE_ID (isSynced)         // per-deck bool: this deck is synced to the master clock
DECLARE_ID (masterDeckIndex)  // root-level int: Decks-array index of current master, -1 = dormant

// Active controller tab: "loop" | "cue" | "jump" | "grid"
DECLARE_ID (controllerTab)

// Per-deck EQ (message-thread state; audio engine reads on next processBlock)
DECLARE_ID (eqHigh)  // normalised 0..1 (default 1.0 = flat)
DECLARE_ID (eqMid)   // normalised 0..1 (default 1.0 = flat)
DECLARE_ID (eqLow)   // normalised 0..1 (default 1.0 = flat)

// Global mixer (written by MixerMidiHandler, read by audio engine)
DECLARE_ID (crossfader)      // normalised 0..1 (0.5 = centre)
DECLARE_ID (masterGain)      // normalised 0..1 (default 1.0)
DECLARE_ID (headphonesGain)  // normalised 0..1 (default 1.0)
DECLARE_ID (headphoneCueEnabled) // bool per-deck

// Track loading protocol (PRD-0034)
DECLARE_ID (pendingLoadPath)   // Library writes this; DeckShellComponent watches & calls AudioFileLoader
DECLARE_ID (loadedFilePath)    // Set by DeckShellComponent after initiating a successful load

// Signals that a track's BPM was manually edited and persisted to the DB;
// root-level property consumed by LibraryComponent to refresh its query.
DECLARE_ID (trackBpmManuallyChanged)

#undef DECLARE_ID

} // namespace IDs
