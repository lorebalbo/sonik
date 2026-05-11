#include "LibraryStatusText.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include <sqlite3.h>

namespace SonikLibraryUi
{
juce::String statusTextForUpdate (const LibraryAnalysisQueue::StatusUpdate& update)
{
    switch (update.status)
    {
        case LibraryAnalysisQueue::JobStatus::Queued:
            return update.kind == LibraryAnalysisQueue::JobKind::StemSeparation ? "Queued (Stems)" : "Queued";

        case LibraryAnalysisQueue::JobStatus::Running:
            return update.kind == LibraryAnalysisQueue::JobKind::StemSeparation
                ? "Separating Stems " + juce::String (juce::jlimit (0, 100, update.percent)) + "%"
                : "Analyzing " + juce::String (juce::jlimit (0, 100, update.percent)) + "%";

        case LibraryAnalysisQueue::JobStatus::Complete:
            return update.kind == LibraryAnalysisQueue::JobKind::StemSeparation ? "Stem Complete" : "Complete";

        case LibraryAnalysisQueue::JobStatus::Failed:
            return update.kind == LibraryAnalysisQueue::JobKind::StemSeparation ? "Stem Failed" : "Failed";
    }

    return "Failed";
}

void clearAnalysisCache (TrackDatabase& db,
                         const juce::String& filePath,
                         const juce::String& contentHash)
{
    auto* handle = db.getDbHandle();
    if (handle == nullptr)
        return;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (handle,
                            "DELETE FROM track_data WHERE file_path=? AND content_hash=?",
                            -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1, filePath.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 2, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }
}
} // namespace SonikLibraryUi
