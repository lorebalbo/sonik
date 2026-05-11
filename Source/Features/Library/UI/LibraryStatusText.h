#pragma once

#include "Features/Library/LibraryAnalysisQueue.h"
#include <juce_core/juce_core.h>

class TrackDatabase;

namespace SonikLibraryUi
{
    /// Build the human-readable status string used in the Library "Status" column.
    /// Pure function — exposed at namespace scope so it can be unit-tested without
    /// instantiating LibraryComponent.
    juce::String statusTextForUpdate (const LibraryAnalysisQueue::StatusUpdate& update);

    /// Clear cached analysis state (track_data row) for a track keyed by
    /// (file_path, content_hash).  Used by the "Force Re-Analyze" path.
    void clearAnalysisCache (TrackDatabase& db,
                             const juce::String& filePath,
                             const juce::String& contentHash);
}
